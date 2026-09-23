#include "PolyEditTool.hpp"

#include <Editor/Core/Selection/SelectionManager.hpp>
#include <Editor/Core/Selection/ModelingState.hpp>
#include <Editor/Core/Commands/SceneCommands.hpp>

#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Entity.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/EditableMesh.hpp>
#include <Engine/Geometry/EditMesh.hpp>
#include <Engine/Geometry/EditMeshNormals.hpp>

#include <Common/Core/Logger.hpp>

#include <ImGui/imgui.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <memory>
#include <optional>
#include <unordered_set>

namespace Desert::Editor::Tools
{
    namespace
    {
        // Möller–Trumbore ray/triangle (front+back). Returns the ray parameter t (>0) on hit.
        bool RayTri( const glm::vec3& o, const glm::vec3& d, const glm::vec3& a, const glm::vec3& b,
                     const glm::vec3& c, float& t )
        {
            const glm::vec3 e1 = b - a, e2 = c - a, p = glm::cross( d, e2 );
            const float     det = glm::dot( e1, p );
            if ( std::abs( det ) < 1e-9f )
                return false;
            const float     inv = 1.0f / det;
            const glm::vec3 tv  = o - a;
            const float     u   = glm::dot( tv, p ) * inv;
            if ( u < 0.0f || u > 1.0f )
                return false;
            const glm::vec3 q = glm::cross( tv, e1 );
            const float     v = glm::dot( d, q ) * inv;
            if ( v < 0.0f || u + v > 1.0f )
                return false;
            t = glm::dot( e2, q ) * inv;
            return t > 1e-4f;
        }

        struct EditTarget
        {
            ECS::StaticMeshComponent* Component = nullptr;
            glm::mat4                 World{ 1.0f };
        };

        // The selected entity's EDITABLE mesh (StaticMeshComponent::EditableMesh) + its world transform; an
        // entity drawn from an asset or a primitive has none, and the tool does nothing to it.
        std::optional<EditTarget> GetTarget( ::Desert::Core::Scene& scene, const Common::UUID& id )
        {
            if ( static_cast<uint64_t>( id ) == 0 )
                return std::nullopt;
            auto ref = scene.FindEntityByID( id );
            if ( !ref )
                return std::nullopt;
            ECS::Entity e = ref->get();
            if ( !e.HasComponent<ECS::StaticMeshComponent>() )
                return std::nullopt;
            auto& smc = e.GetComponent<ECS::StaticMeshComponent>();
            if ( !smc.EditableMesh )
                return std::nullopt;
            EditTarget target;
            target.Component = &smc;
            target.World     = e.HasComponent<ECS::TransformComponent>()
                                    ? e.GetComponent<ECS::TransformComponent>().GetTransform()
                                    : glm::mat4( 1.0f );
            return target;
        }
    } // namespace

    bool PolyEditTool::WorldToScreen( const glm::vec3& world, const glm::mat4& vp, const glm::vec2& pos,
                                      const glm::vec2& size, glm::vec2& out )
    {
        const glm::vec4 clip = vp * glm::vec4( world, 1.0f );
        if ( clip.w <= 0.0001f )
            return false;
        const glm::vec3 ndc = glm::vec3( clip ) / clip.w;
        out.x               = pos.x + ( ndc.x * 0.5f + 0.5f ) * size.x;
        out.y               = pos.y + ( 1.0f - ( ndc.y * 0.5f + 0.5f ) ) * size.y;
        return true;
    }

    void PolyEditTool::ClearSelection()
    {
        FinishDrag();
        m_SelVerts.clear();
        m_SelTris.clear();
        m_HasSel = false;
    }

    void PolyEditTool::FinishDrag()
    {
        // A drag that ends any way at all - release, a changed selection, the tool switched off - is ONE
        // undo step, from the mesh the drag started on to whatever the component holds now.
        if ( m_Dragging )
            Commands::RecordEditMeshChange( m_DragEntity, "Push/Pull Face", std::move( m_DragBefore ) );
        m_DragBefore.reset();
        m_Dragging = false;
    }

    bool PolyEditTool::PickFace( ::Desert::Core::Scene& scene, const Common::Math::Ray& ray )
    {
        const auto target = GetTarget( scene, m_Entity );
        if ( !target )
            return false;
        const Geometry::EditMesh& mesh  = *target->Component->EditableMesh;
        const glm::mat4&          world = target->World;

        // Nearest triangle under the ray (world space).
        int   hit   = Geometry::InvalidId;
        float bestT = FLT_MAX;
        for ( const int t : mesh.TriangleIds() )
        {
            const auto&     tri = mesh.GetTriangle( t );
            const glm::vec3 a   = glm::vec3( world * glm::vec4( mesh.GetPosition( tri[0] ), 1.0f ) );
            const glm::vec3 b   = glm::vec3( world * glm::vec4( mesh.GetPosition( tri[1] ), 1.0f ) );
            const glm::vec3 c   = glm::vec3( world * glm::vec4( mesh.GetPosition( tri[2] ), 1.0f ) );
            float           hitT;
            if ( RayTri( ray.Origin, ray.Direction, a, b, c, hitT ) && hitT < bestT )
            {
                bestT = hitT;
                hit   = t;
            }
        }
        if ( hit == Geometry::InvalidId )
        {
            ClearSelection();
            return false;
        }

        // Flood the coplanar face from the hit triangle across the mesh's own edges (shared edge + near-equal
        // normal). The EditMesh knows its topology, so this no longer re-welds render vertices by a quantised
        // position key on every click, which is what the tool had to do while it edited the render buffer.
        const glm::vec3   hN = Geometry::TriangleNormal( mesh, hit );
        std::vector<int>  face{ hit };
        std::vector<char> seen( static_cast<size_t>( mesh.MaxTriangleId() ), 0 );
        seen[hit] = 1;
        for ( size_t f = 0; f < face.size(); ++f )
            for ( const int e : mesh.GetTriangleEdges( face[f] ) )
                for ( const int nb : mesh.GetEdgeTriangles( e ) )
                    if ( nb != Geometry::InvalidId && !seen[nb] &&
                         glm::dot( Geometry::TriangleNormal( mesh, nb ), hN ) > 0.99f )
                    {
                        seen[nb] = 1;
                        face.push_back( nb );
                    }

        // Selection = the face's vertices: shared corners are ONE vertex in an EditMesh, so the neighbours
        // follow the drag by construction.
        std::unordered_set<int> verts;
        for ( const int t : face )
            for ( const int v : mesh.GetTriangle( t ) )
                verts.insert( v );
        m_SelTris = face;
        m_SelVerts.assign( verts.begin(), verts.end() );
        std::sort( m_SelVerts.begin(), m_SelVerts.end() );

        glm::vec3 centroidLocal( 0.0f );
        for ( const int v : m_SelVerts )
            centroidLocal += mesh.GetPosition( v );
        centroidLocal /= static_cast<float>( m_SelVerts.size() );

        m_FaceNormalLocal = hN;
        m_CentroidWorld   = glm::vec3( world * glm::vec4( centroidLocal, 1.0f ) );
        m_HasSel          = !m_SelVerts.empty();
        return m_HasSel;
    }

    void PolyEditTool::Update( ::Desert::Core::Scene& scene, const Common::Math::Ray& ray,
                               const glm::mat4& viewProj, const glm::vec2& viewportPos,
                               const glm::vec2& viewportSize, bool interactive )
    {
        if ( Core::ModelingState::Get().ActiveTool != Core::ModelingState::Tool::PolyEdit )
        {
            ClearSelection();
            return;
        }

        // The edited entity follows the current selection; a changed selection drops the face.
        const auto&        selOpt = Core::SelectionManager::GetSelected();
        const Common::UUID sel    = selOpt.has_value() ? *selOpt : Common::UUID::Null();
        if ( static_cast<uint64_t>( sel ) != static_cast<uint64_t>( m_Entity ) )
        {
            m_Entity = sel;
            ClearSelection();
        }

        const auto target = GetTarget( scene, m_Entity );

        ImDrawList* dl = ::ImGui::GetWindowDrawList();
        if ( !target )
        {
            ClearSelection();
            return;
        }
        const glm::mat4& world = target->World;

        const bool interact = interactive && !::ImGui::IsAnyItemActive();

        // Pick a face on click (when not mid-drag).
        if ( interact && !m_Dragging && ::ImGui::IsMouseClicked( ImGuiMouseButton_Left ) )
            PickFace( scene, ray );

        // Push/pull the selected face along its world normal while dragging.
        if ( m_HasSel )
        {
            const glm::vec3 wN = glm::normalize( glm::mat3( world ) * m_FaceNormalLocal );
            // Closest parameter s on the line (centroid + s*wN) to the cursor ray (Ericson's two-line form).
            const glm::vec3 u = wN, v = ray.Direction, w = m_CentroidWorld - ray.Origin;
            const float     b = glm::dot( u, v ), d = glm::dot( u, w ), e = glm::dot( v, w );
            const float     denom = 1.0f - b * b;
            const float     s     = std::abs( denom ) > 1e-5f ? ( b * e - d ) / denom : 0.0f;

            if ( interact && ::ImGui::IsMouseClicked( ImGuiMouseButton_Left ) )
            {
                m_Dragging   = true;
                m_DragS      = s;
                m_DragEntity = m_Entity;
                m_DragBefore = target->Component->EditableMesh;
            }
            if ( m_Dragging && ::ImGui::IsMouseDown( ImGuiMouseButton_Left ) )
            {
                const float ds = s - m_DragS;
                if ( std::abs( ds ) > 1e-4f )
                {
                    // The component's mesh is immutable (EditableMesh.hpp): each step of the drag is a new
                    // mesh, so the one the drag started from stays intact for the undo record.
                    auto            next       = std::make_shared<Geometry::EditMesh>( *target->Component->EditableMesh );
                    const glm::vec3 deltaLocal = glm::vec3( glm::inverse( glm::mat3( world ) ) * ( ds * wN ) );
                    for ( const int vtx : m_SelVerts )
                        next->SetPosition( vtx, next->GetPosition( vtx ) + deltaLocal );

                    // Lighting follows the deform: every triangle touching a moved vertex gives its normal
                    // elements its new face normal, the rule the tool applied to render vertices before. A
                    // seam stays a seam - only element VALUES change, never which triangles share one.
                    if ( auto* normals = next->Attributes().Normals() )
                    {
                        std::unordered_set<int> touched;
                        for ( const int vtx : m_SelVerts )
                            for ( const int t : next->GetVertexTriangles( vtx ) )
                                touched.insert( t );
                        for ( const int t : touched )
                            if ( normals->IsSetTriangle( t ) )
                            {
                                const glm::vec3 n = Geometry::TriangleNormal( *next, t );
                                for ( const int el : normals->GetTriangle( t ) )
                                    normals->SetElement( el, n );
                            }
                    }
                    if ( auto set = ECS::SetEditableMesh( *target->Component, std::move( next ) ); set.IsSuccess() )
                    {
                        m_CentroidWorld += ds * wN;
                        m_DragS = s;
                    }
                    else
                    {
                        LOG_ERROR( "[PolyEdit] the pushed face could not be built: {0}", set.GetError() );
                    }
                }
            }
            if ( !::ImGui::IsMouseDown( ImGuiMouseButton_Left ) )
                FinishDrag();

            // Highlight the selected face (translucent green + outline).
            const Geometry::EditMesh& mesh = *target->Component->EditableMesh;
            for ( const int ti : m_SelTris )
            {
                if ( !mesh.IsTriangle( ti ) )
                    continue;
                const auto&     tri = mesh.GetTriangle( ti );
                glm::vec2       s0, s1, s2;
                const glm::vec3 a  = glm::vec3( world * glm::vec4( mesh.GetPosition( tri[0] ), 1.0f ) );
                const glm::vec3 bb = glm::vec3( world * glm::vec4( mesh.GetPosition( tri[1] ), 1.0f ) );
                const glm::vec3 c  = glm::vec3( world * glm::vec4( mesh.GetPosition( tri[2] ), 1.0f ) );
                if ( WorldToScreen( a, viewProj, viewportPos, viewportSize, s0 ) &&
                     WorldToScreen( bb, viewProj, viewportPos, viewportSize, s1 ) &&
                     WorldToScreen( c, viewProj, viewportPos, viewportSize, s2 ) )
                {
                    dl->AddTriangleFilled( ImVec2( s0.x, s0.y ), ImVec2( s1.x, s1.y ), ImVec2( s2.x, s2.y ),
                                           IM_COL32( 60, 220, 90, 90 ) );
                    dl->AddTriangle( ImVec2( s0.x, s0.y ), ImVec2( s1.x, s1.y ), ImVec2( s2.x, s2.y ),
                                     IM_COL32( 90, 240, 120, 255 ), 1.5f );
                }
            }
        }
    }
} // namespace Desert::Editor::Tools
