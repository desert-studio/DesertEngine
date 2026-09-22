#include "PickingController.hpp"

#include <Editor/Core/Selection/SelectionManager.hpp>

#include <Common/Core/Math/Ray.hpp>

#include <Engine/ECS/EntityLock.hpp>

namespace Desert::Editor::Tools
{
    const char* Describe( PickOutcome outcome )
    {
        // No `default:` on purpose — a new outcome must be named here or the build stops. A silent
        // "unknown outcome" string is the same defect this enum replaced, one level up.
        switch ( outcome )
        {
            case PickOutcome::NoCamera:
                return "no camera to pick through";
            case PickOutcome::RefusedGizmoHovered:
                return "refused: the cursor is over a gizmo handle";
            case PickOutcome::RefusedLocked:
                return "refused: that entity is locked (unlock it in the Outliner)";
            case PickOutcome::Selected:
                return "selected the entity under the cursor";
            case PickOutcome::Toggled:
                return "toggled the entity under the cursor in the selection";
            case PickOutcome::Missed:
                return "the ray hit nothing; the selection was kept";
            case PickOutcome::Cleared:
                return "the ray hit nothing; the selection was cleared";
        }
        return "unnamed outcome"; // unreachable: the switch above is exhaustive
    }

    PickOutcome PickingController::Pick( ::Desert::Core::Scene&                             scene,
                                         const std::shared_ptr<::Desert::Core::Camera>& camera,
                                         const glm::vec2& mouseViewport, const glm::vec2& viewportSize,
                                         bool gizmoHovered, bool additive )
    {
        if ( gizmoHovered )
            return PickOutcome::RefusedGizmoHovered;

        const auto& mainCamera = camera;
        if ( !mainCamera )
            return PickOutcome::NoCamera;

        const auto ray = Common::Math::Ray::FromScreenPosition(
             { mouseViewport.x, mouseViewport.y }, mainCamera->GetProjectionMatrix(),
             mainCamera->GetViewMatrix(), mainCamera->GetPosition(),
             static_cast<uint32_t>( viewportSize.x ), static_cast<uint32_t>( viewportSize.y ) );

        // Engine-owned ray cast vs scene meshes (shared with the foliage tool — one raycast, one resolution).
        ::Desert::Core::RaycastHit pick;
        if ( !scene.Raycast( ray, pick ) )
        {
            if ( !additive )
            {
                Core::SelectionManager::ClearSelection(); // click empty space = deselect
                return PickOutcome::Cleared;
            }
            return PickOutcome::Missed;
        }

        Common::UUID selectedUUID = pick.Entity;
        auto&        registry     = scene.GetRegistry();

        // If the hit entity is a child of a prefab, select the prefab ROOT so the whole prefab is outlined
        // instead of just one submesh.
        if ( auto hitEntityRef = scene.FindEntityByID( selectedUUID ) )
        {
            const ECS::Entity& hitEntity = hitEntityRef->get();
            if ( !hitEntity.HasComponent<ECS::PrefabComponent>() )
            {
                entt::entity current = hitEntity.GetHandle();
                while ( registry.has<ECS::RelationshipComponent>( current ) )
                {
                    const auto& rel = registry.get<ECS::RelationshipComponent>( current );
                    if ( rel.Parent == entt::null )
                        break;
                    current = rel.Parent;
                    if ( registry.has<ECS::PrefabComponent>( current ) &&
                         registry.has<ECS::UUIDComponent>( current ) )
                    {
                        selectedUUID = registry.get<ECS::UUIDComponent>( current ).UUID;
                        break;
                    }
                }
            }
        }

        // THE LOCK, and it is tested AFTER the prefab promotion above rather than before. Locking a prefab
        // root locks the whole instance, so the question to ask is about the entity that would ACTUALLY be
        // selected — asking it of the raw hit would let a click on a submesh of a locked prefab through
        // whenever that submesh had not been stamped itself.
        //
        // Refused, not silently ignored: the caller gets a name it can put in front of the user.
        if ( auto lockedRef = scene.FindEntityByID( selectedUUID ) )
            if ( ECS::IsLocked( registry, lockedRef->get().GetHandle() ) )
                return PickOutcome::RefusedLocked;

        if ( additive )
        {
            Core::SelectionManager::Toggle( selectedUUID );
            return PickOutcome::Toggled;
        }

        Core::SelectionManager::SetSelected( selectedUUID );
        return PickOutcome::Selected;
    }
} // namespace Desert::Editor::Tools
