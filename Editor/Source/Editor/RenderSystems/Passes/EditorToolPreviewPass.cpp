#include "EditorToolPreviewPass.hpp"
#include <Engine/Graphic/ViewTargetLayouts.hpp>

#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/SceneRenderer.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <ImGui/imgui.h>

#include <algorithm>
#include <cmath>
#include <format>
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

        // An edge lying on a surface the scene also drew (a Disc placed on the floor) ties with that surface only
        // ON the edge's own line. A line fragment is the pixel the rasterizer stepped to, up to half a pixel off
        // the line, and the scene's depth there was written at a jitter of up to another half pixel: where the
        // surface is grazing, its depth one pixel to the side is nearer by far more than any fixed pull, and the
        // edge loses every few pixels - a regular dash pattern. Each end is therefore moved along its view ray
        // (its pixel does not change) by the surface's depth change across that pixel: a polygon's slope-scaled
        // depth bias (UE's line DepthBias), worked out per frame because it depends on the view.
        constexpr float kLiftPixels  = 1.0f;  // half a pixel of line stepping + half a pixel of jitter
        constexpr float kMinGrazing  = 0.05f; // the cosine below which the lift stops growing (~87 degrees)
        constexpr float kFloorPixels = 0.25f; // the lift of a surface square to the view: depth precision only

        Common::BoolResultStr LiftEdges( const Graphic::ViewFrame& view, const ToolPreviewMesh& mesh,
                                         std::vector<Graphic::MaterialDebugLine::LineVertex>& out )
        {
            if ( mesh.EdgeSurfaces.size() * 2 != mesh.Edges.size() )
                return Common::MakeError( std::format( "EditorToolPreviewPass: {} edge vertices but {} edge surfaces "
                                                       "(one surface per two vertices)",
                                                       mesh.Edges.size(), mesh.EdgeSurfaces.size() ) );
            out                = mesh.Edges;
            const float height = static_cast<float>( view.Split.Render.Height );
            const float focal  = view.Projection[1][1];
            if ( height <= 0.0f || focal == 0.0f )
                return Common::MakeError( "EditorToolPreviewPass: the view has no render height or no focal length" );
            // The world size of one render pixel at a point is its clip w times this (perspective: w is the view
            // depth; orthographic: w is 1, and the size is the same everywhere).
            const float     pixelPerW    = 2.0f / ( std::abs( focal ) * height );
            const bool      orthographic = view.Projection[3][3] == 1.0f;
            const glm::vec3 back         = glm::normalize( glm::vec3( view.InvView[2] ) );
            for ( size_t v = 0; v < out.size(); ++v )
            {
                glm::vec3       p      = glm::vec3( out[v].PositionWS );
                const glm::vec3 normal = mesh.EdgeSurfaces[v / 2];
                const glm::vec3 toEye  = orthographic ? back : view.CameraPosition - p;
                const float     dist   = glm::length( toEye );
                if ( dist <= 0.0f )
                    continue;
                const glm::vec3 ray     = toEye / dist;
                const float     w       = ( view.ViewProjection * glm::vec4( p, 1.0f ) ).w;
                const float     pixel   = std::abs( w ) * pixelPerW;
                const float     cosine  = std::min( std::abs( glm::dot( normal, ray ) ), 1.0f );
                const float     tangent = std::sqrt( 1.0f - cosine * cosine ) / std::max( cosine, kMinGrazing );
                const float     lift    = pixel * ( kLiftPixels * tangent + kFloorPixels );
                p += ray * ( orthographic ? lift : std::min( lift, dist * 0.5f ) );
                out[v].PositionWS = glm::vec4( p, out[v].PositionWS.w );
            }
            return BOOLSUCCESS;
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
        // Line primitives are never depth-biased by the rasterizer: LiftEdges gives the edges their slope-scaled
        // bias on the CPU instead, not a pipeline bias that would read as if it worked.
        edges.DepthBiasConstant = 0.0f;
        edges.DepthBiasSlope    = 0.0f;

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
                if ( auto lifted = LiftEdges( *view, *mesh, m_LiftedEdges ); !lifted.IsSuccess() )
                    return lifted;
                m_EdgeMaterial->Update( *view, m_LiftedEdges );
                Graphic::Renderer::SubmitPulled( m_EdgePipeline.get(), static_cast<uint32_t>( mesh->Edges.size() ),
                                                 1.0f, m_EdgeMaterial->GetMaterialExecutor() );
            }
            return BOOLSUCCESS;
        };

        scene->RegisterExternalPass( std::move( pass ) );
        return BOOLSUCCESS;
    }
} // namespace Desert::Editor::Render
