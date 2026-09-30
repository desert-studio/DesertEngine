#include "TonemapRenderer.hpp"
#include <Engine/Graphic/ViewTargetFormats.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>

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

    void TonemapRenderer::Resize( uint32_t width, uint32_t height )
    {
        if ( m_Framebuffer )
            m_Framebuffer->Resize( width, height );
    }

    Common::BoolResultStr TonemapRenderer::Record( const RDG::PassContext& context, const GraphInputs& inputs )
    {
        const auto& framebuffer =
             m_TargetFramebuffer.lock(); // We call lock internally to avoid cyclic dependencies.
        if ( !framebuffer )
            return Common::MakeError( "TonemapRenderer: the source framebuffer was destroyed or wasn't set up" );

        // The auto-exposure image always exists once created; when disabled the auto flag makes its contents
        // ignored, but the descriptor stays validly bound.
        std::shared_ptr<Image2D> avgLuminance = m_AutoExposureImage.lock();

        std::shared_ptr<Image2D> lightShafts = m_LightShaftImage.lock();

        std::shared_ptr<Image2D> lensFlare = m_LensFlareImage.lock();

        const float bloomIntensity = inputs.BloomProduced ? m_BloomIntensity : 0.0f;

        MaterialTonemap::Params params{ m_TonemapOperator, m_Exposure,           m_Gamma,
                                        bloomIntensity,    m_ExposureKey,        m_AutoExposureEnabled,
                                        m_ChromaticBloom,  m_WhitePoint,         m_LightShaftIntensity,
                                        m_LightShaftTint,  m_LensFlareIntensity, m_LensFlareTint };

        m_MaterialTonemap->BindInputs( framebuffer->GetColorAttachmentImage(), avgLuminance, lightShafts, lensFlare,
                                       params );

        RDG::PassBindings bindings( context );
        bindings.Sampled( "u_BloomTexture", inputs.Bloom, RDG::Access::SampledGraphics,
                          RDG::SubresourceRange::Mip( 0 ) );
        return Renderer::GetInstance().DrawFullscreen( bindings, *m_Pipeline,
                                                       m_MaterialTonemap->GetMaterialExecutor() );
    }
} // namespace Desert::Graphic::System
