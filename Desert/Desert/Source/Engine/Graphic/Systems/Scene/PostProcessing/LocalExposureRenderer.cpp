#include "LocalExposureRenderer.hpp"

#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <glm/glm.hpp>

namespace Desert::Graphic::System
{
    namespace
    {
        constexpr uint32_t kBlurGroupSize = 8; // LEBlur LocalSize(8, 8, 1)

        struct SetupPush
        {
            glm::vec2 InvOutputSize;
            glm::vec2 OutputSize;
            float     InversePreExposure;
            float     SliceLogMin;
            float     InvSliceEV;
            float     Pad0;
        };

        struct BlurPush
        {
            glm::ivec2 Direction;
            float      Radius;
            float      Pad0;
        };
    } // namespace

    Common::BoolResultStr LocalExposureRenderer::Initialize()
    {
        const auto shaderService = Runtime::ResourceRegistry::GetShaderService();
        const auto make          = [&]( const std::string& name ) -> std::shared_ptr<ComputePipeline>
        {
            const auto shader = shaderService->GetByName( name );
            if ( !shader )
            {
                LOG_ERROR( "LocalExposureRenderer: missing compute shader '{}'", name );
                return nullptr;
            }
            const auto built = ComputePipeline::Create( { .Shader = shader, .DebugName = name } );
            if ( !built )
            {
                LOG_ERROR( "LocalExposureRenderer: {}", built.GetError() );
                return nullptr;
            }
            return built.GetValue();
        };
        m_SetupPipeline = make( "LESetup" );
        m_BlurPipeline  = make( "LEBlur" );
        if ( !m_SetupPipeline || !m_BlurPipeline )
            return Common::MakeError( "LocalExposureRenderer: the LESetup / LEBlur pipelines were not built" );
        if ( const auto valid = m_SetupLayout.Validate( m_SetupPipeline->GetSpecification().Shader,
                                                        LocalExposureSetupShaderParameters::kAll );
             !valid )
            return valid;
        if ( const auto valid = m_BlurLayout.Validate( m_BlurPipeline->GetSpecification().Shader,
                                                       LocalExposureBlurShaderParameters::kAll );
             !valid )
            return valid;
        return BOOLSUCCESS;
    }

    RDG::TextureDesc LocalExposureRenderer::GetGridDesc( uint32_t width, uint32_t height )
    {
        const glm::uvec3 grid = LocalExposureGridExtent( width, height );
        return RDG::TextureDesc{ .Size   = { grid.x, grid.y, grid.z },
                                 .Format = Core::Formats::ImageFormat::RGBA32F,
                                 .Dim    = RDG::TextureDim::Tex3D };
    }

    RDG::TextureDesc LocalExposureRenderer::GetLogLumDesc( uint32_t width, uint32_t height )
    {
        const glm::uvec2 blur = LocalExposureBlurExtent( width, height );
        return RDG::TextureDesc{ .Size = { blur.x, blur.y, 1u }, .Format = Core::Formats::ImageFormat::R32F };
    }

    void LocalExposureRenderer::DeclareSetupBindings( RDG::PassBuilder& pass, RDG::TextureRef scene,
                                                      RDG::TextureRef grid, RDG::TextureRef logLum ) const
    {
        if ( !m_SetupPipeline )
            return;
        // One bilinear tap at a half-res texel's centre (the 2x2 output pixels' corner) = UE's half-res box.
        pass.Bindings( m_SetupLayout.Get( m_SetupPipeline->GetSpecification().Shader ),
                       Renderer::GetPipelineRouteFill( *m_SetupPipeline ) )
             .Texture( LocalExposureSetupShaderParameters::SceneColorTexture, scene, RDG::Access::SampledCompute,
                       RDG::SubresourceRange::All() )
             .Sampler( LocalExposureSetupShaderParameters::LinearClampSampler, RDG::SamplerDesc::LinearClamp() )
             .Storage( LocalExposureSetupShaderParameters::OutBilateralGrid, grid, RDG::Access::StorageWrite, 0 )
             .Storage( LocalExposureSetupShaderParameters::OutLogLuminance, logLum, RDG::Access::StorageWrite, 0 )
             .PushConstantBytes( static_cast<uint32_t>( sizeof( SetupPush ) ) );
    }

    Common::BoolResultStr LocalExposureRenderer::RecordSetup( const RDG::PassContext& context, uint32_t width,
                                                              uint32_t height )
    {
        if ( !m_SetupPipeline )
            return Common::MakeError( "PostFX: LocalExposureSetup: the setup pipeline is not initialised" );
        const SetupPush   push{ glm::vec2( 1.0f / float( width ), 1.0f / float( height ) ),
                              glm::vec2( float( width ), float( height ) ),
                              1.0f / m_PreExposure,
                              m_SliceLogMin,
                              1.0f / kLocalExposureSliceEV,
                              0.0f };
        RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
        bindings.PushConstants( &push, sizeof( push ) );
        const glm::uvec3 grid = LocalExposureGridExtent( width, height );
        return Renderer::GetInstance().DispatchCompute( bindings, *m_SetupPipeline, grid.x, grid.y, 1 );
    }

    void LocalExposureRenderer::DeclareBlurBindings( RDG::PassBuilder& pass, RDG::TextureRef input,
                                                     RDG::TextureRef output ) const
    {
        if ( !m_BlurPipeline )
            return;
        pass.Bindings( m_BlurLayout.Get( m_BlurPipeline->GetSpecification().Shader ),
                       Renderer::GetPipelineRouteFill( *m_BlurPipeline ) )
             .Texture( LocalExposureBlurShaderParameters::InputTexture, input, RDG::Access::SampledCompute,
                       RDG::SubresourceRange::All() )
             .Sampler( LocalExposureBlurShaderParameters::PointClampSampler, RDG::SamplerDesc::PointClamp() )
             .Storage( LocalExposureBlurShaderParameters::OutputTexture, output, RDG::Access::StorageWrite, 0 )
             .PushConstantBytes( static_cast<uint32_t>( sizeof( BlurPush ) ) );
    }

    Common::BoolResultStr LocalExposureRenderer::RecordBlur( const RDG::PassContext& context, bool horizontal,
                                                             float radius, uint32_t blurWidth,
                                                             uint32_t blurHeight )
    {
        if ( !m_BlurPipeline )
            return Common::MakeError( "PostFX: LocalExposureBlur: the blur pipeline is not initialised" );
        const BlurPush    push{ horizontal ? glm::ivec2( 1, 0 ) : glm::ivec2( 0, 1 ), radius, 0.0f };
        RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
        bindings.PushConstants( &push, sizeof( push ) );
        return Renderer::GetInstance().DispatchCompute( bindings, *m_BlurPipeline,
                                                        LocalExposureDivUp( blurWidth, kBlurGroupSize ),
                                                        LocalExposureDivUp( blurHeight, kBlurGroupSize ), 1 );
    }
} // namespace Desert::Graphic::System
