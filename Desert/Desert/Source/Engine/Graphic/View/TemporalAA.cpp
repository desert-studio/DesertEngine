#include "TemporalAA.hpp"

#include <Engine/Core/ShaderCompiler/ShaderVariant.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/Shader/ShaderService.hpp>

#include <cstddef>
#include <format>
#include <span>
#include <string>

namespace Desert::Graphic
{
    namespace
    {
        constexpr uint32_t    kGroupSize       = 8; // TemporalAA.shader LocalSize(8, 8, 1)
        constexpr const char* kShaderName      = "TemporalAA";
        constexpr const char* kHistoryName     = "TAA.History";
        constexpr const char* kPreviousName    = "TAA.History.Previous";
        constexpr const char* kOutputName      = "TAA.Output";
        constexpr const char* kParamsName      = "TAA.Params";
        constexpr auto        kHistoryFormat   = Core::Formats::ImageFormat::RGBA16F;
        constexpr uint32_t    kQualityVariants = 3; // TemporalAAQuality Low / Medium / High

        uint32_t GroupCount( uint32_t size )
        {
            return ( size + kGroupSize - 1 ) / kGroupSize;
        }

        bool IsTemporalSplit( const ResolutionSplit& split )
        {
            return split.Render.Width > 0 && split.Render.Height > 0 && split.Output.Width > 0 &&
                   split.Output.Height > 0;
        }
    } // namespace

    const std::shared_ptr<const RDG::ShaderBindingLayout>& TemporalAALayout()
    {
        using Kind = RDG::ShaderResourceKind;
        static const std::shared_ptr<const RDG::ShaderBindingLayout> layout =
             std::make_shared<const RDG::ShaderBindingLayout>(
                  RDG::ShaderBindingLayout{ .ShaderName        = kShaderName,
                                            .Slots             = { { "u_SceneColor", Kind::SampledTexture },
                                                                   { "u_SceneDepth", Kind::SampledTexture },
                                                                   { "u_Velocity", Kind::SampledTexture },
                                                                   { "u_Exposure", Kind::SampledTexture },
                                                                   { "u_History", Kind::SampledTexture },
                                                                   { "u_HistoryOut", Kind::StorageTexture },
                                                                   { "u_Output", Kind::StorageTexture },
                                                                   { "TemporalAABuffer", Kind::StorageBuffer } },
                                            .PushConstantBytes = 0 } );
        return layout;
    }

    TemporalAAParams MakeTemporalAAParams( const ViewFrame& frame, RDG::Extent3D output )
    {
        TemporalAAParams params;
        params.InvViewProjection  = frame.InvViewProjection;
        params.PrevViewProjection = frame.PrevViewProjection;
        params.RenderSize         = glm::vec2( static_cast<float>( frame.Split.Render.Width ),
                                               static_cast<float>( frame.Split.Render.Height ) );
        params.OutputSize   = glm::vec2( static_cast<float>( output.Width ), static_cast<float>( output.Height ) );
        params.JitterUv     = frame.JitterNdc * glm::vec2( 0.5f, -0.5f ); // Velocity.hpp: uv = ndc * (0.5, -0.5)
        params.HistoryValid = frame.HistoryValid() ? 1.0f : 0.0f;
        return params;
    }

    TemporalAA::TemporalAA( TemporalMethod method ) : m_Method( method )
    {
    }

    TemporalAA::~TemporalAA() = default;

    std::string_view TemporalAA::DebugName() const
    {
        return m_Method == TemporalMethod::TAAU ? "TAAU" : "TAA";
    }

    bool TemporalAA::Supports( const ResolutionSplit& split ) const
    {
        using Common::Scalability::ScaleMode;
        if ( m_Method == TemporalMethod::TAAU )
            return split.Mode == ScaleMode::Upscale;
        return split.Mode == ScaleMode::Native || split.Mode == ScaleMode::Supersample;
    }

    RDG::Extent3D TemporalAA::ResolveExtent( const ResolutionSplit& split ) const
    {
        const ViewExtent& extent = m_Method == TemporalMethod::TAAU ? split.Output : split.Render;
        return RDG::Extent3D{ extent.Width, extent.Height, 1 };
    }

    std::vector<HistoryTextureDesc> TemporalAA::HistoryDescs( const ResolutionSplit& split ) const
    {
        HistoryTextureDesc history;
        history.Desc.Size    = ResolveExtent( split );
        history.Desc.Format  = kHistoryFormat;
        history.Name         = kHistoryName;
        history.PreviousName = kPreviousName;
        return { history };
    }

    Common::ResultStr<TemporalUpscalerOutputs> TemporalAA::AddPasses( RDG::Builder& graph, const ViewFrame& frame,
                                                                      const TemporalUpscalerInputs& inputs ) const
    {
        const std::string_view name    = DebugName();
        const auto             missing = [&]( const char* what ) -> Common::ResultStr<TemporalUpscalerOutputs>
        {
            return Common::MakeFormattedError<TemporalUpscalerOutputs>(
                 "{}", std::format( "{}: the input {} is not a texture of this graph", name, what ) );
        };
        if ( !inputs.SceneColor.IsValid() )
            return missing( "SceneColor" );
        if ( !inputs.SceneDepth.IsValid() )
            return missing( "SceneDepth" );
        if ( !inputs.Velocity.IsValid() )
            return missing( "Velocity" );
        if ( !inputs.Exposure.IsValid() )
            return missing( "Exposure" );
        if ( inputs.History.size() != 1 )
            return Common::MakeFormattedError<TemporalUpscalerOutputs>(
                 "{}",
                 std::format( "{}: {} histories given, HistoryDescs declares 1", name, inputs.History.size() ) );
        const HistoryRefs history = inputs.History[0];
        if ( !history.Previous.IsValid() || !history.Current.IsValid() )
            return missing( !history.Previous.IsValid() ? kPreviousName : kHistoryName );
        if ( !Supports( frame.Split ) || !IsTemporalSplit( frame.Split ) )
            return Common::MakeFormattedError<TemporalUpscalerOutputs>(
                 "{}", std::format( "{}: cannot resolve the split {}x{} -> {}x{} ({} %)", name,
                                    frame.Split.Render.Width, frame.Split.Render.Height, frame.Split.Output.Width,
                                    frame.Split.Output.Height, frame.Split.RenderScalePercent ) );

        const RDG::Extent3D extent = ResolveExtent( frame.Split );
        RDG::TextureDesc    outputDesc;
        outputDesc.Size              = extent;
        outputDesc.Format            = kHistoryFormat;
        const RDG::TextureRef output = graph.CreateTexture( outputDesc, kOutputName );

        const TemporalAAParams params = MakeTemporalAAParams( frame, extent );
        const RDG::BufferRef   buffer =
             graph.CreateBuffer( RDG::BufferDesc{ sizeof( TemporalAAParams ) }, kParamsName );
        graph.QueueBufferUpload( buffer, std::as_bytes( std::span( &params, 1 ) ) );

        const TemporalAAQuality quality = frame.Quality;
        graph.AddPass(
             kShaderName, RDG::PassFlags::Compute,
             [&]( RDG::PassBuilder& pass )
             {
                 // Every texture is read at texel centres or filtered inside the shader; the two linear samplers
                 // are the unjitter of the current colour and the bilinear history fetch (Low / Medium).
                 pass.Bindings( TemporalAALayout(), RDG::OtherRouteFill{} )
                      .Sampled( "u_SceneColor", inputs.SceneColor, RDG::Access::SampledCompute,
                                RDG::SubresourceRange::All(), RDG::SamplerDesc::LinearClamp() )
                      .Sampled( "u_SceneDepth", inputs.SceneDepth, RDG::Access::SampledCompute,
                                RDG::SubresourceRange::All(), RDG::SamplerDesc::PointClamp() )
                      .Sampled( "u_Velocity", inputs.Velocity, RDG::Access::SampledCompute,
                                RDG::SubresourceRange::All(), RDG::SamplerDesc::PointClamp() )
                      .Sampled( "u_Exposure", inputs.Exposure, RDG::Access::SampledCompute,
                                RDG::SubresourceRange::All(), RDG::SamplerDesc::PointClamp() )
                      .Sampled( "u_History", history.Previous, RDG::Access::SampledCompute,
                                RDG::SubresourceRange::All(), RDG::SamplerDesc::LinearClamp() )
                      .Storage( "u_HistoryOut", history.Current, RDG::Access::StorageWrite, 0 )
                      .Storage( "u_Output", output, RDG::Access::StorageWrite, 0 )
                      .Storage( "TemporalAABuffer", buffer, RDG::Access::StorageRead );
             },
             [this, quality, extent]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 std::string                            error;
                 const std::shared_ptr<ComputePipeline> pipeline = PipelineFor( quality, error );
                 if ( !pipeline )
                     return Common::MakeError( error );
                 const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                 return Renderer::GetInstance().DispatchCompute( bindings, *pipeline, GroupCount( extent.Width ),
                                                                 GroupCount( extent.Height ), 1 );
             } );

        return Common::MakeSuccess( TemporalUpscalerOutputs{ .SceneColor = output } );
    }

    std::shared_ptr<ComputePipeline> TemporalAA::PipelineFor( TemporalAAQuality quality, std::string& error ) const
    {
        const auto index = static_cast<uint32_t>( quality );
        if ( index >= kQualityVariants )
        {
            error = std::format( "{}: TemporalAAQuality {} has no shader variant", DebugName(), index );
            return nullptr;
        }

        const std::lock_guard lock( m_PipelineMutex );
        if ( m_Pipelines[index] )
            return m_Pipelines[index];

        const std::string       define  = std::format( "TAA_QUALITY={}", index );
        Runtime::ShaderService* service = Runtime::ResourceRegistry::GetShaderService();
        if ( !service )
        {
            error = std::format( "{}: no shader service to compile '{}' ({})", DebugName(), kShaderName, define );
            return nullptr;
        }
        const std::shared_ptr<Shader> shader =
             service->AcquireVariant( kShaderName, Core::ShaderVariant{ .Defines = { define } } );
        if ( !shader )
        {
            error = std::format( "{}: the shader '{}' ({}) did not compile — expected "
                                 "Editor/Resources/Shaders/Programs/TemporalAA/TemporalAA.shader",
                                 DebugName(), kShaderName, define );
            return nullptr;
        }
        const auto built = ComputePipeline::Create(
             { .Shader = shader, .DebugName = std::format( "{} ({})", kShaderName, define ) } );
        if ( !built )
        {
            error =
                 std::format( "{}: pipeline '{}' ({}): {}", DebugName(), kShaderName, define, built.GetError() );
            return nullptr;
        }
        m_Pipelines[index] = built.GetValue();
        return m_Pipelines[index];
    }
} // namespace Desert::Graphic
