#include "MeshProcessingTool.hpp"

#include <Editor/Core/Commands/SceneCommands.hpp>
#include <Editor/Core/Selection/MeshBooleanTool.hpp>
#include <Editor/Core/Selection/ModelingToolTarget.hpp>
#include <Editor/Core/Selection/SelectionManager.hpp>
#include <Editor/Core/ToastManager.hpp>
#include <Editor/Panels/ViewportPanel/Tools/GizmoController.hpp>

#include <Common/Core/JobSystem.hpp>
#include <Common/Core/Logger.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/EditableMesh.hpp>
#include <Engine/ECS/Entity.hpp>
#include <Engine/Geometry/MeshDeformOperations.hpp>
#include <Engine/Geometry/MeshVoxelOperations.hpp>
#include <Engine/Geometry/MeshCore/DynamicMesh/DynamicMeshAttributeSet.hpp>

#include <ImGui/imgui.h>
#include <ImGuizmo.h>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <chrono>

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
                case MS::Tool::Lattice:
                    return "Lattice";
                case MS::Tool::Normals:
                    return "Normals";
                case MS::Tool::Tangents:
                    return "Tangents";
                case MS::Tool::VoxWrap:
                    return "VoxWrap";
                case MS::Tool::VoxBoolean:
                    return "VoxBoolean";
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

    bool MeshProcessingTool::IsMeshProcessingTool( MS::Tool tool )
    {
        return tool == MS::Tool::Smooth || tool == MS::Tool::Offset || tool == MS::Tool::Warp ||
               tool == MS::Tool::Lattice || tool == MS::Tool::Normals || tool == MS::Tool::Tangents ||
               tool == MS::Tool::VoxWrap || tool == MS::Tool::VoxBoolean;
    }

    bool MeshProcessingTool::SelectionStillMatches() const
    {
        if ( m_Session->Tool == MS::Tool::VoxBoolean )
        {
            const auto& selection = Core::SelectionManager::GetSelection();
            return selection.size() == 2 && selection[0] == m_Session->Entity && selection[1] == m_Session->Other;
        }
        const auto selected = Core::SelectionManager::GetSelected();
        return selected.has_value() && *selected == m_Session->Entity;
    }

    void MeshProcessingTool::Update( ::Desert::Core::Scene& scene )
    {
        MS&        ms     = MS::Get();
        const bool active = IsMeshProcessingTool( ms.ActiveTool );

        // The session ends without Accept when its tool is no longer the active one (the bar's Cancel ends the
        // tool and raises ReqCancel, which was this tool's) or the selection moved to another entity.
        if ( m_Session )
        {
            if ( ms.ActiveTool != m_Session->Tool || !SelectionStillMatches() ||
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
        if ( m_Session && m_Session->Lattice )
        {
            // UE: a resolution change rebuilds the lattice and drops its edits.
            const glm::ivec3 dims( ms.DeformLattice.XResolution, ms.DeformLattice.YResolution,
                                   ms.DeformLattice.ZResolution );
            if ( dims != m_Session->Lattice->Dims && !BuildLattice( ms ) )
            {
                Restore( scene );
                m_Session.reset();
                return;
            }
            ApplyLatticeGizmo( scene, ms );
        }
        // UE UMeshOpPreviewWithBackgroundCompute::InvalidateResult: a settings change cancels the compute in
        // flight (its result, even one already landed, is stale and never shown) and starts the next at once.
        // The preview on screen stays the last one that was current (UE keeps it while recomputing).
        if ( m_Session && SettingsChanged( ms ) )
        {
            if ( m_Session->Pending.valid() )
            {
                LOG_INFO( "[Modeling] {0}: settings changed, the compute running for {1:.1f} ms is cancelled",
                          ToolName( m_Session->Tool ),
                          std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() -
                                                                     m_Session->PendingStarted )
                               .count() );
                m_Session->PendingCancel.Raise();
                m_Session->Pending = {};
            }
            Recompute( scene, ms );
        }
        if ( m_Session && m_Session->Pending.valid() &&
             m_Session->Pending.wait_for( std::chrono::seconds( 0 ) ) == std::future_status::ready )
            ApplyResult( scene, ms );

        if ( ms.ReqAccept )
        {
            // Accept takes the result of the settings on the panel: it waits (the request stays raised) until
            // that compute has landed - UE disables Accept for the same reason (HaveValidResult).
            if ( m_Session && m_Session->Pending.valid() )
                return;
            ms.ReqAccept         = false;
            const bool endTool   = ms.ReqAcceptEndsTool;
            ms.ReqAcceptEndsTool = false;
            if ( !( m_Session && m_Session->Tool == MS::Tool::VoxBoolean ? AcceptCreateFromSelected( scene, ms )
                                                                         : Accept( scene, ms ) ) )
                return; // refused: the tool stays open with its preview, and says why
            if ( endTool )
                ms.ActiveTool = MS::Tool::None;
            else
                Begin( scene, ms ); // "Accept and Start New": the committed mesh is the next input
        }
    }

    void MeshProcessingTool::Begin( ::Desert::Core::Scene& scene, MS& ms )
    {
        m_Session.reset();
        if ( ms.ActiveTool == MS::Tool::VoxBoolean )
        {
            auto pair = Core::SelectedPairInSpaceOfA( scene, "VoxBoolean" );
            if ( !pair.IsSuccess() )
            {
                ms.DeformStatus = pair.GetError();
                return;
            }
            Core::SelectedMeshPair p = pair.ExtractValue();
            m_Session = Session{ ms.ActiveTool, p.A, std::move( p.MeshA ), std::move( p.CommittedA ), nullptr };
            m_Session->Other          = p.B;
            m_Session->OtherInput     = std::move( p.MeshBInA );
            m_Session->OtherCommitted = std::move( p.CommittedB );
            m_Session->NameA          = std::move( p.NameA );
            Recompute( scene, ms );
            return;
        }
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
        if ( ms.ActiveTool == MS::Tool::Lattice && !BuildLattice( ms ) )
        {
            m_Session.reset();
            return;
        }
        Recompute( scene, ms );
    }

    void MeshProcessingTool::DrawWarpVisualization( const MS& ms, const glm::mat4& viewProj, const glm::vec2& viewportPos,
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

    void MeshProcessingTool::Recompute( ::Desert::Core::Scene& scene, MS& ms )
    {
        m_Smooth        = ms.DeformSmooth;
        m_Offset        = ms.DeformOffset;
        m_VoxWrap       = ms.VoxWrap;
        m_VoxBoolean    = ms.VoxBoolean;
        m_Normals       = ms.AttribNormals;
        m_Tangents      = ms.AttribTangents;
        m_Warp          = ms.DeformWarp;
        m_WarpGizmo     = ms.WarpGizmo;
        m_Lattice       = ms.DeformLattice;
        m_LatticePoints = ms.LatticePoints;
        // Everything the operator reads is copied here, on the main thread: the worker never touches the scene
        // or the panel (UE's FDynamicMeshOperator gets the same snapshot from its factory, MakeNewOperator).
        // Warp: object space -> the gizmo's frame, as UE's FMeshSpaceDeformerOp::ObjectToGizmo.
        const glm::dmat4 objectToGizmo =
             m_Session->Tool == MS::Tool::Warp
                  ? glm::inverse( glm::dmat4( ms.WarpGizmo ) ) * glm::dmat4( WorldOf( scene, m_Session->Entity ) )
                  : glm::dmat4( 1.0 );
        std::optional<Geometry::FFDLattice> lattice = m_Session->Lattice;
        auto cancel                                 = std::make_shared<Geometry::ProgressCancel>();
        m_Session->PendingCancel                    = CancelOnDrop( cancel );
        m_Session->PendingStarted                   = std::chrono::steady_clock::now();
        m_Session->Pending                          = Common::JobSystem::Get().Async(
             [cancel = std::move( cancel ), tool = m_Session->Tool, input = m_Session->Input, smooth = m_Smooth,
              offset = m_Offset,
              voxWrap = m_VoxWrap, voxBoolean = m_VoxBoolean, other = m_Session->OtherInput, normals = m_Normals, tangents = m_Tangents, warp = m_Warp,
              latticeSettings = m_Lattice, points = m_LatticePoints, lattice = std::move( lattice ),
              objectToGizmo]() -> Common::ResultStr<Geometry::DeformOutcome>
             {
                 switch ( tool )
                 {
                     case MS::Tool::Smooth:
                         return Geometry::Smooth( *input, smooth );
                     case MS::Tool::Offset:
                         return Geometry::Offset( *input, offset );
                     case MS::Tool::Normals:
                         return Geometry::EditNormals( *input, normals );
                     case MS::Tool::Tangents:
                         return Geometry::ComputeTangents( *input, tangents );
                     case MS::Tool::VoxWrap:
                         return Geometry::VoxWrap( *input, voxWrap, cancel.get() );
                     case MS::Tool::VoxBoolean:
                         return Geometry::VoxBoolean( *input, *other, voxBoolean, cancel.get() );
                     case MS::Tool::Lattice:
                         return Geometry::LatticeDeform( *input, *lattice, points, latticeSettings );
                     default:
                         return Geometry::Warp( *input, warp, objectToGizmo );
                 }
             } );
        ms.DeformStatus = std::format( "{}: computing...", ToolName( m_Session->Tool ) );
    }

    void MeshProcessingTool::ApplyResult( ::Desert::Core::Scene& scene, MS& ms )
    {
        Common::ResultStr<Geometry::DeformOutcome> out = m_Session->Pending.get();
        LOG_INFO( "[Modeling] {0} computed in the background in {1:.1f} ms", ToolName( m_Session->Tool ),
                  std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() -
                                                             m_Session->PendingStarted )
                       .count() );
        ECS::StaticMeshComponent* component = MeshOf( scene, m_Session->Entity );
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

    void MeshProcessingTool::Restore( ::Desert::Core::Scene& scene )
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

    bool MeshProcessingTool::Accept( ::Desert::Core::Scene& scene, MS& ms )
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

    bool MeshProcessingTool::AcceptCreateFromSelected( ::Desert::Core::Scene& scene, MS& ms )
    {
        const auto refuse = [&]( const std::string& why )
        {
            LOG_ERROR( "[Modeling] VoxBoolean Accept refused: {0}", why );
            ToastManager::Push( std::format( "VoxBoolean Accept refused: {}", why ), ToastLevel::Error, 10.0f );
            return false;
        };
        if ( !m_Session || !m_Session->Preview )
            return refuse( ms.DeformStatus.empty() ? std::string( "select two entities with a mesh" )
                                                   : ms.DeformStatus );
        const auto refA = scene.FindEntityByID( m_Session->Entity );
        const auto refB = scene.FindEntityByID( m_Session->Other );
        if ( !refA || !refB )
            return refuse( "an input is no longer in the scene" );
        // A goes back to what it held; the result lands on a copy of A, in A's frame (the operator ran there).
        Restore( scene );
        const auto& tc = refA->get().GetComponent<ECS::TransformComponent>();
        Commands::XformEntityState state{ m_Session->Entity, m_Session->Preview, tc.Translation, tc.Rotation,
                                          tc.Scale,          std::nullopt,       std::nullopt };
        std::vector<Commands::XformEntityState> changes;
        std::vector<Commands::XformNewEntity>   creates{
             { m_Session->Entity, std::format( "{} VoxBoolean", m_Session->NameA ), std::move( state ) } };
        std::vector<Common::UUID>               deletes;
        const struct
        {
            Common::UUID                                  Id;
            ECS::Entity                                   Entity;
            std::shared_ptr<const Geometry::DynamicMesh3> Committed;
        } inputs[2] = { { m_Session->Entity, refA->get(), m_Session->Committed },
                        { m_Session->Other, refB->get(), m_Session->OtherCommitted } };
        for ( const auto& in : inputs )
        {
            if ( ms.VoxBooleanInputs == Core::BooleanInputs::Delete )
                deletes.push_back( in.Id );
            else if ( ms.VoxBooleanInputs == Core::BooleanInputs::Hide )
            {
                const auto& itc = in.Entity.GetComponent<ECS::TransformComponent>();
                changes.push_back(
                     { in.Id, in.Committed, itc.Translation, itc.Rotation, itc.Scale, std::nullopt, false } );
            }
        }
        const std::string label = std::format( "Modeling: VoxBoolean {}", Geometry::ToString( m_VoxBoolean.Operation ) );
        auto              applied = Commands::ApplyXformEdit( label, changes, creates, deletes );
        if ( !applied.IsSuccess() )
        {
            ECS::StaticMeshComponent* component = MeshOf( scene, m_Session->Entity );
            if ( component != nullptr )
                (void)ECS::SetEditableMesh( *component, m_Session->Preview ); // the preview stays on screen
            return refuse( applied.GetError() );
        }
        LOG_INFO( "[Modeling] {0} accepted as a new entity, inputs: {1}: {2}", label,
                  Core::ToString( ms.VoxBooleanInputs ), ms.DeformStatus );
        m_Session.reset();
        Core::SelectionManager::SetSelection( applied.GetValue() );
        return true;
    }

    bool MeshProcessingTool::BuildLattice( MS& ms )
    {
        auto lattice = Geometry::MakeFFDLattice( *m_Session->Input, ms.DeformLattice );
        if ( !lattice.IsSuccess() )
        {
            ms.DeformStatus = lattice.GetError();
            return false;
        }
        m_Session->Lattice = lattice.GetValue();
        ms.LatticePoints   = m_Session->Lattice->RestPoints();
        ms.LatticeSelection.clear();
        m_LatticeGizmo = ms.LatticeGizmo;
        return true;
    }

    void MeshProcessingTool::ApplyLatticeGizmo( ::Desert::Core::Scene& scene, MS& ms )
    {
        if ( ms.LatticeGizmo == m_LatticeGizmo )
            return;
        const glm::dmat4 world( WorldOf( scene, m_Session->Entity ) );
        if ( std::abs( glm::determinant( world ) ) > 1e-12 )
        {
            // The gizmo's motion this frame, in world space, applied to each selected point and taken back
            // into the mesh's object space.
            const glm::dmat4 motion = glm::inverse( world ) * glm::dmat4( ms.LatticeGizmo ) *
                                      glm::inverse( glm::dmat4( m_LatticeGizmo ) ) * world;
            for ( const int32_t index : ms.LatticeSelection )
                if ( index >= 0 && static_cast<size_t>( index ) < ms.LatticePoints.size() )
                    ms.LatticePoints[static_cast<size_t>( index )] =
                         glm::dvec3( motion * glm::dvec4( ms.LatticePoints[static_cast<size_t>( index )], 1.0 ) );
        }
        m_LatticeGizmo = ms.LatticeGizmo;
    }

    void MeshProcessingTool::PlaceLatticeGizmo( ::Desert::Core::Scene& scene, MS& ms )
    {
        const glm::dmat4 world( WorldOf( scene, m_Session->Entity ) );
        glm::dvec3       centroid( 0.0 );
        int32_t          count = 0;
        for ( const int32_t index : ms.LatticeSelection )
            if ( index >= 0 && static_cast<size_t>( index ) < ms.LatticePoints.size() )
            {
                centroid += glm::dvec3( world * glm::dvec4( ms.LatticePoints[static_cast<size_t>( index )], 1.0 ) );
                ++count;
            }
        if ( count > 0 )
            centroid /= static_cast<double>( count );
        ms.LatticeGizmo = glm::translate( glm::mat4( 1.0f ), glm::vec3( centroid ) );
        m_LatticeGizmo  = ms.LatticeGizmo;
    }

    void MeshProcessingTool::DrawLatticeControls( ::Desert::Core::Scene& scene, GizmoController& gizmo,
                                          const std::shared_ptr<::Desert::Core::Camera>& camera,
                                          const glm::mat4& viewProj, const glm::vec2& viewportPos,
                                          const glm::vec2& viewportSize, bool hovered )
    {
        if ( !m_Session || !m_Session->Lattice )
            return;
        MS&                     ms      = MS::Get();
        const Geometry::FFDLattice& lattice = *m_Session->Lattice;
        if ( static_cast<int32_t>( ms.LatticePoints.size() ) != lattice.PointCount() )
            return;
        const glm::dmat4    world( WorldOf( scene, m_Session->Entity ) );
        std::vector<ImVec2> screen( ms.LatticePoints.size() );
        std::vector<char>   visible( ms.LatticePoints.size(), 0 );
        for ( size_t i = 0; i < ms.LatticePoints.size(); ++i )
        {
            const glm::vec4 clip =
                 viewProj * glm::vec4( glm::vec3( world * glm::dvec4( ms.LatticePoints[i], 1.0 ) ), 1.0f );
            if ( clip.w <= 0.0001f )
                continue; // behind the camera
            screen[i]  = ImVec2( viewportPos.x + ( clip.x / clip.w * 0.5f + 0.5f ) * viewportSize.x,
                                 viewportPos.y + ( 0.5f - clip.y / clip.w * 0.5f ) * viewportSize.y );
            visible[i] = 1;
        }
        std::vector<char> selected( ms.LatticePoints.size(), 0 );
        for ( const int32_t index : ms.LatticeSelection )
            if ( index >= 0 && static_cast<size_t>( index ) < selected.size() )
                selected[static_cast<size_t>( index )] = 1;

        // UE draws the lattice's edges and its points; the selected points in the selection colour.
        ImDrawList*      dl   = ::ImGui::GetWindowDrawList();
        const glm::ivec3 dims = lattice.Dims;
        for ( int32_t k = 0; k < dims.z; ++k )
            for ( int32_t j = 0; j < dims.y; ++j )
                for ( int32_t i = 0; i < dims.x; ++i )
                {
                    const auto a = static_cast<size_t>( lattice.Index( i, j, k ) );
                    for ( const glm::ivec3 step : { glm::ivec3( 1, 0, 0 ), glm::ivec3( 0, 1, 0 ),
                                                   glm::ivec3( 0, 0, 1 ) } )
                    {
                        const glm::ivec3 n = glm::ivec3( i, j, k ) + step;
                        if ( n.x >= dims.x || n.y >= dims.y || n.z >= dims.z )
                            continue;
                        const auto b = static_cast<size_t>( lattice.Index( n.x, n.y, n.z ) );
                        if ( visible[a] && visible[b] )
                            dl->AddLine( screen[a], screen[b], IM_COL32( 230, 120, 40, 200 ), 1.5f );
                    }
                }
        for ( size_t i = 0; i < screen.size(); ++i )
        {
            if ( !visible[i] )
                continue;
            const float r = selected[i] ? 5.0f : 4.0f;
            dl->AddRectFilled( ImVec2( screen[i].x - r, screen[i].y - r ), ImVec2( screen[i].x + r, screen[i].y + r ),
                               selected[i] ? IM_COL32( 255, 255, 0, 255 ) : IM_COL32( 255, 0, 255, 255 ) );
        }

        // The gizmo on the selection (UE's mechanic: translate + rotate about the selection's centroid).
        bool gizmoBusy = false;
        if ( !ms.LatticeSelection.empty() )
        {
            // UE ULatticeControlPointsMechanic's gizmo translates, rotates and scales the selected points.
            gizmo.RenderFrame( "LatticeGizmo", ms.LatticeGizmo, camera, viewportPos, viewportSize,
                               GizmoController::kFrameTranslateRotateScale );
            gizmoBusy = ImGuizmo::IsOver() || ImGuizmo::IsUsing() || gizmo.ScaleHandleBusy();
        }
        const ImVec2 mouse = ::ImGui::GetMousePos();
        const bool   overImage = mouse.x >= viewportPos.x && mouse.y >= viewportPos.y &&
                               mouse.x < viewportPos.x + viewportSize.x && mouse.y < viewportPos.y + viewportSize.y;
        if ( !hovered || !overImage || gizmoBusy || !::ImGui::IsMouseClicked( ImGuiMouseButton_Left ) )
            return;
        int32_t hit  = -1;
        float   best = 81.0f; // 9 px
        for ( size_t i = 0; i < screen.size(); ++i )
        {
            if ( !visible[i] )
                continue;
            const float dx = screen[i].x - mouse.x, dy = screen[i].y - mouse.y;
            if ( dx * dx + dy * dy <= best )
            {
                best = dx * dx + dy * dy;
                hit  = static_cast<int32_t>( i );
            }
        }
        const ImGuiIO& io = ::ImGui::GetIO();
        auto&          sel = ms.LatticeSelection;
        const auto     it  = std::find( sel.begin(), sel.end(), hit );
        if ( hit < 0 )
        {
            if ( !io.KeyShift && !io.KeyCtrl )
                sel.clear();
        }
        else if ( io.KeyCtrl )
        {
            if ( it != sel.end() )
                sel.erase( it );
            else
                sel.push_back( hit );
        }
        else if ( io.KeyShift )
        {
            if ( it == sel.end() )
                sel.push_back( hit );
        }
        else
            sel = { hit };
        PlaceLatticeGizmo( scene, ms );
    }

    bool MeshProcessingTool::SettingsChanged( const MS& ms ) const
    {
        switch ( m_Session->Tool )
        {
            case MS::Tool::Smooth:
                return !( ms.DeformSmooth == m_Smooth );
            case MS::Tool::Offset:
                return !( ms.DeformOffset == m_Offset );
            case MS::Tool::Normals:
                return !( ms.AttribNormals == m_Normals );
            case MS::Tool::Tangents:
                return ms.AttribTangents.CalculationMethod != m_Tangents.CalculationMethod;
            case MS::Tool::VoxWrap:
                return !( ms.VoxWrap == m_VoxWrap );
            case MS::Tool::VoxBoolean:
                return !( ms.VoxBoolean == m_VoxBoolean );
            case MS::Tool::Warp:
                return !( ms.DeformWarp == m_Warp ) || ms.WarpGizmo != m_WarpGizmo;
            case MS::Tool::Lattice:
                return !( ms.DeformLattice == m_Lattice ) || ms.LatticePoints != m_LatticePoints;
            default:
                return false;
        }
    }
    // UE UMeshTangentsTool::UpdateVisualization: per triangle corner, the tangent (red) and bitangent (green)
    // lines, and the normal overlay's elements (blue), of the previewed mesh, LineLength long in world units.
    void MeshProcessingTool::DrawTangentsVisualization( ::Desert::Core::Scene& scene, const MS& ms,
                                                        const glm::mat4& viewProj, const glm::vec2& viewportPos,
                                                        const glm::vec2& viewportSize ) const
    {
        const Geometry::TangentsSettings& s    = ms.AttribTangents;
        const Geometry::DynamicMesh3&     mesh = *m_Session->Preview;
        if ( !mesh.HasAttributes() )
            return;
        const auto* attributes = mesh.Attributes();
        const auto* normals    = attributes->PrimaryNormals();
        const auto* tangents   = attributes->HasTangentSpace() ? attributes->PrimaryTangents() : nullptr;
        const auto* bitangents = attributes->HasTangentSpace() ? attributes->PrimaryBiTangents() : nullptr;
        const glm::mat4 world  = WorldOf( scene, m_Session->Entity );
        const glm::mat3 basis( world );
        ImDrawList*     dl      = ::ImGui::GetWindowDrawList();
        const auto      project = [&]( const glm::vec3& p, ImVec2& out )
        {
            const glm::vec4 clip = viewProj * glm::vec4( p, 1.0f );
            if ( clip.w <= 0.0001f )
                return false;
            out = ImVec2( viewportPos.x + ( clip.x / clip.w * 0.5f + 0.5f ) * viewportSize.x,
                          viewportPos.y + ( 0.5f - clip.y / clip.w * 0.5f ) * viewportSize.y );
            return true;
        };
        const auto line = [&]( const glm::vec3& origin, const glm::vec3& dir, ImU32 colour )
        {
            const float len = glm::length( basis * dir );
            if ( len <= 0.0f )
                return;
            ImVec2 a, b;
            if ( project( origin, a ) && project( origin + basis * dir / len * s.LineLength, b ) )
                dl->AddLine( a, b, colour, s.LineThickness );
        };
        for ( const int tid : mesh.TriangleIndicesItr() )
        {
            const Geometry::Index3i tri = mesh.GetTriangle( tid );
            for ( int k = 0; k < 3; ++k )
            {
                const glm::vec3 origin = glm::vec3( world * glm::vec4( glm::vec3( mesh.GetVertex( tri[k] ) ), 1.0f ) );
                if ( s.ShowTangents && tangents && tangents->IsSetTriangle( tid ) && bitangents->IsSetTriangle( tid ) )
                {
                    line( origin, tangents->GetElement( tangents->GetTriangle( tid )[k] ), IM_COL32( 240, 15, 15, 255 ) );
                    line( origin, bitangents->GetElement( bitangents->GetTriangle( tid )[k] ),
                          IM_COL32( 15, 240, 15, 255 ) );
                }
                if ( s.ShowNormals && normals && normals->IsSetTriangle( tid ) )
                    line( origin, normals->GetElement( normals->GetTriangle( tid )[k] ), IM_COL32( 15, 15, 240, 255 ) );
            }
        }
    }
} // namespace Desert::Editor::Tools
