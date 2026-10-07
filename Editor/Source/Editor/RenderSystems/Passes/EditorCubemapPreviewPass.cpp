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
            scene->UnregisterExternalPass( "CubemapPreview" );
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

        Graphic::ExternalPassSpecification pass;
        pass.Name                  = "CubemapPreview";
        pass.Phase                 = Graphic::RenderPhase::Debug;
        pass.Dependencies          = { Graphic::RenderPassDependency( Graphic::RenderPhase::Geometry ) };
        pass.PipelineSpecification = m_Pipeline->GetSpecification();
        pass.Execute               = [this]( const Graphic::ExternalPassContext& ctx,
                               Graphic::RDG::PassContext&          context ) -> Common::BoolResultStr
        {
            if ( !ctx.Camera || !m_ResolveCube )
                return BOOLSUCCESS;

            // Resolved EVERY frame on purpose — the pass owns no copy of the material's state, so a
            // cubemap dropped onto (or cleared from) the subject shows next frame with no
            // invalidation protocol. The closure is two map lookups; see the header.
            const Graphic::SampledCube source = m_ResolveCube();
            if ( source.Cube == nullptr )
                return BOOLSUCCESS;

            m_Material->Update( ctx.Camera, source.Cube, source.Look, m_Radius, m_Backdrop, source.Lod,
                                m_LongLat );
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
        pass.Declare = [this]( Graphic::RenderPassDeclaration& declared, const Graphic::ExternalPassContext& )
        {
            declared.Bindings( m_BindingLayout.Get( m_Pipeline->GetSpecification().Shader ),
                               m_Material->GetMaterialExecutor()->GetRouteFill() );
        };

        scene->RegisterExternalPass( std::move( pass ) );
        return BOOLSUCCESS;
    }
} // namespace Desert::Editor::Render
