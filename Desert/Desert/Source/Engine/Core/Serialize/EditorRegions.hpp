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
//   - a composite with no place (an unplaced prefab instance or landscape tile) is loaded only by Load All
//     (HeldRegions::Whole), or by an entity list or a pin naming one of its records.
// Changing the regions unloads what left them. An entity that differs from its file is not unloaded: the change
// is refused, naming it, until it is saved (UE asks the same before it unloads a dirty actor).
//
// The entities that stay on disk are NOT deleted: the scene's packages keep them as not loaded
// (EntityPackages::AdoptRegion), the header keeps listing them, and a save writes only loaded entities.

#include <Engine/Core/Serialize/WorldPartitionRules.hpp>

#include <Common/Core/ResultStr.hpp>
#include <Common/Core/UUID.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
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
        std::size_t                Unplaced     = 0; // composites with no footprint: held by Whole or by entity
        std::size_t                ByEntities   = 0; // composites held by an entity list or a pin, no region
        // Composites left on disk because every data layer they belong to is not loaded in the editor (UE
        // UDataLayerManager::ResolveIsLoadedInEditor) - wherever the regions are, always-loaded ones included.
        std::size_t UnloadedByDataLayers = 0;
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
    // `entities` are record ids an entity-list loader or a pin holds: the composite of each is held whole.
    [[nodiscard]] RegionSelection SelectRecords( const Rules::WorldPartitionPlan&   plan,
                                                 std::span<const std::uint64_t>     rowIds,
                                                 std::span<const Rules::CellBounds> regions,
                                                 std::span<const std::uint64_t>     entities,
                                                 std::span<const bool>              layerLoaded );

    // PURE. The whole world as Load All holds it: every record, but for the composites whose data layers are all
    // unloaded in the editor (as SelectRecords).
    [[nodiscard]] RegionSelection SelectWholeWorld( const Rules::WorldPartitionPlan& plan,
                                                    std::span<const std::uint64_t>   rowIds,
                                                    std::span<const bool>            layerLoaded );

    // What a region change did.
    struct RegionOutcome
    {
        RegionSelection Selection;
        std::size_t     Loaded    = 0; // records read from their files and made
        std::size_t     Unloaded  = 0; // records whose entities were destroyed (their files kept)
        std::size_t     NotLoaded = 0; // records of the world the scene does not hold now
    };

    // THE EDITOR'S LOADERS (WP-LOADER-ADAPTERS) - UE IWorldPartitionActorLoaderInterface and its adapters
    // (UWorldPartitionEditorLoaderAdapter). What the editor holds of a world besides its always-loaded part is the
    // UNION of every loaded loader: a composite is held when any of them reaches it. Kept per user and per map (UE
    // UWorldPartitionEditorPerProjectUserSettings; the editor's copy is EditorLoadedRegions) and loaded again when
    // the map is opened.

    // A rectangle on the ground, X/Z cm (UE FLoaderAdapterShape, the map's Load Region); listed while switched off.
    struct RegionLoader
    {
        Rules::CellBounds Region;
        bool              Loaded = true;
    };

    // A Location Volume of the world by its entity id (UE ALocationVolume::GetLoaderAdapter): its box is the one its
    // saved record states (LocationVolumesOf).
    struct VolumeLoader
    {
        std::uint64_t Volume = 0;
        bool          Loaded = false;
    };

    // Entities by id (UE FLoaderAdapterActorList, Load Region from Selection): each one's whole composite is held.
    struct EntityListLoader
    {
        std::vector<std::uint64_t> Entities;
        bool                       Loaded = true;
    };

    struct HeldRegions
    {
        std::vector<RegionLoader>     Regions;
        std::vector<VolumeLoader>     Volumes; // the world's volumes with this user's state: WithWorldVolumes
        std::vector<EntityListLoader> EntityLists;
        // UE FLoaderAdapterPinnedActors: held whatever the other loaders are; Unload All keeps them.
        std::vector<std::uint64_t> Pinned;
        bool                       Whole = false; // Load All: every record, whatever the loaders
        // This user's data-layer state (UE's LoadedDataLayers / NotLoadedDataLayers, kept beside the loaders).
        DataLayerOverrides DataLayers;
    };

    // One Location Volume of a world, read from its descriptors: its name (the entity's tag), its X/Z box (the
    // rectangle around the +-|Extent| box placed by the entity's world transform - the parent chain composed as the
    // planner composes it, rotation and scale included) and its Auto Load.
    struct LocationVolume
    {
        std::uint64_t     Id = 0;
        std::string       Name;
        Rules::CellBounds Region;
        bool              AutoLoad = false;
    };

    // PURE. Every Location Volume of the world @p descriptors describe, in their order.
    [[nodiscard]] std::vector<LocationVolume>
    LocationVolumesOf( std::span<const Rules::EntityDescriptor> descriptors );

    // PURE. @p held with one VolumeLoader per volume of @p volumes, in the world's order: this user's state for a
    // volume it names, else loaded when the volume Auto Loads (UE bIsAutoLoad); a volume the world no longer has
    // leaves the list.
    [[nodiscard]] HeldRegions WithWorldVolumes( const HeldRegions& held, std::span<const LocationVolume> volumes );

    // What the loaded loaders of a HeldRegions reach: rectangles (regions, loaded volumes' boxes) and record ids
    // (entity lists, pins). A VolumeLoader naming no volume of @p volumes reaches nothing.
    struct HeldShape
    {
        std::vector<Rules::CellBounds> Regions;
        std::vector<std::uint64_t>     Entities;
    };
    [[nodiscard]] HeldShape ShapeOf( const HeldRegions& held, std::span<const LocationVolume> volumes );

    // PURE. Load Region from Selection: the records of every composite (always-loaded ones aside) whose footprint
    // meets the rectangle around the footprints of the composites holding @p selected, in row order. Empty when no
    // selected id is a record of the plan with a place.
    [[nodiscard]] std::vector<std::uint64_t> RecordsAroundSelection( const Rules::WorldPartitionPlan& plan,
                                                                     std::span<const std::uint64_t>   rowIds,
                                                                     std::span<const std::uint64_t>   selected );

    // PURE. Unload Selected (UE FLoaderAdapterActorList::RemoveActors): @p held with every record of each
    // composite holding one of @p selected (and each selected id itself) out of its entity lists; a list left
    // empty goes. Regions, volumes, pins and Whole are as they were: an entity one of them reaches stays held.
    [[nodiscard]] HeldRegions WithoutEntities( const HeldRegions& held, const Rules::WorldPartitionPlan& plan,
                                               std::span<const std::uint64_t> rowIds,
                                               std::span<const std::uint64_t> selected );

    // ONE ROW OF THE WORLD PARTITION WINDOW'S LOADER LIST (UE's World Partition editor lists its loader adapters)
    // and the name the palette's "Load / Unload / Remove loader <name>" entries address it by - so the window and
    // the palette cannot name a loader two ways.
    enum class LoaderKind : std::uint8_t
    {
        Whole,      // Load All, "All"; listed while it is on
        Volume,     // a Location Volume of the world, by its tag ("Location Volume <id>" when it has none)
        Region,     // a rectangle, "Region <n>"
        EntityList, // Load Region from Selection, "Selection <n>"
        Pinned,     // the pins, "Pinned"; listed while any entity is pinned
    };
    struct LoaderRow
    {
        LoaderKind  Kind = LoaderKind::Region;
        std::string Name;                     // unique among the rows: a repeated one gains " #<n>"
        std::size_t Index  = 0;               // Volume / Region / EntityList: the position in HeldRegions' list
        std::size_t Count  = 0;               // EntityList / Pinned: how many entity ids it names
        bool        Loaded = false;           // Pinned: always
        std::optional<Rules::CellBounds> Box; // Volume / Region: what it loads (a volume @p volumes lacks: none)
    };

    // PURE. The rows of @p held (with the world's volumes: WithWorldVolumes) in the order All, volumes, regions,
    // entity lists, pins.
    [[nodiscard]] std::vector<LoaderRow> LoaderRowsOf( const HeldRegions&              held,
                                                       std::span<const LocationVolume> volumes );

    // THE EDITOR'S LOAD: the scene holds exactly what @p held selects of the world it was opened from (its packages'
    // baseline) - every record when held.Whole, else the union of its loaded loaders, the world's Auto Load volumes
    // included (WithWorldVolumes) - read through the descriptor index (refreshed first, so an index older than its
    // files is rebuilt rather than used). A composite whose data layers are all unloaded in the editor under
    // held.DataLayers is not held, wherever the loaders are. Refused, changing nothing, naming why: the scene is not
    // a partitioned world opened from its files, the index or a file cannot be read, an entity to unload differs
    // from its file.
    [[nodiscard]] Common::ResultStr<RegionOutcome> LoadHeld( Scene& scene, Assets::AssetManager* assets,
                                                             const HeldRegions& held );

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
    // always-loaded part plus what `held` selects (LoadHeld), decided
    // over the descriptor index before any record is read - a record outside the selection is never read.
    // Every other scene is ReadSceneFileText and `held` is not looked at. After the scene is made from Text and
    // adopted as saved, the caller hands NotLoaded to EntityPackages::AdoptRegion so a save keeps the records on
    // disk. Refused, naming the file, as ReadSceneFileText and LoadHeld refuse.
    [[nodiscard]] Common::ResultStr<OpenRead> ReadForOpen( const std::filesystem::path& path,
                                                           const HeldRegions&           held );

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
