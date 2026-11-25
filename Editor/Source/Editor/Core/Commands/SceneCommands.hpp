#pragma once

#include <Editor/Core/CommandHistory.hpp> // ICommand: RecordMeshChange takes one by unique_ptr
#include <Common/Core/ResultStr.hpp>
#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/UUID.hpp>

#include <glm/glm.hpp>

#include <entt/entt.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
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
    class DynamicMesh3;
}

namespace Desert::ECS
{
    class Entity;
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
    void SetContext( ::Desert::Core::Scene* scene, ::Desert::Assets::AssetManager* assetManager );

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

    // Lock or unlock each root AND its subtree (ECS::SetLockedRecursive's rule) as ONE undo step. THE route of
    // every lock control - the Outliner's padlock and context menu, the Details padlock, the palette - so a lock
    // is a recorded entity edit like any other: it names the entities, and a partitioned save writes their files.
    void SetLocked( const std::vector<Common::UUID>& roots, bool locked );

    // Record a finished edit of the entity's editable mesh (UE's FMeshReplacementChange) (a PolyEdit drag): @p
    // before is the mesh the component held when the edit began; the component's CURRENT EditableMesh is the
    // "after". One undo step; no-op when the two are the same object. The meshes are immutable, so both are kept
    // by reference. @p alongside is undone / redone with the mesh as part of the SAME step (a mesh operation's
    // selection change).
    void RecordMeshChange( const Common::UUID& uuid, const std::string& label,
                           std::shared_ptr<const Geometry::DynamicMesh3> before,
                           std::unique_ptr<ICommand>                     alongside = nullptr );

    // Record a mesh operation that SPLIT the entity in two (Plane Cut, Keep Both Halves): the entity's CURRENT
    // EditableMesh is one half; a copy of the entity alone (not its children), named "<name> Half", is created
    // and given @p otherHalf. The mesh change, the copy and @p alongside are ONE undo step. Refused - nothing
    // left created or recorded - when the copy cannot be made or cannot take the mesh; the caller then puts
    // @p before back on the source.
    [[nodiscard]] Common::ResultStr<Common::UUID> RecordMeshSplit(
         const Common::UUID& uuid, const std::string& label, std::shared_ptr<const Geometry::DynamicMesh3> before,
         std::shared_ptr<const Geometry::DynamicMesh3> otherHalf, std::unique_ptr<ICommand> alongside = nullptr );

    // ---- XForm (Modeling): whole-entity mesh + transform edits as ONE undo step ----

    // What an entity holds after an XForm operation: its mesh (by reference, like MeshReplacementCommand), its
    // LOCAL transform, and - when set - its material slots (Merge unites the parts' slots) and its visibility.
    struct XformEntityState
    {
        Common::UUID                                    Entity;
        std::shared_ptr<const Geometry::DynamicMesh3>   Mesh;
        glm::dvec3                                      Translation{ 0.0 };
        glm::vec3                                       Rotation{ 0.0f };
        glm::vec3                                       Scale{ 1.0f };
        std::optional<std::vector<Common::AssetHandle>> MaterialSlots;
        // Set: the entity is shown or hidden (VisibilityComponent) - a Boolean's "Hide inputs".
        std::optional<bool> Visible;
    };

    // A new entity: a copy of CopyOf alone (not its children, same parent), named Name, then given State.
    struct XformNewEntity
    {
        Common::UUID     CopyOf;
        std::string      Name;
        XformEntityState State; // State.Entity is ignored
    };

    // Applies every change, creates every new entity and destroys every deleted one (with its subtree), and
    // records the lot as ONE undo step labelled @p label. Refused - everything already applied put back, nothing
    // recorded - when an entity is missing or a mesh cannot be set. Returns the new entities' UUIDs in order.
    [[nodiscard]] Common::ResultStr<std::vector<Common::UUID>>
    ApplyXformEdit( const std::string& label, const std::vector<XformEntityState>& changes,
                    const std::vector<XformNewEntity>& creates, const std::vector<Common::UUID>& deletes );

    // Record a finished transform edit (gizmo drag): oldT/R/S = values before the drag; the entity's
    // CURRENT transform is captured as the "new" state. No-ops if nothing actually changed.
    void RecordTransformEdit( const Common::UUID& uuid, const glm::dvec3& oldTranslation,
                              const glm::vec3& oldRotation, const glm::vec3& oldScale );

    // Pre-drag TRS of one entity (group gizmo drags capture one per selected root).
    struct TransformSnapshot
    {
        Common::UUID Entity;
        glm::dvec3   Translation{ 0.0 };
        glm::vec3    Rotation{ 0.0f };
        glm::vec3    Scale{ 1.0f };
    };

    // Multi-entity variant: one undo step for the whole group drag (entities whose transform did not
    // actually change are skipped). True when an entry was pushed.
    bool RecordTransformEdits( const std::vector<TransformSnapshot>& before );

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

    // ---- Prefab edit in place (UE 5.5 ULevelInstanceSubsystem::EditLevelInstance / CommitLevelInstance /
    //      CancelLevelInstanceEdit, with FLevelInstanceEditorMode's "the rest of the world is inactive") ----
    //
    // THE BODY IS EDITED, NOT THE OVERRIDES. Begin turns the chosen instance into the prefab's bare body (its own
    // overrides are set aside, so they are not baked into the file) standing where the instance stands; every
    // entity outside it is locked for authoring (IsAuthoringLocked) and dimmed in the Outliner. A nested instance
    // is edited at the level chosen: Begin on a nested root edits THAT prefab's file.
    //
    // Commit writes the edited body to the .deprefab and rebuilds every instance of that prefab in the scene - top
    // level or nested inside another prefab's instance - with the overrides each held when the edit began, so an
    // instance that had its own colour keeps it. ONE undo step: Undo puts back the file and every instance as they
    // were before Begin; the steps made inside the session are folded into it (CommandHistory::CloseSession).
    // Discard puts the edited instance back as it was before Begin and records nothing.
    //
    // Refused, naming why: no scene, a session already open, an entity that is not a prefab instance root, a
    // prefab that does not load, an instance holding entities its prefab does not have (a rebuild would drop
    // them), a file that is not written (the session stays open, nothing changed).
    [[nodiscard]] Common::BoolResultStr BeginPrefabEdit( const Common::UUID& instanceRoot );
    [[nodiscard]] Common::BoolResultStr CommitPrefabEdit();
    [[nodiscard]] Common::BoolResultStr DiscardPrefabEdit();

    // The root of the instance being edited; nullopt when no prefab edit is open.
    [[nodiscard]] std::optional<Common::UUID> PrefabEditRoot();

    // True while a prefab edit is open and @p entity of the edited scene's registry is not inside the edited
    // instance - the Outliner dims it.
    [[nodiscard]] bool IsOutsidePrefabEdit( const entt::registry& registry, entt::entity entity );

    // "May this entity be picked, moved or edited?" - the authoring lock (ECS::IsLocked) OR outside an open prefab
    // edit. The picker and the gizmo ask THIS; the Details panel disables its fields outside the edit (IsOutsidePrefabEdit).
    [[nodiscard]] bool IsAuthoringLocked( const entt::registry& registry, entt::entity entity );

    // Where a replacing instance's geometry comes from (see ReplaceWithPrefab).
    enum class ReplaceMeshSource
    {
        Prefab,         // UE's Replace Selected Actors with: the instance is exactly the prefab
        KeepEntityMesh, // the replaced entity's static mesh and material slots go onto the instance's root
    };

    /**
     * @brief UE's "Replace Selected Actors with": every entity in @p uuids becomes an instance of the prefab
     * at @p prefabFile, with the entity's parent, local translation, rotation, scale and name.
     *
     * THE OLD SUBTREE GOES, CHILDREN INCLUDED. A lamp whose light was authored as a child of its glass is
     * replaced by a prefab that carries its own light; keeping the old child would light the street twice.
     * (UE re-attaches the replaced actor's attached children instead; here a child of a mesh is part of what
     * the mesh was, and an entity that must survive is reparented before the replace.)
     *
     * KeepEntityMesh is for a kit whose every instance was baked as its own mesh (Bistro: 28 street-lamp
     * glasses, 28 meshes): the prefab supplies the fixture — its lights, its shadow flags — and each instance
     * keeps the geometry it stood for, saved as that instance's field-level override of the prefab's mesh.
     * Refused when the prefab's root carries no static mesh, since there would be nothing to keep it on.
     *
     * Entries whose ancestor is also listed are covered by that ancestor. Refused before anything changes
     * when the prefab cannot be loaded or an entity is missing; ONE undo step brings every entity back.
     * @return the new instance roots, in order; they become the selection.
     */
    [[nodiscard]] Common::ResultStr<std::vector<Common::UUID>>
    ReplaceWithPrefab( const std::vector<Common::UUID>& uuids, const std::filesystem::path& prefabFile,
                       ReplaceMeshSource mesh );

    // UE's FNewLevelInstanceParams::PivotType: where a prefab cut from a selection has its origin, which is where
    // its instance stands and its gizmo sits.
    enum class PrefabPivot
    {
        Center,      // the centre of the selection's box
        CenterMin,   // the centre of the box's base (Y is up)
        Entity,      // a chosen entity of the selection (UE's pivot actor)
        WorldOrigin, // the world's zero
    };

    // The pivot's name as the Outliner and the palette spell it.
    [[nodiscard]] std::string_view PrefabPivotLabel( PrefabPivot pivot );

    // A file in the project's Prefabs directory that no file holds yet, named after @p stem (UE's
    // IAssetTools::CreateUniqueAssetName: "<stem>", then "<stem>_1", "<stem>_2", ...).
    [[nodiscard]] std::filesystem::path UniquePrefabPath( std::string_view stem );

    /**
     * @brief UE's ULevelInstanceSubsystem::CreateLevelInstanceFrom: the selection becomes ONE new prefab and is
     * replaced by one instance of it, as ONE undo step.
     *
     * The prefab's root is a new entity named after the file, standing at the pivot; the selected entities
     * (top-level ones - a listed descendant is covered by its ancestor) hang under it with their subtrees, each
     * where it stood. A selected prefab instance stays a link (a nested prefab). The instance hangs under the
     * selection's shared parent, or at the scene root when the selection has none in common. No group entity is
     * made on the way: the new root exists only in the file (PrefabAsset::CreateFromEntities).
     *
     * The box behind Center / CenterMin is the one the new file states (PrefabAsset::BodyBounds). Entity takes
     * @p pivotEntity, which must be in the selection. Refused before anything changes when the file exists
     * already, when a selected entity sits inside a prefab instance (only an instance's root can leave it) or is a
     * UI element (a UI prefab is cut from its layout root with Save as Prefab), or when a box pivot has no box to
     * take. THE FILE STAYS after an undo, as UE leaves the level package the creation wrote (ConvertToStaticMesh's
     * rule).
     * @return the instance root, which becomes the selection.
     */
    [[nodiscard]] Common::ResultStr<Common::UUID>
    CreatePrefabFromSelection( const std::vector<Common::UUID>& uuids, const std::filesystem::path& prefabFile,
                               PrefabPivot pivot, const Common::UUID& pivotEntity );

    // How far a Break reaches (UE's BreakLevelInstance Levels: one, or all of them).
    enum class BreakDepth
    {
        OneLevel,  // the instance's own entities become plain; a prefab nested in it stays an instance, of the
                   // scene now
        AllLevels, // every instance nested under it is broken as well
    };

    /**
     * @brief UE's ULevelInstanceSubsystem::BreakLevelInstance: the instance at @p uuid stops being one - its root
     * loses the prefab link and its entities their record addresses, so the scene saves them as its own entities.
     * THE ONE ROUTE of every Break control (Outliner, Details, palette). One undo step puts every link back.
     * Refused when @p uuid is no prefab instance's root, or is the root of one nested inside another (break the
     * outer one first).
     * @return how many entities changed.
     */
    [[nodiscard]] Common::ResultStr<size_t> BreakPrefabInstance( const Common::UUID& uuid, BreakDepth depth );

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

    /**
     * @brief PACK / UNPACK PREFAB INSTANCES (UE: a Packed Level Actor, and "Edit" on it).
     *
     * Every prefab instance root among @p uuids that is not already in the asked mode is rebuilt in it under the
     * same UUID, parent and placement: packed, it draws its prefab's packed form (one ISM per mesh group,
     * Assets::PackPrefab); unpacked, every record is an entity again, ready to be edited in place and committed,
     * after which every packed instance of the prefab is rebuilt from the new form. Packing refuses an instance
     * whose prefab cannot pack (the packer's reason, naming the entity and what it carries) and one that holds
     * overrides (a packed instance draws its source as it is) - before anything is destroyed. An instance
     * nested inside another instance is refused (switch the outer one).
     *
     * One undo step, its own record: a packed instance is always rebuilt from its prefab (its component is
     * runtime-only, so no snapshot can bring one back), and the unpacked subtree it replaced is restored with its
     * ids. The switched instances are selected. @return how many instances changed mode, or why none did.
     */
    [[nodiscard]] Common::ResultStr<std::size_t> SetPrefabInstancesPacked( const std::vector<Common::UUID>& uuids,
                                                                           bool packed );

    // Select every instance root of the prefab @p like is an instance root of (UE: Select > All of the same
    // Level Instance). @return how many were selected, or why none could be.
    [[nodiscard]] Common::ResultStr<std::size_t> SelectPrefabInstancesLike( const Common::UUID& like );

    // Apply to Prefab from anywhere inside an unpacked instance: the nearest prefab root at or above @p inside is
    // written to its prefab (ApplyPrefabInstance), after which every packed instance of that prefab is rebuilt
    // from the new form (PrefabFactory::RefreshPackedInstances). @return the instance root applied.
    [[nodiscard]] Common::ResultStr<Common::UUID> ApplyEnclosingPrefabInstance( const Common::UUID& inside );

    // A tool that turns an entity into a DIFFERENT one (UE's Mesh To Volume: the static mesh actor becomes a
    // volume actor): @p source is removed with its subtree and a new root entity named @p name is created and
    // handed to @p build, as ONE undo step - undo brings the source back with its UUID and removes the new
    // entity. The new entity is selected. Returns its UUID, or why nothing was done.
    [[nodiscard]] Common::ResultStr<Common::UUID>
    ReplaceWithNewEntity( const Common::UUID& source, const std::string& name,
                          const std::function<void( ECS::Entity& )>& build );

    // THE ENTITY'S EDITMESH BECOMES A STATIC MESH ASSET (UE: Convert to Static Mesh / Output: New Static Mesh).
    // Writes a NEW .stmesh (StaticMeshOutput.hpp: under the cooked mesh folder, `folder` inside it, named
    // `name` or the entity's name, never overwriting), registers and loads it, points the component's
    // MeshHandle at it and REMOVES the editable mesh: the asset is now the one copy of the geometry, as a UE
    // StaticMeshActor holds a reference and no mesh of its own. Material slots are kept - the asset's
    // submesh k carries slot k's material, which is what the entity drew with.
    //
    // ONE undo step restores the editable-mesh entity. THE FILE STAYS after an undo, as UE leaves the package a
    // transaction created: redo needs it, another entity may already reference it, and deleting user content
    // from an undo is the one undo nobody can take back. Returns the path written.
    [[nodiscard]] Common::ResultStr<std::filesystem::path>
    ConvertToStaticMesh( const Common::UUID& uuid, std::string_view folder, std::string_view name );

    // The same conversion WITHOUT an undo step of its own, for a creating tool whose Accept records the
    // entity's creation (NotifyCreated) right after: the created state is then already the static mesh.
    [[nodiscard]] Common::ResultStr<std::filesystem::path>
    OutputStaticMesh( const Common::UUID& uuid, std::string_view folder, std::string_view name );

    // An edit that spans a whole tool session (Cube Grid reopened on an entity): the entity subtree as it was
    // when the session began. CommitEntityState makes "then -> now" ONE undo step (the same swap
    // MutateEntityUndoable records); RevertEntityState puts the entity back as it was and records nothing (the
    // session's Cancel). Null when there is no scene or no such entity.
    struct EntityStateSnapshot;
    [[nodiscard]] std::shared_ptr<const EntityStateSnapshot> CaptureEntityState( const Common::UUID& uuid );
    void CommitEntityState( const std::shared_ptr<const EntityStateSnapshot>& before );
    void RevertEntityState( const std::shared_ptr<const EntityStateSnapshot>& before );

    // Runs `mutate` (a component add/remove from the Details panel) undoably: the entity subtree is
    // snapshotted before and after, and undo/redo swap between the two serialized states (delete +
    // recreate with preserved UUIDs — so selection and later history entries stay valid).
    void MutateEntityUndoable( const Common::UUID& uuid, const std::function<void()>& mutate );

    // MutateEntityUndoable over several entities as ONE history entry named @p label: each top-level root's
    // subtree before and after @p mutate (UE FScopedTransaction around AddActorsToDataLayers).
    void MutateEntitiesUndoable( const std::vector<Common::UUID>& roots, const std::string& label,
                                 const std::function<void()>& mutate );

    // ONE MEMBER OF A COMPONENT THAT HAS NO REFLECTED BLOCK (Script: its slots are a list of variant values the
    // reflection cannot spell), set from the JSON its scene block holds for that member - UE Remote Control's
    // property set from JSON under one FScopedTransaction. The entity's block is written by its own
    // ComponentSerializer, @p member is replaced by @p json, and the block is read back by the same serializer
    // the scene load uses, so `set` accepts exactly what a scene file holds. One undo step (the Details panel's
    // MutateEntityUndoable); a value the loader refuses changes nothing and records nothing.
    [[nodiscard]] Common::BoolResultStr SetComponentBlockMember( const Common::UUID& uuid, std::string_view block,
                                                                 std::string_view member, std::string_view json );

    // UE's actor editor context (FActorEditorContext / IActorEditorContextClient): what the editor stamps on every
    // entity it creates for the user - added, placed, pasted or duplicated - BEFORE the creation is recorded, so
    // the one undo step holds it. The Data Layers outliner installs the current data layers
    // (DataLayerEditor::ApplyCurrentLayers); an empty function stamps nothing.
    using CreatedEntitiesContext =
         std::function<void( ::Desert::Core::Scene& scene, const std::vector<Common::UUID>& roots )>;
    void SetCreatedEntitiesContext( CreatedEntitiesContext context );
} // namespace Desert::Editor::Commands
