#include "FXAARenderer.hpp"
#include <Engine/Graphic/ViewTargetFormats.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>

#include <Engine/Runtime/ResourceRegistry.hpp>

namespace Desert::Graphic::System
{
    Common::BoolResultStr FXAARenderer::Initialize()
    {
        constexpr std::string_view debugName = "SceneFXAA";

        const auto shader = Runtime::ResourceRegistry::GetShaderService()->GetByName( "FXAA" );

        Graphic::GraphicsPipelineSpecification pipeSpec;
        pipeSpec.DebugName = debugName;
        pipeSpec.TargetLayout =
             RenderTargetLayout{ .ColorFormats = { ViewTargetFormats::kFXAA }, .DepthFormat = std::nullopt };
        pipeSpec.Shader = shader;

        // Same unchecked GetByName as TonemapRenderer: a missing 'FXAA' shader was a null dereference.
        const auto pipeline = Graphic::GraphicsPipeline::Create( pipeSpec );
        if ( !pipeline )
            return Common::MakeError( pipeline.GetError() );
        m_Pipeline = pipeline.GetValue();

        m_MaterialFXAA = std::make_unique<MaterialFXAA>();

        return BOOLSUCCESS;
    }

    void FXAARenderer::DeclareBindings( RDG::PassBuilder& pass, RDG::TextureRef input ) const
    {
        if ( !m_Pipeline || !m_MaterialFXAA )
            return;
        pass.Bindings( m_BindingLayout.Get( m_Pipeline->GetSpecification().Shader ),
                       m_MaterialFXAA->GetMaterialExecutor()->GetRouteFill() )
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
