#include "SpatialUpscale.hpp"

#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/Shader/ShaderService.hpp>

#include <format>
#include <mutex>
#include <string>

namespace Desert::Graphic
{
    namespace
    {
        constexpr uint32_t    kGroupSize                = 8; // both shaders: LocalSize(8, 8, 1)
        constexpr const char* kSpatialUpscaleShaderName = "SpatialUpscale";
        constexpr const char* kSharpenShaderName        = "Sharpen";

        uint32_t Groups( const uint32_t size )
        {
            return ( size + kGroupSize - 1 ) / kGroupSize;
        }

        // The compute pipeline of one shader, made on the first record and kept for the owner's life (a
        // function-local static would outlive the device).
        struct LazyComputePipeline
        {
            const char*                      ShaderName = nullptr;
            std::mutex                       Mutex;
            std::shared_ptr<ComputePipeline> Pipeline;

            std::shared_ptr<ComputePipeline> Get( std::string& error )
            {
                const std::lock_guard lock( Mutex );
                if ( Pipeline )
                    return Pipeline;
                Runtime::ShaderService*       service = Runtime::ResourceRegistry::GetShaderService();
                const std::shared_ptr<Shader> shader  = service ? service->GetByName( ShaderName ) : nullptr;
                if ( !shader )
                {
                    error = std::format( "the shader '{}' is not registered — expected "
                                         "Editor/Resources/Shaders/Programs/TemporalAA/{}.shader",
                                         ShaderName, ShaderName );
                    return nullptr;
                }
                const auto built = ComputePipeline::Create( { .Shader = shader, .DebugName = ShaderName } );
                if ( !built )
                {
                    error = std::format( "{}: {}", ShaderName, built.GetError() );
                    return nullptr;
                }
                Pipeline = built.GetValue();
                return Pipeline;
            }
        };
    } // namespace

    bool IsSpatialUpscale( const ViewFrame& frame )
    {
        return frame.Split.Mode == Common::Scalability::ScaleMode::Upscale && frame.Method == TemporalMethod::None;
    }

    bool SharpenRuns( const ViewFrame& frame, const int sharpnessPercent )
    {
        return sharpnessPercent > 0 && ( frame.Method != TemporalMethod::None || IsSpatialUpscale( frame ) );
    }

    const std::shared_ptr<const RDG::ShaderBindingLayout>& SpatialUpscaleLayout()
    {
        using Kind = RDG::ShaderResourceKind;
        static const std::shared_ptr<const RDG::ShaderBindingLayout> layout =
             std::make_shared<const RDG::ShaderBindingLayout>( RDG::ShaderBindingLayout{
                  .ShaderName = kSpatialUpscaleShaderName,
                  .Slots = { { "u_Source", Kind::SampledTexture }, { "u_Destination", Kind::StorageTexture } },
                  .PushConstantBytes = static_cast<uint32_t>( sizeof( SpatialUpscalePush ) ) } );
        return layout;
    }

    const std::shared_ptr<const RDG::ShaderBindingLayout>& SharpenLayout()
    {
        using Kind = RDG::ShaderResourceKind;
        static const std::shared_ptr<const RDG::ShaderBindingLayout> layout =
             std::make_shared<const RDG::ShaderBindingLayout>( RDG::ShaderBindingLayout{
                  .ShaderName = kSharpenShaderName,
                  .Slots = { { "u_Source", Kind::SampledTexture }, { "u_Destination", Kind::StorageTexture } },
                  .PushConstantBytes = static_cast<uint32_t>( sizeof( SharpenPush ) ) } );
        return layout;
    }

    struct SpatialUpscale::PipelineHolder : LazyComputePipeline
    {
    };

    struct Sharpen::PipelineHolder : LazyComputePipeline
    {
    };

    SpatialUpscale::SpatialUpscale() : m_Pipeline( std::make_unique<PipelineHolder>() )
    {
        m_Pipeline->ShaderName = kSpatialUpscaleShaderName;
    }

    SpatialUpscale::~SpatialUpscale() = default;

    Common::ResultStr<RDG::TextureRef> SpatialUpscale::AddPasses( RDG::Builder& graph, const ViewFrame& frame,
                                                                  RDG::TextureRef sceneColor ) const
    {
        const ResolutionSplit& split = frame.Split;
        if ( !sceneColor.IsValid() )
            return Common::MakeFormattedError<RDG::TextureRef>(
                 "{}", "SpatialUpscale: the input SceneColor is not a texture of this graph" );
        if ( !IsSpatialUpscale( frame ) || split.Render.Width == 0 || split.Render.Height == 0 ||
             split.Output.Width == 0 || split.Output.Height == 0 )
            return Common::MakeFormattedError<RDG::TextureRef>(
                 "{}",
                 std::format( "SpatialUpscale: the split {}x{} -> {}x{} ({} %) under temporal method {} is "
                              "not a spatial upscale",
                              split.Render.Width, split.Render.Height, split.Output.Width, split.Output.Height,
                              split.RenderScalePercent, static_cast<int>( frame.Method ) ) );

        RDG::TextureDesc desc;
        desc.Size                       = RDG::Extent3D{ split.Output.Width, split.Output.Height, 1 };
        desc.Format                     = Core::Formats::ImageFormat::RGBA16F;
        const RDG::TextureRef    output = graph.CreateTexture( desc, "SpatialUpscale.Output" );
        const SpatialUpscalePush push   = { .SourceWidth       = static_cast<int32_t>( split.Render.Width ),
                                            .SourceHeight      = static_cast<int32_t>( split.Render.Height ),
                                            .DestinationWidth  = static_cast<int32_t>( split.Output.Width ),
                                            .DestinationHeight = static_cast<int32_t>( split.Output.Height ) };
        PipelineHolder*          holder = m_Pipeline.get();
        graph.AddPass(
             "SpatialUpscale", RDG::PassFlags::Compute,
             [&]( RDG::PassBuilder& pass )
             {
                 // texelFetch only: the EASU lobes are the filter, the sampler filters nothing.
                 pass.Bindings( SpatialUpscaleLayout(), RDG::OtherRouteFill{} )
                      .Sampled( "u_Source", sceneColor, RDG::Access::SampledCompute, RDG::SubresourceRange::All(),
                                RDG::SamplerDesc::PointClamp() )
                      .Storage( "u_Destination", output, RDG::Access::StorageWrite, 0 )
                      .PushConstantBytes( static_cast<uint32_t>( sizeof( SpatialUpscalePush ) ) );
             },
             [holder, push]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 std::string                            error;
                 const std::shared_ptr<ComputePipeline> pipeline = holder->Get( error );
                 if ( !pipeline )
                     return Common::MakeError( "SpatialUpscale: " + error );
                 RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                 bindings.PushConstants( &push, sizeof( push ) );
                 return Renderer::GetInstance().DispatchCompute(
                      bindings, *pipeline, Groups( static_cast<uint32_t>( push.DestinationWidth ) ),
                      Groups( static_cast<uint32_t>( push.DestinationHeight ) ), 1 );
             } );
        return Common::MakeSuccess( output );
    }

    Sharpen::Sharpen() : m_Pipeline( std::make_unique<PipelineHolder>() )
    {
        m_Pipeline->ShaderName = kSharpenShaderName;
    }

    Sharpen::~Sharpen() = default;

    Common::ResultStr<RDG::TextureRef> Sharpen::AddPasses( RDG::Builder& graph, const ViewFrame& frame,
                                                           RDG::TextureRef color,
                                                           const int       sharpnessPercent ) const
    {
        if ( !color.IsValid() )
            return Common::MakeFormattedError<RDG::TextureRef>(
                 "{}", "Sharpen: the input colour is not a texture of this graph" );
        if ( !SharpenRuns( frame, sharpnessPercent ) || sharpnessPercent > 100 )
            return Common::MakeFormattedError<RDG::TextureRef>(
                 "{}", std::format( "Sharpen: sharpness {} on a frame with temporal method {} at {} % has nothing "
                                    "to sharpen (0 adds no node; 1-100 follow a resolve)",
                                    sharpnessPercent, static_cast<int>( frame.Method ),
                                    frame.Split.RenderScalePercent ) );
        const auto inputDesc = graph.GetTextureDesc( color );
        if ( !inputDesc )
            return Common::MakeFormattedError<RDG::TextureRef>( "Sharpen: {}", inputDesc.GetError() );

        RDG::TextureDesc desc;
        desc.Size                    = inputDesc.GetValue().Size;
        desc.Format                  = Core::Formats::ImageFormat::RGBA16F;
        const RDG::TextureRef output = graph.CreateTexture( desc, "Sharpen.Output" );
        const SharpenPush     push   = { .Width  = static_cast<int32_t>( desc.Size.Width ),
                                         .Height = static_cast<int32_t>( desc.Size.Height ),
                                         .Amount = static_cast<float>( sharpnessPercent ) / 100.0f };
        PipelineHolder*       holder = m_Pipeline.get();
        graph.AddPass(
             "Sharpen", RDG::PassFlags::Compute,
             [&]( RDG::PassBuilder& pass )
             {
                 pass.Bindings( SharpenLayout(), RDG::OtherRouteFill{} )
                      .Sampled( "u_Source", color, RDG::Access::SampledCompute, RDG::SubresourceRange::All(),
                                RDG::SamplerDesc::PointClamp() )
                      .Storage( "u_Destination", output, RDG::Access::StorageWrite, 0 )
                      .PushConstantBytes( static_cast<uint32_t>( sizeof( SharpenPush ) ) );
             },
             [holder, push]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 std::string                            error;
                 const std::shared_ptr<ComputePipeline> pipeline = holder->Get( error );
                 if ( !pipeline )
                     return Common::MakeError( "Sharpen: " + error );
                 RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                 bindings.PushConstants( &push, sizeof( push ) );
                 return Renderer::GetInstance().DispatchCompute(
                      bindings, *pipeline, Groups( static_cast<uint32_t>( push.Width ) ),
                      Groups( static_cast<uint32_t>( push.Height ) ), 1 );
             } );
        return Common::MakeSuccess( output );
    }
} // namespace Desert::Graphic
