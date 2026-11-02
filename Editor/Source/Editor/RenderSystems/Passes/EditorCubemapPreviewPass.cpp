#include "EditorCubemapPreviewPass.hpp"
#include <Engine/Graphic/ViewTargetLayouts.hpp>

#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

namespace Desert::Editor::Render
{
    EditorCubemapPreviewPass::~EditorCubemapPreviewPass()
    {
        if ( const auto scene = m_Scene.lock() )
            scene->UnregisterExtensionPass( "CubemapPreview" );
    }

    Common::BoolResultStr EditorCubemapPreviewPass::Install( const std::shared_ptr<::Desert::Core::Scene>& scene )
    {
        m_Scene = scene;

        const auto shader = Runtime::ResourceRegistry::GetShaderService()->GetByName( "CubemapSphere" );
        if ( !shader )
            return Common::MakeError( "EditorCubemapPreviewPass: missing shader 'CubemapSphere'" );

        Graphic::GraphicsPipelineSpecification spec;
        spec.DebugName   = "EditorCubemapPreviewPipeline";
        spec.Shader      = shader;
        spec.TargetLayout = Desert::Graphic::SceneTargetLayout();
        // A real object, not an overlay: the ball writes its own ray-traced depth so the scene's
        // backdrop stays behind it whichever order the graph runs the sky, and tests against what the
        // geometry pass left so a mesh in the same scene would still occlude it correctly.
        spec.DepthTestEnabled  = true;
        spec.DepthWriteEnabled = true;
        spec.DepthCompareOp    = Graphic::DepthCompare::Closer;
        spec.CullMode          = Graphic::CullMode::None;
        spec.BlendEnable       = false; // radiance, opaque; the post chain tonemaps it like any sky

        const auto pipeline = Graphic::GraphicsPipeline::Create( spec );
        if ( !pipeline )
            return Common::MakeError( "EditorCubemapPreviewPass: " + pipeline.GetError() );
        m_Pipeline = pipeline.GetValue();

        m_Material = std::make_unique<Graphic::MaterialCubemapSphere>();

        Graphic::ExtensionPass pass;
        pass.Name                  = "CubemapPreview";
        pass.Point                 = Graphic::RDG::ExtensionPoint::Overlay;
        pass.Execute               = [this]( const Graphic::ExtensionPassContext& ctx,
                               Graphic::RDG::PassContext&           context ) -> Common::BoolResultStr
        {
            const Graphic::SampledCube& source = m_Gathered.Source;
            if ( !ctx.Camera || source.Cube == nullptr )
                return BOOLSUCCESS;

            m_Material->Update( ctx.Camera, source.Cube, source.Look, m_Gathered.Radius, m_Gathered.Backdrop,
                                source.Lod, m_Gathered.LongLat );
            // The block declared in setup (pass.Declare below).
            return Graphic::Renderer::GetInstance().DrawFullscreen(
                 Graphic::RDG::PassBindings( context, context.GetBindingBlock( 0 ) ), *m_Pipeline,
                 m_Material->GetMaterialExecutor() );
        };

        // RDG-FAULT1 C3b: the node's one binding block is declared in SETUP (the layout kept per shader, the
        // material's route fill), so a broken block faults this node before anything is recorded. Declared every
        // frame the pass exists: the block names no graph resource, and the exec opens it only when it draws.
        // The material's VALUES stay per view (Update( camera, cube, ... ) in the exec): one pass object serves
        // every view of the scene.
        // Resolved EVERY frame on purpose - the pass owns no copy of the material's state, so a cubemap dropped
        // onto (or cleared from) the subject shows next frame with no invalidation protocol. The closure is two
        // map lookups; see the header. ON THE GAME THREAD (ExtensionPass::Gather): the panel sets the source
        // and its look while the view records on the render task, and the closure reads the material.
        pass.Gather = [this]( const Graphic::ExtensionPassGatherContext& )
        {
            m_Gathered          = GatheredSource{};
            m_Gathered.Radius   = m_Radius;
            m_Gathered.Backdrop = m_Backdrop;
            m_Gathered.LongLat  = m_LongLat;
            if ( m_ResolveCube )
                m_Gathered.Source = m_ResolveCube();
        };
        pass.Declare = [this]( Graphic::RenderPassDeclaration& declared, const Graphic::ExtensionPassContext& )
        {
            // RENDER-THREADS M1: the material's view sets and block copies exist before the graph records; the
            // exec's Update writes into them and creates nothing.
            declared.Prepares( *m_Material->GetMaterialExecutor() );
            declared.Bindings( m_BindingLayout.Get( m_Pipeline->GetSpecification().Shader ),
                               m_Material->GetMaterialExecutor()->GetRouteFill() );
        };

        scene->RegisterExtensionPass( std::move( pass ) );
        return BOOLSUCCESS;
    }
} // namespace Desert::Editor::Render
