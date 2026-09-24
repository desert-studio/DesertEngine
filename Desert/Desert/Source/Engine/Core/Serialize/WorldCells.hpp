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
// ── THE ENVELOPE: VERSION, LENGTH AND CRC-32C, CHECKED BEFORE THE CONTENT IS BELIEVED ─────────────
//
//   magic (4: "DWIX" index / "DWCL" cell) | u32 container version | u64 payload bytes | payload (JSON) |
//   u32 Crc32c of everything before it
//
// The same layout and the same order of checks as the landscape tile (.dlht). The index also carries every
// file's size and checksum, so a cell that is intact on its own but belongs to ANOTHER cook of the world (a
// stale file left in the directory) is refused too. Every refusal names the file.

#include <Engine/Core/Serialize/SceneFormat.hpp>
#include <Engine/Core/Serialize/WorldCellSource.hpp>
#include <Engine/Core/Serialize/WorldPartitionRules.hpp>

#include <Common/Core/ResultStr.hpp>
#include <Common/Utilities/AssetRegistry.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::Core::WorldCells
{
    // Raised with any change to the envelope or to the index/cell payload shapes below. A reader refuses any
    // other number by name; a cooked world is re-derivable, so the answer to an old one is a re-cook.
    inline constexpr std::uint32_t kContainerVersion = 1;

    inline constexpr std::string_view kIndexFileName        = "World.dwindex";
    inline constexpr std::string_view kCellExtension        = ".dwcell";
    inline constexpr std::string_view kAlwaysLoadedFileName = "AlwaysLoaded.dwcell";

    // ── The index ────────────────────────────────────────────────────────────────────────────────

    // One residency unit. `Level`/`X`/`Z`/`Square` only for a cell; `Reason` only for an always-loaded unit.
    struct IndexUnit
    {
        std::string                      Name; // Rules::DescribeResidencyUnit
        std::string                      File;
        std::optional<int>               Level;
        std::optional<std::int32_t>      X;
        std::optional<std::int32_t>      Z;
        std::optional<Rules::CellBounds> Square;
        std::optional<std::string>       Reason;
        std::vector<std::uint64_t>       Ids;    // record ids, in ResidencyUnitMembers order
        std::vector<std::string>         Assets; // registry keys the unit needs, transitively; sorted
    };

    struct IndexFile
    {
        std::string   Name;
        std::uint64_t Bytes = 0;
        std::uint32_t Crc   = 0; // Crc32c of the whole file, trailer included
    };

    // A kEntityReferences reference whose two ends are in different units. Only those: within a unit both ends
    // are always resident together, so only a crossing one can be deferred or unbound.
    struct IndexReference
    {
        std::uint64_t From     = 0;
        std::uint64_t To       = 0;
        std::uint32_t FromUnit = 0;
        std::uint32_t ToUnit   = 0;
        std::string   Component;
        std::string   Field;
    };

    struct WorldIndex
    {
        // The scene-wide part of the source, so a world is whole without the .desce it came from.
        std::string                 SceneName;
        std::optional<rfl::Generic> Settings;
        int                         SceneVersion = 0;
        int                         UnitVersion  = 0;
        WorldPartitionSerialized    WorldPartition;

        int           LevelCount = 0;
        std::uint64_t Records    = 0;
        // False when the cook had no asset registry: every unit's `Assets` is then empty because it is
        // UNKNOWN, not because the unit needs nothing — and the plan placed mesh assets by position alone.
        bool AssetClosureKnown = false;

        std::vector<IndexUnit>      Units; // residency unit order: always-loaded, then cells
        std::vector<IndexFile>      Files; // every cell file, in the order the units first name them
        std::vector<IndexReference> References;
    };

    // What a cell file's payload holds: which units, in order, then their records concatenated.
    struct CellPayload
    {
        std::string                     World;
        std::vector<std::string>        Units;
        std::vector<Assets::EntityData> Records;
    };

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
    [[nodiscard]] Common::ResultStr<CookedWorld>
    CookWorld( const SceneSerialized& scene, std::span<const Common::Utils::AssetRegistry> registries );

    // The bounds source a cook plans with: the first registry that knows the mesh answers.
    [[nodiscard]] Rules::AssetBoundsSource BoundsFrom( std::span<const Common::Utils::AssetRegistry> registries );

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

    // A WorldCellSource over cooked files: what WP9's streamer reads through. A unit's file is read and
    // checked on first use and kept, since the always-loaded file serves several units.
    class CookedCellSource final : public Rules::WorldCellSource
    {
    public:
        CookedCellSource( const WorldIndex& index, FileReader reader );

        [[nodiscard]] Common::ResultStr<std::vector<Assets::EntityData>> UnitRecords( std::size_t unit ) override;

    private:
        const WorldIndex*                  m_Index;
        FileReader                         m_Reader;
        std::map<std::string, CellPayload> m_Read;
    };

    // The world back from its cells: every unit's records in unit order, and the index's scene-wide part.
    // What the round trip is checked with; the records are in unit order, not the source's file order.
    [[nodiscard]] Common::ResultStr<SceneSerialized> AssembleWorld( const WorldIndex& index,
                                                                    const FileReader& reader );

    // The scene's records in a canonical order (by id) as JSON, one per record: two scenes hold the same
    // records exactly when these are equal. What a round trip compares.
    [[nodiscard]] std::vector<std::string> CanonicalRecords( const SceneSerialized& scene );
} // namespace Desert::Core::WorldCells
