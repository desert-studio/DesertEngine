#include "DeformTool.hpp"

#include <Editor/Core/Commands/SceneCommands.hpp>
#include <Editor/Core/Selection/ModelingToolTarget.hpp>
#include <Editor/Core/Selection/SelectionManager.hpp>
#include <Editor/Core/ToastManager.hpp>

#include <Common/Core/Logger.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/EditableMesh.hpp>
#include <Engine/ECS/Entity.hpp>
#include <Engine/Geometry/MeshDeformOperations.hpp>

#include <ImGui/imgui.h>

#include <format>
#include <string>

namespace Desert::Editor::Tools
{
    namespace
    {
        using MS = Core::ModelingState;

        const char* ToolName( MS::Tool tool )
        {
            switch ( tool )
            {
                case MS::Tool::Smooth:
                    return "Smooth";
                case MS::Tool::Offset:
                    return "Offset";
                case MS::Tool::Warp:
                    return "Warp";
                default:
                    return "?";
            }
        }

        glm::mat4 WorldOf( ::Desert::Core::Scene& scene, const Common::UUID& id )
        {
            auto ref = scene.FindEntityByID( id );
            return ref ? ref->get().GetWorldTransform() : glm::mat4( 1.0f );
        }

        // UE UMeshSpaceDeformerTool::Setup: the gizmo on the mesh's bounds centre, its Z (the deformation axis)
        // along the longest side after the entity's scale, the bounds +-half that length, turned with the
        // entity. UE prefers its up axis Z on a tie; ours is Y, so Y wins ties here, then Z, then X - the same
        // cyclic frames UE builds (Z major: X,Y,Z; Y major: Z,X,Y; X major: Y,Z,X), re-ordered for Y-up.
        void PlaceWarpGizmo( const Geometry::DynamicMesh3& mesh, const glm::mat4& world, MS& ms )
        {
            const auto      box = mesh.GetBounds();
            const glm::vec3 lo = glm::vec3( box.Min ), hi = glm::vec3( box.Max );
            const bool      empty  = !( lo.x <= hi.x );
            const glm::vec3 centre = empty ? glm::vec3( 0.0f ) : 0.5f * ( lo + hi );
            const glm::vec3 scale( glm::length( glm::vec3( world[0] ) ), glm::length( glm::vec3( world[1] ) ),
                                   glm::length( glm::vec3( world[2] ) ) );
            const glm::vec3 dims = empty ? glm::vec3( 0.0f ) : ( hi - lo ) * scale;
            glm::mat3       rotation( 1.0f );
            for ( int c = 0; c < 3; ++c )
                if ( scale[c] > 0.0f )
                    rotation[c] = glm::vec3( world[c] ) / scale[c];
            const glm::vec3 X( 1, 0, 0 ), Y( 0, 1, 0 ), Z( 0, 0, 1 );
            glm::vec3       ax, ay, az;
            float           major;
            if ( dims.y >= dims.x && dims.y >= dims.z )
                ax = Z, ay = X, az = Y, major = dims.y;
            else if ( dims.z >= dims.x )
                ax = X, ay = Y, az = Z, major = dims.z;
            else
                ax = Y, ay = Z, az = X, major = dims.x;
            ms.WarpGizmo = glm::mat4( glm::vec4( rotation * ax, 0.0f ), glm::vec4( rotation * ay, 0.0f ),
                                      glm::vec4( rotation * az, 0.0f ), world * glm::vec4( centre, 1.0f ) );
            ms.DeformWarp.UpperBound = 0.5f * major;
            ms.DeformWarp.LowerBound = -0.5f * major;
        }

        ECS::StaticMeshComponent* MeshOf( ::Desert::Core::Scene& scene, const Common::UUID& id )
        {
            auto ref = scene.FindEntityByID( id );
            if ( !ref || !ref->get().HasComponent<ECS::StaticMeshComponent>() )
                return nullptr;
            return &ref->get().GetComponent<ECS::StaticMeshComponent>();
        }
    } // namespace

    bool DeformTool::IsDeformTool( MS::Tool tool )
    {
        return tool == MS::Tool::Smooth || tool == MS::Tool::Offset || tool == MS::Tool::Warp;
    }

    void DeformTool::Update( ::Desert::Core::Scene& scene )
    {
        MS&        ms     = MS::Get();
        const bool active = IsDeformTool( ms.ActiveTool );

        // The session ends without Accept when its tool is no longer the active one (the bar's Cancel ends the
        // tool and raises ReqCancel, which was this tool's) or the selection moved to another entity.
        if ( m_Session )
        {
            const auto selected = Core::SelectionManager::GetSelected();
            if ( ms.ActiveTool != m_Session->Tool || !selected.has_value() || *selected != m_Session->Entity ||
                 MeshOf( scene, m_Session->Entity ) == nullptr )
            {
                if ( ms.ActiveTool != m_Session->Tool && ms.ReqCancel )
                    ms.ReqCancel = false;
                Restore( scene );
                m_Session.reset();
            }
        }
        if ( !active )
            return;

        if ( !m_Session )
            Begin( scene, ms );
        if ( m_Session && SettingsChanged( ms ) )
            Recompute( scene, ms );

        if ( ms.ReqAccept )
        {
            ms.ReqAccept         = false;
            const bool endTool   = ms.ReqAcceptEndsTool;
            ms.ReqAcceptEndsTool = false;
            if ( !Accept( scene, ms ) )
                return; // refused: the tool stays open with its preview, and says why
            if ( endTool )
                ms.ActiveTool = MS::Tool::None;
            else
                Begin( scene, ms ); // "Accept and Start New": the committed mesh is the next input
        }
    }

    void DeformTool::Begin( ::Desert::Core::Scene& scene, MS& ms )
    {
        m_Session.reset();
        const auto selected = Core::SelectionManager::GetSelected();
        if ( !selected.has_value() )
        {
            ms.DeformStatus = "Select an entity with a static mesh.";
            return;
        }
        ECS::StaticMeshComponent* component = MeshOf( scene, *selected );
        if ( component == nullptr )
        {
            ms.DeformStatus = "The selected entity has no static mesh.";
            return;
        }
        auto target = GetToolTargetMesh( *component );
        if ( !target.IsSuccess() )
        {
            ms.DeformStatus = target.GetError();
            return;
        }
        m_Session =
             Session{ ms.ActiveTool, *selected, target.GetValue().Mesh, target.GetValue().Committed, nullptr };
        if ( ms.ActiveTool == MS::Tool::Warp )
            PlaceWarpGizmo( *m_Session->Input, WorldOf( scene, *selected ), ms );
        Recompute( scene, ms );
    }

    void DeformTool::DrawWarpVisualization( const MS& ms, const glm::mat4& viewProj, const glm::vec2& viewportPos,
                                            const glm::vec2& viewportSize )
    {
        const Geometry::WarpSettings& warp = ms.DeformWarp;
        if ( warp.Operation != Geometry::WarpOperation::Bend || warp.UpperBound - warp.LowerBound <= 0.0f )
            return;
        constexpr int    kSegments = 48;
        ImDrawList*      dl        = ::ImGui::GetWindowDrawList();
        const glm::dmat4 gizmo( ms.WarpGizmo );
        ImVec2           prev{};
        bool             havePrev = false;
        for ( int i = 0; i <= kSegments; ++i )
        {
            const double     z    = warp.LowerBound + ( warp.UpperBound - warp.LowerBound ) * i / kSegments;
            const glm::dvec3 g    = Geometry::WarpBendGizmoPoint( warp, { 0.0, 0.0, z } );
            const glm::vec4  clip = viewProj * glm::vec4( glm::vec3( gizmo * glm::dvec4( g, 1.0 ) ), 1.0f );
            if ( clip.w <= 0.0001f )
            {
                havePrev = false;
                continue;
            }
            const ImVec2 p( viewportPos.x + ( clip.x / clip.w * 0.5f + 0.5f ) * viewportSize.x,
                            viewportPos.y + ( 0.5f - clip.y / clip.w * 0.5f ) * viewportSize.y );
            if ( havePrev )
                dl->AddLine( prev, p, IM_COL32( 80, 220, 255, 255 ), 2.5f );
            prev     = p;
            havePrev = true;
        }
    }

    void DeformTool::Recompute( ::Desert::Core::Scene& scene, MS& ms )
    {
        m_Smooth                                             = ms.DeformSmooth;
        m_Offset                                             = ms.DeformOffset;
        m_Warp                                               = ms.DeformWarp;
        m_WarpGizmo                                          = ms.WarpGizmo;
        const Geometry::DynamicMesh3&              input     = *m_Session->Input;
        Common::ResultStr<Geometry::DeformOutcome> out       = [&]
        {
            switch ( m_Session->Tool )
            {
                case MS::Tool::Smooth:
                    return Geometry::Smooth( input, ms.DeformSmooth );
                case MS::Tool::Offset:
                    return Geometry::Offset( input, ms.DeformOffset );
                default: // Warp: object space -> the gizmo's frame, as UE's FMeshSpaceDeformerOp::ObjectToGizmo
                    return Geometry::Warp( input, ms.DeformWarp,
                                           glm::inverse( glm::dmat4( ms.WarpGizmo ) ) *
                                                glm::dmat4( WorldOf( scene, m_Session->Entity ) ) );
            }
        }();
        ECS::StaticMeshComponent*                  component = MeshOf( scene, m_Session->Entity );
        if ( !out.IsSuccess() )
        {
            // A refused setting shows the input, not a stale preview.
            ms.DeformStatus = out.GetError();
            Restore( scene );
            m_Session->Preview.reset();
            return;
        }
        std::string report = out.GetValue().Report;
        auto preview = std::make_shared<const Geometry::DynamicMesh3>( std::move( out.ExtractValue().Mesh ) );
        if ( auto set = ECS::SetEditableMesh( *component, preview ); !set.IsSuccess() )
        {
            ms.DeformStatus = set.GetError();
            Restore( scene );
            m_Session->Preview.reset();
            return;
        }
        m_Session->Preview = std::move( preview );
        ms.DeformStatus    = std::move( report );
    }

    void DeformTool::Restore( ::Desert::Core::Scene& scene )
    {
        ECS::StaticMeshComponent* component = MeshOf( scene, m_Session->Entity );
        if ( component == nullptr )
            return;
        switch ( PlanMeshRestore( component->EditableMesh, m_Session->Committed ) )
        {
            case MeshRestore::Unchanged:
                break;
            case MeshRestore::Clear:
                ECS::ClearEditableMesh( *component );
                break;
            case MeshRestore::Set:
                if ( auto set = ECS::SetEditableMesh( *component, m_Session->Committed ); !set.IsSuccess() )
                    LOG_ERROR( "[Modeling] {0}: the mesh could not be put back: {1}", ToolName( m_Session->Tool ),
                               set.GetError() );
                break;
        }
    }

    bool DeformTool::Accept( ::Desert::Core::Scene& scene, MS& ms )
    {
        const auto refuse = [&]( const std::string& why )
        {
            LOG_ERROR( "[Modeling] {0} Accept refused: {1}", ToolName( ms.ActiveTool ), why );
            ToastManager::Push( std::format( "{} Accept refused: {}", ToolName( ms.ActiveTool ), why ),
                                ToastLevel::Error, 10.0f );
            return false;
        };
        if ( !m_Session )
            return refuse( ms.DeformStatus.empty() ? std::string( "no mesh is selected" ) : ms.DeformStatus );
        if ( !m_Session->Preview )
            return refuse( ms.DeformStatus );
        ECS::StaticMeshComponent* component = MeshOf( scene, m_Session->Entity );
        if ( component == nullptr || component->EditableMesh != m_Session->Preview )
            return refuse( "the entity no longer draws the preview" );
        Commands::RecordMeshChange( m_Session->Entity, std::format( "Modeling: {}", ToolName( m_Session->Tool ) ),
                                    m_Session->Committed );
        LOG_INFO( "[Modeling] {0} accepted: {1}", ToolName( m_Session->Tool ), ms.DeformStatus );
        m_Session.reset();
        return true;
    }

    bool DeformTool::SettingsChanged( const MS& ms ) const
    {
        switch ( m_Session->Tool )
        {
            case MS::Tool::Smooth:
                return !( ms.DeformSmooth == m_Smooth );
            case MS::Tool::Offset:
                return !( ms.DeformOffset == m_Offset );
            case MS::Tool::Warp:
                return !( ms.DeformWarp == m_Warp ) || ms.WarpGizmo != m_WarpGizmo;
            default:
                return false;
        }
    }
} // namespace Desert::Editor::Tools
