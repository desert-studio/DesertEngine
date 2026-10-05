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

    Common::BoolResultStr FXAARenderer::Record( const RDG::PassContext& context, RDG::TextureRef input )
    {
        // The sampler the material route sampled the input with (the image's own: linear, REPEAT).
        RDG::PassBindings bindings( context );
        bindings.Sampled( "u_InputTexture", input, RDG::Access::SampledGraphics, RDG::SubresourceRange::All(),
                          RDG::SamplerDesc::LinearRepeat() );
        return Renderer::GetInstance().DrawFullscreen( bindings, *m_Pipeline,
                                                       m_MaterialFXAA->GetMaterialExecutor() );
    }
} // namespace Desert::Graphic::System
