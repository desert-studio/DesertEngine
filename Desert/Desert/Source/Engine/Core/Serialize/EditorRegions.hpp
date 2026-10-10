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
// Changing the regions unloads what left them. An entity that differs from its file is not unloaded: the change
// is refused, naming it, until it is saved (UE asks the same before it unloads a dirty actor).
//
// The entities that stay on disk are NOT deleted: the scene's packages keep them as not loaded
// (EntityPackages::AdoptRegion), the header keeps listing them, and a save writes only loaded entities.

#include <Engine/Core/Serialize/WorldPartitionRules.hpp>

#include <Common/Core/ResultStr.hpp>

#include <cstddef>
#include <cstdint>
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
        std::vector<std::uint64_t> Records;      // record ids to hold, in the world's (descriptor row) order
        std::size_t                InRegions    = 0; // composites whose footprint meets a region
        std::size_t                AlwaysLoaded = 0; // composites held whatever the regions
        std::size_t                Unplaced     = 0; // composites with no footprint: held by LoadWholeWorld only
    };

    // PURE. `plan` is PlanWorldPartition over the world's descriptors and `rowIds[r]` is the id of record `r`.
    // A region and a footprint meet when they share any point (edges included).
    [[nodiscard]] RegionSelection SelectRecords( const Rules::WorldPartitionPlan& plan,
                                                 std::span<const std::uint64_t>   rowIds,
                                                 std::span<const Rules::CellBounds> regions );

    // What a region change did.
    struct RegionOutcome
    {
        RegionSelection Selection;
        std::size_t     Loaded    = 0; // records read from their files and made
        std::size_t     Unloaded  = 0; // records whose entities were destroyed (their files kept)
        std::size_t     NotLoaded = 0; // records of the world the scene does not hold now
    };

    // THE EDITOR'S REGION LOAD: the scene holds exactly the records `regions` select of the world it was opened
    // from (its packages' baseline), read through the descriptor index (refreshed first, so an index older than
    // its files is rebuilt rather than used). Refused, changing nothing, naming why: the scene is not a
    // partitioned world opened from its files, the index or a file cannot be read, an entity to unload differs
    // from its file.
    [[nodiscard]] Common::ResultStr<RegionOutcome> LoadRegions( Scene& scene, Assets::AssetManager* assets,
                                                                std::span<const Rules::CellBounds> regions );

    // Every record of the world: the scene holds all of it again, as an open would have made it.
    [[nodiscard]] Common::ResultStr<RegionOutcome> LoadWholeWorld( Scene& scene, Assets::AssetManager* assets );

    // THE PLAYED WORLD OF A WORLD HELD IN PART (WP20). UE's Play-in-Editor plays the WHOLE partitioned world:
    // every actor the editor holds as it is in memory (unsaved edits included), every other one from its
    // package. Here: every record the scene does not hold (its packages' not-loaded set) is read from its file
    // and made beside what the scene holds, so the scene is the whole world as Play must stream it. The
    // packages are NOT told - the played scene is not the edited one; the caller keeps the edit world's packages
    // and puts them back with the edit world at Stop. Returns how many records were made (0: the scene already
    // holds the whole world, nothing is read). Refused, changing nothing, when the index or a file cannot be read.
    [[nodiscard]] Common::ResultStr<std::size_t> MakeNotLoadedForPlay( Scene& scene, Assets::AssetManager* assets );
} // namespace Desert::Core::EditorRegions
