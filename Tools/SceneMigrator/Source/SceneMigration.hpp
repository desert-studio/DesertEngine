#pragma once

// THIS IS TOOL CODE, AND THAT IS THE POINT OF IT BEING HERE.
//
// LEG1 (2026-09-26): every step below kSceneVersionShaderGuids (v31) was deleted along with the legacy
// material-id register it depended on (LegacyMaterialIds.*). The owner's decision: legacy formats are not
// supported at all — a file below the minimum this tool reads is REFUSED, naming its stated version,
// rather than migrated through a translation table. The engine loader already refuses anything not at
// Core::kSceneVersion; this tool now refuses anything below v31/v1 too, so there is exactly one accepted
// band of input (v31/v1 through the head) instead of a chain of forty migrations nobody can delete.
//
// What remains is the one step still needed to reach the head from the oldest file the corpus can still
// contain: MigratePathOnlyMeshGuidsV31ToV32 (MSH1), and the machinery MigrateScene/MigratePrefab need to
// gate, run and stamp it.

// The current on-disk shape and the two head version integers. Owned by the ENGINE because the engine's
// saver writes it and its loader parses it; read here because a migration whose input is "the parsed tree"
// needs the tree's type, and a second copy of that struct is a format that can silently fork.
#include <Engine/Core/Serialize/SceneFormat.hpp>

// The `.deprefab` payload and its gate. A prefab's entities ARE Core::SceneSerialized::Entities - the
// same struct, written by the same ComponentRegistry - so it is raised by the SAME step chain rather
// than by a second one that would have to be kept in step by hand (И11). See MigratePrefab.
#include <Engine/Assets/Prefab/PrefabFormat.hpp>

#include <Common/Core/Constants.hpp>
#include <Common/Core/ResultStr.hpp>

#include <array>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Desert::Migration
{
    using Core::kSceneVersion;
    using Core::kUnitVersion;

    // THE TREE THIS TOOL READS AND WRITES: the engine's Core::SceneSerialized, member for member, PLUS the two
    // top-level integers every file before v26 stated its generations in. The engine's struct lost them to
    // the text header (AF6g), so it cannot say what generation an old file is - and a migrator that cannot
    // read the generation cannot report what it refuses. They are read, never written: MigrateScene clears
    // them when it stamps the header, and rfl omits an empty optional, so the file this tool writes is the
    // file the engine reads. A deliberate second copy of the shape; the static_assert below is what keeps
    // it from forking (a member added to the engine's struct and not here fails to compile).
    struct SceneSerialized
    {
        std::optional<Common::Content::TextAssetHeaderSerialized> Header;
        std::string                                               SceneName;
        std::vector<Assets::EntityData>                           Entities;
        std::optional<rfl::Generic>                               Settings;
        std::optional<Core::WorldPartitionSerialized>             WorldPartition;
        // Pre-v26 files only; read so a refusal can name the version, never written.
        std::optional<int> UnitVersion;
        std::optional<int> SceneVersion;
    };
    static_assert( rfl::named_tuple_t<SceneSerialized>::size() ==
                        rfl::named_tuple_t<Core::SceneSerialized>::size() + 2,
                   "Core::SceneSerialized gained or lost a member: mirror it in Migration::SceneSerialized, or "
                   "this tool drops it from every scene it rewrites" );

    // The same for a .deprefab: Assets::PrefabData plus the two pre-v26 integers.
    struct PrefabData
    {
        std::optional<Common::Content::TextAssetHeaderSerialized> Header;
        std::optional<Assets::PrefabBoundsSer>                    Bounds;
        std::string                                               Name;
        std::vector<Assets::EntityData>                           Entities;
        Common::UUID                                              Root;
        // Pre-v26 files only; read so a refusal can name the version, never written.
        std::optional<int> SceneVersion;
        std::optional<int> UnitVersion;
    };
    static_assert( rfl::named_tuple_t<PrefabData>::size() == rfl::named_tuple_t<Assets::PrefabData>::size() + 2,
                   "Assets::PrefabData gained or lost a member: mirror it in Migration::PrefabData" );

    // The engine's prefab, for the engine's writer (Assets::WritePrefabJson): everything but the two
    // pre-v26 integers, which a migrated prefab no longer states.
    [[nodiscard]] inline Assets::PrefabData ToEnginePrefab( const PrefabData& prefab )
    {
        return Assets::PrefabData{ prefab.Header, prefab.Bounds, prefab.Name, prefab.Entities, prefab.Root };
    }

    // THE MINIMUM GENERATION THIS TOOL READS. A file stating less than this - on either integer - is
    // REFUSED, naming the version, and not migrated: every step that used to carry a file up to v31 was
    // deleted with the legacy material-id register in LEG1, on the owner's decision that legacy formats
    // are not supported. The tracked corpus is entirely at v31 or v32 already (MSH1 raised it), so nothing
    // committed needs a step below this one.
    inline constexpr int kSceneVersionShaderGuids = 31;

    //  32 - A PATH-ONLY MESH BLOCK STATES ITS GUID (MSH1). The v28 step (since deleted) rewrote MeshGuid
    //       VALUES and skipped a block with no MeshGuid key at all, so a block written as `{"MeshPath":
    //       ...}` alone (M10_MeshSlot, from the era when the path WAS the identity) crossed v28-v31 as it
    //       was, and the loader leaves it an empty slot: the path is a locator, not an identity. Each
    //       StaticMesh / SkinnedMesh / InstancedStaticMesh block with a MeshPath and a missing or ""
    //       MeshGuid gains the GUID the file's v3 header states; a missing file or a file stating no GUID
    //       REFUSES the file, naming the block - in entity records AND in prefab-override records
    //       (MigratePathOnlyMeshGuidsV31ToV32).
    inline constexpr int kSceneVersionPathOnlyMeshGuids = 32;

    // The last step this tool knows and the generation the engine requires are ONE number, and this is
    // where that is checked. If a schema step is ever added here without raising Core::kSceneVersion, the
    // tool would stamp files at a version the loader refuses - every scene in the repository would stop
    // opening at once, and the file that caused it would look correct in isolation.
    //  33 - A FOLIAGE FIELD NAMES ITS TYPE (FO-1). The Foliage block's inline scatter numbers (Density,
    //       ScaleMin/Max, ZOffsetMin/Max, MaxPitchDeg, SlopeMin/MaxDeg, AlignToNormal, RandomYaw) move into a
    //       `.defoliage` under Foliage/ (one file per distinct set of numbers and mesh), and the block becomes
    //       {FoliageTypeGuid, FoliageTypePath} (MigrateInlineFoliageV32ToV33).
    inline constexpr int kSceneVersionFoliageTypes = 33;

    //  34 - A LANDSCAPE'S LAYERS ARE `.delayerinfo` REFERENCES (LS-12b). The root's `Layers` list was inline
    //       {Name, Hardness, NoWeightBlend, Color} objects; it is now [{Guid, Path}] naming layer info assets
    //       (UE: ALandscape target layers -> ULandscapeLayerInfoObject). No tracked
    //       file carries an inline layer, so the step is the identity on the corpus; a file that does carry
    //       one is REFUSED by name (MigrateLandscapeLayerRefsV33ToV34) rather than guessed into assets.
    inline constexpr int kSceneVersionLandscapeLayerRefs = 34;

    //  35 - A PARTITIONED WORLD KEEPS ONE FILE PER ENTITY (WP16, Engine/Core/Serialize/ExternalEntities.hpp).
    //       The tree does not change; a scene that states a WorldPartition block is WRITTEN as its header plus
    //       one `.deent` per record, which is the caller's write (MigratorMain), counted here. Every other
    //       scene and every prefab only gains the stamp.
    inline constexpr int kSceneVersionExternalEntities = 35;

    //  36 - THE GRADE AND THE SHADOW POLICY LEAVE THE SETTINGS BLOCK (SET1). SceneSettings' post-process keys
    //       move into an Unbound PostProcessVolume entity, EnableShadows/ShadowBias/CascadeSplitLambda onto the
    //       DirectionLight (MigrateSceneSettingsHomesV35ToV36). Scene-only; a prefab only gains the stamp.
    inline constexpr int kSceneVersionSceneSettingsHomes = 36;

    static_assert( kSceneVersionSceneSettingsHomes == kSceneVersion,
                   "the last migration step and the engine's required scene version must be the same "
                   "generation - raise Core::kSceneVersion in Engine/Core/Serialize/SceneFormat.hpp" );

    // What MigratePathOnlyMeshGuidsV31ToV32 did to one file (also the shape of the v27->28 step this tool
    // no longer carries: kept as one struct name so a future step over the same MeshGuid slot need not
    // invent a second report shape for the same three fields).
    struct MeshGuidsMigrationReport
    {
        int Rewritten = 0; // MeshGuid values that now state the mesh file's header GUID

        // Blocks that cannot be raised, as "Tag > SkinnedMesh.MeshGuid = 123 (why)". Non-empty REFUSES the
        // file: a missing file, or a file stating no GUID, leaves no identity to write, and guessing one is
        // the silent fallback this step retires.
        std::vector<std::string> UnknownNames;
    };

    // Gives every StaticMesh / SkinnedMesh / InstancedStaticMesh block that names a MeshPath but states no
    // MeshGuid (key missing or "") the GUID the mesh file's v3 header states, in entity records and in their
    // prefab-override records; a block that already states a GUID is left alone. `MeshPath` is
    // project-relative ("Cooked/Meshes/X.skmesh"); the project is the nearest ancestor of `assetsRoot` under
    // which that file exists. `Rewritten` counts the blocks that gained a GUID; UnknownNames REFUSES the
    // file. PURE - no GPU, no filesystem write, no global state.
    MeshGuidsMigrationReport MigratePathOnlyMeshGuidsV31ToV32( std::vector<Assets::EntityData>& entities,
                                                               const std::filesystem::path&     assetsRoot );

    // What MigrateSceneSettingsHomesV35ToV36 did to one scene.
    struct SceneSettingsHomesReport
    {
        int  PostKeysMoved   = 0;     // Settings keys now stated by the Unbound PostProcessVolume
        bool VolumeCreated   = false; // false when the Settings block stated no grade key at all
        int  ShadowKeysFound = 0;     // EnableShadows / ShadowBias / CascadeSplitLambda stated by Settings
        int  LightsStamped   = 0;     // DirectionLight blocks (records and prefab overrides) that took them
    };

    // Moves every grade key of the Settings block (the fields of Core::PostProcessSettings) into the
    // `Settings` object of a new Unbound PostProcessVolume entity, and EnableShadows (renamed CastShadows),
    // ShadowBias and CascadeSplitLambda onto every DirectionLight block of the scene, entity records and
    // prefab overrides alike; all of them leave the Settings block. A key the block did not state is not
    // written anywhere: its old default and its new default are the same number. Shadow keys of a scene
    // with no DirectionLight are dropped - no light, no cascades to configure. The new entity's id is
    // derived from the scene's GUID, so two runs on two branches mint one entity. PURE.
    SceneSettingsHomesReport MigrateSceneSettingsHomesV35ToV36( SceneSerialized& scene );

    // The inline landscape layers a file still carries, as "Tag > Landscape.Layers[i] = 'Name'". Non-empty
    // REFUSES the file: the step would have to invent a `.delayerinfo` per layer, and it does not write assets.
    // PURE - no filesystem access.
    std::vector<std::string> MigrateLandscapeLayerRefsV33ToV34( const std::vector<Assets::EntityData>& entities );

    // Everything that ran, so the caller can say which FILE moved and how far.
    //
    // `File` and not `Scene` since И11: the same report comes back from MigratePrefab, because a
    // `.deprefab` is raised by the same chain.
    // FOLT 1 -> 2 (FO-3): A `.defoliage` STATES DENSITY IN UE's UNITS. v1's Density was "instances per paint
    // dab"; the brush it was painted with scattered that many in its disk, whose radius was the brush's
    // default of kFoliageV1ReferenceBrushRadiusCm unless the painter moved the slider. v2's Density is
    // instances per 1000x1000 cm (UFoliageType::Density), so one dab of the reference brush places the same
    // count under both. Instances already painted live in the scene's InstancedStaticMesh and are not touched.
    inline constexpr float kFoliageV1ReferenceBrushRadiusCm = 300.0f;

    // v1's per-dab count as v2's areal density: perDab / (pi r^2) * 1000^2, r = the reference radius.
    float FoliageDensityFromPerDab( float perDab );

    // The v2 text of a v1 `.defoliage`: Density converted, the v2 fields (Height, LandscapeLayers,
    // MinimumLayerWeight) at UE's defaults, the header's GUID kept. A file that does not state FOLT 1, or
    // whose v1 body does not read, is an error naming why. PURE - no filesystem access.
    Common::ResultStr<std::string> MigrateFoliageTypeV1ToV2( const std::string& text );

    // The v3 text of a v2 `.defoliage`: every v2 number kept, CullDistance at UE's default {0, 0} (never
    // culled), the header's GUID kept. A file that does not state FOLT 2 is an error naming what it states.
    // PURE - no filesystem access.
    Common::ResultStr<std::string> MigrateFoliageTypeV2ToV3( const std::string& text );

    // The v4 text of a v3 `.defoliage`: every v3 number kept, Wind at Strength 0 (the instances stand still,
    // as every v3 field drew), the header's GUID kept. A file that does not state FOLT 3 is an error naming
    // what it states. PURE - no filesystem access.
    Common::ResultStr<std::string> MigrateFoliageTypeV3ToV4( const std::string& text );

    // What MigrateInlineFoliageV32ToV33 did to one file, and the `.defoliage` files it needs written. The
    // step itself writes nothing: the files are written by the tool's write pass, beside the scene.
    struct FoliageTypesMigrationReport
    {
        int Rewritten = 0;                                                   // Foliage blocks now naming a type
        std::vector<std::pair<std::filesystem::path, std::string>> NewTypes; // absolute path, canonical text
        std::vector<std::string>                                   UnknownNames;
    };

    FoliageTypesMigrationReport MigrateInlineFoliageV32ToV33( std::vector<Assets::EntityData>& entities,
                                                              const std::string&               ownerName,
                                                              const std::filesystem::path&     assetsRoot );

    struct FileMigrationReport
    {
        // Non-empty: the tree states a generation this tool does not read - either ABOVE the head (a build
        // this one predates) or BELOW kSceneVersionShaderGuids (a legacy format this tool no longer
        // supports). NOTHING was run and NOTHING was stamped - the caller must report the refusal and leave
        // the file alone.
        std::string Refused;

        bool                     PathOnlyMeshGuidsRaised = false; // below kSceneVersionPathOnlyMeshGuids
        MeshGuidsMigrationReport PathOnlyMeshGuids;
        bool                     LandscapeLayerRefsRaised = false; // below kSceneVersionLandscapeLayerRefs

        bool                        FoliageTypesRaised = false; // below kSceneVersionFoliageTypes
        FoliageTypesMigrationReport FoliageTypes;

        bool        ExternalEntitiesRaised = false; // below kSceneVersionExternalEntities
        std::size_t EntitiesMovedOut       = 0;     // records a partitioned world now keeps in their own files

        bool                     SceneSettingsHomesRaised = false; // below kSceneVersionSceneSettingsHomes
        SceneSettingsHomesReport SceneSettingsHomes;

        bool Changed() const
        {
            return PathOnlyMeshGuidsRaised || FoliageTypesRaised || LandscapeLayerRefsRaised ||
                   ExternalEntitiesRaised || SceneSettingsHomesRaised;
        }
    };

    // THE MIGRATION'S GUID for a file that has none: derived from the file's path relative to the content
    // root it belongs to (generic form), so a re-run over the same tree - on any machine - states the same
    // identity, and a scene migrated on two branches is the same asset on both. This is the ONLY place a
    // GUID is derived from anything: everywhere else it is minted once and then kept (StampTextHeader), and
    // AssetEnvelope.hpp deliberately has no path constructor - a moved file keeps its GUID, so after
    // migration the path this was derived from is history, not a key. Never null.
    Common::Content::AssetGuid MigrationGuidForPath( const std::filesystem::path& relativeToContentRoot );

    // Raises a parsed scene file to the current generation of BOTH version integers and stamps them, so a
    // tree that has been through this function is one nothing will migrate again. This is the single entry
    // point: the loader calls it on the tree it just parsed, and SceneMigrator calls it on every .desce in
    // the repository and writes the result back.
    //
    // GATING, IN ORDER: a stated pair ABOVE the head is refused (a later build wrote it). A stated scene
    // version below kSceneVersionShaderGuids, a stated unit version other than kUnitVersion, or no Header
    // at all is refused too - this tool's oldest supported input is v31/v1, and none of those three is
    // reachable by anything this tool can still raise. Otherwise the one remaining step runs and the file
    // is stamped, whether or not that step changed anything (an already-current file is stamped again,
    // idempotently: MigrationHeader keeps its existing GUID).
    //
    // `assetsRoot` is what the step measures a MeshPath against, and DEFAULTS to the live content root.
    // `sourceFile` is the file the tree was read from, used only to derive a GUID for a tree that states
    // none.
    FileMigrationReport
    MigrateScene( SceneSerialized&             scene,
                  const std::filesystem::path& assetsRoot = Common::Constants::Path::ASSETS_PATH,
                  const std::filesystem::path& sourceFile = {} );

    // What MigratePrefab did to one `.deprefab`, on top of the chain's own report.
    struct PrefabMigrationOutcome
    {
        FileMigrationReport Steps; // what the shared chain did; false when nothing ran

        bool AlreadyCurrent = false; // both integers already at the head; nothing to do

        // Non-empty: this pair is one no build of this engine writes, is above the head, or is below the
        // minimum generation this tool reads (kSceneVersionShaderGuids / kUnitVersion). Nothing was changed.
        std::string Refused;

        int FoundSceneVersion = 0; // what the tree stated before the step (absent = 0), for the report
        int FoundUnitVersion  = 0;
    };

    // Raises a parsed `.deprefab` to the current generation and stamps it - through the SAME step chain
    // MigrateScene runs, which is the whole point of И11.
    //
    // THE THREE THINGS THE PAIR (SceneVersion, UnitVersion) CAN BE, and what each gets:
    //
    //   (head, head)                current - nothing runs.
    //   (kSceneVersionShaderGuids..head-1, kUnitVersion)   a stamped v31 prefab: the one remaining step runs.
    //   anything else               refused by its own numbers, naming them - including the pre-Д28
    //                                unstamped (0, 0) case and every generation below v31: this tool no
    //                                longer carries a file up from either.
    //
    // PURE - no GPU, no filesystem, no global state, like the step it calls.
    static_assert( kUnitVersion == 1,
                   "the world unit has moved: a prefab can now legitimately state a UnitVersion below the "
                   "head, so MigratePrefab's gating above has to be re-argued at the same time" );

    PrefabMigrationOutcome
    MigratePrefab( PrefabData&                  prefab,
                   const std::filesystem::path& assetsRoot = Common::Constants::Path::ASSETS_PATH,
                   const std::filesystem::path& sourceFile = {} );

} // namespace Desert::Migration
