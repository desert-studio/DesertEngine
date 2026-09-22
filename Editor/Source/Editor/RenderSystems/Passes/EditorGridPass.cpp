#include "EditorGridPass.hpp"

#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/SceneRenderer.hpp> // the view's own debug/show state
#include <Engine/Runtime/ResourceRegistry.hpp>

namespace Desert::Editor::Render
{
    EditorGridPass::~EditorGridPass()
    {
        if ( const auto scene = m_Scene.lock() )
            scene->UnregisterExternalPass( "EditorGrid" );
    }

    Common::BoolResultStr EditorGridPass::Install( const std::shared_ptr<::Desert::Core::Scene>& scene )
    {
        m_Scene = scene;

        const auto shader = Runtime::ResourceRegistry::GetShaderService()->GetByName( "Grid" );
        if ( !shader )
            return Common::MakeError( "EditorGridPass: missing shader 'Grid'" );

        Graphic::GraphicsPipelineSpecification spec;
        spec.DebugName         = "EditorGridPipeline";
        spec.Shader            = shader;
        spec.Framebuffer       = scene->GetTargetFramebuffer();
        spec.DepthTestEnabled  = true;  // occluded by opaque geometry
        spec.DepthWriteEnabled = false; // overlay; don't write depth
        spec.DepthCompareOp    = Graphic::DepthCompare::Closer;
        spec.CullMode          = Graphic::CullMode::None;
        spec.BlendEnable       = true; // alpha-composite the lines over the scene

        const auto pipeline = Graphic::GraphicsPipeline::Create( spec );
        if ( !pipeline )
            return Common::MakeError( "EditorGridPass: " + pipeline.GetError() );
        m_Pipeline = pipeline.GetValue();

        m_Material = std::make_unique<Graphic::MaterialGrid>();

        Graphic::ExternalPassSpecification pass;
        pass.Name                  = "EditorGrid";
        pass.Phase                 = Graphic::RenderPhase::Transparency;
        pass.Dependencies          = { Graphic::RenderPassDependency( Graphic::RenderPhase::Geometry ) };
        pass.PipelineSpecification = m_Pipeline->GetSpecification();
        pass.Execute               = [this]( const Graphic::ExternalPassContext& ctx )
        {
            // The flag is asked of the RENDERER this pass is drawing into, not of the scene and not of a
            // global: it is what THIS view is showing (Graphic/DebugViewState.hpp). A scene rendered into
            // two views could legitimately have the grid in one of them, and a preview renderer that
            // nobody pushes to gets the all-off default without having to opt out.
            const auto scene = m_Scene.lock();
            if ( !scene || ctx.ScenePlaying || !ctx.Camera )
                return;
            const auto* renderer = scene->GetSceneRenderer();
            if ( !renderer || !renderer->GetDebugView().ShowGrid )
                return;

            m_Material->Update( ctx.Camera );
            Graphic::Renderer::GetInstance().SubmitFullscreenQuad( m_Pipeline.get(),
                                                                   m_Material->GetMaterialExecutor() );
        };

        scene->RegisterExternalPass( std::move( pass ) );
        return BOOLSUCCESS;
    }
} // namespace Desert::Editor::Render
