#include "SculptTool.hpp"

#include <Editor/Core/Commands/SceneCommands.hpp>
#include <Editor/Core/Selection/ModelingState.hpp>
#include <Editor/Core/Selection/ModelingToolTarget.hpp>
#include <Editor/Core/Selection/SelectionManager.hpp>

#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/EditableMesh.hpp>
#include <Engine/ECS/Entity.hpp>
#include <Engine/Geometry/DynamicMeshRemesh.hpp>
#include <Engine/Geometry/DynamicMeshSculpt.hpp>
#include <Engine/Geometry/ElementSelection.hpp>
#include <Engine/Geometry/MeshCore/DynamicMesh/DynamicMesh3.hpp>

#include <Common/Core/Logger.hpp>

#include <ImGui/imgui.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <vector>

namespace Desert::Editor::Tools
{
    namespace
    {
        // A stroke lays at most this many stamps in one frame: a cursor that jumped across the screen is a
        // new contact, not a hundred stamps along a line the user never drew (UE resets the stroke the same
        // way when the hit leaves the mesh).
        constexpr int kMaxStampsPerFrame = 64;

        struct SculptTarget
        {
            ECS::Entity    Entity;
            glm::dmat4     World{ 1.0 };
            ToolTargetMesh Target;
        };

        std::optional<SculptTarget> GetTarget( ::Desert::Core::Scene& scene, const Common::UUID& id )
        {
            if ( static_cast<uint64_t>( id ) == 0 )
                return std::nullopt;
            auto ref = scene.FindEntityByID( id );
            if ( !ref )
                return std::nullopt;
            ECS::Entity e = ref->get();
            if ( !e.HasComponent<ECS::StaticMeshComponent>() )
                return std::nullopt;
            auto mesh = GetToolTargetMesh( e.GetComponent<ECS::StaticMeshComponent>() );
            if ( !mesh.IsSuccess() )
                return std::nullopt;
            const glm::mat4 world = e.HasComponent<ECS::TransformComponent>()
                                         ? e.GetComponent<ECS::TransformComponent>().GetTransform()
                                         : glm::mat4( 1.0f );
            SculptTarget target{ e, glm::dmat4( world ), mesh.ExtractValue() };
            if ( !target.Target.Mesh )
                return std::nullopt;
            return target;
        }

        // The brush on the surface: a ring of the brush radius in the plane of the hit (UE draws its brush
        // indicator the same way), and the inner ring where the falloff begins.
        void DrawBrush( ImDrawList* dl, const Geometry::SurfaceSculptStamp& hit,
                        const Geometry::SculptBrushSettings& brush, const glm::mat4& viewProj,
                        const glm::vec2& viewportPos, const glm::vec2& viewportSize )
        {
            const glm::vec3 n  = glm::vec3( hit.Normal );
            const glm::vec3 a  = glm::normalize( std::abs( n.y ) < 0.9f ? glm::cross( n, glm::vec3( 0, 1, 0 ) )
                                                                        : glm::cross( n, glm::vec3( 1, 0, 0 ) ) );
            const glm::vec3 b  = glm::cross( n, a );
            const ImU32 colour = brush.Brush == Geometry::SculptBrushType::Smooth ? IM_COL32( 120, 200, 255, 255 )
                                 : brush.Brush == Geometry::SculptBrushType::Move ? IM_COL32( 140, 240, 140, 255 )
                                 : brush.Invert                                   ? IM_COL32( 255, 140, 90, 255 )
                                                                                  : IM_COL32( 240, 240, 120, 255 );
            for ( const float share : { 1.0f, std::clamp( 1.0f - brush.Falloff, 0.0f, 1.0f ) } )
            {
                if ( share <= 0.0f )
                    continue;
                constexpr int kSegments = 48;
                ImVec2        pts[kSegments];
                int           count = 0;
                for ( int i = 0; i < kSegments; ++i )
                {
                    const float     ang = 6.2831853f * static_cast<float>( i ) / kSegments;
                    const glm::vec3 p   = glm::vec3( hit.Center ) +
                                        brush.Radius * share * ( std::cos( ang ) * a + std::sin( ang ) * b );
                    glm::vec2 s;
                    if ( Geometry::ProjectToViewport( p, viewProj, viewportPos, viewportSize, s ) )
                        pts[count++] = ImVec2( s.x, s.y );
                }
                if ( count == kSegments )
                    dl->AddPolyline( pts, count, colour, ImDrawFlags_Closed, share == 1.0f ? 2.0f : 1.0f );
            }
        }
    } // namespace

    void SculptTool::FinishStroke()
    {
        // A stroke that ends any way at all - release, a changed selection, the tool switched off - is ONE undo
        // step, from the mesh the stroke started on to whatever the component holds now.
        if ( m_Stroking )
            Commands::RecordMeshChange( m_StrokeEntity, "Sculpt Stroke", std::move( m_StrokeBefore ) );
        m_StrokeBefore.reset();
        m_Stroking = false;
        m_LastStamp.reset();
        m_PlanePoint.reset();
    }

    void SculptTool::Update( ::Desert::Core::Scene& scene, const Common::Math::Ray& ray, const glm::mat4& viewProj,
                             const glm::vec2& viewportPos, const glm::vec2& viewportSize, bool interactive )
    {
        m_Cursor = ToolCursor::None;
        if ( Core::ModelingState::Get().ActiveTool != Core::ModelingState::Tool::Sculpt )
        {
            FinishStroke();
            return;
        }

        const auto&        selOpt = Core::SelectionManager::GetSelected();
        const Common::UUID sel    = selOpt.has_value() ? *selOpt : Common::UUID::Null();
        if ( static_cast<uint64_t>( sel ) != static_cast<uint64_t>( m_Entity ) )
        {
            FinishStroke();
            m_Entity = sel;
        }

        const auto target = GetTarget( scene, m_Entity );
        if ( !target )
        {
            FinishStroke();
            m_Cursor = ToolCursor::Unavailable; // nothing selected to sculpt
            return;
        }

        // The stroke's brush: the panel's, with UE's modifiers (Shift smooths, Ctrl inverts) on top.
        Geometry::SculptBrushSettings brush = Core::ModelingState::Get().Sculpt;
        const ImGuiIO&                io    = ::ImGui::GetIO();
        if ( io.KeyShift )
            brush.Brush = Geometry::SculptBrushType::Smooth;
        if ( io.KeyCtrl )
            brush.Invert = !brush.Invert;

        Geometry::SurfaceSculptStamp hit;
        const bool onMesh = Geometry::RaycastMesh( *target->Target.Mesh, target->World, glm::dvec3( ray.Origin ),
                                                   glm::normalize( glm::dvec3( ray.Direction ) ), hit );

        const bool interact = interactive && !::ImGui::IsAnyItemActive();
        if ( interact && !m_Stroking && onMesh && ::ImGui::IsMouseClicked( ImGuiMouseButton_Left ) )
        {
            m_Stroking     = true;
            m_StrokeEntity = m_Entity;
            // Null for a lifted asset: the stroke's undo returns the entity to its asset.
            m_StrokeBefore = target->Target.Committed;
            m_LastStamp.reset();
            // UE's active plane for Move: through the first hit, facing the cursor ray.
            if ( brush.Brush == Geometry::SculptBrushType::Move )
            {
                m_PlanePoint  = hit.Center;
                m_PlaneNormal = -glm::normalize( glm::dvec3( ray.Direction ) );
                m_LastStamp   = hit.Center;
            }
        }

        // Move: the brush is where the cursor ray meets the active plane, on the mesh or off it; one stamp a
        // frame carries the vertices under the previous centre to this one.
        const bool moving = m_Stroking && m_PlanePoint.has_value();
        if ( moving )
        {
            brush.Brush            = Geometry::SculptBrushType::Move;
            const glm::dvec3 o     = glm::dvec3( ray.Origin );
            const glm::dvec3 d     = glm::normalize( glm::dvec3( ray.Direction ) );
            const double     denom = glm::dot( d, m_PlaneNormal );
            if ( std::abs( denom ) > 1e-9 )
            {
                hit.Center = o + glm::dot( *m_PlanePoint - o, m_PlaneNormal ) / denom * d;
                hit.Normal = m_PlaneNormal;
                if ( ::ImGui::IsMouseDown( ImGuiMouseButton_Left ) && m_LastStamp &&
                     glm::length( hit.Center - *m_LastStamp ) > 0.0 )
                {
                    auto                  next  = std::make_shared<Geometry::DynamicMesh3>( *target->Target.Mesh );
                    Geometry::SurfaceSculptStamp stamp = hit;
                    stamp.PrevCenter            = *m_LastStamp;
                    const auto&                     ms = Core::ModelingState::Get();
                    const Geometry::RemeshSettings* topology =
                         ms.SculptDynamic ? &ms.DynamicSculptTopology : nullptr;
                    Geometry::ApplySculptStamp( *next, target->World, stamp, brush, topology );
                    auto& component = target->Entity.GetComponent<ECS::StaticMeshComponent>();
                    if ( auto set = ECS::SetEditableMesh( component, std::move( next ) ); set.IsSuccess() )
                        m_LastStamp = hit.Center;
                    else
                        LOG_ERROR( "[Sculpt] the moved mesh could not be built: {0}", set.GetError() );
                }
            }
        }

        if ( !moving && m_Stroking && ::ImGui::IsMouseDown( ImGuiMouseButton_Left ) && onMesh )
        {
            // Stamps from the last one to the cursor, a spacing apart; the first contact stamps where it lands.
            const double spacing = std::max( 1e-3, static_cast<double>( brush.Spacing * brush.Radius ) );
            std::vector<glm::dvec3> centres;
            if ( !m_LastStamp )
                centres.push_back( hit.Center );
            else
            {
                const glm::dvec3 from = *m_LastStamp;
                const double     len  = glm::length( hit.Center - from );
                const int        n    = static_cast<int>( len / spacing );
                if ( n > kMaxStampsPerFrame )
                    centres.push_back( hit.Center );
                else
                    for ( int i = 1; i <= n; ++i )
                        centres.push_back( from + ( hit.Center - from ) * ( spacing * i / len ) );
            }
            if ( !centres.empty() )
            {
                // The component's mesh is immutable (EditableMesh.hpp): each frame of the stroke is a new mesh,
                // so the one the stroke started from stays intact for the undo record.
                auto next = std::make_shared<Geometry::DynamicMesh3>( *target->Target.Mesh );
                // Dynamic Sculpt remeshes each stamp's region first (UE UDynamicMeshSculptTool).
                const auto&                     ms       = Core::ModelingState::Get();
                const Geometry::RemeshSettings* topology = ms.SculptDynamic ? &ms.DynamicSculptTopology : nullptr;
                for ( const glm::dvec3& c : centres )
                {
                    Geometry::SurfaceSculptStamp stamp = hit;
                    stamp.Center                = c;
                    Geometry::ApplySculptStamp( *next, target->World, stamp, brush, topology );
                }
                auto& component = target->Entity.GetComponent<ECS::StaticMeshComponent>();
                if ( auto set = ECS::SetEditableMesh( component, std::move( next ) ); set.IsSuccess() )
                    m_LastStamp = centres.back();
                else
                    LOG_ERROR( "[Sculpt] the sculpted mesh could not be built: {0}", set.GetError() );
            }
        }
        // Off the mesh mid-stroke: the next contact starts a fresh run of stamps, not a line across the gap.
        if ( !moving && m_Stroking && !onMesh )
            m_LastStamp.reset();
        if ( m_Stroking && !::ImGui::IsMouseDown( ImGuiMouseButton_Left ) )
            FinishStroke();

        m_Cursor = m_Stroking ? ToolCursor::Drag : onMesh ? ToolCursor::Select : ToolCursor::Unavailable;
        if ( onMesh || moving )
            DrawBrush( ::ImGui::GetWindowDrawList(), hit, brush, viewProj, viewportPos, viewportSize );
    }
} // namespace Desert::Editor::Tools
