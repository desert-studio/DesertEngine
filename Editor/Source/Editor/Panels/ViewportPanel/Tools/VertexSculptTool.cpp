#include "VertexSculptTool.hpp"

#include <Editor/Core/Commands/SceneCommands.hpp>
#include <Editor/Core/Selection/ModelingState.hpp>
#include <Editor/Core/Selection/ModelingToolTarget.hpp>
#include <Editor/Core/Selection/SelectionManager.hpp>

#include <Common/Core/Logger.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/EditableMesh.hpp>
#include <Engine/ECS/Entity.hpp>
#include <Engine/Geometry/MeshSculptBrushes.hpp>

#include <ImGui/imgui.h>

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

        // The entity's world scale as one number: the brush radius is world centimetres, the mesh is edited in
        // its own space.
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
    } // namespace

    void VertexSculptTool::EndStroke( ::Desert::Core::Scene& scene )
    {
        if ( !m_Stroke )
            return;
        Stroke stroke = std::move( *m_Stroke );
        m_Stroke.reset();
        LOG_INFO( "[Modeling] Vertex Sculpt stroke ended: {0} vertex moves", stroke.Moved );
        ECS::StaticMeshComponent* component = MeshOf( EntityOf( scene, stroke.Entity ) );
        if ( component == nullptr )
            return;
        if ( stroke.Moved == 0 )
        {
            // Nothing moved: the component goes back to what it held, no undo step.
            if ( stroke.Before )
                (void)ECS::SetEditableMesh( *component, stroke.Before );
            return;
        }
        const auto finished = Geometry::FinishSculptStroke( *stroke.Working );
        if ( !finished.IsSuccess() )
            LOG_ERROR( "[Modeling] Vertex Sculpt: {0}", finished.GetError() );
        const auto set = ECS::SetEditableMesh(
             *component, std::shared_ptr<const Geometry::DynamicMesh3>( std::move( stroke.Working ) ) );
        if ( !set.IsSuccess() )
        {
            LOG_ERROR( "[Modeling] Vertex Sculpt stroke refused: {0}", set.GetError() );
            if ( stroke.Before )
                (void)ECS::SetEditableMesh( *component, stroke.Before );
            return;
        }
        Commands::RecordMeshChange(
             stroke.Entity, std::format( "Vertex Sculpt: {}", Geometry::ToString( MS::Get().DeformSculpt.Brush ) ),
             stroke.Before );
    }

    void VertexSculptTool::Update( ::Desert::Core::Scene& scene, const Common::Math::Ray& ray,
                                   const glm::mat4& viewProj, const glm::vec2& viewportPos,
                                   const glm::vec2& viewportSize, bool hovered )
    {
        m_HoverHit = false;
        MS& ms     = MS::Get();
        if ( ms.ActiveTool != MS::Tool::VertexSculpt )
        {
            EndStroke( scene );
            return;
        }
        const auto         selected = Core::SelectionManager::GetSelected();
        const Common::UUID sel      = selected.has_value() ? *selected : Common::UUID::Null();
        if ( m_Stroke && static_cast<uint64_t>( m_Stroke->Entity ) != static_cast<uint64_t>( sel ) )
            EndStroke( scene );

        const ECS::Entity*              entity    = EntityOf( scene, sel );
        ECS::StaticMeshComponent* component = MeshOf( entity );
        if ( component == nullptr )
        {
            ms.DeformStatus = "Select an entity with a static mesh.";
            return;
        }
        const glm::dmat4 world( entity->GetWorldTransform() );
        const glm::dmat4 toObject = glm::inverse( world );
        const double     scale    = MeanScale( world );

        // The mesh under the brush: the stroke's working copy, else the tool target.
        const Geometry::DynamicMesh3*                 target = nullptr;
        std::shared_ptr<const Geometry::DynamicMesh3> targetHold;
        std::shared_ptr<const Geometry::DynamicMesh3> committed;
        if ( m_Stroke )
        {
            targetHold = m_Stroke->Working;
            target     = targetHold.get();
        }
        else
        {
            auto t = GetToolTargetMesh( *component );
            if ( !t.IsSuccess() )
            {
                ms.DeformStatus = t.GetError();
                return;
            }
            targetHold = t.GetValue().Mesh;
            committed  = t.GetValue().Committed;
            target     = targetHold.get();
        }
        if ( target == nullptr || target->TriangleCount() == 0 )
            return;
        if ( m_TreeMesh != target || m_TreeStamp != target->GetChangeStamp() )
        {
            m_Tree.SetMesh( target, true );
            m_TreeMesh  = target;
            m_TreeHold  = targetHold;
            m_TreeStamp = target->GetChangeStamp();
        }

        const glm::dvec3 rayO = glm::dvec3( toObject * glm::dvec4( glm::dvec3( ray.Origin ), 1.0 ) );
        const glm::dvec3 rayD = glm::dvec3( toObject * glm::dvec4( glm::dvec3( ray.Direction ), 0.0 ) );
        double           t    = 0.0;
        const int        tri  = m_Tree.FindNearestHitTriangle( rayO, rayD, t );
        const bool       hit  = tri != Geometry::DynamicMesh3::InvalidID;
        glm::dvec3       hitWorld( 0.0 ), normalWorld( 0.0, 1.0, 0.0 );
        if ( hit )
        {
            hitWorld    = glm::dvec3( world * glm::dvec4( rayO + t * rayD, 1.0 ) );
            normalWorld = glm::normalize( glm::transpose( glm::inverse( glm::dmat3( world ) ) ) *
                                          target->GetTriNormal( tri ) );
        }
        m_HoverHit = hovered && hit;

        const bool interact = hovered && !::ImGui::IsAnyItemActive();
        if ( !m_Stroke && interact && hit && ::ImGui::IsMouseClicked( ImGuiMouseButton_Left ) )
        {
            Stroke stroke;
            stroke.Entity      = sel;
            stroke.Before      = committed;
            stroke.Working     = std::make_shared<Geometry::DynamicMesh3>( *target );
            stroke.PlanePoint  = hitWorld;
            stroke.PlaneNormal = -glm::normalize( glm::dvec3( ray.Direction ) );
            stroke.PrevCenter  = glm::dvec3( toObject * glm::dvec4( hitWorld, 1.0 ) );
            m_Stroke           = std::move( stroke );
            LOG_INFO( "[Modeling] Vertex Sculpt stroke began at ({0}, {1}, {2})", hitWorld.x, hitWorld.y,
                      hitWorld.z );
        }

        // The brush centre this frame (world): Move rides UE's active plane, Smooth the surface.
        const bool movePlane   = m_Stroke && ms.DeformSculpt.Brush == Geometry::SculptBrush::Move;
        glm::dvec3 centreWorld = hitWorld;
        bool       haveCentre  = hit;
        if ( movePlane )
        {
            const glm::dvec3 o     = glm::dvec3( ray.Origin );
            const glm::dvec3 d     = glm::dvec3( ray.Direction );
            const double     denom = glm::dot( d, m_Stroke->PlaneNormal );
            haveCentre             = std::abs( denom ) > 1e-9;
            if ( haveCentre )
                centreWorld = o + glm::dot( m_Stroke->PlanePoint - o, m_Stroke->PlaneNormal ) / denom * d;
        }

        if ( m_Stroke )
        {
            if ( !::ImGui::IsMouseDown( ImGuiMouseButton_Left ) )
            {
                EndStroke( scene );
                return;
            }
            if ( haveCentre )
            {
                Geometry::SculptSettings settings = ms.DeformSculpt;
                settings.Radius                   = static_cast<float>( settings.Radius / scale );
                Geometry::SculptStamp stamp;
                stamp.Center     = glm::dvec3( toObject * glm::dvec4( centreWorld, 1.0 ) );
                stamp.PrevCenter = m_Stroke->PrevCenter;
                const auto moved = Geometry::ApplySculptStamp( *m_Stroke->Working, settings, stamp );
                if ( !moved.IsSuccess() )
                    ms.DeformStatus = moved.GetError();
                else if ( moved.GetValue() > 0 )
                {
                    m_Stroke->Moved += moved.GetValue();
                    // The entity draws a snapshot of the working copy; the copy goes on being stamped.
                    (void)ECS::SetEditableMesh(
                         *component, std::make_shared<const Geometry::DynamicMesh3>( *m_Stroke->Working ) );
                    ms.DeformStatus = std::format( "{} vertex moves in this stroke", m_Stroke->Moved );
                }
                m_Stroke->PrevCenter = stamp.Center;
            }
        }

        // UE's brush indicator: a circle of the brush radius across the surface normal at the brush centre.
        if ( !haveCentre || !hovered )
            return;
        const glm::dvec3    n         = movePlane ? m_Stroke->PlaneNormal : normalWorld;
        const glm::dvec3    helper    = std::abs( n.y ) < 0.9 ? glm::dvec3( 0, 1, 0 ) : glm::dvec3( 1, 0, 0 );
        const glm::dvec3    u         = glm::normalize( glm::cross( n, helper ) );
        const glm::dvec3    v         = glm::cross( n, u );
        constexpr int       kSegments = 48;
        const double        radius    = ms.DeformSculpt.Radius;
        std::vector<ImVec2> ring;
        ring.reserve( kSegments );
        for ( int i = 0; i < kSegments; ++i )
        {
            const double     a = 2.0 * 3.14159265358979323846 * i / kSegments;
            const glm::dvec3 p = centreWorld + radius * ( std::cos( a ) * u + std::sin( a ) * v );
            ImVec2           s;
            if ( !ToScreen( viewProj, glm::vec3( p ), viewportPos, viewportSize, s ) )
                return;
            ring.push_back( s );
        }
        ImDrawList* dl     = ::ImGui::GetWindowDrawList();
        const ImU32 colour = m_Stroke ? IM_COL32( 255, 200, 60, 255 ) : IM_COL32( 230, 230, 230, 230 );
        dl->AddPolyline( ring.data(), static_cast<int>( ring.size() ), colour, ImDrawFlags_Closed, 2.0f );
        ImVec2 c;
        if ( ToScreen( viewProj, glm::vec3( centreWorld ), viewportPos, viewportSize, c ) )
            dl->AddCircleFilled( c, 3.0f, colour );
    }
} // namespace Desert::Editor::Tools
