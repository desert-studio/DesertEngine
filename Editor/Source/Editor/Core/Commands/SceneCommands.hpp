#pragma once

#include <Editor/Core/CommandHistory.hpp> // ICommand: RecordEditMeshChange takes one by unique_ptr
#include <Common/Core/ResultStr.hpp>
#include <Common/Core/UUID.hpp>

#include <glm/glm.hpp>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Desert::Core
{
    class Scene;
}
namespace Desert::Assets
{
    class AssetManager;
}
namespace Desert::Geometry
{
    class EditMesh;
}

namespace Desert::Editor::Commands
{
    // Undoable structural scene operations. Each helper PERFORMS the action (where there is one) and
    // records a UUID-addressed command in the CommandHistory, so entries survive anything except the
    // scene itself being replaced (EditorLayer clears the history on scene load / Play / Stop).
    //
    // Subtree snapshots reuse the prefab serialization path (EntitySerializer), so delete-undo and
    // duplication preserve every serializable component — same fidelity as saving the scene.

    // The scene/asset-manager the commands operate on. Set once by EditorLayer.
    void SetContext( ::Desert::Core::Scene* scene, const ::Desert::Assets::AssetManager* assetManager );

    // Record entities just created by UI code (Add menu, prefab instantiate, viewport drag-drop) as ONE
    // undo step. Undo deletes them (snapshotting first), redo restores them with the same UUIDs.
    void NotifyCreated( const std::vector<Common::UUID>& roots );

    // Destroy the entity (and subtree) undoably: undo restores it — same UUIDs, same parent.
    void DeleteEntity( const Common::UUID& uuid );

    // Multi-selection delete: drops entries whose ancestor is also in the list (the ancestor's delete
    // already covers them), destroys the rest as ONE undo step.
    void DeleteEntities( const std::vector<Common::UUID>& uuids );

    // Clone the entity subtree (fresh UUIDs, same parent, root tagged "<name> Copy").
    // Returns the duplicate's root UUID (Null on failure).
    Common::UUID DuplicateEntity( const Common::UUID& uuid );

    // Multi-selection duplicate (top-level roots only) as ONE undo step. Returns the new root UUIDs.
    std::vector<Common::UUID> DuplicateEntities( const std::vector<Common::UUID>& uuids );

    // Attach child to newParent (Null -> detach to scene root), recording the previous parent for undo.
    void Reparent( const Common::UUID& child, const Common::UUID& newParent );

    // Multi-selection reparent (top-level roots only) as ONE undo step.
    void ReparentMany( const std::vector<Common::UUID>& children, const Common::UUID& newParent );

    // Rename the entity's tag undoably. No-ops on empty/unchanged names.
    void Rename( const Common::UUID& uuid, const std::string& newName );

    // Record a finished edit of the entity's EditMesh (a PolyEdit drag): @p before is the mesh the component
    // held when the edit began; the component's CURRENT EditableMesh is the "after". One undo step; no-op when
    // the two are the same object. The meshes are immutable, so both are kept by reference. @p alongside is
    // undone / redone with the mesh as part of the SAME step (a mesh operation's selection change).
    void RecordEditMeshChange( const Common::UUID& uuid, const std::string& label,
                               std::shared_ptr<const Geometry::EditMesh> before,
                               std::unique_ptr<ICommand>                 alongside = nullptr );

    // Record a finished transform edit (gizmo drag): oldT/R/S = values before the drag; the entity's
    // CURRENT transform is captured as the "new" state. No-ops if nothing actually changed.
    void RecordTransformEdit( const Common::UUID& uuid, const glm::vec3& oldTranslation,
                              const glm::vec3& oldRotation, const glm::vec3& oldScale );

    // Pre-drag TRS of one entity (group gizmo drags capture one per selected root).
    struct TransformSnapshot
    {
        Common::UUID Entity;
        glm::vec3    Translation{ 0.0f };
        glm::vec3    Rotation{ 0.0f };
        glm::vec3    Scale{ 1.0f };
    };

    // Multi-entity variant: one undo step for the whole group drag (entities whose transform did not
    // actually change are skipped).
    void RecordTransformEdits( const std::vector<TransformSnapshot>& before );

    // ---- Editor clipboard (in-memory, serialized snapshots — survives deleting the originals) ----

    // Ctrl+C: snapshot the top-level roots of the given entities into the clipboard.
    void CopySelectionToClipboard( const std::vector<Common::UUID>& uuids );

    bool ClipboardHasContent();

    // Ctrl+V: instantiate the clipboard with fresh UUIDs (each root re-attaches to its original parent
    // when that parent still exists). One undo step; returns the pasted root UUIDs.
    std::vector<Common::UUID> PasteClipboard();

    // ---- Prefab instance tools ----

    // Overwrites the instance's source .deprefab file with the instance's CURRENT state (no undo — it is
    // a file operation; the caller shows a confirmation). Returns false when the entity is not a prefab
    // instance or its asset cannot be resolved.
    bool ApplyPrefabInstance( const Common::UUID& uuid );

    // Replaces the instance subtree with a fresh instantiation of its source prefab (same world position,
    // same parent). ONE undo step (undo brings the modified instance back). Returns the new root UUID.
    Common::UUID RevertPrefabInstance( const Common::UUID& uuid );

    /**
     * @brief Select every entity in the scene whose static mesh matches @p like's, @p like included.
     *
     * UE's "Select ▸ Matching", and here it is what makes a fold reachable at all: the collapse below
     * takes a SELECTION, and nothing in this editor could build a five-hundred-entity one without five
     * hundred ctrl-clicks — which is also a gesture no unattended run can make.
     *
     * THE MATCH IS THE FOLD'S OWN RULE (Commands::FoldMeshIdentity::SameAs), not a second one written
     * beside it: mesh, material slots and the shadow flag. A "select matching" that matched on less than
     * the fold requires would hand back a selection the fold then refuses, and the person would have no
     * way to see which member was the odd one.
     *
     * @return how many entities are now selected (0 = @p like carries no static mesh, or no scene).
     */
    size_t SelectMatchingStaticMeshes( const Common::UUID& like );

    // ---- Collapse a selection into ONE instanced draw (UE's "Merge Actors -> Instanced") ----

    /**
     * @brief Replace N identically-meshed entities with ONE carrying an InstancedStaticMeshComponent.
     *
     * THE POINT IS THE ENTITY COUNT, NOT THE DRAW COUNT. Auto-batching already folds repeated static
     * draws (50 179 entities of the world-scale scene collapse to ~25 draws) — but the ECS mesh walk
     * has already visited all 50 179 by the time it runs, and that walk is the frame's most expensive
     * line at ~55 ms against 22.6 ms of GPU. One ISM entity holding N world matrices is visited once.
     *
     * REFUSES RATHER THAN FOLDING WHAT FITS, and the reason is in Commands::PlanInstanceFold: this
     * DESTROYS its sources, so anything they carry that an ISM cannot (a script, a collider, children,
     * a forced LOD, a hidden submesh) would be gone with no message. The refusal names the entity and
     * the property.
     *
     * One undo step: undo brings every source back with its original UUID and removes the ISM.
     * @return the new entity's UUID, or the refusal.
     */
    [[nodiscard]] Common::ResultStr<Common::UUID>
    CollapseIntoInstancedMesh( const std::vector<Common::UUID>& uuids );

    // Runs `mutate` (a component add/remove from the Details panel) undoably: the entity subtree is
    // snapshotted before and after, and undo/redo swap between the two serialized states (delete +
    // recreate with preserved UUIDs — so selection and later history entries stay valid).
    void MutateEntityUndoable( const Common::UUID& uuid, const std::function<void()>& mutate );
} // namespace Desert::Editor::Commands
