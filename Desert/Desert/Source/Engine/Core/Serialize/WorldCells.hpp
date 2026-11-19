#pragma once

// A PARTITIONED WORLD, COOKED: ONE FILE PER CELL AND ONE INDEX THAT SAYS WHAT IS IN EACH (WP8, analysis A2).
//
// The source of a world is the .desce its author saves — one file, every record. A runtime that streams the
// world must not read that file to find a cell, so the cook splits it: every residency unit's records go to a
// file of their own, and an index states everything the streamer decides with — the units, their levels and
// squares, which records (by id) each holds, the references that cross a unit boundary, and the assets each
// unit needs — so that deciding never reads a cell. That is the pattern of UE's cooked runtime cells and their
// actor descriptors (UWorldPartitionRuntimeCell + the ActorDescContainer's StreamingGeneration); the pattern
// and not the format.
//
// ── THE RECORDS ARE THE SCENE'S OWN, NOT A NEW FORMAT ────────────────────────────────────────────
//
// A cell file's records are Assets::EntityData written by the same rfl writer the saver uses: InstantiateRecords
// makes a cell out of them exactly as it makes a whole scene, and nothing about a record is restated here. What
// the cook adds is the envelope (below) and the split.
//
// ── THE SPLIT IS THE PLANNER'S, AND THE ORDER IS ResidencyUnitMembers ─────────────────────────────
//
// Units are WorldPartitionPlan's residency units (always-loaded composites first, then cells), and a unit's
// records are in ResidencyUnitMembers order: the order the executor names them in. All always-loaded units
// share ONE file (they are all resident from the first frame, so splitting them would only add opens); each
// cell has its own, named by level and coordinate, so a cell's file name does not depend on what else the
// world holds.
//
// ── DETERMINISM AND LOCALITY ─────────────────────────────────────────────────────────────────────
//
// No clock, no path, no machine enters a byte: two cooks of one source are identical, byte for byte. A cell
// file depends on nothing but its own records, so editing one record changes that record's cell file and the
// index (which carries the file's checksum) and nothing else — what makes a re-cook of a large world an upload
// of two files.
//
// ── THE HLOD OF EVERY CELL (WP10) ────────────────────────────────────────────────────────────────
//
// Beside each cell that draws something the cook writes that cell's Instancing HLOD
// (WorldPartitionHLODRules.hpp): the cell's meshes as InstancedStaticMesh records in a file of its own,
// `HLOD_<cell file>`, listed in the index (WorldIndex::HLODs) with the cell unit it stands in for, the records it
// covers, the ones it could not cover and why, and its asset closure. It is a cell file in every other respect
// (same envelope, same checksum rule); its records belong to no unit, so assembling the world never reads it.
// Which one is DRAWN, the cell or its HLOD, is WP11's; this is only the builder, as UE's HLOD builder is.
//
// ── THE HLOD CELLS OF A SPATIALLY LOADED LAYER (WP-FAR-8) ─────────────────────────────────────────
//
// When an HLOD layer is spatially loaded (CellSize, LoadingRange) the HLODs are not one per world cell resident
// for the session: they are the nodes of the hierarchy (WorldPartitionHLODHierarchy.hpp), and the cook packs
// them as UE cooks the HLOD actors of a layer with bIsSpatiallyLoaded - into runtime cells of the layer's own
// grid. Every HLOD residency unit (Rules::MakeHLODResidency: the nodes resident the whole session, then the HLOD
// cells) is an ordinary unit of the index (WorldIndex::HLODUnits) with a file of its own holding its nodes'
// records, its squares, its record ids and its asset closure; WorldIndex::HLODNodes states each node's records.
// The HLOD cells' files go to region chunks as the world's cells' do, and the streamer reads them through the
// same cell source, the same loader and a second residency with the HLOD gate (WorldStreamer.hpp).
//
// ── THE ENVELOPE: THE SHARED ASSET ENVELOPE (AF1), CHECKED BEFORE THE CONTENT IS BELIEVED ──────────
//
// Every file is a Common/Content/AssetEnvelope: kind WorldIndex (the index) or WorldCell (a cell file), a GUID
// derived from the world's name and the file's name (so a re-cook reproduces it), the world format version under
// the subsystem tag kWorldFormatTag, and one Payload section holding the JSON below, unchanged. As in UE, where
// a cooked cell is an ordinary level package, a cooked cell is an ordinary asset file: its header answers what
// it is without its body being read (ReadEnvelopeHeader). The index also carries every file's size and
// checksum, so a cell that is intact on its own but belongs to ANOTHER cook of the world (a stale file left in
// the directory) is refused too. Every refusal names the file.

#include <Engine/Core/Serialize/PrefabExpansion.hpp>
#include <Engine/Core/Serialize/SceneFormat.hpp>
#include <Engine/Core/Serialize/WorldCellSource.hpp>
#include <Engine/Core/Serialize/WorldPartitionHLODHierarchy.hpp>
#include <Engine/Core/Serialize/WorldPartitionHLODBuilders.hpp>
#include <Engine/Core/Serialize/WorldPartitionHLODRules.hpp>
#include <Engine/Core/Serialize/WorldPartitionRules.hpp>

#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Core/ResultStr.hpp>
#include <Common/Json/Json.hpp>
#include <Common/Utilities/AssetRegistry.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Desert::Core::WorldCells
{
    // The world format's entry in an envelope's subsystem table. The version is raised with any change to the
    // index/cell payload shapes below. A reader refuses any other number by name, older included; a cooked
    // world is re-derivable, so the answer to an old one is a re-cook.
    inline constexpr std::uint32_t kWorldFormatTag     = Common::Content::FourCC( "WPCW" );
    inline constexpr std::uint32_t kWorldFormatVersion =
         11; // 4: HLOD layers (WP-FAR-7); 5: a cell row states its grid (WP22);
            // 6: a cell row states its streaming bounds (WP-FAR-6B 9g); 7: prefab instances expanded at cook
            // (WP21); 8: a cell row states the height span of what it holds (WP-SRC-ALTITUDE); 9: a unit row states
            // its runtime data layers, the cell's third coordinate, and a cell or always-loaded composite of runtime
            // layers has a file of its own, `..._DL<hash>.dwcell` (WP-DATA-LAYERS-RUNTIME); 10: the HLOD cells of a
            // spatially loaded layer are units of the index (HLODUnits, HLODNodes; WP-FAR-8); 11: the crossing
            // references are every one Rules::ResolveEntityReferences finds - a script's Entity property ("Script",
            // "<script key>:<property>") and a reference by name besides the register's - all of them observation,
            // which residency rebinds (WP-ENTITY-REFS); an index of 10 lacks them, so a runtime would never rebind them

    inline constexpr std::string_view kIndexFileName        = "World.dwindex";
    inline constexpr std::string_view kCellExtension        = ".dwcell";
    inline constexpr std::string_view kAlwaysLoadedFileName = "AlwaysLoaded.dwcell";
    // The `Reason` of an HLOD unit resident the whole session: the top of a chain, whose layer is not spatially
    // loaded (WorldIndex::HLODUnits).
    inline constexpr std::string_view kHLODResidentReason = "HLODLayerNotSpatiallyLoaded";

    // ── The index ────────────────────────────────────────────────────────────────────────────────

    // One residency unit. `Level`/`X`/`Z`/`Square`/`Bounds` only for a cell; `Reason` only for an always-loaded
    // unit.
    struct IndexUnit
    {
        std::string                      Name; // Rules::DescribeResidencyUnit
        std::string                      File;
        std::optional<std::size_t>       Grid; // index into WorldPartition.RuntimePartitions (WP22)
        std::optional<int>               Level;
        std::optional<std::int32_t>      X;
        std::optional<std::int32_t>      Z;
        std::optional<Rules::CellBounds> Square;
        std::optional<Rules::CellBounds>
             Bounds; // Rules::PlannedCell::Bounds: what the loading range is measured to
        std::optional<Rules::HeightSpan> Height; // Rules::PlannedCell::Height: what a source's altitude is measured to
        std::optional<std::string>       Reason;
        // Rules::PlannedCell::DataLayers of a cell, the runtime layers of an always-loaded composite (indices into
        // WorldPartition.DataLayers, ascending; empty = none): what residency gates the unit by (WP v3).
        std::vector<std::size_t>         DataLayers;
        std::vector<std::uint64_t>       Ids;    // record ids, in ResidencyUnitMembers order
        std::vector<std::string>         Assets; // registry keys the unit needs, transitively; sorted
    };

    struct IndexFile
    {
        std::string   Name;
        std::uint64_t Bytes = 0;
        std::uint32_t Crc   = 0; // Crc32c of the whole file, envelope included
    };

    // A reference whose two ends are in different units (Rules::ResolveEntityReferences: the register's, a script's
    // Entity property, a name). Only those: within a unit both ends are always resident together, so only a crossing
    // one can be deferred, rebound or unbound. Always an observation: the cook refuses a crossing containment.
    // `Component`/`Field` are Rules::ResolvedReference's (a script's: "Script", "<script key>:<property>").
    struct IndexReference
    {
        std::uint64_t From     = 0;
        std::uint64_t To       = 0;
        std::uint32_t FromUnit = 0;
        std::uint32_t ToUnit   = 0;
        std::string   Component;
        std::string   Field;
    };

    // A record of a cell that draws something and has no instance in the cell's HLOD, and why (the closed
    // list in WorldPartitionHLODRules.hpp): what WP11 must treat as a hole when the cell unloads.
    struct IndexNotInstanced
    {
        std::uint64_t        Id     = 0;
        Rules::HLODExclusion Reason = Rules::HLODExclusion::Unreadable;
    };

    // THE INSTANCING HLOD OF ONE CELL (WP10): the InstancedStaticMesh records that stand in for cell unit
    // `Unit` while it is not loaded. One per cell that draws anything. `File` is absent when nothing in the
    // cell could be batched (then every drawing record is in NotInstanced). The HLOD's records are not the
    // world's: no unit holds them, `Records` does not count them, AssembleWorld never reads them.
    struct IndexHLOD
    {
        std::uint32_t                  Unit = 0;
        std::optional<std::string>     File;
        std::vector<std::uint64_t>     Ids; // the HLOD's own records, one per batch, in batch order
        std::uint64_t                  Instances = 0;
        std::vector<std::uint64_t>     Sources; // the cell's records it stands in for, in member order
        std::vector<IndexNotInstanced> NotInstanced;
        std::vector<std::string>       Assets; // registry keys the HLOD needs, transitively; sorted
    };

    // ONE NODE OF A STREAMED HLOD HIERARCHY (WP-FAR-8): Rules::HLODNode of PlanHLODHierarchy over the cooked plan,
    // in its node order, with what Build HLODs saved for it. Its records are a run of its unit's file, in the
    // unit's node order.
    struct IndexHLODNode
    {
        std::string                    Name;     // Rules::HLODNode::Name: the key Build HLODs saved it by
        std::uint32_t                  Unit = 0; // index into WorldIndex::HLODUnits: the unit whose file holds it
        std::vector<std::uint64_t>     Ids;      // its records, one per batch; empty when it batched nothing
        std::uint64_t                  Instances = 0;
        std::vector<std::uint64_t>     Sources; // the world's records it stands in for, in member order
        std::vector<IndexNotInstanced> NotInstanced;
    };

    struct WorldIndex
    {
        // The scene-wide part of the source, so a world is whole without the .desce it came from.
        std::string                 SceneName;
        std::optional<Common::Json::Value> Settings;
        // The .desce's text header, verbatim: the scene's GUID and its SCNE/UNIT generations (scene v26).
        std::optional<Common::Content::TextAssetHeaderSerialized> Header;
        WorldPartitionSerialized    WorldPartition;

        int           LevelCount = 0;
        std::uint64_t Records    = 0;
        // False when the cook had no asset registry: every unit's `Assets` is then empty because it is
        // UNKNOWN, not because the unit needs nothing — and the plan placed mesh assets by position alone.
        bool AssetClosureKnown = false;

        std::vector<IndexUnit>      Units; // residency unit order: always-loaded, then cells
        std::vector<IndexFile>      Files; // every cell file, in the order the units first name them
        std::vector<IndexReference> References;
        std::vector<IndexHLOD>      HLODs; // cell order; a cell that draws nothing has none
        // THE HLOD CELLS (WP-FAR-8), when an HLOD layer is spatially loaded (Rules::HLODHierarchy::Streams; HLODs
        // is then empty): the residency units of Rules::MakeHLODResidency in its unit order - each node resident
        // the whole session (`Reason` kHLODResidentReason, a file of its own), then the HLOD cells by (grid, X,
        // Z), `Grid` indexing Rules::HLODResidency::Partition.RuntimePartitions - and every node of the hierarchy, in node
        // order.
        std::vector<IndexUnit>     HLODUnits;
        std::vector<IndexHLODNode> HLODNodes;
    };
    DESERT_JSON_STRUCT( WorldIndex, "WorldIndex", 11 )

    // What a cell file's payload holds: which units, in order, then their records concatenated.
    struct CellPayload
    {
        std::string                     World;
        std::vector<std::string>        Units;
        std::vector<Assets::EntityData> Records;
    };
    DESERT_JSON_STRUCT( CellPayload, "WorldCell", 11 )
    // Both payloads are stamped with the world format version by the envelope; the marks state the same number.
    static_assert( Common::Json::FormatOf<WorldIndex>.Version == kWorldFormatVersion &&
                   Common::Json::FormatOf<CellPayload>.Version == kWorldFormatVersion );

    // ── Cooking ──────────────────────────────────────────────────────────────────────────────────

    struct CookedFile
    {
        std::string                Name;
        std::vector<unsigned char> Bytes;
    };

    struct CookedWorld
    {
        std::vector<CookedFile> Files; // every cell file, then the index last
        WorldIndex              Index;
    };

    // Splits @p scene (which must state a WorldPartition block) into cell files and an index. @p registries are
    // the asset registries the editor would load, asked in order, for mesh bounds and the asset closure; empty
    // means the cook has none, and the index says so (AssetClosureKnown). Refused, by record: a scene with no
    // WorldPartition block, a record without an id, two records with one id, a containment reference that
    // crosses units.
    //
    // THE HLODs ARE PACKED, NOT BUILT (WP-FAR-6B D1; UE: the cook packages what Build HLODs saved): @p hlods are
    // the world's saved HLODs (ReadBuiltHLODs). A world with a default HLOD layer and no @p hlods is refused, and
    // so is one with a cell whose saved HLOD is missing or stale (StaleHLODCells), naming the cells. A spatially
    // loaded layer's HLODs are packed into the HLOD cells of its grid (WorldIndex::HLODUnits, WP-FAR-8).
    //
    // @p prefabs reads the prefab every instance record states: instances are expanded first
    // (ExpandPrefabInstances, WP21), so the cells hold the prefab's records, each placed by its own bounds. An
    // empty reader is a stated condition - a world with an instance is then refused by the prefab's name.
    struct StoredHLODs;
    [[nodiscard]] Common::ResultStr<CookedWorld>
    CookWorld( const SceneSerialized& scene, std::span<const Common::Utils::AssetRegistry> registries,
               const PrefabReader& prefabs, const StoredHLODs* hlods = nullptr );

    // The same cook, PLANNED FROM DESCRIPTORS (WP18): `descriptors` (the world's descriptor index,
    // EntityDescriptorIndex.hpp) are what the cells are decided with; the records only fill the cell files.
    // Refused when the descriptors are not the records', id for id and in order. An instance record's descriptor
    // is replaced by its expanded records' (DescribeEntities); every other record keeps the index's.
    [[nodiscard]] Common::ResultStr<CookedWorld>
    CookWorld( const SceneSerialized& scene, std::span<const Rules::EntityDescriptor> descriptors,
               std::span<const Common::Utils::AssetRegistry> registries, const PrefabReader& prefabs,
               const StoredHLODs* hlods = nullptr );

    // THE WORLD A COOK CUTS (WP21): @p scene with every prefab instance expanded (ExpandPrefabInstances) and the
    // descriptors it is planned with - the source's own for a plain record, the expanded records' own
    // (DescribeEntities) in place of an instance's. CookWorld and BuildWorldHLODs both cut this world, so the
    // HLODs Build HLODs saves are the ones the cook checks. Refused when @p descriptors are not the source
    // records', id for id and in order, or when an instance does not expand (naming the prefab).
    struct ExpandedWorld
    {
        ExpandedScene                        World;
        std::vector<Rules::EntityDescriptor> Descriptors; // one per World.Scene record
    };
    [[nodiscard]] Common::ResultStr<ExpandedWorld>
    ExpandWorldForCook( const SceneSerialized& scene, std::span<const Rules::EntityDescriptor> descriptors,
                        const PrefabReader& prefabs );

    // The bounds source a cook plans with: the first registry that knows the mesh answers.
    [[nodiscard]] Rules::AssetBoundsSource BoundsFrom( std::span<const Common::Utils::AssetRegistry> registries );

    // WHETHER A MATERIAL DRAWS WITH ITS OWN SHADER, from the registries' rows alone: the material's row (by GUID,
    // then path) names its shader among its dependencies, and a material instance names its parent — followed
    // until a Shader row answers. A shader row whose Role tag (the manifest's `Role <Name>`) is not
    // Content::kStandardSurfaceRole is a custom shader — the shader's NAME decides nothing. A material the
    // registries do not know answers "not custom" (its load refuses it by path). First registry that answers wins,
    // as in BoundsFrom. Empty registries give an empty source (see Rules::CustomShaderSource).
    [[nodiscard]] Rules::CustomShaderSource
    CustomShaderFrom( std::span<const Common::Utils::AssetRegistry> registries );

    // Whether a foliage type stands in its cell's HLOD (FO-6): the `.defoliage` the registry row of
    // {guid, path} keys is read and its IncludeInHLOD answered. An error names the type when no registry holds
    // it or its file does not read. Empty registries give an empty source (see Rules::FoliageHLODSource).
    [[nodiscard]] Rules::FoliageHLODSource
    FoliageInHLODFrom( std::span<const Common::Utils::AssetRegistry> registries );

    // WHAT EACH SCRIPT ENTITY PROPERTY IS (WP-ENTITY-REFS): the script's declaration, read from its file
    // (Scripting::ReadScriptSchema, the script key resolved by AssetHandle::PathForStableKey) - Property.Entity(
    // { hard = true } ) answers Containment, Property.Entity() Observation, and a script that does not load or does
    // not declare the property as an Entity (or an Array of them) answers nullopt. Each script is read once per
    // source; the source may be asked from several workers at once. The source every engine plan is made with -
    // cook, Play, editor regions, the World Partition panel - so all of them cluster alike.
    [[nodiscard]] Rules::ScriptReferenceSource ScriptReferenceKinds();

    // ── Reading ──────────────────────────────────────────────────────────────────────────────────

    // Checks the envelope and parses the index; every error names @p fileName.
    [[nodiscard]] Common::ResultStr<WorldIndex> ReadWorldIndex( std::string_view               fileName,
                                                                std::span<const unsigned char> bytes );

    // Checks the envelope, that the file is the one @p index lists (size and checksum), and that its units
    // and record ids are exactly what the index says; every error names @p fileName.
    [[nodiscard]] Common::ResultStr<CellPayload> ReadCellFile( const WorldIndex& index, std::string_view fileName,
                                                               std::span<const unsigned char> bytes );

    // How a reader gets a cooked file's bytes: a directory, a pak through the VFS, or memory in a test.
    using FileReader = std::function<Common::ResultStr<std::vector<unsigned char>>( std::string_view fileName )>;

    // Which of a cooked world's two residencies a reader reads: the world's units (WorldIndex::Units) or the HLOD
    // cells' (WorldIndex::HLODUnits, WP-FAR-8).
    enum class UnitSet : std::uint8_t
    {
        World,
        HLOD,
    };
    [[nodiscard]] const std::vector<IndexUnit>& UnitsOf( const WorldIndex& index, UnitSet set );

    // A WorldCellSource over cooked files: what the streamer reads through, on a JobSystem worker. HOLDS NO
    // STATE BUT ITS INPUTS, so any number of workers may ask it at once; each call reads and checks the unit's
    // whole file and keeps nothing, because a streamed world is bigger than what it keeps resident. (A file
    // holding several units — the always-loaded one — is read once per unit; that happens once, at the start.)
    // @p set picks the units it reads: the world's, or the HLOD cells' of a streamed hierarchy.
    class CookedCellSource final : public Rules::WorldCellSource
    {
    public:
        CookedCellSource( const WorldIndex& index, FileReader reader, UnitSet set = UnitSet::World );

        [[nodiscard]] Common::ResultStr<std::vector<Assets::EntityData>>
        UnitRecords( std::size_t unit ) const override;

    private:
        const WorldIndex* m_Index;
        FileReader        m_Reader;
        UnitSet           m_Set;
    };

    // ── Streaming a cooked world without its records ─────────────────────────────────────────────

    // What the residency executor needs, from the index alone. Record `r` is the r-th id of the index's units
    // taken in unit order (the order ResidencyUnitMembers names them), so the plan's composites — one per unit —
    // hold consecutive record indices.
    struct IndexedWorld
    {
        Rules::WorldPartitionPlan                        Plan;
        std::vector<std::uint64_t>                       RecordIds;
        std::vector<std::pair<std::size_t, std::size_t>> Observations; // (from, to), crossing a unit
        std::size_t                                      AlwaysLoadedRecords = 0;
    };

    // The plan the cook planned with, as far as residency reads it: every unit's composite, its cell and
    // square, the always-loaded reasons, in unit order. Refused, by unit: a cell without its level, coordinate
    // or square, an always-loaded unit after a cell, cells out of (level, X, Z) order — the streaming query
    // searches them by that order — a record id twice, and a reference naming an id no unit holds.
    [[nodiscard]] Common::ResultStr<IndexedWorld> PlanFromIndex( const WorldIndex& index );

    // THE STREAMED HLOD HIERARCHY OF A COOKED WORLD (WP-FAR-8): the hierarchy PlanHLODHierarchy makes of @p plan
    // (PlanFromIndex's) and the residency MakeHLODResidency makes of it with the record counts of the index's
    // nodes - the plan Build HLODs and the cook made, made again by the same functions - checked against the
    // index's HLODNodes and HLODUnits name for name, unit for unit, kind and cell for cell and id for id. nullopt
    // when the index states no HLOD unit and the world's hierarchy does not stream (its HLODs, if any, are
    // resident the whole session). Refused, naming the node or unit: an index the hierarchy does not match, or a
    // world whose hierarchy streams and whose index states no HLOD unit (a cook of another build: re-cook it).
    struct IndexedHLODs
    {
        Rules::HLODHierarchy Hierarchy;
        Rules::HLODResidency Residency;
    };
    [[nodiscard]] Common::ResultStr<std::optional<IndexedHLODs>>
    PlanHLODsFromIndex( const WorldIndex& index, const Rules::WorldPartitionPlan& plan );

    // The part of a cooked world that is loaded before its first frame: the scene-wide part and the records of
    // every always-loaded unit, as a scene the ordinary loader takes. Each file is read once.
    [[nodiscard]] Common::ResultStr<SceneSerialized> AssembleAlwaysLoaded( const WorldIndex& index,
                                                                           const FileReader& reader );

    // Where a scene's cooked world lives beside it: `Worlds/X.desce` -> `Worlds/X.dwworld/`. One statement of
    // it, for the packager that writes there and the runtime that looks there.
    [[nodiscard]] std::string CookedWorldDirectory( std::string_view scenePath );

    // HOW A PACKAGED BUILD DIVIDES ONE COOKED WORLD INTO CONTENT CHUNKS (WP23; UE: a World Partition cell's
    // package lands in a chunk the runtime mounts when the cell streams in). Every cell unit's file goes to the
    // chunk of the square region (side WorldPartition.ChunkRegionSize) holding the CENTRE of the cell's square:
    // `World_<dir>_R<rx>_<rz>`, and so does every HLOD cell's file (HLODUnits, WP-FAR-8) by its own square. The
    // index, the always-loaded file, every session-resident HLOD file (HLODs, and the HLODUnits resident the whole
    // session) go to `World_<dir>_Shared`, which is mounted at start (the streamer decides from the index and
    // draws those HLODs from the first frame). `Regions` are the region chunk names, sorted; `FileChunk` names the
    // chunk of every file the index lists, and of the index itself.
    struct WorldChunkLayout
    {
        std::string                        Shared;
        std::vector<std::string>           Regions;
        std::map<std::string, std::string> FileChunk;
    };

    // The layout of @p index's world, cooked into @p worldDirectory (CookedWorldDirectory). nullopt when the world
    // states no ChunkRegionSize (it ships in the base chunk). Refused: a region size that is not finite and > 0, a
    // cell unit (or HLOD cell unit) without its square, a file two units place in different chunks, a file the
    // index lists that no unit or HLOD names.
    [[nodiscard]] Common::ResultStr<std::optional<WorldChunkLayout>>
    ChunkLayoutOf( const WorldIndex& index, std::string_view worldDirectory );

    // The world back from its cells: every unit's records in unit order, and the index's scene-wide part.
    // What the round trip is checked with; the records are in unit order, not the source's file order.
    [[nodiscard]] Common::ResultStr<SceneSerialized> AssembleWorld( const WorldIndex& index,
                                                                    const FileReader& reader );

    // The records of HLOD @p hlod (an index into index.HLODs): its InstancedStaticMesh records, read and checked
    // as ReadCellFile checks a cell. Empty when the HLOD has no file (nothing in its cell could be batched).
    [[nodiscard]] Common::ResultStr<std::vector<Assets::EntityData>>
    HLODRecords( const WorldIndex& index, const FileReader& reader, std::size_t hlod );

    // The name of the HLOD file of the cell whose file is @p cellFile: `L0_3_4.dwcell` -> `HLOD_L0_3_4.dwcell`.
    [[nodiscard]] std::string HLODFileName( std::string_view cellFile );

    // The scene's records in a canonical order (by id) as JSON, one per record: two scenes hold the same
    // records exactly when these are equal. What a round trip compares.
    [[nodiscard]] std::vector<std::string> CanonicalRecords( const SceneSerialized& scene );
    // ── Built HLODs (WP-FAR-6B, lead D1) ───────────────────────────────────────────────────────────
    // UE World Partition: HLODs are BUILT by an explicit editor step (Build > Build HLODs, UE's
    // WorldPartitionBuilderCommandlet writing HLOD packages) and SAVED next to the world; Play and the cook
    // read what was saved. They live in `<scene dir>/<Scene>_HLOD/`: an index `HLODs.dhlod` and one record file
    // per cell that batched anything. The extension is NOT .dwcell/.dwindex: those are content kinds rooted at
    // the scene path (ContentKinds.hpp), and the registry scan would take a stored HLOD for a cooked cell.
    inline constexpr std::string_view kBuiltHLODIndexName = "HLODs.dhlod";
    inline constexpr std::string_view kBuiltHLODExtension = ".dhlod";

    // UE's HLODBaseKey (FWorldPartitionHLODUtilities::ComputeHLODHash): the version of what the HLOD builders make
    // from one input. Raised with any change to their output for the same records and assets, it makes every saved
    // HLOD stale, so the next Build HLODs rebuilds them all.
    inline constexpr std::uint32_t kHLODBuilderVersion = 1;

    // One cell unit's built HLOD. There is a row for EVERY cell unit, also one that draws nothing, so a plan
    // cell without a row is stale and never ambiguous. `BuiltFrom` is HLODBuildChecksum of the cell's records
    // when it was built: a different checksum now is a stale HLOD.
    struct BuiltHLODCell
    {
        std::string                    Unit; // Rules::HLODRowName (a parent: its Rules::HLODNode::Name)
        std::uint32_t                  BuiltFrom = 0;
        std::optional<std::string>     File; // absent when nothing in the cell could be batched
        std::vector<std::uint64_t>     Ids;  // the HLOD's own records, one per batch, in batch order
        std::uint64_t                  Instances = 0;
        std::vector<std::uint64_t>     Sources; // the cell's records it stands in for, in member order
        std::vector<IndexNotInstanced> NotInstanced;
    };

    struct BuiltHLODIndex
    {
        std::string                SceneName;
        std::vector<BuiltHLODCell> Cells; // cell unit order
    };
    DESERT_JSON_STRUCT( BuiltHLODIndex, "BuiltHLODIndex", 11 )
    static_assert( Common::Json::FormatOf<BuiltHLODIndex>.Version == kWorldFormatVersion );

    // Where BuildWorldHLODs hands each cell's record file (CellPayload, kind WorldCell) the moment the cell is
    // built; the build keeps none, so a world's HLODs never sit in memory all at once (UE builds and saves its
    // HLOD actors cell by cell). An error stops the build by name.
    using BuiltHLODSink = std::function<Common::BoolResultStr( CookedFile file )>;

    // THE SAVED CONTENT AN HLOD IS BUILT FROM BEYOND ITS RECORDS (UE: UHLODBuilder::ComputeHLODHash reaches the
    // static mesh, its materials and their textures, each by the hash of what was saved - the asset registry's
    // FAssetPackageData::PackageSavedHash): every asset the members name, transitively through the registry's
    // dependency edges (mesh -> material -> texture), as "<registry key> <content hash>" lines in key order. An
    // asset envelope's content hash is folded from its table of contents (each section's tag and hash, read with
    // the header and never the body); any other file's from its bytes. A file that does not read hashes as
    // absent, so an HLOD built without it is stale once it is back. A hasher remembers each asset's hash for its
    // own lifetime - one build or one staleness check - never across an edit.
    class HLODAssetHasher
    {
    public:
        explicit HLODAssetHasher( std::span<const Common::Utils::AssetRegistry> registries );
        ~HLODAssetHasher();
        HLODAssetHasher( const HLODAssetHasher& )            = delete;
        HLODAssetHasher& operator=( const HLODAssetHasher& ) = delete;

        [[nodiscard]] std::string ContentOf( std::span<const Assets::EntityData> records,
                                             std::span<const std::size_t>        members );

    private:
        struct State;
        std::unique_ptr<State> m_State;
    };

    // What an HLOD is built from (UE: the HLOD actor's hash - its input actors, the assets they use, its layer's
    // settings and the builder's base key): the cell's records (Json::Write of each member without its outliner
    // sort key `siblingIndex`, in member order; the one form Build, Cook and Play hash), the saved content of
    // every asset they name (@p assets), the world's partition block (grids, HLOD layers, data layers by Name and
    // Type - not their editor default or runtime state, which change when an HLOD is drawn and not what it holds)
    // and kHLODBuilderVersion. Crc32c. A mesh or material edited and saved makes the HLODs that use it stale.
    [[nodiscard]] std::uint32_t HLODBuildChecksum( std::span<const Assets::EntityData> records,
                                                   std::span<const std::size_t>        members,
                                                   const WorldPartitionSerialized&     partition,
                                                   HLODAssetHasher&                    assets );

    // BUILD HLODs: the HLOD of every cell unit of @p scene by the world's default HLOD layer, planned from
    // @p descriptors as CookWorld plans. Refused, by name, as CookWorld refuses: no WorldPartition block, no
    // default HLOD layer, a record without an id or with a taken one, descriptors that are not the records'.
    // Each cell's file goes to @p sink as soon as it is built and is freed with the cell; the index is returned.
    // Prefab instances are expanded first with @p prefabs (ExpandWorldForCook), as CookWorld expands them, so
    // the cook of the same world finds every HLOD fresh. Each layer is built by its builder in @p builders
    // (Core::HLODBuilders, or the editor's copy of it); a layer whose builder is not registered or refuses its
    // Settings refuses the build with the first such line (Rules::HLODLayerBuilderIssues).
    //
    // INCREMENTAL (UE UWorldPartitionHLODsBuilder: an HLOD actor whose hash did not change is not rebuilt): a row
    // of @p previous whose BuiltFrom is the checksum now and whose file reads back as that row's is kept as it is,
    // and its file stays; only the other rows are built and only they reach the builders and @p sink. nullptr
    // builds every row (Build HLODs, force all).
    struct HLODBuild
    {
        BuiltHLODIndex           Index;
        std::vector<std::string> Built; // the rows built, by unit name; every other row was kept
    };
    [[nodiscard]] Common::ResultStr<HLODBuild>
    BuildWorldHLODs( const SceneSerialized& scene, std::span<const Rules::EntityDescriptor> descriptors,
                     std::span<const Common::Utils::AssetRegistry> registries, const PrefabReader& prefabs,
                     const Rules::HLODBuilderRegistry& builders, const StoredHLODs* previous,
                     const BuiltHLODSink& sink );

    // `<dir>/<stem>_HLOD/` of the scene file @p scenePath (beside `<stem>_Landscape/`).
    [[nodiscard]] std::filesystem::path BuiltHLODDirectory( const std::filesystem::path& scenePath );

    // The sink that writes each built cell file into BuiltHLODDirectory(scenePath) atomically (the directory is
    // made on the first file).
    [[nodiscard]] BuiltHLODSink BuiltHLODWriter( const std::filesystem::path& scenePath );

    // Writes @p index into BuiltHLODDirectory(scenePath) atomically, after its cell files (BuiltHLODWriter); every
    // other .dhlod in the directory is removed (a cell that no longer batches anything keeps no file).
    [[nodiscard]] Common::BoolResultStr WriteBuiltHLODIndex( const std::filesystem::path& scenePath,
                                                             const BuiltHLODIndex&        index );

    struct StoredHLODs
    {
        BuiltHLODIndex Index;
        FileReader     Reader; // a record file's bytes by its name
    };

    // The saved HLODs of the scene file @p scenePath (disk, then a mounted pak). Missing is an error naming the
    // scene and the step that makes them.
    [[nodiscard]] Common::ResultStr<StoredHLODs> ReadBuiltHLODs( const std::filesystem::path& scenePath );

    // The records of @p cell's file: envelope, kind, format version, units and ids checked against the row.
    // Empty when the row has no file.
    [[nodiscard]] Common::ResultStr<std::vector<Assets::EntityData>>
    StoredCellRecords( const StoredHLODs& stored, const BuiltHLODCell& cell );

    // The cell units of @p scene (planned from @p descriptors as BuildWorldHLODs plans) whose stored HLOD is
    // missing or stale against @p index; empty = every cell's HLOD is current.
    [[nodiscard]] Common::ResultStr<std::vector<std::string>>
    StaleHLODCells( const BuiltHLODIndex& index, const SceneSerialized& scene,
                    std::span<const Rules::EntityDescriptor>      descriptors,
                    std::span<const Common::Utils::AssetRegistry> registries );

    // ── MAP CHECK AND THE STREAMING GENERATION LOG (WP-MAP-CHECK) ────────────────────────────────────────────
    // UE UWorldPartition::CheckForErrors (Map Check, the check commandlet) and StreamingGeneration-*.log: ONE list of
    // what is wrong with a partitioned world - Rules::PlanIssues (the planner's and the streaming generation's
    // findings) and the saved HLODs that are missing or stale - read by Tools > Check World Partition, the warning
    // after a save and `WorldCook --check`; and one dump of the plan per cell, written at the cook, at the start of
    // Play and after a save.
    struct WorldPartitionCheck
    {
        std::string                          World;
        WorldPartitionSerialized             Partition;
        Rules::WorldPartitionPlan            Plan;
        std::vector<Rules::EntityDescriptor> Descriptors; // the records the plan was made from
        std::vector<Rules::GenerationIssue>  Issues;      // Rules::PlanIssues, then the saved HLODs'
    };

    // A finding per cell whose saved HLOD is missing or stale (StaleHLODCells), and the one for HLODs not built.
    [[nodiscard]] std::vector<Rules::GenerationIssue> StaleHLODIssues( std::span<const std::string> units );
    [[nodiscard]] Rules::GenerationIssue              MissingHLODsIssue( std::string_view why );

    // THE CHECK of @p scene planned from @p descriptors (its records', one for one), its saved HLODs read beside
    // @p scenePath (empty: not saved - when a grid states an HLOD layer, that is a finding). Refused, by name, when the
    // scene states no WorldPartition block or the HLOD plan cannot be made.
    [[nodiscard]] Common::ResultStr<WorldPartitionCheck>
    CheckWorldPartition( const SceneSerialized& scene, std::vector<Rules::EntityDescriptor> descriptors,
                         std::span<const Common::Utils::AssetRegistry> registries,
                         const std::filesystem::path&                  scenePath );

    // The check of the world saved at @p scenePath, read as the cook reads it (ReadSceneFileText, the descriptor
    // index brought up to date). nullopt: the file is not a partitioned world.
    [[nodiscard]] Common::ResultStr<std::optional<WorldPartitionCheck>>
    CheckSavedWorldPartition( const std::filesystem::path&                  scenePath,
                              std::span<const Common::Utils::AssetRegistry> registries );

    // Logs @p check: Rules::ReportPlanIssues of its plan, then its HLOD findings. Returns the logged text.
    std::string ReportWorldPartitionCheck( const WorldPartitionCheck& check );

    // `<project>/Saved/Logs/WorldPartition/<world>-StreamingGeneration.log`.
    [[nodiscard]] std::filesystem::path StreamingGenerationLogPath( const std::filesystem::path& projectDir,
                                                                    std::string_view             world );

    // THE DUMP (UE FStreamingGenerationLogErrorHandler + the generation's per-cell dump): the plan's summary, every
    // always-loaded composite with its reason, every cell with its grid, level, square, streaming bounds, height span,
    // data layers, composites and records and the references that leave it, then every finding.
    [[nodiscard]] std::string StreamingGenerationLogText( std::string_view                         world,
                                                          const WorldPartitionSerialized&          partition,
                                                          const Rules::WorldPartitionPlan&         plan,
                                                          std::span<const Rules::EntityDescriptor> descriptors,
                                                          std::span<const Rules::GenerationIssue>  issues );

    // Writes the dump to StreamingGenerationLogPath atomically (its directory made); returns the file.
    [[nodiscard]] Common::ResultStr<std::filesystem::path>
    WriteStreamingGenerationLog( const std::filesystem::path& projectDir, std::string_view world,
                                 const WorldPartitionSerialized& partition, const Rules::WorldPartitionPlan& plan,
                                 std::span<const Rules::EntityDescriptor> descriptors,
                                 std::span<const Rules::GenerationIssue>  issues );
} // namespace Desert::Core::WorldCells
