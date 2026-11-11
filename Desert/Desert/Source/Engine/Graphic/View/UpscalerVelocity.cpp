#include <Engine/Graphic/View/UpscalerVelocity.hpp>

#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/RDG/RDGBindingDecl.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/View/ViewFrame.hpp>
#include <Engine/Graphic/ViewTargetFormats.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/Shader/ShaderService.hpp>

#include <format>
#include <mutex>

namespace Desert::Graphic
{
    namespace
    {
        constexpr const char* kVelocityShaderName = "UpscalerVelocity";
        constexpr uint32_t    kVelocityGroupSize  = 8; // UpscalerVelocity.shader LocalSize
        // The slot names this pass binds, spelled once (RDGShaderParameters.hpp).
        struct UpscalerVelocityShaderParameters
        {
            DESERT_SHADER_PARAMETER( Texture, VelocityTexture );
            DESERT_SHADER_PARAMETER( Texture, SceneDepthTexture );
            DESERT_SHADER_PARAMETER( Sampler, PointClampSampler );
            DESERT_SHADER_PARAMETER( StorageTexture, UpscalerVelocityTexture );
        };
        // UpscalerVelocity.shader's push block.
        struct VelocityPush
        {
            glm::mat4 ClipToPrevClip; // PrevViewProjection * InvViewProjection, both unjittered
            glm::vec4 Params;         // xy render extent
        };
        static_assert( sizeof( VelocityPush ) == 80, "UpscalerVelocity.shader PushConstants: mat4 + vec4" );

    } // namespace

    const std::shared_ptr<const RDG::ShaderBindingLayout>& UpscalerVelocityLayout()
    {
        using Kind = RDG::ShaderResourceKind;
        static const std::shared_ptr<const RDG::ShaderBindingLayout> layout =
             RDG::MakeIndexedLayout( RDG::ShaderBindingLayout{
                  .ShaderName        = kVelocityShaderName,
                  .Slots             = { { "VelocityTexture", Kind::Texture },
                                         { "SceneDepthTexture", Kind::Texture },
                                         { "PointClampSampler", Kind::Sampler },
                                         { "UpscalerVelocityTexture", Kind::StorageTexture } },
                  .PushConstantBytes = static_cast<uint32_t>( sizeof( VelocityPush ) ) } );
        return layout;
    }

    struct UpscalerVelocity::PipelineHolder
    {
        std::mutex                       Mutex;
        std::shared_ptr<ComputePipeline> Pipeline;

        std::shared_ptr<ComputePipeline> Get( std::string& error )
        {
            const std::lock_guard lock( Mutex );
            if ( Pipeline )
                return Pipeline;
            Runtime::ShaderService*       service = Runtime::ResourceRegistry::GetShaderService();
            const std::shared_ptr<Shader> shader  = service ? service->GetByName( kVelocityShaderName ) : nullptr;
            if ( !shader )
            {
                error = std::format( "UpscalerVelocity: the shader '{}' is not registered - expected "
                                     "Editor/Resources/Shaders/Programs/TemporalAA/UpscalerVelocity.shader",
                                     kVelocityShaderName );
                return nullptr;
            }
            const auto built = ComputePipeline::Create( { .Shader = shader, .DebugName = kVelocityShaderName } );
            if ( !built )
            {
                error = std::format( "UpscalerVelocity: {}", built.GetError() );
                return nullptr;
            }
            Pipeline = built.GetValue();
            return Pipeline;
        }
    };

    UpscalerVelocity::UpscalerVelocity() : m_Pipeline( std::make_unique<PipelineHolder>() )
    {
    }

    UpscalerVelocity::~UpscalerVelocity() = default;

    Common::ResultStr<RDG::TextureRef> UpscalerVelocity::AddPass( RDG::Builder& graph, const ViewFrame& frame,
                                                                  const RDG::TextureRef velocity,
                                                                  const RDG::TextureRef sceneDepth ) const
    {
        if ( !velocity.IsValid() )
            return Common::MakeFormattedError<RDG::TextureRef>(
                 "{}", "UpscalerVelocity: the velocity is not a texture of this graph" );
        if ( !sceneDepth.IsValid() )
            return Common::MakeFormattedError<RDG::TextureRef>(
                 "{}", "UpscalerVelocity: the scene depth is not a texture of this graph" );

        const uint32_t   width  = frame.Split.Render.Width;
        const uint32_t   height = frame.Split.Render.Height;
        RDG::TextureDesc desc;
        desc.Size                      = { width, height, 1 };
        desc.Format                    = ViewTargetFormats::kVelocity;
        const RDG::TextureRef combined = graph.CreateTexture( desc, kVelocityShaderName );
        const VelocityPush    push{
                .ClipToPrevClip = frame.ClipToPrevClip(),
                .Params = glm::vec4( static_cast<float>( width ), static_cast<float>( height ), 0.0f, 0.0f ) };
        PipelineHolder* holder = m_Pipeline.get();
        graph.AddPass(
             kVelocityShaderName, RDG::PassFlags::Compute,
             [&]( RDG::PassBuilder& pass )
             {
                 pass.Bindings( UpscalerVelocityLayout(), RDG::NoOtherRouteFill() )
                      .Texture( UpscalerVelocityShaderParameters::VelocityTexture, velocity,
                                RDG::Access::SampledCompute, RDG::SubresourceRange::All() )
                      .Texture( UpscalerVelocityShaderParameters::SceneDepthTexture, sceneDepth,
                                RDG::Access::SampledCompute, RDG::SubresourceRange::All() )
                      .Sampler( UpscalerVelocityShaderParameters::PointClampSampler,
                                RDG::SamplerDesc::PointClamp() )
                      .Storage( UpscalerVelocityShaderParameters::UpscalerVelocityTexture, combined,
                                RDG::Access::StorageWrite, 0 )
                      .PushConstantBytes( static_cast<uint32_t>( sizeof( VelocityPush ) ) );
             },
             [holder, push, width, height]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 std::string                            error;
                 const std::shared_ptr<ComputePipeline> pipeline = holder->Get( error );
                 if ( !pipeline )
                     return Common::MakeError( error );
                 RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                 bindings.PushConstants( &push, static_cast<uint32_t>( sizeof( push ) ) );
                 return Renderer::GetInstance().DispatchCompute(
                      bindings, *pipeline, ( width + kVelocityGroupSize - 1 ) / kVelocityGroupSize,
                      ( height + kVelocityGroupSize - 1 ) / kVelocityGroupSize, 1 );
             } );
        return Common::MakeSuccess( combined );
    }
} // namespace Desert::Graphic
