#include <Engine/Graphic/View/UpscalerExposure.hpp>

#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/RDG/RDGBindingDecl.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/Shader/ShaderService.hpp>

#include <format>
#include <mutex>

namespace Desert::Graphic
{
    namespace
    {
        constexpr const char* kExposureShaderName = "UpscalerExposure";
        // The slot names this pass binds, spelled once (RDGShaderParameters.hpp).
        struct UpscalerExposureShaderParameters
        {
            DESERT_SHADER_PARAMETER( Texture, AdaptedLuminanceTexture );
            DESERT_SHADER_PARAMETER( Sampler, PointClampSampler );
            DESERT_SHADER_PARAMETER( StorageTexture, ExposureTexture );
        };

    } // namespace

    const std::shared_ptr<const RDG::ShaderBindingLayout>& UpscalerExposureLayout()
    {
        using Kind = RDG::ShaderResourceKind;
        static const std::shared_ptr<const RDG::ShaderBindingLayout> layout =
             std::make_shared<const RDG::ShaderBindingLayout>(
                  RDG::ShaderBindingLayout{ .ShaderName = kExposureShaderName,
                                            .Slots      = { { "AdaptedLuminanceTexture", Kind::Texture },
                                                            { "PointClampSampler", Kind::Sampler },
                                                            { "ExposureTexture", Kind::StorageTexture } } } );
        return layout;
    }

    struct UpscalerExposure::PipelineHolder
    {
        std::mutex                       Mutex;
        std::shared_ptr<ComputePipeline> Pipeline;

        std::shared_ptr<ComputePipeline> Get( std::string& error )
        {
            const std::lock_guard lock( Mutex );
            if ( Pipeline )
                return Pipeline;
            Runtime::ShaderService*       service = Runtime::ResourceRegistry::GetShaderService();
            const std::shared_ptr<Shader> shader  = service ? service->GetByName( kExposureShaderName ) : nullptr;
            if ( !shader )
            {
                error = std::format( "UpscalerExposure: the shader '{}' is not registered — expected "
                                     "Editor/Resources/Shaders/Programs/TemporalAA/UpscalerExposure.shader",
                                     kExposureShaderName );
                return nullptr;
            }
            const auto built = ComputePipeline::Create( { .Shader = shader, .DebugName = kExposureShaderName } );
            if ( !built )
            {
                error = std::format( "UpscalerExposure: {}", built.GetError() );
                return nullptr;
            }
            Pipeline = built.GetValue();
            return Pipeline;
        }
    };

    UpscalerExposure::UpscalerExposure() : m_Pipeline( std::make_unique<PipelineHolder>() )
    {
    }

    UpscalerExposure::~UpscalerExposure() = default;

    Common::ResultStr<RDG::TextureRef> UpscalerExposure::AddPass( RDG::Builder&         graph,
                                                                  const RDG::TextureRef adaptedLuminance ) const
    {
        if ( !adaptedLuminance.IsValid() )
            return Common::MakeFormattedError<RDG::TextureRef>(
                 "{}", "UpscalerExposure: the adapted luminance is not a texture of this graph" );

        RDG::TextureDesc desc;
        desc.Size                      = { 1, 1, 1 };
        desc.Format                    = Core::Formats::ImageFormat::R32F;
        const RDG::TextureRef exposure = graph.CreateTexture( desc, kExposureShaderName );
        PipelineHolder*       holder   = m_Pipeline.get();
        graph.AddPass(
             kExposureShaderName, RDG::PassFlags::Compute,
             [&]( RDG::PassBuilder& pass )
             {
                 pass.Bindings( UpscalerExposureLayout(), RDG::OtherRouteFill{} )
                      .Texture( UpscalerExposureShaderParameters::AdaptedLuminanceTexture, adaptedLuminance,
                                RDG::Access::SampledCompute, RDG::SubresourceRange::All() )
                      .Sampler( UpscalerExposureShaderParameters::PointClampSampler,
                                RDG::SamplerDesc::PointClamp() )
                      .Storage( UpscalerExposureShaderParameters::ExposureTexture, exposure,
                                RDG::Access::StorageWrite, 0 );
             },
             [holder]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 std::string                            error;
                 const std::shared_ptr<ComputePipeline> pipeline = holder->Get( error );
                 if ( !pipeline )
                     return Common::MakeError( error );
                 RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                 return Renderer::GetInstance().DispatchCompute( bindings, *pipeline, 1, 1, 1 );
             } );
        return Common::MakeSuccess( exposure );
    }
} // namespace Desert::Graphic
