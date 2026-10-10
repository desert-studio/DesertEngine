#include "EditorToolPreviewPass.hpp"
#include <Engine/Graphic/ViewTargetLayouts.hpp>

#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/SceneRenderer.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <ImGui/imgui.h>

#include <unordered_map>

namespace Desert::Editor::Render
{
    namespace
    {
        constexpr const char* kPassName = "EditorToolPreview";

        struct Shown
        {
            ToolPreviewMesh Mesh;
            int             Frame = 0; // the editor (ImGui) frame that showed it
        };

        // Keyed by the scene the preview stands in; the entry goes with the scene's pass (~EditorToolPreviewPass).
        std::unordered_map<const ::Desert::Core::Scene*, Shown>& Previews()
        {
            static std::unordered_map<const ::Desert::Core::Scene*, Shown> previews;
            return previews;
        }
    } // namespace

    void ToolPreview::Show( const ::Desert::Core::Scene& scene, ToolPreviewMesh mesh )
    {
        Previews()[&scene] = { std::move( mesh ), ::ImGui::GetFrameCount() };
    }

    void ToolPreview::Hide( const ::Desert::Core::Scene& scene )
    {
        Previews().erase( &scene );
    }

    const ToolPreviewMesh* ToolPreview::Current( const ::Desert::Core::Scene& scene )
    {
        const auto found = Previews().find( &scene );
        if ( found == Previews().end() )
            return nullptr;
        // The scene renders before or after the editor's panels in a frame, so the preview the tool showed
        // in the previous editor frame is this frame's; one older than that has no tool updating it.
        if ( found->second.Frame < ::ImGui::GetFrameCount() - 1 )
            return nullptr;
        return &found->second.Mesh;
    }

    EditorToolPreviewPass::~EditorToolPreviewPass()
    {
        if ( const auto scene = m_Scene.lock() )
        {
            scene->UnregisterExternalPass( kPassName );
            ToolPreview::Hide( *scene );
        }
    }

    Common::BoolResultStr EditorToolPreviewPass::Install( const std::shared_ptr<::Desert::Core::Scene>& scene )
    {
        m_Scene = scene;

        const auto shader = Runtime::ResourceRegistry::GetShaderService()->GetByName( "DebugLine" );
        if ( !shader )
            return Common::MakeError( "EditorToolPreviewPass: missing shader 'DebugLine'" );

        // Both draws test the scene's depth and write none: the preview is hidden where it stands behind
        // geometry, and leaves nothing behind it for later passes to read.
        Graphic::GraphicsPipelineSpecification faces;
        faces.DebugName         = "EditorToolPreviewFaces";
        faces.Shader            = shader;
        faces.TargetLayout      = Desert::Graphic::SceneTargetLayout();
        faces.Topology          = Graphic::PrimitiveTopology::Triangles;
        faces.DepthTestEnabled  = true;
        faces.DepthWriteEnabled = false;
        faces.DepthCompareOp    = Graphic::DepthCompare::CloserOrEqual;
        // The tool keeps a closed shape's near side and both sides of an open one; nothing to cull here.
        faces.CullMode    = Graphic::CullMode::None;
        faces.BlendEnable = true; // UE's preview material is translucent
        // A flat shape (Disc, Rectangle) placed on a surface lies IN it: without a pull toward the viewer the
        // coplanar surface wins every other pixel and the preview shows as broken dashes.
        faces.DepthBiasConstant = 4.0f;
        faces.DepthBiasSlope    = 1.5f;

        Graphic::GraphicsPipelineSpecification edges = faces;
        edges.DebugName                              = "EditorToolPreviewEdges";
        edges.Topology                               = Graphic::PrimitiveTopology::Lines;
        edges.LineWidth                              = 1.0f;

        const auto facePipeline = Graphic::GraphicsPipeline::Create( faces );
        if ( !facePipeline )
            return Common::MakeError( "EditorToolPreviewPass: " + facePipeline.GetError() );
        const auto edgePipeline = Graphic::GraphicsPipeline::Create( edges );
        if ( !edgePipeline )
            return Common::MakeError( "EditorToolPreviewPass: " + edgePipeline.GetError() );
        m_FacePipeline = facePipeline.GetValue();
        m_EdgePipeline = edgePipeline.GetValue();
        m_FaceMaterial = std::make_unique<Graphic::MaterialDebugLine>();
        m_EdgeMaterial = std::make_unique<Graphic::MaterialDebugLine>();

        Graphic::ExternalPassSpecification pass;
        pass.Name                  = kPassName;
        pass.Phase                 = Graphic::RenderPhase::Debug;
        pass.Dependencies          = { Graphic::RenderPassDependency( Graphic::RenderPhase::Geometry ) };
        pass.PipelineSpecification = m_FacePipeline->GetSpecification();
        pass.Execute               = [this]( const Graphic::ExternalPassContext& ctx,
                               Graphic::RDG::PassContext& ) -> Common::BoolResultStr
        {
            const auto scene = m_Scene.lock();
            if ( !scene || ctx.ScenePlaying || !ctx.Camera || !ctx.Renderer )
                return BOOLSUCCESS;
            const Graphic::ViewFrame* view = ctx.Renderer->GetViewFrame();
            if ( view == nullptr )
                return BOOLSUCCESS;
            const ToolPreviewMesh* mesh = ToolPreview::Current( *scene );
            if ( mesh == nullptr )
                return BOOLSUCCESS;

            if ( !mesh->Triangles.empty() )
            {
                m_FaceMaterial->Update( *view, mesh->Triangles );
                Graphic::Renderer::SubmitPulled( m_FacePipeline.get(),
                                                 static_cast<uint32_t>( mesh->Triangles.size() ), 1.0f,
                                                 m_FaceMaterial->GetMaterialExecutor() );
            }
            if ( !mesh->Edges.empty() )
            {
                m_EdgeMaterial->Update( *view, mesh->Edges );
                Graphic::Renderer::SubmitPulled( m_EdgePipeline.get(), static_cast<uint32_t>( mesh->Edges.size() ),
                                                 1.0f, m_EdgeMaterial->GetMaterialExecutor() );
            }
            return BOOLSUCCESS;
        };

        scene->RegisterExternalPass( std::move( pass ) );
        return BOOLSUCCESS;
    }
} // namespace Desert::Editor::Render
