#include "GizmoController.hpp"
#include "GizmoTransformMath.hpp"

#include <Editor/Core/Selection/SelectionManager.hpp>
#include <Editor/Core/Selection/SkeletonEditMode.hpp>
#include <Editor/Core/Commands/SceneCommands.hpp>
#include <Editor/Core/CommandHistory.hpp>

#include <memory>

#include <ImGui/imgui.h>

#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Geometry/SkinnedMesh.hpp>
#include <Engine/Animation/Skeleton.hpp>
#include <Engine/Animation/Animator.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/EntityLock.hpp>

#include <ImGuizmo.h>
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtc/quaternion.hpp>

#include <functional>
#include <vector>

namespace Desert::Editor::Tools
{
    namespace
    {
        // Undoable single-bone rest-pose edit. Re-resolves the skeleton via the mesh handle on each Undo/Redo,
        // so it stays valid across skeleton reloads (unlike a raw pointer into the bone array). The mesh
        // re-skins from LocalBindTransform every frame, so restoring the matrix is all that's needed.
        class BoneTransformCommand final : public ICommand
        {
        public:
            BoneTransformCommand( Assets::AssetHandle mesh, int bone, const glm::mat4& oldM,
                                  const glm::mat4& newM )
                 : m_Mesh( mesh ), m_Bone( bone ), m_Old( oldM ), m_New( newM )
            {
            }

            bool Undo() override
            {
                return Apply( m_Old );
            }
            bool Redo() override
            {
                return Apply( m_New );
            }

        private:
            bool Apply( const glm::mat4& m )
            {
                auto* base = Runtime::ResourceRegistry::GetMeshService()->Get( m_Mesh );
                if ( !base || !base->IsSkinned() )
                    return false;
                auto& bones = static_cast<SkinnedMesh*>( base )->GetSkeletonMutable()->GetBonesMutable();
                if ( m_Bone < 0 || m_Bone >= static_cast<int>( bones.size() ) )
                    return false;
                bones[m_Bone].LocalBindTransform = m;
                return true;
            }

            Assets::AssetHandle m_Mesh;
            int                 m_Bone;
            glm::mat4           m_Old, m_New;
        };
    } // namespace

    void GizmoController::RenderObject( ::Desert::Core::Scene& scene, const glm::vec2& viewportPos,
                                        const glm::vec2& viewportSize )
    {
        // NOTE: ImGuizmo::BeginFrame() is issued once per frame by EditorLayer, before any panel runs.
        const auto& mainCamera = scene.GetMainCamera().lock();
        if ( !mainCamera )
            return;

        const auto& selected = Core::SelectionManager::GetSelected();
        if ( !selected )
            return;

        const auto& selectedEntityOpt = scene.FindEntityByID( *selected );
        if ( !selectedEntityOpt )
            return;

        auto& selectedEntity = selectedEntityOpt->get();

        auto& transformComponent = selectedEntity.GetComponent<ECS::TransformComponent>();
        auto& reg                = scene.GetRegistry();

        // A LOCKED primary draws no gizmo at all, which is the whole second half of the lock: the picker
        // refuses to select a locked entity, but the OUTLINER deliberately still can — that is how you
        // reach the padlock to undo it — and a selection made there must not come with draggable handles.
        // Returning before SetRect/Manipulate also leaves m_Hovered false, so picking does not stand down
        // for a gizmo that is not on screen.
        if ( ECS::IsLocked( reg, selectedEntity.GetHandle() ) )
            return;

        // The gizmo must work in WORLD space. For a CHILD entity (e.g. a camera parented to the character),
        // the world transform = parentWorld * local, and an edit must be converted back to LOCAL before
        // writing. parentWorld = identity for a root entity (so this is a no-op there).
        auto parentWorldOf = [&reg]( entt::entity e ) -> glm::mat4
        {
            glm::mat4 world( 1.0f );
            if ( reg.has<ECS::RelationshipComponent>( e ) )
            {
                std::vector<entt::entity> chain; // [parent, grandparent, ... root]
                entt::entity              cur = reg.get<ECS::RelationshipComponent>( e ).Parent;
                while ( cur != entt::null )
                {
                    chain.push_back( cur );
                    cur = reg.has<ECS::RelationshipComponent>( cur )
                               ? reg.get<ECS::RelationshipComponent>( cur ).Parent
                               : entt::null;
                }
                for ( auto it = chain.rbegin(); it != chain.rend(); ++it ) // root -> ... -> parent
                    if ( reg.has<ECS::TransformComponent>( *it ) )
                        world = world * reg.get<ECS::TransformComponent>( *it ).GetTransform();
            }
            return world;
        };

        // An entity with a selected ANCESTOR is carried by that ancestor's transform already — the group
        // logic must skip it (else it would move twice).
        auto coveredBySelection = [&reg]( entt::entity e )
        {
            entt::entity cur = e;
            while ( reg.has<ECS::RelationshipComponent>( cur ) )
            {
                const auto parent = reg.get<ECS::RelationshipComponent>( cur ).Parent;
                if ( parent == entt::null )
                    break;
                cur = parent;
                if ( reg.has<ECS::UUIDComponent>( cur ) &&
                     Core::SelectionManager::IsSelected( reg.get<ECS::UUIDComponent>( cur ).UUID ) )
                    return true;
            }
            return false;
        };

        const glm::mat4 parentWorld = parentWorldOf( selectedEntity.GetHandle() );

        auto            modelMatrix    = parentWorld * transformComponent.GetTransform(); // world, for the gizmo
        const glm::mat4 oldModelMatrix = modelMatrix;

        // SetRect MUST match the rendered scene-image rect (content region), NOT the raw window rect.
        ImGuizmo::SetOrthographic( false );
        ImGuizmo::SetDrawlist();
        ImGuizmo::SetRect( viewportPos.x, viewportPos.y, viewportSize.x, viewportSize.y );

        const auto& view = mainCamera->GetViewMatrix();
        const auto& proj = mainCamera->GetProjectionMatrix();

        // Snap: grid for translate, fixed angles for rotate, increments for scale. Active when the
        // toolbar's magnet toggle is on OR Ctrl is held (Ctrl inverts the toggle).
        const auto  operation     = Core::GizmoState::Get();
        float       snapValues[3] = { 0.0f, 0.0f, 0.0f };
        const float snapUnit =
             ( operation == Core::GizmoState::Operation::Rotate )  ? Core::GizmoState::RotateSnapDegrees()
             : ( operation == Core::GizmoState::Operation::Scale ) ? Core::GizmoState::ScaleSnap()
                                                                   : Core::GizmoState::TranslateSnap();
        snapValues[0] = snapValues[1] = snapValues[2] = snapUnit;
        const float* snap = Core::GizmoState::SnapActive( ::ImGui::GetIO().KeyCtrl ) ? snapValues : nullptr;

        // World or Local, from the toolbar's toggle. EffectiveSpace() and not GetSpace(): ImGuizmo ignores
        // the mode for scaling, and the toolbar reads the same function, so the handles and the button
        // cannot disagree about which space is in force. Core::GizmoState::Space mirrors ImGuizmo::MODE.
        const auto space = static_cast<ImGuizmo::MODE>( Core::GizmoState::EffectiveSpace( operation ) );

        const bool manipulated =
             ImGuizmo::Manipulate( &view[0][0], &proj[0][0], static_cast<ImGuizmo::OPERATION>( operation ), space,
                                   &modelMatrix[0][0], nullptr, snap );

        // Picking must stand down whenever the cursor is OVER the gizmo — not only mid-drag. Setting this
        // only while manipulating meant a first click on a gizmo axis drawn over another mesh SELECTED
        // that mesh instead of starting the manipulation.
        if ( ImGuizmo::IsOver() || ImGuizmo::IsUsing() )
            m_Hovered = true;

        // One undo entry per drag: when the drag STARTS this frame, capture the pre-drag TRS of every
        // selected top-level root NOW — before any of this frame's deltas are written below.
        const bool usingNow = ImGuizmo::IsUsing();
        if ( usingNow && !m_DragActive )
        {
            m_DragActive = true;
            m_DragEntity = *selected;
            m_DragSnapshots.clear();
            for ( const auto& id : Core::SelectionManager::GetSelection() )
            {
                auto ref = scene.FindEntityByID( id );
                if ( !ref )
                    continue;
                ECS::Entity e = ref->get();
                if ( !e.HasComponent<ECS::TransformComponent>() || coveredBySelection( e.GetHandle() ) )
                    continue;
                // A locked follower is not moved below, so it must not be snapshotted either — an undo
                // entry for an entity that never changed is an undo step that appears to do nothing.
                if ( ECS::IsLocked( reg, e.GetHandle() ) )
                    continue;
                const auto& tc = e.GetComponent<ECS::TransformComponent>();
                m_DragSnapshots.push_back( { id, tc.Translation, tc.Rotation, tc.Scale } );
            }
        }

        if ( manipulated )
        {
            // Convert the manipulated WORLD matrix back to the entity's LOCAL space (inverse parent) before
            // decomposing — so dragging a child entity edits its local offset correctly. The composition
            // itself lives in GizmoTransformMath.hpp so the GizmoTransformSpace suite can reach it.
            const auto local = WorldToLocalTRS( parentWorld, modelMatrix );

            transformComponent.Translation = local.Translation;
            transformComponent.Rotation    = local.Rotation;
            transformComponent.Scale       = local.Scale;

            // Group manipulation: apply the same WORLD-space delta to every other selected top-level root,
            // so the whole selection moves/rotates/scales as one rigid group around the primary's gizmo.
            const auto& allSelected = Core::SelectionManager::GetSelection();
            if ( allSelected.size() > 1 )
            {
                const glm::mat4 delta = modelMatrix * glm::inverse( oldModelMatrix );
                for ( const auto& id : allSelected )
                {
                    if ( id == *selected )
                        continue;
                    auto ref = scene.FindEntityByID( id );
                    if ( !ref )
                        continue;
                    ECS::Entity e = ref->get();
                    if ( !e.HasComponent<ECS::TransformComponent>() || coveredBySelection( e.GetHandle() ) )
                        continue;
                    // The group delta stops at a locked member. Dragging five things of which one is
                    // locked moves four — the lock is a property of the entity, not of how it happened
                    // to be selected, so a multi-selection cannot be a way around it.
                    if ( ECS::IsLocked( reg, e.GetHandle() ) )
                        continue;

                    auto&           tc = e.GetComponent<ECS::TransformComponent>();
                    const glm::mat4 pw = parentWorldOf( e.GetHandle() );

                    const auto follower = WorldToLocalTRS( pw, delta * ( pw * tc.GetTransform() ) );
                    tc.Translation      = follower.Translation;
                    tc.Rotation         = follower.Rotation;
                    tc.Scale            = follower.Scale;
                }
            }
        }

        // Commit old->current for the whole group on release. The UUID guard drops the pending capture if
        // the selection changed mid-drag.
        if ( !usingNow && m_DragActive )
        {
            m_DragActive = false;
            if ( m_DragEntity == *selected )
                Commands::RecordTransformEdits( m_DragSnapshots );
            m_DragSnapshots.clear();
        }
    }

    void GizmoController::RenderBone( ::Desert::Core::Scene& scene, const glm::vec2& viewportPos,
                                      const glm::vec2& viewportSize )
    {
        const auto& mainCamera = scene.GetMainCamera().lock();
        if ( !mainCamera )
            return;

        const int boneIdx = Core::SkeletonEditMode::GetSelectedBone();
        if ( boneIdx < 0 )
            return;

        const auto& selected = Core::SelectionManager::GetSelected();
        if ( !selected )
            return;
        const auto& entOpt = scene.FindEntityByID( *selected );
        if ( !entOpt )
            return;
        auto& entity = entOpt->get();
        if ( !entity.HasComponent<ECS::SkinnedMeshComponent>() )
            return;

        auto& smc  = entity.GetComponent<ECS::SkinnedMeshComponent>();
        auto* mesh = Runtime::ResourceRegistry::GetMeshService()->Get( smc.MeshHandle );
        if ( !mesh || !mesh->IsSkinned() )
            return;
        auto* skeleton = static_cast<SkinnedMesh*>( mesh )->GetSkeletonMutable();
        auto& bones    = skeleton->GetBonesMutable();
        if ( boneIdx >= static_cast<int>( bones.size() ) )
            return;

        // Pose mode (Sequencer authoring): the gizmo edits the Animator's EDITABLE pose buffer instead of the
        // rig's bind pose, so posing to key a clip never mutates the shared rest pose. Falls back to bind
        // editing when there's no animator. See SkeletonEditMode::PoseMode / Animator::SetBoneLocalPose.
        Animation::Animator* animator = nullptr;
        if ( Core::SkeletonEditMode::PoseMode() && entity.HasComponent<ECS::AnimationComponent>() )
            animator = entity.GetComponent<ECS::AnimationComponent>().Animator.get();
        const bool usePose = ( animator != nullptr );

        // The local (parent-relative) transform to build the bone chain from: the animated pose in pose mode,
        // else the bind pose.
        const auto localOf = [&]( size_t i ) -> glm::mat4 {
            return usePose ? animator->GetBoneLocalPose( static_cast<uint32_t>( i ) )
                           : bones[i].LocalBindTransform;
        };

        const glm::mat4 entityWorld = entity.GetComponent<ECS::TransformComponent>().GetTransform();

        // Chain global per bone — the SAME space the mesh is skinned in, so the gizmo sits on the bone.
        std::vector<glm::mat4>             chainGlobal( bones.size(), glm::mat4( 1.0f ) );
        std::vector<bool>                  done( bones.size(), false );
        std::function<glm::mat4( size_t )> resolve = [&]( size_t i ) -> glm::mat4
        {
            if ( done[i] )
                return chainGlobal[i];
            glm::mat4 g = localOf( i );
            if ( bones[i].ParentBoneID.has_value() && bones[i].ParentBoneID.value() < bones.size() )
                g = resolve( bones[i].ParentBoneID.value() ) * localOf( i );
            chainGlobal[i] = g;
            done[i]        = true;
            return g;
        };

        glm::mat4 gizmoWorld = entityWorld * resolve( static_cast<size_t>( boneIdx ) );

        ImGuizmo::SetOrthographic( false );
        ImGuizmo::SetDrawlist();
        ImGuizmo::SetRect( viewportPos.x, viewportPos.y, viewportSize.x, viewportSize.y );

        const auto& view = mainCamera->GetViewMatrix();
        const auto& proj = mainCamera->GetProjectionMatrix();
        // Scale on a bone's rest pose is rarely wanted; default None/Scale to Translate.
        const auto op = ( Core::GizmoState::Get() == Operation::Rotate ) ? ImGuizmo::ROTATE : ImGuizmo::TRANSLATE;

        // The bone gizmo honours the SAME toolbar toggle as the object gizmo — one control, both gizmos,
        // or the button would mean different things depending on a mode the user is not looking at.
        // Asked with the operation this call actually passes (never Scale here), not with the toolbar's,
        // so the space in force matches the handles being drawn.
        const auto boneSpace = static_cast<ImGuizmo::MODE>( Core::GizmoState::EffectiveSpace(
             op == ImGuizmo::ROTATE ? Operation::Rotate : Operation::Translate ) );

        const bool boneManipulated =
             ImGuizmo::Manipulate( &view[0][0], &proj[0][0], op, boneSpace, &gizmoWorld[0][0] );

        // Same rule as the object gizmo: picking stands down on HOVER, not only mid-drag.
        if ( ImGuizmo::IsOver() || ImGuizmo::IsUsing() )
            m_Hovered = true;

        // One undo entry per drag: capture the bone's pre-drag rest transform when the drag STARTS, before
        // this frame's edit is written below. Rig editing only — pose-mode edits are persisted by keying.
        const bool usingNow = ImGuizmo::IsUsing();
        if ( !usePose && usingNow && !m_BoneDragActive )
        {
            m_BoneDragActive = true;
            m_BoneDragIndex  = boneIdx;
            m_BoneDragMesh   = smc.MeshHandle;
            m_BoneDragOld    = bones[boneIdx].LocalBindTransform;
        }

        if ( boneManipulated )
        {
            // The new local (parent-relative) transform from the gizmo's world matrix.
            const glm::mat4 newGlobalMesh = glm::inverse( entityWorld ) * gizmoWorld;
            glm::mat4       parentGlobal( 1.0f );
            const auto&     parentBone = bones[boneIdx].ParentBoneID;
            if ( parentBone.has_value() && *parentBone < bones.size() )
                parentGlobal = resolve( *parentBone );
            const glm::mat4 newLocal = glm::inverse( parentGlobal ) * newGlobalMesh;

            if ( usePose )
            {
                // Pose the ANIMATED buffer (never the bind pose) and re-render it so the viewport updates.
                animator->SetBoneLocalPose( static_cast<uint32_t>( boneIdx ), newLocal );
                animator->ApplyLocalPose();
            }
            else
            {
                bones[boneIdx].LocalBindTransform = newLocal; // rig / rest-pose editing
            }
        }

        // On release: record the net rig change (old -> current) as one undoable command (rig editing only).
        if ( !usePose && !usingNow && m_BoneDragActive )
        {
            m_BoneDragActive = false;
            if ( m_BoneDragIndex == boneIdx && bones[boneIdx].LocalBindTransform != m_BoneDragOld )
                CommandHistory::Get().PushCommand( std::make_unique<BoneTransformCommand>(
                     m_BoneDragMesh, boneIdx, m_BoneDragOld, bones[boneIdx].LocalBindTransform ) );
        }
    }
} // namespace Desert::Editor::Tools
