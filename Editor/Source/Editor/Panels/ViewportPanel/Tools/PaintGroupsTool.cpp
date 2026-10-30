#include "PaintGroupsTool.hpp"

#include <Editor/Core/Commands/SceneCommands.hpp>
#include <Editor/Core/Selection/ModelingState.hpp>
#include <Editor/Core/Selection/ModelingToolTarget.hpp>
#include <Editor/Core/Selection/SelectionManager.hpp>
#include <Editor/Core/ToastManager.hpp>
#include <Editor/RenderSystems/Passes/EditorToolPreviewPass.hpp>

#include <Common/Core/Logger.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/EditableMesh.hpp>
#include <Engine/ECS/Entity.hpp>

#include <ImGui/imgui.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <vector>

namespace Desert::Editor::Tools
{
    namespace
    {
        using MS = Core::ModelingState;

        const ECS::Entity* EntityOf( ::Desert::Core::Scene& scene, const Common::UUID& id )
        {
            auto ref = scene.FindEntityByID( id );
            return ref ? &ref->get() : nullptr;
        }

        ECS::StaticMeshComponent* MeshOf( const ECS::Entity* e )
        {
            return e != nullptr && e->HasComponent<ECS::StaticMeshComponent>()
                        ? &e->GetComponent<ECS::StaticMeshComponent>()
                        : nullptr;
        }

        // The brush radius is world centimetres; the mesh is painted in its own space.
        double MeanScale( const glm::dmat4& world )
        {
            const double s = ( glm::length( glm::dvec3( world[0] ) ) + glm::length( glm::dvec3( world[1] ) ) +
                               glm::length( glm::dvec3( world[2] ) ) ) /
                             3.0;
            return s > 1e-9 ? s : 1.0;
        }

        bool ToScreen( const glm::mat4& viewProj, const glm::vec3& p, const glm::vec2& vpPos,
                       const glm::vec2& vpSize, ImVec2& out )
        {
            const glm::vec4 clip = viewProj * glm::vec4( p, 1.0f );
            if ( clip.w <= 0.0001f )
                return false;
            out = ImVec2( vpPos.x + ( clip.x / clip.w * 0.5f + 0.5f ) * vpSize.x,
                          vpPos.y + ( 0.5f - clip.y / clip.w * 0.5f ) * vpSize.y );
            return true;
        }

        // UE LinearColors::SelectFColor(GroupID): a stable, well-spread colour per group id (golden-ratio hue).
        glm::vec4 GroupColour( int group, float alpha )
        {
            const float h = std::fmod( 0.13f + 0.6180339887f * static_cast<float>( group < 0 ? 0 : group ), 1.0f );
            float       r = 0.0f, g = 0.0f, b = 0.0f;
            ::ImGui::ColorConvertHSVtoRGB( h, 0.65f, 0.95f, r, g, b );
            return glm::vec4( r, g, b, alpha );
        }

        void Refuse( MS& ms, const std::string& what, const std::string& why )
        {
            ms.PaintGroupsStatus = std::format( "{} refused: {}", what, why );
            LOG_ERROR( "[Modeling] Paint PolyGroups: {0}", ms.PaintGroupsStatus );
            ToastManager::Push( "Paint PolyGroups: " + ms.PaintGroupsStatus, ToastLevel::Error, 10.0f );
        }
    } // namespace

    void PaintGroupsTool::Begin( ::Desert::Core::Scene& scene )
    {
        MS& ms = MS::Get();
        m_Session.reset();
        const auto selected = Core::SelectionManager::GetSelected();
        if ( !selected.has_value() )
        {
            ms.PaintGroupsStatus = "Select an entity with a static mesh.";
            return;
        }
        ECS::StaticMeshComponent* component = MeshOf( EntityOf( scene, *selected ) );
        if ( component == nullptr )
        {
            ms.PaintGroupsStatus = "The selected entity has no static mesh.";
            return;
        }
        auto target = GetToolTargetMesh( *component );
        if ( !target.IsSuccess() )
        {
            ms.PaintGroupsStatus = target.GetError();
            return;
        }
        Session session{ *selected, target.GetValue().Committed,
                         std::make_shared<Geometry::DynamicMesh3>( *target.GetValue().Mesh ), 0 };
        if ( !session.Working->HasTriangleGroups() )
            session.Working->EnableTriangleGroups( 0 );
        // UE starts on the group of the mesh's first triangle; ours keeps the panel's choice while it is a group
        // the mesh can hold (any id >= 0).
        ms.PaintGroupsCurrent = std::max( ms.PaintGroupsCurrent, 0 );
        ms.PaintGroupsStatus  = std::format( "{} triangles, groups < {}. Paint with the left button.",
                                             session.Working->TriangleCount(), session.Working->MaxGroupID() );
        m_Session             = std::move( session );
        m_TreeMesh            = nullptr;
    }

    // UE UMeshGroupPaintTool::ApplyStamp (brush: the ROI triangles inside the stamp, reached over triangle
    // adjacency from the hit one so the far side of a thin part is not painted) and the flood fill action
    // (the connected triangles of the seed's group).
    int PaintGroupsTool::Paint( int seed, const glm::dvec3& centreObject, double radiusObject )
    {
        Geometry::DynamicMesh3& mesh  = *m_Session->Working;
        const int               group = MS::Get().PaintGroupsCurrent;
        const bool              fill  = MS::Get().PaintGroupsMode == MS::PaintGroupsModeKind::Fill;
        const int               from  = mesh.GetTriangleGroup( seed );
        const double            r2    = radiusObject * radiusObject;

        std::vector<char> seen( static_cast<std::size_t>( mesh.MaxTriangleID() ), 0 );
        std::vector<int>  stack{ seed };
        seen[static_cast<std::size_t>( seed )] = 1;
        int changed                            = 0;
        while ( !stack.empty() )
        {
            const int t = stack.back();
            stack.pop_back();
            if ( mesh.GetTriangleGroup( t ) != group )
            {
                mesh.SetTriangleGroup( t, group );
                ++changed;
            }
            const auto nbrs = mesh.GetTriNeighbourTris( t );
            for ( int k = 0; k < 3; ++k )
            {
                const int n = nbrs[k];
                if ( n < 0 || !mesh.IsTriangle( n ) || seen[static_cast<std::size_t>( n )] )
                    continue;
                const bool inside = fill ? mesh.GetTriangleGroup( n ) == from
                                         : glm::dot( mesh.GetTriCentroid( n ) - centreObject,
                                                     mesh.GetTriCentroid( n ) - centreObject ) <= r2;
                if ( !inside )
                    continue;
                seen[static_cast<std::size_t>( n )] = 1;
                stack.push_back( n );
            }
        }
        return changed;
    }

    bool PaintGroupsTool::Accept( ::Desert::Core::Scene& scene )
    {
        MS& ms = MS::Get();
        if ( !m_Session )
        {
            Refuse( ms, "Accept", ms.PaintGroupsStatus );
            return false;
        }
        ECS::StaticMeshComponent* component = MeshOf( EntityOf( scene, m_Session->Entity ) );
        if ( component == nullptr )
        {
            Refuse( ms, "Accept", "the entity is gone" );
            return false;
        }
        if ( m_Session->Painted == 0 )
        {
            ms.PaintGroupsStatus = "Nothing was painted: no undo step.";
            m_Session.reset();
            return true;
        }
        auto after = std::shared_ptr<const Geometry::DynamicMesh3>( std::move( m_Session->Working ) );
        const ECS::Entity* entity = EntityOf( scene, m_Session->Entity );
        if ( auto set = ECS::SetEditableMesh( *entity->GetRegistry(), entity->GetHandle(), after ); !set.IsSuccess() )
        {
            Refuse( ms, "Accept", set.GetError() );
            m_Session.reset();
            return false;
        }
        Commands::RecordMeshChange( m_Session->Entity, "Modeling: Paint PolyGroups", m_Session->Committed );
        LOG_INFO( "[Modeling] Paint PolyGroups accepted: {0} triangle repaints, groups < {1}", m_Session->Painted,
                  after->MaxGroupID() );
        ms.PaintGroupsStatus = std::format( "Accepted: {} triangle repaints.", m_Session->Painted );
        m_Session.reset();
        return true;
    }

    void PaintGroupsTool::DrawPreview( ::Desert::Core::Scene& scene, const glm::dmat4& world,
                                       const glm::dvec3& eyeWorld ) const
    {
        // UE UMeshGroupPaintTool's preview: the mesh itself tinted with each triangle's group colour, drawn BY
        // THE SCENE RENDER with its depth (Render::ToolPreview), so whatever stands in front of it covers it;
        // the group borders are depth-tested lines lifted off their surface, not an overlay over the frame.
        using Vertex                         = Graphic::MaterialDebugLine::LineVertex;
        const Geometry::DynamicMesh3& mesh   = *m_Session->Working;
        const glm::dmat3              normal = glm::transpose( glm::inverse( glm::dmat3( world ) ) );
        const auto at = [&]( int v ) { return glm::vec3( world * glm::dvec4( mesh.GetVertex( v ), 1.0 ) ); };
        const auto worldNormal = [&]( int t )
        {
            const glm::dvec3 n   = normal * mesh.GetTriNormal( t );
            const double     len = glm::length( n );
            return len > 0.0 ? glm::vec3( n / len ) : glm::vec3( 0.0f );
        };
        struct Face
        {
            int    Tri;
            double Depth;
        };
        std::vector<Face> faces;
        std::vector<char> front( static_cast<size_t>( std::max( mesh.MaxTriangleID(), 0 ) ), 0 );
        for ( int t = 0; t < mesh.MaxTriangleID(); ++t )
        {
            if ( !mesh.IsTriangle( t ) )
                continue;
            const glm::dvec3 c = glm::dvec3( world * glm::dvec4( mesh.GetTriCentroid( t ), 1.0 ) );
            if ( glm::dot( normal * mesh.GetTriNormal( t ), c - eyeWorld ) >= 0.0 )
                continue; // back-facing: hidden behind the front of the mesh
            front[static_cast<size_t>( t )] = 1;
            faces.push_back( { t, glm::length( c - eyeWorld ) } );
        }
        // Far to near: the translucent faces blend in the order a depth write would have kept.
        std::sort( faces.begin(), faces.end(), []( const Face& a, const Face& b ) { return a.Depth > b.Depth; } );

        Render::ToolPreviewMesh preview;
        preview.Triangles.reserve( faces.size() * 3 );
        std::vector<char> edgeDone( static_cast<size_t>( std::max( mesh.MaxEdgeID(), 0 ) ), 0 );
        const glm::vec4   borderColour( 0.08f, 0.08f, 0.08f, 1.0f );
        for ( const Face& f : faces )
        {
            const glm::vec4 colour = GroupColour( mesh.GetTriangleGroup( f.Tri ), 0.6f );
            const auto      tri    = mesh.GetTriangle( f.Tri );
            for ( const int v : { tri.A, tri.B, tri.C } )
                preview.Triangles.push_back( Vertex{ glm::vec4( at( v ), 1.0f ), colour } );
            // UE's group borders: an edge whose two triangles are in different groups (or an open edge).
            const auto edges = mesh.GetTriEdges( f.Tri );
            const auto nbrs  = mesh.GetTriNeighbourTris( f.Tri );
            for ( int k = 0; k < 3; ++k )
            {
                if ( edgeDone[static_cast<size_t>( edges[k] )] != 0 )
                    continue;
                edgeDone[static_cast<size_t>( edges[k] )] = 1;
                if ( nbrs[k] >= 0 && mesh.GetTriangleGroup( nbrs[k] ) == mesh.GetTriangleGroup( f.Tri ) )
                    continue;
                // The surface the edge lies on: the more grazing of its front faces, for the pass's lift.
                glm::vec3 surface = worldNormal( f.Tri );
                if ( nbrs[k] >= 0 && front[static_cast<size_t>( nbrs[k] )] != 0 )
                {
                    const glm::vec3 other = worldNormal( nbrs[k] );
                    const auto      ev    = mesh.GetEdgeV( edges[k] );
                    const glm::vec3 toEye = glm::vec3( eyeWorld ) - ( at( ev.A ) + at( ev.B ) ) * 0.5f;
                    if ( std::abs( glm::dot( other, toEye ) ) < std::abs( glm::dot( surface, toEye ) ) )
                        surface = other;
                }
                const auto ev = mesh.GetEdgeV( edges[k] );
                preview.Edges.push_back( Vertex{ glm::vec4( at( ev.A ), 1.0f ), borderColour } );
                preview.Edges.push_back( Vertex{ glm::vec4( at( ev.B ), 1.0f ), borderColour } );
                preview.EdgeSurfaces.push_back( surface );
            }
        }
        Render::ToolPreview::Show( scene, std::move( preview ) );
    }

    void PaintGroupsTool::Update( ::Desert::Core::Scene& scene, const Common::Math::Ray& ray,
                                  const glm::mat4& viewProj, const glm::vec2& viewportPos,
                                  const glm::vec2& viewportSize, bool hovered )
    {
        m_HoverHit        = false;
        MS&        ms     = MS::Get();
        const bool active = ms.ActiveTool == MS::Tool::PaintGroups;

        // As EditMaterialsTool: another tool (the bar's Cancel included) or another entity ends the session.
        if ( m_Session )
        {
            const auto selected = Core::SelectionManager::GetSelected();
            if ( !active || !selected.has_value() || *selected != m_Session->Entity ||
                 MeshOf( EntityOf( scene, m_Session->Entity ) ) == nullptr ||
                 // Undo / Redo (or any other edit) replaced the component's mesh under the session: start over
                 // from what it holds now, or the preview would show the copy of the mesh it no longer has.
                 MeshOf( EntityOf( scene, m_Session->Entity ) )->GetEditableMesh() != m_Session->Committed )
            {
                if ( !active && ms.ReqCancel )
                    ms.ReqCancel = false;
                m_Session.reset();
                m_Painting = false;
                m_TreeMesh = nullptr;
            }
        }
        if ( !active )
            return;
        if ( !m_Session )
            Begin( scene );
        if ( !m_Session )
        {
            ms.PaintGroupsReqNewGroup = false;
            return;
        }

        Geometry::DynamicMesh3& mesh = *m_Session->Working;
        if ( ms.PaintGroupsReqNewGroup )
        {
            ms.PaintGroupsReqNewGroup = false;
            ms.PaintGroupsCurrent     = mesh.AllocateTriangleGroup();
            ms.PaintGroupsStatus      = std::format( "New group {}.", ms.PaintGroupsCurrent );
        }

        if ( ms.ReqAccept )
        {
            ms.ReqAccept         = false;
            const bool endTool   = ms.ReqAcceptEndsTool;
            ms.ReqAcceptEndsTool = false;
            m_Painting           = false;
            if ( !Accept( scene ) )
                return;
            m_TreeMesh = nullptr;
            if ( endTool )
                ms.ActiveTool = MS::Tool::None;
            else
                Begin( scene );
            return;
        }

        const ECS::Entity* entity = EntityOf( scene, m_Session->Entity );
        const glm::dmat4   world( entity->GetWorldTransform() );
        const glm::dmat4   toObject = glm::inverse( world );
        if ( m_TreeMesh != &mesh || m_TreeStamp != mesh.GetChangeStamp() )
        {
            m_Tree.SetMesh( &mesh, true );
            m_TreeMesh  = &mesh;
            m_TreeStamp = mesh.GetChangeStamp();
        }
        const glm::dvec3 rayO = glm::dvec3( toObject * glm::dvec4( glm::dvec3( ray.Origin ), 1.0 ) );
        const glm::dvec3 rayD = glm::dvec3( toObject * glm::dvec4( glm::dvec3( ray.Direction ), 0.0 ) );
        double           t    = 0.0;
        const int        tri  = m_Tree.FindNearestHitTriangle( rayO, rayD, t );
        const bool       hit  = tri != Geometry::DynamicMesh3::InvalidID;
        m_HoverHit            = hovered && hit;

        const bool interact = hovered && !::ImGui::IsAnyItemActive();
        const bool fill     = ms.PaintGroupsMode == MS::PaintGroupsModeKind::Fill;
        if ( !::ImGui::IsMouseDown( ImGuiMouseButton_Left ) )
            m_Painting = false;
        else if ( !m_Painting && interact && hit && ::ImGui::IsMouseClicked( ImGuiMouseButton_Left ) )
            m_Painting = true;
        if ( m_Painting && hit )
        {
            const double radius = ms.PaintGroupsRadius / MeanScale( world );
            const int    n      = Paint( tri, rayO + t * rayD, radius );
            if ( n > 0 )
            {
                m_Session->Painted += n;
                ms.PaintGroupsStatus =
                     std::format( "{} triangle repaints (group {}). Accept keeps them as one undo "
                                  "step.",
                                  m_Session->Painted, ms.PaintGroupsCurrent );
            }
            if ( fill )
                m_Painting = false; // a fill is one click
        }

        // The preview stays while the cursor is elsewhere (the panel, the bar).
        DrawPreview( scene, world, glm::dvec3( ray.Origin ) );

        // UE's brush indicator: a circle of the brush radius across the surface normal at the hit.
        if ( !hit || !hovered || fill )
            return;
        const glm::dvec3 centreWorld = glm::dvec3( world * glm::dvec4( rayO + t * rayD, 1.0 ) );
        const glm::dvec3 n =
             glm::normalize( glm::transpose( glm::inverse( glm::dmat3( world ) ) ) * mesh.GetTriNormal( tri ) );
        const glm::dvec3    helper    = std::abs( n.y ) < 0.9 ? glm::dvec3( 0, 1, 0 ) : glm::dvec3( 1, 0, 0 );
        const glm::dvec3    u         = glm::normalize( glm::cross( n, helper ) );
        const glm::dvec3    v         = glm::cross( n, u );
        constexpr int       kSegments = 48;
        std::vector<ImVec2> ring;
        ring.reserve( kSegments );
        for ( int i = 0; i < kSegments; ++i )
        {
            const double     a = 2.0 * 3.14159265358979323846 * i / kSegments;
            const glm::dvec3 p = centreWorld + static_cast<double>( ms.PaintGroupsRadius ) *
                                                    ( std::cos( a ) * u + std::sin( a ) * v );
            ImVec2 s;
            if ( !ToScreen( viewProj, glm::vec3( p ), viewportPos, viewportSize, s ) )
                return;
            ring.push_back( s );
        }
        ImDrawList* dl = ::ImGui::GetWindowDrawList();
        const glm::vec4 group  = GroupColour( ms.PaintGroupsCurrent, 1.0f );
        const ImU32     colour = m_Painting
                                      ? ::ImGui::ColorConvertFloat4ToU32( ImVec4( group.r, group.g, group.b, 1.0f ) )
                                      : IM_COL32( 230, 230, 230, 230 );
        dl->AddPolyline( ring.data(), static_cast<int>( ring.size() ), colour, ImDrawFlags_Closed, 2.0f );
    }
} // namespace Desert::Editor::Tools
