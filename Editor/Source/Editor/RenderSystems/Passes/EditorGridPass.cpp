#include "EditorGridPass.hpp"
#include <Engine/Graphic/ViewTargetLayouts.hpp>

#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/SceneRenderer.hpp> // the view's own debug/show state
#include <Engine/Runtime/ResourceRegistry.hpp>

namespace Desert::Editor::Render
{
    EditorGridPass::~EditorGridPass()
    {
        if ( const auto scene = m_Scene.lock() )
            scene->UnregisterExtensionPass( "EditorGrid" );
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
        spec.TargetLayout      = Desert::Graphic::SceneTargetLayout();
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

        Graphic::ExtensionPass pass;
        pass.Name                  = "EditorGrid";
        pass.Point                 = Graphic::RDG::ExtensionPoint::AfterTranslucency;
        pass.Execute               = [this]( const Graphic::ExtensionPassContext& ctx,
                               Graphic::RDG::PassContext&           context ) -> Common::BoolResultStr
        {
            // The flag is asked of the RENDERER this pass is drawing into, not of the scene and not of a
            // global: it is what THIS view is showing (Graphic/DebugViewState.hpp). A scene rendered into
            // two views could legitimately have the grid in one of them, and a preview renderer that
            // nobody pushes to gets the all-off default without having to opt out.
            const auto scene = m_Scene.lock();
            if ( !scene || ctx.ScenePlaying || !ctx.Camera || !ctx.Renderer )
                return BOOLSUCCESS;
            if ( !ctx.Renderer->GetDebugView().ShowGrid )
                return BOOLSUCCESS;

            m_Material->Update( ctx.Camera );
            // The block declared in setup (pass.Declare below).
            return Graphic::Renderer::GetInstance().DrawFullscreen(
                 Graphic::RDG::PassBindings( context, context.GetBindingBlock( 0 ) ), *m_Pipeline,
                 m_Material->GetMaterialExecutor() );
        };

        // RDG-FAULT1 C3b: the node's one binding block is declared in SETUP (the layout kept per shader, the
        // material's route fill), so a broken block faults this node before anything is recorded. Declared every
        // frame the pass exists: the block names no graph resource, and the exec opens it only when it draws.
        // The material's VALUES stay per view (Update( camera ) in the exec): one pass object serves every view of
        // the scene.
        pass.Declare = [this]( Graphic::RenderPassDeclaration& declared, const Graphic::ExtensionPassContext& )
        {
            declared.Bindings( m_BindingLayout.Get( m_Pipeline->GetSpecification().Shader ),
                               m_Material->GetMaterialExecutor()->GetRouteFill() );
        };

        scene->RegisterExtensionPass( std::move( pass ) );
        return BOOLSUCCESS;
    }
} // namespace Desert::Editor::Render
