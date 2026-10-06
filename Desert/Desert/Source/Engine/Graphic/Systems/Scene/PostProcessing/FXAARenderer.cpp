#include "FXAARenderer.hpp"
#include <Engine/Graphic/ViewTargetFormats.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>

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

    void FXAARenderer::Resize( uint32_t width, uint32_t height )
    {
        if ( m_Framebuffer )
            m_Framebuffer->Resize( width, height );
    }

    void FXAARenderer::DeclareBindings( RDG::PassBuilder& pass, RDG::TextureRef input ) const
    {
        if ( !m_Pipeline || !m_MaterialFXAA )
            return;
        pass.Bindings( m_BindingLayout.Get( *m_Shader ), m_MaterialFXAA->GetMaterialExecutor()->GetRouteFill() )
             .Sampled( "u_InputTexture", input, RDG::Access::SampledGraphics, RDG::SubresourceRange::All(),
                       RDG::SamplerDesc::LinearRepeat() );
    }

    Common::BoolResultStr FXAARenderer::Record( const RDG::PassContext& context )
    {
        if ( !m_Pipeline || !m_MaterialFXAA )
            return Common::MakeError( "PostFX: FXAA: the FXAA pipeline is not initialised" );
        const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
        return Renderer::GetInstance().DrawFullscreen( bindings, *m_Pipeline,
                                                       m_MaterialFXAA->GetMaterialExecutor() );
    }
} // namespace Desert::Graphic::System
