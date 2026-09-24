#include <optional>
#include <filesystem>
#include <Common/Content/AssetEnvelope.hpp>
#pragma once

// THE WORLD-SCALE SCENE, AS A PURE FUNCTION.
//
// WHY THIS EXISTS AT ALL. The largest scene this repository has ever held is 267 KB
// (MAT_ProbeGraphBatchStress.desce, 1024 cubes). Docs/World/PROGRAMME.md §1: on a scene that size not one
// of the problems the programme is about reproduces, so no improvement against them is measurable and no
// "done" is checkable. A world-scale scene is therefore the programme's INSTRUMENT, not its prize - it has
// to exist before step 2 can measure anything.
//
// WHY A TOOL AND NOT A SCRIPT. The file it writes has to be one the engine's loader accepts at the current
// schema step and one the engine's own saver would have written byte for byte - that second property is
// what SceneForeignKeys' corpus rule asserts about every .desce in the tree. The only way to hold both
// without a second statement of the format is to build Core::SceneSerialized and Assets::EntityData and
// let rfl write them, which is C++. A Python generator emitting JSON would be a fork of the format that
// nothing checks, which is exactly what Tools/SceneMigrator's own header forbids.
//
// PURE. No filesystem, no GPU, no AssetManager, no reflection registry - parameters and a material table
// in, a parsed tree out. That is what lets Desert/Tests/Tools/WorldSceneGenerator drive the same code the
// tool drives, over the same numbers, without an engine.

#include <Engine/Core/Serialize/SceneFormat.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace Desert::WorldGen
{
    // One material the generated world may name. Both spellings, because that is what the engine's saver
    // writes and what MaterialIdentity's corpus rules check: the GUID is the stable handle and the path is
    // the rename-visible fallback. Writing only the path would produce a file that GAINS a key the first
    // time the editor saves it, which breaks the round-trip identity above.
    struct MaterialRef
    {
        std::string Path; // e.g. "Materials/CB_White.demat", relative to the assets root
        uint64_t    Guid = 0;
    };

    // WHAT A WORLD IS, IN NUMBERS. Every length is an integer number of CENTIMETRES (1 world unit = 1 cm,
    // Common/Core/Units.hpp) and every count an integer, because both feed the exactness argument below.
    struct WorldSpec
    {
        std::string Name = "World";

        // Cells per side. The world is Cells x Cells cells and is SQUARE.
        int Cells = 32;

        // Cell edge. 25600 cm = 256 m is UE's own World Partition cell size in the measurement
        // Docs/World/01_streaming.md quotes, and the number a 768 m load radius was measured against
        // (~29 resident cells). Keeping it means our resident-set arithmetic is comparable with theirs.
        int CellSizeCm = 25600;

        // Buildings per cell. The ground tile is extra, so a cell costs PerCell + 1 entities.
        int PerCell = 48;

        uint64_t Seed = 1;
    };

    // What a generated world is made of, reported rather than recomputed by the caller.
    struct WorldStats
    {
        int     Entities    = 0; // every record in the file, fixtures included
        int     Cells       = 0;
        int     Buildings   = 0;
        int     GroundTiles = 0;
        int64_t ExtentCm    = 0; // edge of the square world
    };

    // THE GENERATOR. Deterministic: same spec and same material table give the same tree, on any machine
    // and in any build, and the tool's own --verify flag plus WorldSceneGenerator pin that.
    //
    // HOW DETERMINISM IS BOUGHT, because "use a seeded RNG" is not enough on its own:
    //
    //   * the generator does its own integer mixing (splitmix64) rather than <random>. The std
    //     distributions are implementation-defined - libc++ and libstdc++ disagree on the same seed - so a
    //     scene generated on this Mac and on the Windows target would differ, and the file is supposed to
    //     be the same MEASURING STICK on both.
    //   * every number that reaches the file is an integer count of centimetres or hundredths,
    //     divided at most by 100 or 2. The world is 2^23 cm across at the shipped preset, so every
    //     coordinate is exactly representable as a float and survives float -> double -> shortest-decimal
    //     with no rounding to disagree about. This is the same trap SettingsCanonical documents from the
    //     other side: a value that is not exactly representable has a TEXT that depends on who wrote it.
    //   * each cell is seeded from (Seed, cx, cz) alone, not from a stream advanced in iteration order. So
    //     a cell's contents do not depend on how many cells were generated before it - which keeps the
    //     file stable under a change of Cells, and is the property a per-cell regeneration would need.
    Core::SceneSerialized BuildWorld( const WorldSpec& spec, const std::vector<MaterialRef>& buildingMaterials,
                                      const MaterialRef& groundMaterial, const Common::Content::AssetGuid& guid,
                                      WorldStats& stats );

    // The GUID a REGENERATION of `outputFile` must keep: parsed from that file's own header when it
    // already exists and the header is well-formed. Absent otherwise (no file yet, or one this build
    // cannot read as a text asset) - the caller mints a fresh AssetGuid in that case, the same rule every
    // other text asset follows (Assets::StampTextHeader). Never derives a GUID from the spec's name: two
    // never-before-written files of one spec must not collide on one identity.
    std::optional<Common::Content::AssetGuid> ExistingWorldGuid( const std::filesystem::path& outputFile );
} // namespace Desert::WorldGen
