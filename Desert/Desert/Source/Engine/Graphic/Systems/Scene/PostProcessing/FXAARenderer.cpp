#include "FXAARenderer.hpp"
#include <Engine/Graphic/ViewTargetFormats.hpp>

#include <Engine/Runtime/ResourceRegistry.hpp>

namespace Desert::Graphic::System
{
    Common::BoolResultStr FXAARenderer::Initialize()
    {
        const auto& targetFramebuffer = m_TargetFramebuffer.lock();
        if ( !targetFramebuffer )
        {
            DESERT_VERIFY( false );
        }

        constexpr std::string_view debugName = "SceneFXAA";

        FramebufferSpecification fbSpec;
        fbSpec.DebugName = debugName;
        fbSpec.Attachments.Attachments.emplace_back( ViewTargetFormats::kFXAA );

        m_Framebuffer = Graphic::Framebuffer::Create( fbSpec );
        m_Framebuffer->Resize( targetFramebuffer->GetFramebufferWidth(),
                               targetFramebuffer->GetFramebufferHeight() );

        m_Shader = Runtime::ResourceRegistry::GetShaderService()->GetByName( "FXAA" );

        Graphic::GraphicsPipelineSpecification pipeSpec;
        pipeSpec.DebugName   = debugName;
        pipeSpec.Framebuffer = m_Framebuffer;
        pipeSpec.Shader      = m_Shader;

        // Same unchecked GetByName as TonemapRenderer: a missing 'FXAA' shader was a null dereference.
        const auto pipeline = Graphic::GraphicsPipeline::Create( pipeSpec );
        if ( !pipeline )
            return Common::MakeError( pipeline.GetError() );
        m_Pipeline = pipeline.GetValue();

        m_MaterialFXAA = std::make_unique<MaterialFXAA>();

        return BOOLSUCCESS;
    }

    void FXAARenderer::Execute()
    {
        auto& renderer = Renderer::GetInstance();

        auto renderPass = RenderPass::Create( {
             .TargetFramebuffer = m_Framebuffer,
             .DebugName         = "FXAAPass",
        } );

        renderer.BeginRenderPass( renderPass.get() );
        Render();
        renderer.EndRenderPass();
    }

    void FXAARenderer::Resize( uint32_t width, uint32_t height )
    {
        if ( m_Framebuffer )
            m_Framebuffer->Resize( width, height );
    }

    void FXAARenderer::Render()
    {
        const auto& inputFramebuffer = m_TargetFramebuffer.lock();
        if ( !inputFramebuffer )
        {
            LOG_ERROR( "FXAARenderer::Render: input framebuffer was destroyed or not set up" );
            return;
        }

        auto& renderer = Renderer::GetInstance();
        m_MaterialFXAA->BindInputs( inputFramebuffer->GetColorAttachmentImage() );
        renderer.SubmitFullscreenQuad( m_Pipeline.get(), m_MaterialFXAA->GetMaterialExecutor() );
    }
} // namespace Desert::Graphic::System
