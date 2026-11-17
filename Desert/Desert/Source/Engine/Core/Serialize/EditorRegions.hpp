#pragma once

// WHICH PART OF A PARTITIONED WORLD THE EDITOR HOLDS (WP19, owner's decision O3: a world opens in the editor
// unloaded and the user loads regions by hand).
//
// The pattern is UE's editor loader adapter (UWorldPartitionEditorLoaderAdapter, "Load Region from Selection" in
// the World Partition window), and not its letter: a region is a rectangle on the ground (X/Z, cm); what it loads
// is decided over the DESCRIPTOR INDEX (EntityDescriptorIndex.hpp), so nothing outside it is read:
//   - every composite (PlanWorldPartition) whose footprint meets a region is loaded WHOLE - a parent never
//     arrives without its children, a reference holder never without what it contains;
//   - every always-loaded composite is loaded whatever the regions (the sun, the sky, the player start);
//   - a composite with no place (an unplaced prefab instance or landscape tile) is loaded only by LoadWholeWorld.
// Changing the regions unloads what left them. An entity that differs from its file is not unloaded behind the
// user's back: the change names every such entity and changes nothing (UnsavedPolicy::Ask) until the user saves
// them or discards their changes (UE's save prompt before it unloads dirty actors, FEditorFileUtils::
// SaveDirtyPackages). Records the user PINNED are held whatever the regions (UE's pinned actors loader adapter,
// FLoaderAdapterPinnedActors - the Scene Outliner's Pinned column).
//
// The entities that stay on disk are NOT deleted: the scene's packages keep them as not loaded
// (EntityPackages::AdoptRegion), the header keeps listing them, and a save writes only loaded entities.

#include <Engine/Core/Serialize/WorldPartitionRules.hpp>

#include <Common/Core/ResultStr.hpp>
#include <Common/Core/UUID.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <span>
#include <vector>

namespace Desert::Assets
{
    class AssetManager;
}

namespace Desert::Core
{
    class Scene;
}

namespace Desert::Core::EditorRegions
{
    // What a set of regions selects of a world.
    struct RegionSelection
    {
        std::vector<std::uint64_t> Records;          // record ids to hold, in the world's (descriptor row) order
        std::size_t                InRegions    = 0; // composites whose footprint meets a region
        std::size_t                AlwaysLoaded = 0; // composites held whatever the regions
        std::size_t                Unplaced     = 0; // composites with no footprint: held by LoadWholeWorld only
        // Composites left on disk because every data layer they belong to is not loaded in the editor (UE
        // UDataLayerManager::ResolveIsLoadedInEditor) - wherever the regions are, always-loaded ones included.
        std::size_t UnloadedByDataLayers = 0;
        // Composites held only because one of their records is pinned (outside every region, not always loaded).
        std::size_t Pinned = 0;
    };

    // WHICH DATA LAYERS ONE USER LOADED OR UNLOADED IN THE EDITOR, against the world's own
    // DataLayerSerialized::InitiallyLoadedInEditor - UE UWorldPartitionEditorPerProjectUserSettings::
    // {LoadedDataLayers, NotLoadedDataLayers}, per user and per map, never in the world (two people editing one
    // world hold different layers). A layer named in neither list is at the world's initial state; a name the
    // world no longer states is ignored (it reaches no layer).
    struct DataLayerOverrides
    {
        std::vector<std::string> Loaded;    // loaded although the world's layer is not initially loaded
        std::vector<std::string> NotLoaded; // not loaded although the world's layer is initially loaded

        bool operator==( const DataLayerOverrides& ) const = default;
    };

    // PURE. Whether each of the world's layers (index = WorldPartitionSerialized::DataLayers) is loaded in the
    // editor under @p overrides: the world's InitiallyLoadedInEditor, unless the user's lists say otherwise.
    [[nodiscard]] std::vector<bool> DataLayersLoadedInEditor( const WorldPartitionSerialized& settings,
                                                              const DataLayerOverrides&       overrides );

    // @p overrides with the layer @p name set to @p loaded for this user: the name leaves both lists and joins the
    // one that differs from the world's initial state (none when it matches it), so the lists never restate the
    // world's default.
    [[nodiscard]] DataLayerOverrides WithDataLayerLoaded( const WorldPartitionSerialized& settings,
                                                          DataLayerOverrides overrides, const std::string& name,
                                                          bool loaded );

    // PURE. `plan` is PlanWorldPartition over the world's descriptors and `rowIds[r]` is the id of record `r`.
    // A region and a footprint meet when they share any point (edges included). `layerLoaded[l]` is whether the
    // world's data layer `l` is loaded in the editor (DataLayersLoadedInEditor): a composite that belongs to
    // layers is held only when ANY of them is loaded (UE ResolveIsLoadedInEditor); one in no layer is held by the
    // regions alone.
    // `pinned` are record ids the user pinned: a composite with a pinned member is held whole wherever the regions
    // are (UE's pinned actors loader adapter), unless its data layers are all unloaded.
    [[nodiscard]] RegionSelection SelectRecords( const Rules::WorldPartitionPlan&   plan,
                                                 std::span<const std::uint64_t>     rowIds,
                                                 std::span<const Rules::CellBounds> regions,
                                                 std::span<const bool>              layerLoaded,
                                                 std::span<const std::uint64_t>     pinned );

    // PURE. The whole world as Load All holds it: every record, but for the composites whose data layers are all
    // unloaded in the editor (as SelectRecords).
    [[nodiscard]] RegionSelection SelectWholeWorld( const Rules::WorldPartitionPlan& plan,
                                                    std::span<const std::uint64_t>   rowIds,
                                                    std::span<const bool>            layerLoaded );

    // What a region change does with an entity that differs from its file and would leave the loaded part.
    enum class UnsavedPolicy
    {
        Ask, // nothing changes: the outcome names every such entity (the editor's Save / Discard / Cancel prompt)
        Discard, // it is unloaded with its changes dropped - its file is what the world holds of it
    };

    // One entity a region change would drop the edits of: it differs from its file and would leave, or it was
    // deleted since the last save (Name: "<id> (deleted)").
    struct UnsavedEntity
    {
        Common::UUID Id;
        std::string  Name; // its Tag
    };

    // What a region change did.
    struct RegionOutcome
    {
        RegionSelection Selection;
        std::size_t     Loaded    = 0; // records read from their files and made
        std::size_t     Unloaded  = 0; // records whose entities were destroyed (their files kept)
        std::size_t     NotLoaded = 0; // records of the world the scene does not hold now
        // UnsavedPolicy::Ask: the entities that differ from their files and would leave, and those deleted since
        // the last save (EntityPackages::GoneSince). Not empty = NOTHING was loaded or unloaded; the caller asks
        // the user and calls again (Discard, or Ask after a save).
        std::vector<UnsavedEntity> Unsaved;
    };

    // THE EDITOR'S REGION LOAD: the scene holds exactly the records `regions` select of the world it was opened
    // from (its packages' baseline), read through the descriptor index (refreshed first, so an index older than
    // its files is rebuilt rather than used). Refused, changing nothing, naming why: the scene is not a
    // partitioned world opened from its files, the index or a file cannot be read. An entity to unload that
    // differs from its file is `policy`'s (UnsavedPolicy). `pinned` (HeldRegions::Pinned) are held as well.
    // `layers` is this user's data-layer state (HeldRegions::DataLayers): a composite whose layers are all
    // unloaded in the editor is not held, wherever the regions are.
    [[nodiscard]] Common::ResultStr<RegionOutcome>
    LoadRegions( Scene& scene, Assets::AssetManager* assets, std::span<const Rules::CellBounds> regions,
                 const DataLayerOverrides& layers, std::span<const std::uint64_t> pinned, UnsavedPolicy policy );

    // Every record of the world: the scene holds all of it again, as an open would have made it - but for the
    // composites whose data layers are all unloaded in the editor under `layers` (UE's Load All loads every region,
    // and an unloaded data layer stays unloaded).
    [[nodiscard]] Common::ResultStr<RegionOutcome> LoadWholeWorld( Scene& scene, Assets::AssetManager* assets,
                                                                   const DataLayerOverrides& layers,
                                                                   UnsavedPolicy             policy );

    // What of a world the editor holds besides its always-loaded part: the regions of its last region load, or
    // the whole world. UE keeps the same per user and per map (UWorldPartitionEditorPerProjectUserSettings::
    // EditorLoadedRegions) and loads it again when the map is opened; the editor's copy is EditorLoadedRegions.
    struct HeldRegions
    {
        std::vector<Rules::CellBounds> Regions; // X/Z, cm; empty: the always-loaded part only
        bool                           Whole = false;
        // This user's data-layer state (UE's LoadedDataLayers / NotLoadedDataLayers, kept beside the regions).
        DataLayerOverrides DataLayers;
        // The records this user pinned (UE's Scene Outliner Pinned column): held whatever the regions.
        std::vector<std::uint64_t> Pinned;
    };

    // What an editor open reads of the scene at a path.
    struct OpenRead
    {
        std::string               Text;             // the document ParseLoadableScene reads
        bool                      ByRegion = false; // a partitioned world: Text holds its always-loaded part only
        RegionSelection           Selection;        // ByRegion: what of the world Text holds
        std::vector<Common::UUID> NotLoaded;        // ByRegion: the records left on disk, in the world's order
    };

    // THE EDITOR'S OPEN (WP20/WP19-REST, owner's decision O3 - UE's World Partition editor opens a world with
    // its always-loaded actors and the regions the user had loaded): a partitioned world is read as its
    // always-loaded part plus what `held` selects (LoadRegions over held.Regions, or the whole world), decided
    // over the descriptor index before any record is read - a record outside the selection is never read.
    // Every other scene is ReadSceneFileText and `held` is not looked at. After the scene is made from Text and
    // adopted as saved, the caller hands NotLoaded to EntityPackages::AdoptRegion so a save keeps the records on
    // disk. Refused, naming the file, as ReadSceneFileText and LoadRegions refuse.
    [[nodiscard]] Common::ResultStr<OpenRead> ReadForOpen( const std::filesystem::path& path,
                                                           const HeldRegions&           held );

    // ONE RECORD OF THE WORLD THE SCENE DOES NOT HOLD, as the editor's outliner lists it (UE's FActorDescTreeItem:
    // an unloaded actor is listed from its descriptor, never by loading it).
    struct NotLoadedRow
    {
        std::uint64_t            Id     = 0;
        std::uint64_t            Parent = 0; // 0: the record states no parent
        std::string              Name;       // its Tag
        std::vector<std::string> Components; // its component keys, in file order: its type
        std::vector<std::string> DataLayers;
        // Where the plan streams its composite: "L<level> (<x>, <z>)" (and its grid past the first), "always
        // loaded", "no place" (an unplaced composite) or "not planned" (a record no composite lists).
        std::string Cell;
    };

    // The world of a partitioned scene opened from its files, as its descriptor index states it.
    struct WorldRows
    {
        std::vector<std::uint64_t> Records;   // every record of the world, in the world's order
        std::vector<NotLoadedRow>  NotLoaded; // the records the scene does not hold, in the world's order
    };

    // THE OUTLINER'S WORLD: every record of the world the scene was opened from (its packages' baseline) and a row
    // per record the scene does not hold, read off the descriptor index (refreshed first, as a region load reads
    // it) and planned as the cook plans it - no record's file is parsed. Refused, naming why: the scene is not a
    // partitioned world opened from its files, the index cannot be read.
    [[nodiscard]] Common::ResultStr<WorldRows> DescribeWorld( Scene& scene );

    // THE PLAYED WORLD OF A WORLD HELD IN PART (WP20). UE's Play-in-Editor plays the WHOLE partitioned world:
    // every actor the editor holds as it is in memory (unsaved edits included), every other one from its
    // package. Here: every record the scene does not hold (its packages' not-loaded set) is read from its file
    // and made beside what the scene holds, so the scene is the whole world as Play must stream it. The
    // packages are NOT told - the played scene is not the edited one; the caller keeps the edit world's packages
    // and puts them back with the edit world at Stop. Returns how many records were made (0: the scene already
    // holds the whole world, nothing is read). Refused, changing nothing, when the index or a file cannot be read.
    [[nodiscard]] Common::ResultStr<std::size_t> MakeNotLoadedForPlay( Scene&                scene,
                                                                       Assets::AssetManager* assets );
} // namespace Desert::Core::EditorRegions
