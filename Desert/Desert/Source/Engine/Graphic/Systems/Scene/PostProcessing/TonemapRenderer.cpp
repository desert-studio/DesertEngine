#include "TonemapRenderer.hpp"
#include <Engine/Graphic/ViewTargetFormats.hpp>

#include <Engine/Runtime/ResourceRegistry.hpp>

namespace Desert::Graphic::System
{
    Common::BoolResultStr TonemapRenderer::Initialize()
    {
        const auto& targetFramebuffer = m_TargetFramebuffer.lock();
        if ( !targetFramebuffer )
        {
            DESERT_VERIFY( false );
        }

        constexpr std::string_view debugName = "SceneToneMap";

        // Framebuffer
        FramebufferSpecification fbSpec;
        fbSpec.DebugName = debugName;
        fbSpec.Attachments.Attachments.emplace_back( ViewTargetFormats::kTonemap );

        m_Framebuffer = Graphic::Framebuffer::Create( fbSpec );
        m_Framebuffer->Resize( targetFramebuffer->GetFramebufferWidth(),
                               targetFramebuffer->GetFramebufferHeight() );

        // RenderPass
        RenderPassSpecification rpSpec;
        rpSpec.DebugName         = debugName;
        rpSpec.TargetFramebuffer = m_Framebuffer;

        // Pipeline
        m_Shader = Runtime::ResourceRegistry::GetShaderService()->GetByName( "SceneComposite" );

        Graphic::GraphicsPipelineSpecification pipeSpec;
        pipeSpec.DebugName   = debugName;
        pipeSpec.Framebuffer = m_Framebuffer;
        pipeSpec.Shader      = m_Shader;

        // m_Shader is whatever GetByName returned, INCLUDING nullptr — this site never checked, and a
        // null shader used to be dereferenced inside the backend. Create's rule names it now.
        const auto pipeline = Graphic::GraphicsPipeline::Create( pipeSpec );
        if ( !pipeline )
            return Common::MakeError( pipeline.GetError() );
        m_Pipeline = pipeline.GetValue();

        m_MaterialTonemap = std::make_unique<MaterialTonemap>();

        return BOOLSUCCESS;
    }

    void TonemapRenderer::Execute()
    {
        auto& renderer = Renderer::GetInstance();

        auto renderPass = RenderPass::Create( {
             .TargetFramebuffer = m_Framebuffer,
             .DebugName         = "TonemapPass",
        } );

        renderer.BeginRenderPass( renderPass.get() );
        Render();
        renderer.EndRenderPass();
    }

    void TonemapRenderer::Resize( uint32_t width, uint32_t height )
    {
        if ( m_Framebuffer )
            m_Framebuffer->Resize( width, height );
    }

    void TonemapRenderer::Render()
    {
        const auto& framebuffer =
             m_TargetFramebuffer.lock(); // We call lock internally to avoid cyclic dependencies.
        if ( !framebuffer )
        {
            LOG_ERROR( "The framebuffer for `TonemapRenderer::ProcessSystem` was destroyed or wasn't set up" );
            return;
        }

        // The bloom image / auto-exposure framebuffer always exist once created; when disabled their values
        // (intensity 0 / auto flag off) make the contents ignored, but the descriptors stay validly bound.
        std::shared_ptr<Image2D> bloomImage = m_BloomImage.lock();

        std::shared_ptr<Image2D> avgLuminance = m_AutoExposureImage.lock();

        std::shared_ptr<Image2D> lightShafts = m_LightShaftImage.lock();

        std::shared_ptr<Image2D> lensFlare = m_LensFlareImage.lock();

        MaterialTonemap::Params params{ m_TonemapOperator, m_Exposure,           m_Gamma,
                                        m_BloomIntensity,  m_ExposureKey,        m_AutoExposureEnabled,
                                        m_ChromaticBloom,  m_WhitePoint,         m_LightShaftIntensity,
                                        m_LightShaftTint,  m_LensFlareIntensity, m_LensFlareTint };

        auto& renderer = Renderer::GetInstance();
        m_MaterialTonemap->BindInputs( framebuffer->GetColorAttachmentImage(), bloomImage, avgLuminance,
                                       lightShafts, lensFlare, params );
        renderer.SubmitFullscreenQuad( m_Pipeline.get(), m_MaterialTonemap->GetMaterialExecutor() );
    }
} // namespace Desert::Graphic::System