#pragma once

// THIS IS TOOL CODE, AND THAT IS THE POINT OF IT BEING HERE.
//
// Every function below used to run inside the engine, on every scene load, forever. Each was written with
// an expiry - "deleted once no v<n> file remains" - and eight of them accumulated, which is eight expiries
// nobody could enforce, because a migration that runs at LOAD never writes its result down and so can never
// observe that the last old file is gone. DEV_CONTRACT §4.3 ends "the runtime knows nothing about the old
// format"; while this file was compiled into the engine it knew eight.
//
// Nothing here was deleted. It moved, unchanged, to the one place a conversion can actually expire: a tool
// that runs over FILES and writes them back. The engine loader now REFUSES a scene that is not at
// Core::kSceneVersion and names this tool (Engine/Core/Serialize/SceneFormat.hpp), so the old formats are
// known here and nowhere else.
//
// The pure functions kept their signatures, their comments and their behaviour, so the files this tool
// writes are byte-for-byte the files the loader used to produce in memory. What changed is WHO runs them
// and WHEN - once, deliberately, rather than on every load of every build.

// The current on-disk shape and the two head version integers. Owned by the ENGINE because the engine's
// saver writes it and its loader parses it; read here because a migration whose input is "the parsed tree"
// needs the tree's type, and a second copy of that struct is a format that can silently fork.
#include <Engine/Core/Serialize/SceneFormat.hpp>

// The `.demat` payload, for the same reason: one step raises a MATERIAL rather than a scene, and its
// input is the engine's own struct so the file this tool writes is the file the engine reads.
#include <Engine/Assets/MaterialData.hpp>

// The `.deprefab` payload and its gate. A prefab's entities ARE Core::SceneSerialized::Entities - the
// same struct, written by the same ComponentRegistry - so it is raised by the SAME step chain rather
// than by a second one that would have to be kept in step by hand (И11). See MigratePrefab.
#include <Engine/Assets/Prefab/PrefabFormat.hpp>

#include "LegacyMaterialIds.hpp"

#include <Common/Core/Constants.hpp>

// EVERY REGISTRY BELOW IS AN std::array AND NONE IS A C ARRAY, WHICH IS A CORRECTNESS RULE HERE RATHER
// THAN A STYLE ONE. A registry of retirements is designed to reach ZERO rows; `T k[] = {}` cannot express
// that, because C++ has no zero-length array. Clang accepts one as a GNU extension and MSVC rejects it
// with C2466 plus a cascade from every loop that walks it, so the terminal state of these tables compiles
// on this machine and fails on Windows 35 minutes later - which happened twice on 2026-09-08, from two
// censuses that had each reached their goal of an empty register. A type that cannot express the success
// of its own structure makes someone keep one dead row for ever just to build.
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
    // read the generation runs every step on every file. They are read, never written: MigrateScene clears
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
        // v25 and earlier only.
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
        std::string                                               Name;
        std::vector<Assets::EntityData>                           Entities;
        Common::UUID                                              Root;
        // v25 and earlier only.
        std::optional<int> SceneVersion;
        std::optional<int> UnitVersion;
    };
    static_assert( rfl::named_tuple_t<PrefabData>::size() == rfl::named_tuple_t<Assets::PrefabData>::size() + 2,
                   "Assets::PrefabData gained or lost a member: mirror it in Migration::PrefabData" );

    // The engine's prefab, for the engine's writer (Assets::WritePrefabJson): everything but the two
    // pre-v26 integers, which a migrated prefab no longer states.
    [[nodiscard]] inline Assets::PrefabData ToEnginePrefab( const PrefabData& prefab )
    {
        return Assets::PrefabData{ prefab.Header, prefab.Name, prefab.Entities, prefab.Root };
    }

    // Schema generation of a .desce file, and what each step of it means:
    //
    //   absent (or 0) - written while the procedural sky still lived inside SkyboxComponent
    //   1             - the sky lives in its own "SkyAtmosphere" payload
    //   2             - the tonemapping operator is a scene property, and the file states which one
    //
    // Each step has its OWN constant and each migration is gated on its own, not on kSceneVersion. Gating
    // them all on the head would re-run every earlier migration the moment the head moved: raising the
    // head to 2 would have sent every v1 file in the repository back through the sky migration and
    // reported a schema move that did not happen.
    //
    // This is deliberately a SECOND integer and not UnitVersion. UnitVersion's contract - "bump this only
    // if the world unit changes again" - would couple two migrations that have nothing to do with each
    // other if reused: an old metres-era scene would be declared sky-migrated the moment someone re-saved
    // it for units, and vice versa.
    //   3             - the cloud noise volume is an ASSET, so the four bake settings are gone from the
    //                   component and from the file
    //   4             - a cloud layer names a SPECIES. The scalar cloud type, its variance and the two
    //                   fields that stated the shell by hand are gone: the species carries its altitudes
    //                   and the shell is computed from them
    //   5             - the species is an ASSET. The enumerator becomes a handle to a `.decloudtype`, and
    //                   the layer's own noise-volume slot moves into that type
    //   6             - a layer carries a SET of up to four kinds of cloud instead of one. `CloudType`
    //                   becomes `CloudType1`, the first of four slots; nothing else about a scene changes
    //                   and a one-type layer renders the sky it rendered before
    //   7             - the terrain's material is a `.demat` named by `Terrain.Material`, so the
    //                   `Material` component a terrain entity used to carry as its authoring channel is
    //                   gone from terrain entities
    //   8             - a material is named by a path RELATIVE to the assets root, like the three cloud
    //                   asset classes already were. The absolute paths the saver used to write carried one
    //                   developer's home directory into every scene in the repository
    //   9             - the scene's gravity is stated in CENTIMETRES per second squared, like every other
    //                   length in the file. 39 scenes carried the metre-era 9.81 under a centimetre stamp,
    //                   a hundred times too weak, and were only harmless because nothing read the field:
    //                   the physics world set its gravity from a literal instead. That literal is gone in
    //                   the same change, so the value now has to be right
    //  10             - a UI element states its visibility and its hit testing as two enums. The
    //                   `Interactable` / `RaycastTarget` booleans on `UILayout` are gone: they said the
    //                   same two things about the element alone, and could say nothing at all about its
    //                   sub-tree
    //  11             - `SSRMaxDistance` is stated in CENTIMETRES, like every other length in the file.
    //                   The field was authored under a metre-scale slider (Range 1..200, default 40) but
    //                   consumed by the shader as world units, so every saved value was a hundred times
    //                   shorter than its author meant
    inline constexpr int kSceneVersionSky             = 1;
    inline constexpr int kSceneVersionTonemap         = 2;
    inline constexpr int kSceneVersionCloudNoise      = 3;
    inline constexpr int kSceneVersionCloudSpecies    = 4;
    inline constexpr int kSceneVersionCloudType       = 5;
    inline constexpr int kSceneVersionCloudSet        = 6;
    inline constexpr int kSceneVersionTerrainMaterial = 7;
    inline constexpr int kSceneVersionMaterialPath    = 8;
    inline constexpr int kSceneVersionGravityUnits    = 9;
    inline constexpr int kSceneVersionUIVisibility    = 10;
    inline constexpr int kSceneVersionSSRUnits        = 11;
    //  12             - the cloud LOOK is a MATERIAL (O1, D-35). Thirty-three fields leave the
    //                   VolumetricCloud payload for a `.demat` on the Volume-domain CloudRaymarch shader:
    //                   the species slots, weather, placement, layout, per-sample detail and lighting.
    //                   A scene that stated any of them gets a material file written beside the assets
    //                   root carrying exactly the stated values; a scene that stated none names no
    //                   material and renders the schema defaults, which are the old component defaults
    //                   digit for digit
    inline constexpr int kSceneVersionCloudMaterial = 12;
    //  13             - a scene no longer states what the VIEWPORT is drawing on top of it. The ten debug
    //                   visualization keys leave the Settings block for Graphic::DebugViewState, which is
    //                   per-SceneRenderer and never serialized: ShowGrid, ShowColliders, ShowBoundingBoxes,
    //                   BoundingBoxColor, BoundingBoxLineWidth, WireframeMode, ShowNormals, LightingDebug,
    //                   ShadowDebug and DeferredDebug
    inline constexpr int kSceneVersionDebugView = 13;

    //  14             - the keys this project RETIRED are gone from the files. Since K11 the engine's saver
    //                   PRESERVES a key it does not declare (Engine/Core/Serialize/ForeignKeys.hpp), which
    //                   is what stops one build deleting another's field - and it means a key we removed on
    //                   purpose would otherwise be carried forever by every file that ever held it. So
    //                   removal moved here, where it belongs: the step names each dead key, drops it, and
    //                   this bump makes the run compulsory. `EnableSSGI` is the first row - the boolean was
    //                   replaced by the GlobalIllumination mode on 2026-08-06 and had been riding along in
    //                   the files for a month with nothing reading it.
    inline constexpr int kSceneVersionRetiredKeys = 14;

    //  15             - a scene no longer states what a MACHINE can afford. `AA`, `MeshLOD`,
    //                   `TextureFilterMode`, `Anisotropy` and `CloudQualityTier` leave the Settings block
    //                   for Common::Settings::MachineSettings, which the editor reads from
    //                   `~/.desertengine/machine.json` and a packaged game from the player's own
    //                   directory. They are five more rows of kRetiredKeys rather than a step of their
    //                   own - see the note on that table, and on why the retirement pass is the one thing
    //                   in this file not gated on its own number
    inline constexpr int kSceneVersionMachineQuality = 15;

    //  16             - a script slot names its `.lua` with a ROOT-TAGGED KEY (`assets:Scripts/x.lua`)
    //                   instead of the rooted path the editor happened to be standing in
    //                   (`Resources/Assets/Scripts/x.lua`). The rooted spelling was the last content
    //                   reference in a scene that did not survive packaging: a packaged game remaps the
    //                   assets root to <package>/Assets/, so the stored string named a directory that
    //                   does not exist there. Measured on a mounted archive by I8 - the stored spelling
    //                   gave Exists=0, the same file through the scripts root gave Exists=1. The JSON
    //                   field is renamed with the value, `Path` -> `ScriptKey`, because the value is no
    //                   longer a path and a name that says otherwise is how it got used as one
    inline constexpr int kSceneVersionScriptRoot = 16;

    //  17             - a reference to a SERVICE-REGISTRY asset - a font, a vector icon, a video - is a
    //                   ROOT-TAGGED KEY too, for the reason step 16 gives about scripts. These three are
    //                   not AssetManager assets: FontService / IconService / VideoService each own a
    //                   handle<->path registry, so a scene stored the PATH the registry held and that
    //                   path does not survive the packager rebasing content under <package>/Assets/. It
    //                   went unseen because every value this repository ships names the ENGINE trees
    //                   Resources/Fonts and Resources/Icons, which are never remapped; a `.ttf` or
    //                   `.svg` dropped in from the project's own assets tree - which both scan roots
    //                   accept - took the broken route. Four sites: `Text.FontPath` (renamed to
    //                   `Text.Font` with the value, since it is no longer a path), `UIText.Font`,
    //                   `UIIcon.Icon` and `UIPanel.Video`
    inline constexpr int kSceneVersionServiceAssetRoot = 17;

    //  18             - a terrain no longer carries the PROCEDURAL GRASS GENERATOR's settings (Г25).
    //                   `EnableGrass`, `GrassDensity`, `GrassHeight`, `GrassBladesPerClump`,
    //                   `GrassWidth` and `GrassBrightness` described blades the renderer SYNTHESIZED -
    //                   a GPU cull compute over a seeded grid feeding an indirect instanced draw of
    //                   hashed geometry. Grass is a MESH ASSET from here on, scattered by the Foliage
    //                   paint tool onto an InstancedStaticMeshComponent, so there is nothing left to
    //                   generate and nothing for these six numbers to drive.
    //
    //                   FIVE ARE DROPPED AND ONE IS CARRIED, and that split is the whole step.
    //                   `EnableGrass` was not only the generator's switch: it was ALSO the gate on the
    //                   terrain's grass SPLAT LAYER in Terrain.shader (LayerModes.w), which is a ground
    //                   TEXTURE and survives the cut. The layer's own authored mode, `GrassMode`, was
    //                   reflected, serialized, shown in Details and read by nothing - so the value a
    //                   scene actually stated about its grass ground lived in the generator's switch.
    //                   The step moves it where it belongs: EnableGrass=false becomes GrassMode=Off,
    //                   EnableGrass=true becomes GrassMode=Auto, and a file already stating GrassMode
    //                   keeps what it states. Without the carry, every terrain that said "no grass"
    //                   would come back from this migration with a green lawn.
    inline constexpr int kSceneVersionGrassGeneration = 18;

    //  19             - a scene no longer states the WIND (Г26). `WindDirection`, `WindStrength` and
    //                   `WindTurbulence` were authored in all 86 scenes and read by NOTHING: their one
    //                   consumer was the procedural grass generator that step 18 removed, and the three
    //                   fields were kept then so that the next wind-driven renderer would find them
    //                   waiting. There was no next one, and a field waiting for a future reader is
    //                   indistinguishable from a forgotten one - §1.3's dead setting. Wind returns with
    //                   the thing it moves, and with a unit chosen by whatever reads it. Three more rows
    //                   of kRetiredKeys rather than a step of its own - see the note on that table.
    inline constexpr int kSceneVersionWindRetired = 19;
    //  19             - AUTHORED TEXT CAN NOW BE A STRING-TABLE KEY, and the two are told apart by their
    //                   FORM: a leading '#' means "the rest of this is a key" (Ю15,
    //                   Engine/Localization/LocalizedText.hpp). That makes one spelling change meaning -
    //                   a label whose author really typed a hash first, which is legal today and reads
    //                   as a key tomorrow - so every authored string that a leading hash would now
    //                   capture is escaped to '##'.
    //
    //                   FOUR SITES, and they are exactly the authored strings that reach a reader's eye:
    //                   `UIText.Text`, `UIInputField.Placeholder`, each `;`-separated item of
    //                   `UIDropdown.Options`, and the world-space `Text.Text`. `UIInputField.Text` is
    //                   deliberately NOT one of them: it is what a player typed, it is never resolved,
    //                   and escaping it would put a hash into somebody's own words.
    //
    //                   THE SAME BUMP RETIRES `UIBinding.Format` - an author-typed printf format that
    //                   was handed straight to std::snprintf with a double (typing `%s` in the Details
    //                   panel was undefined behaviour) and that formatted every bound number in the C
    //                   locale whatever language the reader was in. Its job is the string table's now.
    //                   It is a row of kRetiredKeys rather than part of this step, for the reason that
    //                   table's own note gives: it decides nothing from the value, it only drops.
    // TWENTY, NOT NINETEEN, AND THE COLLISION IS WORTH RECORDING. Two branches each added a step and each
    // numbered it 19: Г26 retired the three wind keys, Ю15 escaped a leading '#' so a literal could not be
    // read as a string-table key. Both were correct against the head they branched from. A schema version
    // is not a label for "the newest thing I did" -- it is a POSITION IN A SEQUENCE, and two steps sharing
    // one number means a file at 19 cannot say which of the two it has had.
    inline constexpr int kSceneVersionTextKeySigil = 20;

    //  21             - the anim graph is an ASSET. The whole state machine used to travel as a JSON STRING
    //                   inside the entity ("Animation" -> GraphJson), which is why two characters could not
    //                   share one walk graph and why copying one copied a blob. Each non-empty blob becomes
    //                   an `AnimGraphs/<name>.danimgraph` file and the entity names it by a path relative to
    //                   the assets root, exactly as ControlRigData::Rig already did.
    //
    //                   THE WINDOW THIS STEP CLOSES WAS ALREADY SHUT. `Components.hpp` said the identity
    //                   question could wait because "today the repository has zero such scenes"; measured
    //                   on 2026-09-16 there were 10 scenes carrying an "Animation" block and 6 non-empty
    //                   GraphJson blobs across three of them, and all three are witness scenes a suite
    //                   reads. A comment asserting a guarantee the tree does not hold is a defect class
    //                   this project has named nine times; this step is what made that one true again.
    inline constexpr int kSceneVersionAnimGraphAsset = 21;

    //  22             - A MESH BUILT IN THE EDITOR IS SAVED AS ITS EditMesh. `StaticMesh.CustomVertices` +
    //                   `CustomIndices` - the render buffer, and only Position/Normal/TexCoord of it - become
    //                   `StaticMesh.EditMesh` (Engine/Geometry/EditMeshSerialization.hpp): topology, polygroups,
    //                   material IDs and every attribute layer. The v21 form lost the tangent frame (it came
    //                   back uninitialised) and folded every submesh into one; the render mesh is now derived
    //                   from the saved source on load (M4). The tracked corpus had ZERO such blocks when this
    //                   step was written (measured 2026-09-23), so it exists for autosaves and scenes outside
    //                   the repository.
    inline constexpr int kSceneVersionEditMesh = 22;

    //  23             - THE PROCEDURAL TERRAIN IS GONE; ITS RELIEF IS BAKED INTO A LANDSCAPE (owner decision O2,
    //                   2026-09-24). A `Terrain` block (value-noise fBm evaluated by the tessellation shader)
    //                   becomes a `Landscape` root + `LandscapeMaterial` on the same entity and a grid of
    //                   `LandscapeTile` entities whose heights are the same fBm, evaluated on the CPU at every
    //                   sample and written as `.dlht` files beside the file (MigrateProceduralTerrainV22ToV23).
    //                   The tracked corpus had SIX such blocks in four scenes (measured 2026-09-24).
    inline constexpr int kSceneVersionProceduralTerrain = 23;
    //  24             - A TEXTURE IS NAMED BY ITS ASSET (AF3e). The per-texture cooked file is gone: a string
    //                   `cooked:Textures/<p>.tex` names nothing any more, and the texture it meant is the asset
    //                   `assets:Textures/<p>.detex` its source was imported into
    //                   (MigrateTextureAssetRefsV23ToV24). The tracked corpus had TWO such strings, both in
    //                   UI_SpriteSlots (measured 2026-09-24).
    inline constexpr int kSceneVersionTextureAssetRefs = 24;
    //  25             - RECORDS SORTED BY ID, SIBLING ORDER STATED (AF6c, decision D3). A .desce lists its
    //                   entities by ascending id, so the file order is a function of the entity SET; the
    //                   order among siblings moves into each record's `siblingIndex`, stamped from the order
    //                   the v24 loader produced so no hierarchy moves (MigrateSiblingOrderV24ToV25). Scenes
    //                   only: a .deprefab keeps hierarchy order and is untouched by this step.
    inline constexpr int kSceneVersionSiblingOrder = 25;
    //  26             - THE TEXT HEADER (AF6g). A .desce / .deprefab opens with the text asset header
    //                   (Common/Content/TextAssetHeader.hpp): kind, GUID, and the two generations as SCNE /
    //                   UNIT - the top-level SceneVersion / UnitVersion integers are gone. A file without a
    //                   GUID gets one derived from its path (MigrationGuidForPath), so re-running the tool
    //                   over the same tree states the same identities. No payload changes.
    inline constexpr int kSceneVersionTextHeader = 26;
    //  27             - MATERIAL SLOTS NAME A GUID (AF7). StaticMesh / SkinnedMesh / InstancedStaticMesh
    //                   `MaterialGuids` held the material's MATL v1 u64 MaterialId, a number MATL 2 no longer
    //                   states anywhere, so the loader missed on it every time and fell back to the path in
    //                   silence. Each id becomes the 32-hex header GUID of the `.demat` that stated it, through
    //                   the old-id register (LegacyMaterialIds.hpp); 0 becomes "" (an empty slot). An id the
    //                   register does not know REFUSES the file, naming it (MigrateMaterialGuidsV26ToV27).
    inline constexpr int kSceneVersionMaterialGuids = 27;
    //  28             - A MESH IS NAMED BY ITS HEADER GUID (AF7o). StaticMesh / SkinnedMesh / InstancedStaticMesh
    //                   `MeshGuid` held the mesh's PATH-derived u64 handle (FromKey of its stable key), so a
    //                   moved file named a different asset. Each becomes the 32-hex GUID the mesh file's v3
    //                   header states. The number must be the handle of the file `MeshPath` names, and that
    //                   file must state a GUID; anything else REFUSES the file, naming the block - in entity
    //                   records AND in prefab-override records (MigrateMeshGuidsV27ToV28).
    inline constexpr int kSceneVersionMeshGuids = 28;

    // The last step this tool knows and the generation the engine requires are ONE number, and this is
    // where that is checked. If a schema step is ever added here without raising Core::kSceneVersion, the
    // tool would stamp files at a version the loader refuses - every scene in the repository would stop
    // opening at once, and the file that caused it would look correct in isolation.
    static_assert( kSceneVersionMeshGuids == kSceneVersion,
                   "the last migration step and the engine's required scene version must be the same "
                   "generation - raise Core::kSceneVersion in Engine/Core/Serialize/SceneFormat.hpp" );

    // Number of reflected fields on ECS::SkyAtmosphereData. The migration needs it to say how many NEW
    // fields were left at their C++ default, and it must not reach for the reflection registry to find out
    // (that is a global, and this function is pure). So the number is stated here and a test asserts it
    // against the registry - which turns "a field was added to the component" into a failing test instead
    // of a silently wrong counter.
    // 47 = the 24 fields of the artistic-gradient era + the 22 physical-atmosphere fields (SkyModel and
    // the Physical Atmosphere / Rayleigh / Mie / Absorption / Art Direction groups) added 2026-08-14,
    // + Aerial Perspective Distance, which the camera aerial-perspective volume added 2026-08-15.
    inline constexpr int kSkyAtmosphereFieldCount = 47;

    // What MigrateSkyV0ToV1 actually did, returned rather than logged, so the pure function stays pure and
    // the LOADER is the one that speaks (the counters are meaningless unless someone names the scene).
    //
    // Every reflected field of SkyAtmosphereData ends up in exactly one of the three counters, so
    // FieldsCarried + FieldsDefaulted + FieldsRejected == kSkyAtmosphereFieldCount x Entities.
    struct SkyMigrationReport
    {
        int Entities        = 0; // entities that carried a "Skybox" payload and gained a "SkyAtmosphere" one
        int FieldsCarried   = 0; // mapped fields found in the old payload and copied (or converted)
        int FieldsDefaulted = 0; // new fields with no value to carry - they keep their C++ default
        int FieldsRejected  = 0; // mapped fields present but unusable (wrong JSON type, wrong arity, NaN)
    };

    // Raises the sky half of a scene from schema v0 to v1: reads each entity's old "Skybox" payload and
    // inserts a "SkyAtmosphere" payload beside it. PURE - no GPU, no filesystem, no global state; the only
    // side effect beyond the argument is a LOG_WARN per rejected value, which DC 1.4 requires (a value we
    // silently dropped is a value nobody will ever find again).
    //
    // It runs on the PARSED TREE, before a single entity exists, and that is the whole point: once
    // SkyboxComponent lost its sky fields, the load loop - which iterates the component REGISTRY, not the
    // file - has nowhere to put the old values, so a migration over the live ECS scene would find them
    // already gone.
    //
    // Idempotent: an entity that already has a "SkyAtmosphere" payload is skipped untouched, so a second
    // run reports all zeros and leaves the tree byte-identical.
    //
    // SHELF LIFE: this function upgrades v0 to v1 and nothing else. It is not "support for the old format";
    // when v2 arrives it gets its own v1 -> v2 function, and this one is deleted once no v0 file remains.
    SkyMigrationReport MigrateSkyV0ToV1( std::vector<Assets::EntityData>& entities );

    // What MigrateMetresToUnits touched, returned rather than logged, for the same reason as above: the
    // function is pure and the caller is the one who knows which file this was.
    struct UnitMigrationReport
    {
        int Entities = 0; // entities that had at least one number rescaled
        int Values   = 0; // individual numbers multiplied by 100 (a vec3 counts as one)
        int Rejected = 0; // fields present but unusable (not a finite number / wrong arity) - left alone
    };

    // Raises a metres-era scene to centimetre world units: every LENGTH the scene file owns is multiplied
    // by 100. PURE - no GPU, no filesystem, no global state; a LOG_WARN per rejected value is the only
    // side effect beyond the arguments.
    //
    // It runs on the PARSED TREE, like the sky migration and unlike the version that used to live in
    // SceneSerializer.cpp and walked the live ECS scene AFTER the load. Three things follow from that, and
    // all three are deliberate:
    //
    //  1. Only keys that are PRESENT are scaled. The live version scaled the TransformComponent every
    //     entity is created with, so an entity whose file entry carries no "Scale" - every UI entity in
    //     MainMenu.desce, for one - had its default (1,1,1) turned into (100,100,100). A default is not a
    //     length somebody authored in metres.
    //  2. Prefab CONTENTS are not touched. The scene file owns where a prefab was placed; the prefab file
    //     owns what is inside it. The live version scaled the instantiated children too, so the same
    //     prefab came out a hundred times bigger in an old scene than in a new one.
    //  3. The values are already in centimetres before a single component is deserialized, so nothing
    //     downstream can observe the metres-era numbers.
    //
    // Idempotent by construction: it does not decide anything from the values themselves, so running it on
    // a scene that has already been raised is a second x100 - which is exactly why the caller must gate it
    // on UnitVersion, and why MigrateScene() below is the only supported way to call it.
    //
    // SHELF LIFE: this raises UnitVersion 0 to 1 and nothing else. When the world unit changes again it
    // gets a 1 -> 2 successor; this function is deleted once no unstamped file remains.
    UnitMigrationReport MigrateMetresToUnits( std::vector<Assets::EntityData>& entities,
                                              std::optional<rfl::Generic>&     settings );

    // What MigrateTonemapperV1ToV2 did, returned rather than logged, for the same reason as the two
    // above: the function is pure and only the caller knows which file this was.
    struct TonemapMigrationReport
    {
        bool SettingsCreated = false; // the file carried no "Settings" block at all - one was made for it
        bool OperatorPinned  = false; // "Tonemapper" was written; false when the file already stated one
    };

    // Raises a scene from schema v1 to v2: writes the tonemapping operator the file was AUTHORED under
    // into its settings block. PURE - no GPU, no filesystem, no global state; a LOG_WARN on a settings
    // payload that is not an object is the only side effect beyond the argument.
    //
    // WHY IT WRITES REINHARD AND NOT THE NEW DEFAULT. Decision D-10 made ACES the default operator. A
    // default change is the one change that silently rewrites every file which never mentioned the
    // setting: an absent key means "the C++ default", so on the day the default moved, every existing
    // scene would have been re-graded through a curve its author never chose, with nothing in the file
    // to say so. Exposure and White Point in those scenes were dialled in by eye on extended Reinhard -
    // the cloud demo carries an exposure of 0.22 against a sun of 22 - so this is not a subtle drift, it
    // is the whole grade. Pinning Reinhard makes the operator EXPLICIT at the value that leaves each
    // picture exactly as it was, and moving a scene to ACES then becomes a deliberate, visible edit.
    // (Ten of the eleven repository scenes were moved that way, by this same task. The eleventh,
    // Fog_Showcase, was measured breaking under ACES because its EXPOSURE was authored for Reinhard, and
    // so it stays on Reinhard until it is re-exposed - which is the case the operator is a per-scene
    // property for in the first place. Numbers in Docs/Clouds/CALIBRATION.md, T-ACES section.)
    //
    // A scene with no "Settings" block at all - MainMenu.desce is one - gets a block containing only the
    // operator. Every other field stays absent, which is how the reflection serializer spells "keep the
    // C++ default", so no value is invented for it.
    //
    // Idempotent: a tree that already states an operator is left byte-identical, whichever operator that
    // is. A scene may be re-read (undo, a second load) after it was raised, and overwriting an operator
    // the user has since chosen would be worse than not migrating at all.
    //
    // SHELF LIFE: this raises v1 to v2 and nothing else. It is not "support for the old format"; when v3
    // arrives it gets its own v2 -> v3 successor, and this one is deleted once no v1 file remains.
    TonemapMigrationReport MigrateTonemapperV1ToV2( std::optional<rfl::Generic>& settings );

    // What MigrateCloudNoiseV2ToV3 removed, returned rather than logged, for the same reason as the three
    // above.
    struct CloudNoiseMigrationReport
    {
        int Entities      = 0; // entities carrying a "VolumetricCloud" payload that was touched
        int FieldsDropped = 0; // individual bake settings deleted (0..4 per entity)
    };

    // Raises a scene from schema v2 to v3: deletes "WeatherSeed", "WeatherOctaves", "DetailSeed" and
    // "DetailOctaves" from every "VolumetricCloud" payload.
    //
    // WHY DELETE AND NOT CARRY. Those four numbers parameterised a GPU bake that no longer exists; the
    // noise volume is an asset now, and the seed and periods that make one live in the volume's own header
    // (Engine/Assets/CloudNoiseVolume.hpp). There is nowhere on the component to carry them TO. Turning
    // each scene's seed into a freshly baked volume would have been the other option and was rejected: a
    // 128^3 bake costs tens of seconds, it would have written an 8 MiB file per scene for a value nobody
    // authored deliberately, and the volumes would differ between scenes for no reason an artist could see.
    // The scenes therefore adopt the default volume, and the entity keeps every parameter that still means
    // something.
    //
    // Left BEHIND on purpose: the new "NoiseVolume" slot is not written. An absent key is how the
    // reflection serializer spells "keep the C++ default", and the C++ default is an empty handle, which is
    // exactly "use the built-in default volume". Writing a path here would invent a choice for the artist.
    //
    // PURE - no GPU, no filesystem, no global state, and not even a log line: the counters go back to the
    // loader, which is the one that knows which file this was.
    //
    // Idempotent: a payload with none of the four keys is left byte-identical and reports zero.
    //
    // SHELF LIFE: this raises v2 to v3 and nothing else. It is deleted once no v2 file remains.
    CloudNoiseMigrationReport MigrateCloudNoiseV2ToV3( std::vector<Assets::EntityData>& entities );

    // What MigrateCloudSpeciesV3ToV4 did to one file.
    struct CloudSpeciesMigrationReport
    {
        int Entities      = 0; // entities carrying a "VolumetricCloud" payload that was touched
        int FieldsDropped = 0; // "LayerBottomAltitude", "LayerThickness", "CloudTypeVariance" (0..3 each)
        int SpeciesSet    = 0; // entities whose old scalar "CloudType" became a named species
    };

    // Raises a scene from schema v3 to v4: the vertical profile stopped being one analytic curve driven by
    // a scalar and became a per-SPECIES table, so the file has to name a species instead of a number.
    //
    // WHAT IS DROPPED AND WHY NOTHING IS CARRIED FROM IT.
    //
    //   * "LayerBottomAltitude", "LayerThickness" - the shell is now the union of the altitude ranges of
    //     the species in the layer and is computed by Graphic::PackCloudParams. An authored shell and a
    //     species' own altitudes are two numbers obliged to agree, which is the defect class this whole
    //     move removes; carrying the authored pair forward would reintroduce it under a new name.
    //   * "CloudTypeVariance" - it mixed noise into the scalar so that neighbouring clouds would not all
    //     reach the same ceiling. The table's second axis is the placement pattern's own value, which
    //     answers the same question with height that CORRELATES with how much cloud is there. There is
    //     nothing on the component for it to become.
    //
    // WHAT IS CARRIED. "CloudType" was a scalar from "flat sheet low in the layer" to "tall heaped
    // cloud", and the library is ordered along exactly that axis, so the scalar picks the species by
    // quarters: below 0.25 Stratus, below 0.55 CumulusMediocris, below 0.85 CumulusCongestus, and above
    // it Cumulonimbus. The boundaries are placed so that the component's own former default of 0.6 lands
    // on CumulusCongestus, which is the species the new default names - a scene that carried the default
    // therefore comes out of the migration looking like what it was.
    //
    // A payload with no "CloudType" at all keeps the C++ default by NOT writing a species: an absent key
    // is how the reflection serializer spells "leave it alone", and inventing Stratus for a file that
    // never said anything would be a guess about intent.
    //
    // PURE - no GPU, no filesystem, no global state. The counters go back to the loader, which is the one
    // that knows which file this was.
    //
    // Idempotent: a payload with none of the four keys is left byte-identical and reports zero.
    //
    // SHELF LIFE: this raises v3 to v4 and nothing else. It is deleted once no v3 file remains.
    CloudSpeciesMigrationReport MigrateCloudSpeciesV3ToV4( std::vector<Assets::EntityData>& entities );

    // What MigrateCloudTypeV4ToV5 did to one file.
    struct CloudTypeMigrationReport
    {
        int Entities     = 0; // entities carrying a "VolumetricCloud" payload that was touched
        int TypesSet     = 0; // entities whose "Species" enumerator became a "CloudType" asset handle
        int VolumesLost  = 0; // entities that named a noise volume the component no longer carries
        int FieldsBroken = 0; // "Species" values that were not a usable integer - the layer keeps default
    };

    // Raises a scene from schema v4 to v5: the kind of cloud a layer is made of stopped being an
    // enumerator compiled into the engine and became an ASSET an artist can author, name and ship.
    //
    // WHAT IS CARRIED, AND WHY IT IS EXACT RATHER THAN APPROXIMATE. The four enumerators of
    // the deleted Graphic::CloudSpecies - stratus, cumulus mediocris, cumulus congestus, cumulonimbus -
    // ship as four files under Resources/Assets/Clouds/Types carrying the SAME twelve numbers T0 compiled
    // in. So the migration is a rename, not a reinterpretation: what it writes is the PATH of that file,
    // relative to the assets root, which is the form Core::MakeAssetResolver reads an asset field back
    // from — and it is composed from Assets::CloudTypeAssetRelativePath rather than spelt out, so this
    // function stays pure and cannot disagree with the directory the preloader scans.
    // Desert/Tests/Engine/CloudType asserts that the four files hold T0's numbers. A scene therefore comes
    // out of this migration rendering the sky it went in with.
    //
    // WHAT IS DROPPED. "NoiseVolume" - the layer's own slot for the 3D noise. It moved onto the cloud TYPE,
    // because the character of a cloud's edge is a property of the kind of cloud rather than of the weather
    // it is having, and the component must not keep a second copy (§4.2). A scene that named one cannot be
    // carried automatically: the handle it holds identifies a `.dcnv`, and turning that into a NEW cloud
    // type file would mean this function writing to disk, which the contract's "migration is a pure
    // function" forbids for good reason. Every such entity is COUNTED and named in the loader's log so the
    // artist is told which layer to re-point, rather than finding out from a sky that lost its edge. No
    // scene in this repository carries one, which is what makes the loud drop affordable.
    //
    // PURE - no GPU, no filesystem, no global state. The counters go back to the loader, which is the one
    // that knows which file this was.
    //
    // Idempotent: a payload with neither key is left byte-identical and reports zero.
    //
    // SHELF LIFE: this raises v4 to v5 and nothing else. It is deleted once no v4 file remains.
    CloudTypeMigrationReport MigrateCloudTypeV4ToV5( std::vector<Assets::EntityData>& entities );

    // What MigrateCloudSetV5ToV6 did to one file.
    struct CloudSetMigrationReport
    {
        int Entities     = 0; // entities carrying a "VolumetricCloud" payload that was touched
        int SlotsCarried = 0; // "CloudType" keys that became "CloudType1"
        int SlotsEmpty   = 0; // of those, the ones whose value was the empty handle
    };

    // Raises a scene from schema v5 to v6: a layer stopped carrying ONE kind of cloud and started carrying
    // a SET of up to four.
    //
    // WHAT IT DOES, AND WHY IT IS A RENAME AND NOTHING MORE. The single `CloudType` key becomes
    // `CloudType1`, the first of four slots, and the other three are left absent — which the component
    // reads as the empty handle they default to. A scene that named one kind of cloud comes out of this
    // naming the same kind of cloud in the first slot, and renders the sky it went in with: the union of a
    // one-element set is that element, and the slot's placement field is the one T1 read (slot 0 takes the
    // zero decorrelation offset for exactly this reason).
    //
    // WHY THE KEY MOVES AT ALL, when leaving it named `CloudType` would have been a migration of nothing.
    // Because four slots that are not called the same thing is the point: `CloudType` beside `CloudType2`,
    // `CloudType3` and `CloudType4` reads as one field of a different kind sitting next to three of
    // another, and the first person to add a fifth would have had to guess which end it belonged at. The
    // rename costs one function and makes the set look like a set in the file as well as in the panel.
    //
    // PURE - no GPU, no filesystem, no global state. The counters go back to the loader, which is the one
    // that knows which file this was.
    //
    // Idempotent: a payload with no "CloudType" key is left byte-identical and reports zero.
    //
    // SHELF LIFE: this raises v5 to v6 and nothing else. It is deleted once no v5 file remains.
    CloudSetMigrationReport MigrateCloudSetV5ToV6( std::vector<Assets::EntityData>& entities );

    // What MigrateTerrainMaterialV6ToV7 did to one file.
    struct TerrainMaterialMigrationReport
    {
        int Entities = 0; // terrain entities that carried a "Material" component, and no longer do
        int Params   = 0; // parameter values those components held
        int Textures = 0; // texture bindings those components held

        // The names of everything above, in the order it was found, so the loader can print WHAT has to be
        // re-authored rather than only how much. A count alone would make this a silent default in all but
        // arithmetic (DC 1.4): "3 values dropped" tells nobody that the grass texture was one of them.
        std::vector<std::string> DroppedNames;
    };

    // Raises a scene from schema v6 to v7: the terrain's material stopped being an ECS::MaterialComponent
    // authored on the terrain entity and became a `.demat` that `Terrain.Material` names by handle.
    //
    // WHAT IT DOES. For every entity carrying BOTH a "Terrain" and a "Material" payload, the "Material"
    // payload is removed and everything it held is reported by name. An entity with a "Material" and no
    // "Terrain" is left alone: there the component is the runtime/Lua `setMaterialParam` channel and the
    // legacy-scene compatibility path, which is all it claims to be and all it now is.
    //
    // WHY IT REMOVES RATHER THAN CARRIES. The new form of these values is a file — a `.demat` with the
    // Terrain shader and these parameters in it — and this function is pure: no filesystem, so it cannot
    // create one, and there is no second place in the scene tree for a material's values to live that is
    // not the inline authoring this task exists to delete. Writing them anywhere else would be the
    // "deprecated but still read" shape of DC 4.1 wearing a migration's clothes.
    //
    // So it drops them, LOUDLY: every name goes back to the loader, which prints them with the scene and
    // tells the reader to re-author them on a terrain material. That is the honest trade and it is a small
    // one — the values are three splat textures and a tint, they were only ever reachable through one
    // editor widget, and no scene in this repository has any (the sweep in the migration's own test suite
    // is what keeps that true).
    //
    // PURE - no GPU, no filesystem, no global state. The counters go back to the loader, which is the one
    // that knows which file this was.
    //
    // Idempotent: an entity with no "Material" payload is left byte-identical and reports zero.
    //
    // SHELF LIFE: this raises v6 to v7 and nothing else. It is deleted once no v6 file remains.
    TerrainMaterialMigrationReport MigrateTerrainMaterialV6ToV7( std::vector<Assets::EntityData>& entities );

    // What MigrateMaterialPathV7ToV8 did to one file.
    struct MaterialPathMigrationReport
    {
        int Entities = 0; // entities in which at least one material path was rewritten
        int Paths    = 0; // individual path strings rewritten to the assets-root-relative form

        // Paths that could NOT be made relative because they do not lie under the assets root at all, and
        // their entity's tag. Left exactly as they were - a file genuinely outside the project has no
        // project-relative form to have - and NAMED, because a scene that carries one still does not open
        // on another machine and a count alone would not say which slot to re-point (DC 1.4).
        std::vector<std::string> OutsideNames;
    };

    // Raises a scene from schema v7 to v8: a material stops being named by the ABSOLUTE path the saver
    // wrote and is named relative to the assets root, which is the form Core::MakeAssetResolver reads back.
    //
    // WHY. `MakeAssetResolver::ToPath`'s MaterialAsset branch wrote `asset->GetMetadata().Filepath` verbatim.
    // With a project open every content root is absolute (Constants::Path::SetProjectRoot), so a scene
    // re-saved in the editor took whoever saved it home directory into the repository: 22 distinct
    // `/Users/<somebody>/.../Materials/*.demat` strings across 42 of the 51 scenes shipped here, none of
    // which names anything on any other machine. The three cloud asset classes went relative for exactly
    // this reason and say so at their branches; this applies the decision already taken to the fourth.
    //
    // WHAT IT REWRITES. The four places a scene can name a material: `MaterialPaths` on `StaticMesh`,
    // `InstancedStaticMesh` and `SkinnedMesh`, and `Material` on `Terrain`.
    //
    // HOW, WITHOUT A FILESYSTEM. std::filesystem::relative() consults the disk (it canonicalises both
    // sides), and this function may not. So the rewrite is LEXICAL: the path's components are searched for
    // the LAST occurrence of `assetsRoot`'s own component sequence, and everything after it is kept.
    // That deliberately makes the answer independent of how the root is spelled - `Resources/Assets/` and
    // `/Users/x/Proj/Editor/Resources/Assets/` both reduce
    // `/Users/x/Proj/Editor/Resources/Assets/Materials/M.demat` to `Materials/M.demat` - which is what lets
    // the editor (working directory `Editor/`) and Tools/SceneMigrator (working directory the repository
    // root) produce the same file from the same input.
    //
    // The root is a PARAMETER and not `Constants::Path::ASSETS_PATH` read from inside, so the function has
    // no global to disagree with and a test can drive it with a root of its own.
    //
    // Idempotent: a path already relative to the root contains no `assetsRoot` sequence to strip, so a
    // second run leaves the tree byte-identical and reports zero. An empty string (which is what ToPath
    // writes for a slot whose handle resolves to nothing) is left alone rather than turned into ".".
    //
    // PURE - no GPU, no filesystem, no global state. The counters go back to the loader, which is the one
    // that knows which file this was.
    //
    // SHELF LIFE: this raises v7 to v8 and nothing else. It is deleted once no v7 file remains.
    MaterialPathMigrationReport MigrateMaterialPathV7ToV8( std::vector<Assets::EntityData>& entities,
                                                           const std::filesystem::path&     assetsRoot );

    // What MigrateGravityUnitsV8ToV9 did to one file.
    struct GravityUnitsMigrationReport
    {
        bool  Found  = false; // the Settings block named a Gravity at all
        bool  Scaled = false; // it was the metre-era value and was multiplied by 100
        bool  Tidied = false; // it was already centimetres, but carried the float noise of an earlier x100
        float Before = 0.0F;
        float After  = 0.0F;

        // Set when the value is neither recognisably metre-era nor recognisably centimetre-era. It is left
        // EXACTLY as it is and named, because guessing which one it meant is the silent substitution §1.4
        // forbids: a deliberately weak gravity and a forgotten metre-era one look identical to a threshold.
        bool Unrecognised = false;
    };

    // Restates the scene's gravity in centimetres per second squared.
    //
    // WHY THIS IS NOT A UNITVERSION STEP even though it is a units fix. UnitVersion is already at its
    // current generation on every one of these files - the metres-to-centimetres pass ran and stamped them
    // - and this key was simply not in its list. Reusing UnitVersion would mean claiming the world unit
    // changed again, which would send every length in every scene through a second x100 (see the header's
    // note on why the sky and unit steps were deliberately given separate integers).
    //
    // WHY IT MATCHES VALUES INSTEAD OF SCALING EVERYTHING. Both populations are in the tree at once: 39
    // files at the metre-era 9.81 and 6 already at ~981. A blanket x100 would put the correct ones at
    // 98100. So the two Earth values are recognised exactly (within 0.01) and everything else is refused
    // and reported rather than guessed at - a scene authored at 50 cm/s^2 for a low-gravity level must not
    // be "corrected" to 5000.
    //
    // Also normalises 981.0000419616699 - which is precisely 9.8100004196167 x 100, the arithmetic
    // signature of the earlier pass - back to a clean 981, so the repository stops carrying the rounding
    // of a migration in its data.
    //
    // PURE - no GPU, no filesystem, no global state.
    //
    // SHELF LIFE: this raises v8 to v9 and nothing else. It is deleted once no v8 file remains.
    GravityUnitsMigrationReport MigrateGravityUnitsV8ToV9( std::optional<rfl::Generic>& settings );

    // What MigrateUIVisibilityV9ToV10 did to one file.
    struct UIVisibilityMigrationReport
    {
        int Entities     = 0; // entities carrying a "UILayout" payload that was touched
        int FlagsDropped = 0; // "Interactable" / "RaycastTarget" keys removed (0..2 per entity)
        int HitTestSet   = 0; // entities that gained an explicit "HitTest" because a flag was off

        // Keys that were present but were not a boolean at all. Removed like the rest - the field they
        // named no longer exists - and NAMED rather than counted, because "1 value dropped" does not tell
        // anyone which element stopped blocking clicks (DC 1.4). The element keeps UIHitTest::All.
        std::vector<std::string> BrokenNames;
    };

    // Raises a scene from schema v9 to v10: an element's hit testing stops being two booleans on
    // `UILayout` and becomes one enum, beside a second enum for its visibility.
    //
    // WHICH OLD FLAG BECAME WHICH VALUE, and this is the whole of the conversion:
    //
    //   * `RaycastTarget = false` -> `HitTest = 1` (ChildrenOnly). Identical behaviour: the element is
    //     transparent to the pointer while its children are not. It is what UE calls
    //     SelfHitTestInvisible, and it is the only one of the two that had a UE name at all.
    //   * `Interactable = false`  -> `HitTest = 2` (Blocking). The element stops the pointer and reacts to
    //     nothing. Behaviour CHANGES in one direction, deliberately: it now applies to the sub-tree too,
    //     which is what makes a greyed-out form or a modal dialog one field instead of one field per
    //     descendant. Nothing in this repository sets the flag, so no shipped scene moves.
    //   * Both false -> ChildrenOnly. A transparent element cannot be pressed anyway, so the raycast
    //     answer subsumes the other; picking Blocking there would ADD blocking the file never asked for.
    //   * `true` (either flag) is the default and writes nothing: an absent key is how the reflection
    //     serializer spells "leave it at the C++ default", and both defaults are UIHitTest::All.
    //
    // `Visibility` is never written. The old format had no way to say anything but "visible" - the single
    // `Visible` bool lived on the CANVAS and is untouched by this step - so every value it could take
    // would be invented.
    //
    // A key that is present but is not a boolean is dropped and reported by name; the element keeps the
    // default rather than being guessed at, which is the same refusal the gravity step makes.
    //
    // PURE - no GPU, no filesystem, no global state. The counters go back to the caller, which is the one
    // that knows which file this was.
    //
    // Idempotent: a payload with neither key is left byte-identical and reports zero.
    //
    // SHELF LIFE: this raises v9 to v10 and nothing else. It is deleted once no v9 file remains.
    UIVisibilityMigrationReport MigrateUIVisibilityV9ToV10( std::vector<Assets::EntityData>& entities );

    // What MigrateSSRUnitsV10ToV11 did to one file.
    struct SSRUnitsMigrationReport
    {
        bool  Found  = false; // the Settings block named an SSRMaxDistance at all
        bool  Scaled = false; // it was a finite number and was multiplied by 100
        float Before = 0.0F;
        float After  = 0.0F;
    };

    // Restates the scene's SSR ray length in centimetres.
    //
    // WHY EVERY VALUE IS SCALED AND NONE IS RECOGNISED. The gravity step above matches Earth's two
    // spellings exactly because both populations - metre-era and centimetre-era - were in the tree at
    // once under the SAME version stamp. This field has no second population: a v10 file could only have
    // been authored under the metre-scale slider (Range 1..200), so inside the version gate the value's
    // magnitude carries no information and a x100 is a restatement, not a guess. That also means this
    // function is NOT idempotent on its own - like MigrateMetresToUnits, it relies on the caller gating
    // it on the scene version, and MigrateScene() below is the only supported way to call it.
    //
    // A value that is not a finite number is left exactly as it is and warned about, like every step
    // above: the field it names is still a length the renderer reads, and guessing at it would be the
    // silent substitution §1.4 forbids.
    //
    // PURE - no GPU, no filesystem, no global state.
    //
    // SHELF LIFE: this raises v10 to v11 and nothing else. It is deleted once no v10 file remains.
    SSRUnitsMigrationReport MigrateSSRUnitsV10ToV11( std::optional<rfl::Generic>& settings );

    // The shared default material every look-less layer is pointed at (D-37, teamlead 2026-09-06):
    // an empty `Material` slot degrading silently to schema defaults was rejected because it gives the
    // question "where did this look come from" two different answers depending on the scene, which is
    // the second-source-of-truth defect §1.3 exists to catch — applied to itself. The file this names
    // carries NO Params and NO Textures (checked in at Editor/Resources/Assets/Materials/M_CloudDefault.demat),
    // so it cannot drift from the schema's own defaults the way a baked-in copy of the thirty-three
    // numbers could: Desert/Tests/Engine/CloudMaterialSchema proves the chain (mirror == schema digit
    // for digit, then an empty MaterialOverrides == the schema, then this file states no overrides).
    inline constexpr const char* kDefaultCloudMaterialRelativePath = "Materials/M_CloudDefault.demat";

    // A `.demat` this migration produced and the TOOL must write: the step is pure, so the bytes and
    // the assets-root-relative path come back to the caller, and main.cpp is the one place that touches
    // the filesystem — the same division every step above keeps. NOT used for the shared default above,
    // which is a checked-in file the migration only REFERENCES by path and never generates or overwrites.
    struct CloudMaterialFile
    {
        std::string RelativePath; // e.g. "Materials/M_Clouds_Protocol_Clouds.demat", relative to assets root
        std::string Json;         // the full MaterialData serialization, ready to write verbatim
    };

    // What MigrateCloudMaterialV11ToV12 did, returned rather than logged, like every report above.
    struct CloudMaterialMigrationReport
    {
        int Entities    = 0; // entities whose VolumetricCloud payload lost fields to a material
        int ValuesMoved = 0; // value keys found in the payload and copied into the material verbatim
        int AssetsMoved = 0; // asset keys (CloudType1..4 / CloudLayout) carried as handles
        int Defaulted   = 0; // moved keys ABSENT from the payload - they keep the schema default
        int Rejected    = 0; // present but unusable (wrong JSON type / non-relative path) - named below

        // Entities that stated NONE of the thirty-three keys and gained kDefaultCloudMaterialRelativePath
        // instead of an empty slot (D-37) - counted separately from Entities' bespoke-file population so
        // the log can say how many scenes simply adopted the shared default versus how many carried a
        // look of their own.
        int DefaultsAssigned = 0;

        // Named, not counted: a rejected value is an authored number that did NOT reach the material,
        // and the operator has to see which one (§1.4 - nothing is dropped silently).
        std::vector<std::string> RejectedNames;

        std::vector<CloudMaterialFile> Materials; // bespoke files for the tool to write; empty when every
                                                  // touched entity used the shared default or nothing moved
    };

    // Raises a scene from schema v11 to v12: the thirty-three cloud LOOK fields leave the
    // "VolumetricCloud" payload for a `.demat` material on the Volume-domain cloud shader (O1, D-35).
    //
    // PURE - no GPU, no filesystem, no global state. The material file's BYTES are part of the return
    // value rather than a side effect, and its MaterialId is DERIVED (FNV of the scene name), not drawn
    // from a generator, so the function is deterministic and its test can pin exact output.
    //
    // WHAT MOVES AND HOW. A value key present in the payload becomes a material Param with the same name
    // and the same number - verbatim, no unit change, no re-scaling. An asset key present and non-empty
    // becomes a material Textures entry whose handle is the same path-derived FNV the runtime mints for
    // that file (AssetHandle::FromKey over "assets:<relative path>"). Keys ABSENT stay absent: the
    // schema's defaults are the old component defaults digit for digit, so an unauthored value keeps
    // meaning "the default" without being written down - the same philosophy the .desce format itself
    // has. The payload keeps its other keys untouched and gains "Material" naming the new file.
    //
    // A scene that stated NONE of the thirty-three keys gains kDefaultCloudMaterialRelativePath instead
    // of a bespoke file (D-37): every migrated cloud layer names SOME material, so "the look lives in the
    // material" holds without exception rather than as a rule with an empty-slot escape hatch. No entry
    // is added to `Materials` for this case - the shared file is checked into the repository once, not
    // regenerated per scene.
    //
    // Idempotent: a payload that already carries "Material" and none of the moved keys is not touched -
    // covers both the bespoke and the shared-default case, so a second pass never overwrites either.
    //
    // SHELF LIFE: this raises v11 to v12 and nothing else. It is deleted once no v11 file remains.
    CloudMaterialMigrationReport MigrateCloudMaterialV11ToV12( std::vector<Assets::EntityData>& entities,
                                                               const std::string&               sceneName );

    // What MigrateCloudMaterialLayoutInputs did to one `.demat`.
    struct CloudMaterialLayoutReport
    {
        int Split = 0; // `CloudLayout` bindings found and turned into the two new inputs

        bool Changed() const
        {
            return Split > 0;
        }
    };

    // O-4: the cloud material's ONE layout input becomes TWO — `CloudLayout` splits into
    // `LayoutPattern` and `LayoutMask`, both naming the same `.dclayout`, which is exactly the sky the
    // single slot rendered (the container carries both tables and the bake read both from it).
    //
    // PURE - a MaterialData in, the same struct raised, no filesystem and no global state.
    //
    // WHY IT IS CONTENT-DETECTED AND NOT VERSION-GATED, unlike every step above. A `.demat` carries no
    // version field: it is a bag of names, and a name the shader does not declare is already dropped
    // with a warning (VolumetricCloudRenderer::ResolveMaterial). So the trigger is the presence of the
    // OLD name, which is exact, and the step is idempotent by construction: after it runs there is no
    // `CloudLayout` binding left to find, and a material that never had one is untouched.
    //
    // SHELF LIFE: this raises materials authored before O-4 and nothing else. It is deleted once no
    // `.demat` naming `CloudLayout` remains anywhere it could be run.
    CloudMaterialLayoutReport MigrateCloudMaterialLayoutInputs( Assets::MaterialData& material );

    // What MigrateCloudMaterialAlbedoToColour did to one `.demat`.
    struct CloudMaterialAlbedoReport
    {
        int Broadcast = 0; // scalar `ScatteringAlbedo` values turned into a neutral colour

        bool Changed() const
        {
            return Broadcast > 0;
        }
    };

    // O1: the cloud material's `ScatteringAlbedo` becomes a COLOUR — the Volume domain's output contract
    // carries a per-channel albedo (O1_DESIGN.md §3.3, §9 п.2), so the shader now reads three components
    // where it read one.
    //
    // THIS IS DATA LOSS IF IT IS NOT RUN, and that is why it exists rather than being handled by a lenient
    // reader. A `.demat` stores every parameter as a vec4 with the tail zeroed, which is the format's own
    // convention for a scalar — so `ScatteringAlbedo: [0.98, 0, 0, 0]`, read as a colour, is a cloud whose
    // medium scatters red and absorbs green and blue entirely. Not a subtle drift: a RED sky.
    //
    // PURE — a MaterialData in, the same struct raised, no filesystem and no global state.
    //
    // CONTENT-DETECTED AND IDEMPOTENT BY SHAPE, not by a version field, for the reason
    // MigrateCloudMaterialLayoutInputs gives: a `.demat` has no version. The trigger is
    // `y == 0 && z == 0`, which is what EVERY file written while the slot was scalar carries and what no
    // file written after this can carry unless the author really did ask for pure red — and the second run
    // then finds `y == x != 0` and does nothing. The one degenerate input, `[0, 0, 0, 0]`, comes out
    // unchanged, and that is CORRECT rather than lucky: a zero albedo is black in one component and black
    // in three.
    //
    // WHY THE READER IS NOT LENIENT INSTEAD. Broadcasting x when y and z are zero inside
    // Graphic::Detail::ApplyCloudOverride would make `(0.98, 0, 0)` — a legal authored colour now that the
    // slot has three components — inexpressible, and it would hide an unmigrated file for ever instead of
    // letting the migrator find it once and say so.
    //
    // SHELF LIFE: this raises materials authored before the albedo became a colour, and nothing else.
    CloudMaterialAlbedoReport MigrateCloudMaterialAlbedoToColour( Assets::MaterialData& material );

    // THE TEN KEYS a scene no longer states, in the order they are reported. Stated ONCE, here, because
    // three things have to agree about the set — this migration, the census that keeps them out of
    // Core::SceneSettings and out of every .desce on disk (Desert/Tests/Engine/SceneDebugFields), and the
    // struct that owns them now (Graphic::DebugViewState) — and two of the three are in different targets.
    // A test asserts this list against DebugViewState's own declaration, so a field ADDED there and
    // forgotten here fails rather than quietly stays serializable.
    //
    // The size is deduced, never typed: a hand-written count is one more thing that can disagree with the
    // rows, and this list already answers to two other declarations.
    //
    // UNLIKE kRetiredKeys BELOW, THIS ONE DOES NOT EMPTY ROW BY ROW, and measuring that is what put the
    // sentence here: dropping it to zero on its own turns SceneDebugFields red twice over, because
    // DebugViewState still declares the ten fields this must mirror. It reaches zero only by being DELETED
    // whole, with MigrateDebugViewV12ToV13, when no v12 file can exist.
    inline constexpr std::array kDebugViewKeys = {
         "ShowGrid",      "ShowColliders", "ShowBoundingBoxes", "BoundingBoxColor", "BoundingBoxLineWidth",
         "WireframeMode", "ShowNormals",   "LightingDebug",     "ShadowDebug",      "DeferredDebug",
    };

    // What MigrateDebugViewV12ToV13 removed from one file.
    struct DebugViewMigrationReport
    {
        int KeysRemoved = 0; // 0..10 - how many of kDebugViewKeys the Settings block actually stated

        // WHICH ones, and what each said, as "ShowColliders=true". Named rather than counted, like every
        // step above that drops a value: these keys were AUTHORED (55 of the 80 scenes in this repository
        // stated ShowColliders true, 72 of 77 stated ShowGrid false), and the operator has to be able to
        // see that the collider wireframes they are used to are gone because the file stopped deciding it,
        // not because something broke. §1.4 - nothing is dropped silently.
        std::vector<std::string> RemovedNames;
    };

    // Raises a scene from schema v12 to v13: the Settings block stops stating what the VIEWPORT draws on
    // top of the world.
    //
    // WHY IT REMOVES AND CARRIES NOTHING. There is nowhere to carry them TO. Graphic::DebugViewState lives
    // on the SceneRenderer, one per view, and is deliberately not serialized anywhere - not in the scene,
    // not per-scene in the editor's config. The editor's own copy is a USER preference
    // (Editor::EditorPreferences::DebugView, editor.json), and writing 80 scenes' worth of one-time view
    // state into it would be turning "the last person to save this level had colliders on" into "this user
    // wants colliders on", which is the same confusion in a new file. Every flag defaults OFF; the user
    // turns on what they want once, and it then follows them across scenes instead of the other way round.
    //
    // A key present but of the wrong JSON type is removed like the rest and reported with its value: the
    // field it named does not exist any more, so there is no type for it to be right for.
    //
    // PURE - no GPU, no filesystem, no global state. The counters go back to the caller, which is the one
    // that knows which file this was.
    //
    // Idempotent: a Settings block stating none of the ten is left byte-identical and reports zero. So is
    // a scene with no Settings block at all.
    //
    // SHELF LIFE: this raises v12 to v13 and nothing else. It is deleted once no v12 file remains.
    DebugViewMigrationReport MigrateDebugViewV12ToV13( std::optional<rfl::Generic>& settings );

    // One key this project deleted on purpose, and where in a .desce it used to live.
    //
    // "Settings" means the scene-wide settings block; anything else is a COMPONENT KEY, and the row then
    // names a field inside that component's payload on every entity that carries one. The distinction is
    // in the data because each level of a .desce answers to a different registry, and a step that guessed
    // the level would delete a live key that happened to share a name.
    struct RetiredKey
    {
        const char* Block; // "Settings", or a ComponentRegistry key such as "DirectionLight"
        const char* Key;
        const char* Why; // the sentence the log prints, so the person running this knows what they lost
    };

    // THE LIST, AND IT IS THE ONLY ONE IN THE REPOSITORY.
    //
    // "Retired" is not "unknown", and since K11 the difference is load-bearing rather than a matter of
    // taste: the engine's saver PRESERVES every key it does not declare, so a key we removed on purpose
    // would ride along in the files for ever unless something deliberately took it out. That deliberate
    // act is this table. It is in the TOOL and not in the engine because a runtime that carries a list of
    // dead key names is keeping legacy alive (contract §4.6), and because a removal has to be written back
    // to the FILES to be finished at all.
    //
    // A row is deleted once no file below the version that introduced it can exist - which, because the
    // loader refuses anything that is not at kSceneVersion, is as soon as the corpus has been run through.
    // The rows are kept for one generation so that a branch merged late still gets converted.
    //
    // AN EMPTY TABLE IS THIS LIST SUCCEEDING, so the type has to be able to spell one: when the last row
    // goes, this declaration becomes `inline constexpr std::array<RetiredKey, 0> kRetiredKeys = {};` and
    // every loop below keeps compiling on both toolchains. `RetiredKey k[] = {}` is not a legal C++
    // declaration at all, which is how a table meant to empty acquires a permanent last row.
    inline constexpr std::array kRetiredKeys = {
         RetiredKey{ "Settings", "EnableSSGI",
                     "the screen-space GI toggle was replaced by the GlobalIllumination mode on 2026-08-06 "
                     "(commit 0b788b1b); nothing has read it since" },

         // K3's five, and they are a different kind of row from the one above: EnableSSGI named a value
         // nothing read, these name values that are still read and are read SOMEWHERE ELSE. Each says so,
         // because a person who authored one has to be told where their choice went rather than that it
         // is gone.
         //
         // NOTHING IS CARRIED INTO THE NEW STORE, AND THAT IS A DECISION RATHER THAN AN OMISSION. There
         // are 51 scenes in this repository and exactly ONE machine.json per host: carrying the values
         // would mean whichever scene the tool happened to convert last deciding what this machine can
         // afford, which is a worse answer than any default. The new store's defaults ARE the old C++
         // defaults digit for digit (Common/Settings/MachineSettings.hpp), so a scene that stated a
         // default renders exactly the frame it rendered before; one that stated something else is named
         // in the log with the value it stated, so the operator can set it once, for the machine.
         RetiredKey{ "Settings", "AA",
                     "post-process anti-aliasing is machine quality (K3): it moved to "
                     "Common::Settings::MachineSettings::AA, which the editor reads from "
                     "~/.desertengine/machine.json and a packaged game from the player's own directory" },
         RetiredKey{ "Settings", "MeshLOD",
                     "distance mesh LOD is machine quality (K3) - LOD0 is byte-identical geometry near the "
                     "camera, so off vs on is fidelity and not authoring; it moved to MachineSettings::MeshLOD" },
         RetiredKey{ "Settings", "TextureFilterMode",
                     "the sampler filter is machine quality (K3) - the same picture, sharper or blurrier; it "
                     "moved to MachineSettings::TextureFilterMode" },
         RetiredKey{ "Settings", "Anisotropy",
                     "sampler anisotropy is machine quality (K3), for the same reason as the filter it belongs "
                     "to; it moved to MachineSettings::Anisotropy" },
         RetiredKey{ "Settings", "CloudQualityTier",
                     "the cloud march's occlusion budget is machine quality (K3) - High reproduces the "
                     "calibrated constants to the digit; it moved to MachineSettings::CloudQualityTier" },

         // Г26's three, and they are the FIRST kind of row again - values nothing reads and nothing
         // reads elsewhere either. The wind's only consumer was the procedural grass generator that
         // step 18 removed; the fields were kept at that moment so the next wind-driven renderer would
         // find them, and there was no next one. Nothing is carried anywhere, because there is nowhere
         // to carry it TO: a heading in degrees, an amplitude in no unit and a gustiness with no scale
         // are three numbers whose meaning was defined by the one reader that is gone.
         RetiredKey{ "Settings", "WindDirection",
                     "the scene's wind had no reader between the procedural grass generator (removed at "
                     "scene schema v18) and a wind-driven ASSET that does not exist yet; wind comes back "
                     "with the thing it sways, and with a unit chosen by whatever reads it" },
         RetiredKey{ "Settings", "WindStrength",
                     "same: the wind amplitude drove the removed grass generator and nothing since" },
         RetiredKey{ "Settings", "WindTurbulence",
                     "same, and this one never had a reader at all - it was authored 'reserved for "
                     "foliage/hair/cloth response' and stayed reserved" },
         // Ю15's one row, and it is the EnableSSGI kind rather than the K3 kind: nothing reads it any
         // more, and what it used to do is done better somewhere else. The field held a printf format an
         // author typed in the Details panel, which the canvas passed to std::snprintf with a double - so
         // `%s` was undefined behaviour at run time - and which formatted every bound number in the C
         // locale regardless of the reader's language. A `{n}` in the string table replaces it, placed by
         // the translator and formatted for the reader.
         RetiredKey{ "UIBinding", "Format",
                     "the printf format on a Text binding was removed by Ю15: a number is now formatted "
                     "for the reader's locale and its prefix/suffix belongs in the string table, where a "
                     "translator can move it" },
    };

    // What MigrateRetiredKeys removed from one file.
    struct RetiredKeysMigrationReport
    {
        int KeysRemoved = 0;
        // WHICH ones, with the value each held and the reason it is gone - "Settings.EnableSSGI=true
        // (the screen-space GI toggle ...)". Named rather than counted for the same reason every other
        // dropping step here names what it drops (§1.4): a person who authored the value has to be able
        // to see that it is gone because it was retired, not because something broke.
        std::vector<std::string> RemovedNames;
    };

    // Removes every key in kRetiredKeys from the block that owns it.
    //
    // WHY THIS PASS EXISTS AT ALL, when nothing reads these keys any more. Until K11 the answer was "the
    // next save deletes them anyway" - the saver enumerated its own registry, so an unknown key
    // evaporated on contact. That is exactly the defect K11 removed, and removing it made this pass
    // necessary: a preserved key is preserved whether or not we still want it, so wanting rid of one is
    // now a decision somebody has to write down. Here.
    //
    // AND IT IS THE ONE THING IN THIS FILE NOT GATED ON ITS OWN STEP NUMBER, which is deliberate and
    // narrow. Every other step is `v(n) -> v(n+1)` and MUST be gated on its own constant, or raising the
    // head would send every file back through migrations that already ran. This one cannot: it is not a
    // step that happened once, it is the standing consequence of a TABLE that grows, and gating it on
    // `kSceneVersionRetiredKeys` meant that rows added after 14 would never fire on a corpus already at
    // 14. So its gate is "not at the head", and that is safe for the reason none of the others is safe
    // with it: this pass decides nothing from the values, only from the table, so re-running it on a file
    // it has already cleaned finds nothing to remove and leaves the tree byte-identical.
    //
    // A retirement is therefore two edits and one run: a row here, and Core::kSceneVersion moved.
    //
    // PURE - no GPU, no filesystem, no global state.
    //
    // SHELF LIFE: the pass stays; kRetiredKeys is what empties, one row at a time, as each row's
    // generation passes out of reach.
    RetiredKeysMigrationReport MigrateRetiredKeys( std::optional<rfl::Generic>&     settings,
                                                   std::vector<Assets::EntityData>& entities );

    // What MigrateScriptRootV15ToV16 did to one file.
    struct ScriptRootMigrationReport
    {
        int Entities = 0; // entities carrying a "Script" payload that was touched
        int Slots    = 0; // script slots re-spelled under the new key, the Empty ones below INCLUDED
        int Empty    = 0; // of those, the ones that named no script - the key stays empty, not "assets:"

        // Slots whose stored spelling names no place under a `Scripts/` folder, so the census has no
        // root to measure it against. The value is carried across UNCHANGED under the new field name -
        // which is exactly what it did before, since PathForStableKey hands an untagged string back as a
        // path - and NAMED, because such a slot still does not resolve in a packaged game and a count
        // alone would not say which entity to re-point (DC 1.4). No scene in this repository has one.
        std::vector<std::string> UnrootedNames;
    };

    // Raises a scene from schema v15 to v16: a script slot stops naming its `.lua` by the path the editor
    // was standing in and names it by the ROOT-TAGGED KEY every other content reference in the file
    // already uses.
    //
    // WHAT IT REWRITES. Inside every entity's "Script" payload, each element of "Scripts" loses its
    // `Path` key and gains `ScriptKey`. `Resources/Assets/Scripts/Examples/MoveAlongX.lua` becomes
    // `assets:Scripts/Examples/MoveAlongX.lua`. The tag comes from Common::AssetHandle::AssetsTag(), so
    // the migration cannot spell it differently from the runtime that reads it back.
    //
    // HOW IT FINDS THE ROOT, WITHOUT A FILESYSTEM AND WITHOUT AN assetsRoot PARAMETER. The v7 -> v8
    // material step measures its paths against the root it is HANDED, and that works there because those
    // paths were absolute and therefore contained the whole root sequence. These do not: the editor's
    // working directory is `Editor/`, so a scene carries `Resources/Assets/Scripts/...` while the tool is
    // run from the repository root and would be handed `Editor/Resources/Assets` - the sequence is simply
    // not in the string, and matching against it would find nothing and report every slot as unrooted.
    // So the root is derived from the STORED PATH instead, through
    // Constants::Path::RootForContentPath(ContentDir::Script) - the census's own lexical inverse, which
    // finds the last `Scripts/` component and hands back what precedes it. That is the same sentence
    // SceneOutputRoot in the tool's main is: derive the root from the FILE, never from where the process
    // is standing. It makes the answer identical for `Resources/Assets/Scripts/x.lua` and for
    // `/Users/somebody/Proj/Editor/Resources/Assets/Scripts/x.lua`, which is what the editor and this
    // tool respectively produce.
    //
    // A value the census cannot place - a `.lua` somewhere that is not a `Scripts/` folder - is carried
    // over unchanged and NAMED. That is not a silent fallback: an untagged key is returned verbatim by
    // PathForStableKey, so the slot keeps exactly the behaviour it had, and the report says which slot
    // still has it.
    //
    // PURE - no GPU, no filesystem, no global state. AssetsTag() reads a table of string literals whose
    // value cannot vary with the project root; RootForContentPath is lexical by construction and says so.
    //
    // Idempotent: a slot object with no `Path` key is left byte-identical and reports zero.
    //
    // SHELF LIFE: this raises v15 to v16 and nothing else. It is deleted once no v15 file remains.
    ScriptRootMigrationReport MigrateScriptRootV15ToV16( std::vector<Assets::EntityData>& entities );

    // What MigrateServiceAssetRootV16ToV17 did to one file.
    struct ServiceAssetRootMigrationReport
    {
        int Entities = 0; // entities carrying at least one of the four payloads that was touched
        int Refs     = 0; // references re-spelled as a root-tagged key
        int Empty    = 0; // of those, the slots that named nothing - they stay empty, not a bare tag

        // References the two content roots cannot place, carried across unchanged and NAMED. Such a
        // value still does not resolve in a packaged game, and a count alone would not say which slot
        // to re-point (DC 1.4). No scene in this repository has one.
        std::vector<std::string> UnrootedNames;
    };

    // Raises a scene from schema v16 to v17: a reference to a font, a vector icon or a video stops being
    // the PATH its service's registry happened to hold and becomes the root-tagged key every other
    // content reference in the file already is.
    //
    // WHAT IT REWRITES - four sites, because the same value reaches the file by two different routes and
    // fixing one would leave the other (the defect shape this project keeps paying for):
    //
    //   * `Text.FontPath`  -> `Text.Font`   (the world-space SDF label's manual serializer; the KEY is
    //                                        renamed with the value, since it is no longer a path)
    //   * `UIText.Font`, `UIIcon.Icon`, `UIPanel.Video`  (the reflected AssetHandle slots; their names
    //                                        already say "reference" rather than "path" and do not move)
    //
    // HOW THE ROOT IS FOUND, WITHOUT A FILESYSTEM. Two roots can contain one of these: the ENGINE tree,
    // which is the compile-time constant `Resources/` and never moves, and the PROJECT'S assets root,
    // whose name is per-project and is therefore the `assetsRoot` this function is handed. Neither can be
    // matched by its full spelling, because the editor writes paths from its own working directory
    // (`Editor/`) while the tool is run from the repository root - so the match is on each root's
    // TRAILING components, longest first, which is the same "longest match wins" rule
    // AssetHandle::StableKeyForPath applies to the absolutised roots at run time. It has to be that rule
    // and not a simpler one: in the sandbox layout `Resources/Assets/` is NESTED INSIDE `Resources/`, so
    // a shorter match would tag every project asset as an engine resource.
    //
    // A value neither root can place is carried over unchanged and NAMED - PathForStableKey returns an
    // untagged string verbatim, so the slot keeps exactly the behaviour it had, and the report says
    // which slot still has it.
    //
    // PURE - no GPU, no filesystem, no global state. The tags come from Common::AssetHandle's own table
    // (AssetsTag / EngineTag), which is string literals whose value cannot vary with the project root.
    //
    // Idempotent: a payload already carrying the new spelling has no old key to find and is left
    // byte-identical.
    //
    // SHELF LIFE: this raises v16 to v17 and nothing else. It is deleted once no v16 file remains.
    ServiceAssetRootMigrationReport MigrateServiceAssetRootV16ToV17( std::vector<Assets::EntityData>& entities,
                                                                     const std::filesystem::path&     assetsRoot );

    // What MigrateGrassGenerationV17ToV18 did to one file.
    struct GrassGenerationMigrationReport
    {
        int Entities    = 0; // entities whose "Terrain" payload was touched
        int KeysRemoved = 0; // 0..6 per terrain - how many of the six the payload actually stated

        // WHICH ones, with the value each held - "Terrain.GrassDensity=512". Named rather than counted,
        // like every other dropping step here: these were AUTHORED numbers and the person who authored
        // one has to be able to see that the field is gone because the feature is, not because something
        // broke (DC 1.4).
        std::vector<std::string> RemovedNames;

        // Terrains that gained a `GrassMode` derived from `EnableGrass`, as "Terrain.GrassMode=Off (was
        // EnableGrass=false)". The carry is the half of this step that changes what is DRAWN, so it is
        // named the same way the drops are.
        std::vector<std::string> CarriedNames;
    };

    // Raises a scene from schema v17 to v18: a terrain stops carrying the procedural grass generator.
    //
    // WHAT IT DOES TO ONE "Terrain" PAYLOAD, in this order:
    //
    //   1. If the payload does NOT already state `GrassMode`, one is written from `EnableGrass`:
    //      false (or a stated-but-unusable value, see below) -> "Off", true -> "Auto". A payload that
    //      states neither key is left without a `GrassMode`, so the component's own default (Auto)
    //      applies - which is what a file that never expressed an opinion has always meant.
    //   2. All six generator keys are removed, `EnableGrass` included: it has been read by then, and
    //      leaving it would keep a key the component no longer declares alive in the file (the saver
    //      preserves unknown keys since K11, so a key we removed on purpose rides along for ever unless
    //      something deliberately takes it out).
    //
    // WHY THE CARRY IS NOT A ROW OF kRetiredKeys. That table only DROPS, and it is deliberately ungated
    // and idempotent because it decides nothing from the values. This step decides `GrassMode` FROM a
    // value, so it must run exactly once and must be gated on its own number - and splitting the six
    // keys between the two mechanisms would let a file be half-converted by a tool run from a branch
    // that had one and not the other.
    //
    // `EnableGrass` present but not a boolean is treated as false and REPORTED: the field it named does
    // not exist any more, so there is no type for it to be right for, and "Off" is the reading that
    // cannot invent grass a scene never asked for.
    //
    // PURE - no GPU, no filesystem, no global state.
    //
    // Idempotent: a payload stating none of the six is left byte-identical and reports zero.
    //
    // SHELF LIFE: this raises v17 to v18 and nothing else. It is deleted once no v17 file remains.
    GrassGenerationMigrationReport MigrateGrassGenerationV17ToV18( std::vector<Assets::EntityData>& entities );

    // What MigrateTextKeySigilV18ToV19 did to one file.
    struct TextKeySigilMigrationReport
    {
        int Entities = 0; // entities with at least one escaped string
        int Escaped  = 0; // strings that gained the escape

        // WHICH ones, as "Title > UIText.Text = #done". Named rather than counted, like every other step
        // here: the change is to an authored string a person typed, and they have to be able to see that
        // it was escaped on purpose rather than corrupted (DC 1.4).
        std::vector<std::string> EscapedNames;
    };

    // Raises a scene from schema v18 to v19: an authored string that a LEADING '#' would now read as a
    // string-table key is escaped to '##', which the resolver turns back into a single '#' on the way to
    // the screen (Localization::LiteralOf).
    //
    // FOUR SITES: `UIText.Text`, `UIInputField.Placeholder`, every `;`-separated item of
    // `UIDropdown.Options`, and `Text.Text`. `UIInputField.Text` is NOT one - it is the player's own text
    // and nothing resolves it.
    //
    // ONLY A LEADING HASH IS ESCAPED. A hash anywhere else is just a hash, and this repository's main menu
    // is full of them: `[color=#FF7A33]` is a rich-text colour, and doubling every hash in a string would
    // have broken every one of those labels.
    //
    // PURE - no GPU, no filesystem, no global state.
    //
    // NOT IDEMPOTENT, AND IT CANNOT BE. This was written claiming it was, and its own suite disproved the
    // claim on the first run: a v18 literal that really begins "##" must become "###" (the resolver strips
    // ONE leading hash, so that is the only spelling that still means "##"), and a second pass over the
    // result cannot tell that string from one it has not seen. Correctness of the one-time conversion and
    // idempotence are in direct conflict here, and correctness wins — a step that decides FROM a value
    // must run exactly once and must be gated on its own number, which is what MigrateScene does with
    // `statedSceneVersion < kSceneVersionTextKeySigil` and what the suite asserts through that entry point
    // rather than by calling this twice. (The same rule the grass carry next door states for the same
    // reason; kRetiredKeys is the one pass allowed an ungated gate, because it decides nothing.)
    //
    // SHELF LIFE: this raises v18 to v19 and nothing else. It is deleted once no v18 file remains.
    TextKeySigilMigrationReport MigrateTextKeySigilV18ToV19( std::vector<Assets::EntityData>& entities );

    // A `.danimgraph` this migration produced and the TOOL must write: the step is pure, so the bytes and
    // the assets-root-relative path come back to the caller, and main.cpp is the one place that touches the
    // filesystem — the same division `CloudMaterialFile` keeps and for the same reason.
    struct AnimGraphFile
    {
        std::string RelativePath; // e.g. "AnimGraphs/ScriptDriven.danimgraph", relative to the assets root
        std::string Json;         // the graph's own serialization, ready to write verbatim
    };

    // What MigrateEditMeshV21ToV22 did to one file.
    struct EditMeshMigrationReport
    {
        int Entities = 0; // StaticMesh blocks whose render arrays became an EditMesh
        int Rejected = 0; // blocks whose arrays could not be read or welded - left EXACTLY as they were

        // Per converted entity, "Tag: <render verts> render vertices -> <verts> vertices / <tris> triangles",
        // plus what the weld had to drop, so a person reading the log can see the conversion was a weld and
        // not a copy (the v21 file stated no topology; FromRenderMesh decides it by distance).
        std::vector<std::string> ConvertedNames;
        std::vector<std::string> RejectedNames;
    };

    // Raises StaticMesh blocks from schema v21 to v22: `CustomVertices` (Position, Normal, TexCoord per render
    // vertex) + `CustomIndices` (3 per triangle) become `EditMesh`, the saved form of the EditMesh the render
    // mesh is now derived from.
    //
    // THE WELD IS Geometry::FromRenderMesh WITH ITS DEFAULTS, the one the editor itself uses to lift render
    // data, so a converted scene holds the same EditMesh the editor would build from that buffer. One submesh
    // over everything, MaterialID 0 - the v21 loader drew exactly that. NO TANGENT LAYER: v21 never stored a
    // tangent, and the loader left the field uninitialised, so there is no value to carry; "no layer" is the
    // honest statement of that, and ToRenderMesh then emits the zero frame the mesh actually has.
    //
    // A block stating only one of the two arrays, arrays that are not what v21 wrote, or indices FromRenderMesh
    // refuses is NAMED and left untouched rather than dropped - the scene loader then ignores the two unknown
    // keys and the entity draws no edited mesh, which is visible, instead of the migration deciding for it.
    //
    // PURE - no GPU, no filesystem, no global state. Idempotent: a block with no CustomVertices is untouched.
    EditMeshMigrationReport MigrateEditMeshV21ToV22( std::vector<Assets::EntityData>& entities );

    // One `.dlht` the v22 -> v23 step produced: where it goes and what it holds, for the tool to write.
    struct LandscapeTileFile
    {
        std::filesystem::path
             Path; // on disk, as the tool sees it: the source file's directory + "<stem>_Landscape/"
        std::vector<unsigned char> Bytes; // EncodeLandscapeTile's output — the whole file
    };

    // What MigrateProceduralTerrainV22ToV23 did to one file.
    struct ProceduralTerrainMigrationReport
    {
        int Entities = 0; // Terrain blocks that became a landscape
        int Tiles    = 0; // tile entities (and files) created for them
        int Rejected = 0; // Terrain blocks that could not be baked - named below, left EXACTLY as they were

        // "Tag: <quads> x <quads> quads in <n> x <n> tiles, <spacing> cm, max |h| <cm>" per baked terrain, and
        // every value the new form cannot say (a Manual layer, which only a never-saved brush could feed).
        std::vector<std::string> ConvertedNames;
        std::vector<std::string> RejectedNames;

        std::vector<LandscapeTileFile> Files; // for the tool to write; empty when nothing moved
    };

    // The v22 procedural terrain's height, in centimetres above the entity, at a point of its LOCAL grid
    // plane (x, z measured from the entity's origin, the grid centred on it). This is the formula
    // Programs/Terrain/TerrainTessEval.glslh evaluated per tessellated vertex until v23 - value noise, five
    // octaves, the lattice hashed with fract() - transcribed to float arithmetic, and it lives HERE because
    // the migration is now its only consumer: the shader that drew it no longer exists.
    float ProceduralTerrainHeightV22( float x, float z, float noiseFrequency, int seed, float heightScale );

    // How a v22 terrain of side @p sizeCm and grid @p resolution is sampled: the rendered surface was the
    // fBm at the corners of `min(resolution, 64)` patches per side, each tessellated at most 16 times, so
    // at least that many quads per side reproduces the relief the camera saw at its finest. Split into equal
    // tiles of one of UE's section sizes (7..255 quads, the only ones a landscape root accepts).
    struct ProceduralTerrainGrid
    {
        uint32_t TilesPerSide = 0;
        uint32_t QuadsPerTile = 0;
        float    SpacingCm    = 0.0f;
    };
    ProceduralTerrainGrid ProceduralTerrainGridFor( float sizeCm, int resolution );

    // Raises Terrain blocks from schema v22 to v23: each becomes a LANDSCAPE whose tiles hold the terrain's
    // fBm baked at ProceduralTerrainGridFor's spacing, in UE's encoding with ZScale = HeightScale / 256 - so
    // the local range +-256 IS the old +-HeightScale, the shader's height rules normalise by the same number
    // as before, and the only error is the 16-bit step (HeightScale / 32768 cm).
    //
    // THE ENTITY STAYS: same id, tag and hierarchy; its Translation moves to the landscape's sample (0, 0)
    // (the grid's corner), its Scale is baked in (x/z into the spacing, y into the heights) and reset to 1.
    // The material and the three layer modes move to `LandscapeMaterial`; Off stays Off, and Manual - which
    // read a splat map no build ever saved - becomes Auto and is NAMED in the report.
    //
    // A terrain the landscape frame cannot state - rotated, or scaled unequally in x and z - is NAMED and left
    // untouched: a landscape has no rotation, and inventing one would place the relief somewhere else.
    //
    // Tile ids are derived from the entity id and the tile coordinate (splitmix64), so a second run over
    // the same v22 file writes the same files. `sourceFile` names them (LandscapeTileBlobPath); `HeightFile`
    // is spelled the way the engine's own save spells it - under Common::Constants::Path::ASSETS_PATH -
    // from the file's place under `assetsRoot`. PURE apart from reading those two paths.
    ProceduralTerrainMigrationReport MigrateProceduralTerrainV22ToV23( std::vector<Assets::EntityData>& entities,
                                                                       const std::filesystem::path&     sourceFile,
                                                                       const std::filesystem::path& assetsRoot );

    // What MigrateTextureAssetRefsV23ToV24 did to one file.
    struct MaterialGuidsMigrationReport
    {
        int Rewritten = 0; // slots that now state a material GUID
        int Emptied   = 0; // slots that stated 0 (no material) and now state ""

        // Ids the register does not know, as "Tag > StaticMesh.MaterialGuids[2] = 123". Non-empty REFUSES
        // the file: the id names a material no register row states, and guessing one (or dropping the
        // slot to its path) is exactly the silent fallback this step retires.
        std::vector<std::string> UnknownNames;
    };

    // Raises StaticMesh / SkinnedMesh / InstancedStaticMesh `MaterialGuids` from v26 (MATL v1 u64 ids, read
    // back as int64 and therefore often negative) to v27 (the GUID text of the `.demat` that stated the id).
    // `legacyIds` is the old-id register of the file's assets root. A payload is rebuilt only when a slot
    // changes. SHELF LIFE: deleted once no v26 file remains.
    MaterialGuidsMigrationReport MigrateMaterialGuidsV26ToV27( std::vector<Assets::EntityData>& entities,
                                                               const LegacyMaterialIdMap&       legacyIds );

    // What MigrateMeshGuidsV27ToV28 did to one file.
    struct MeshGuidsMigrationReport
    {
        int Rewritten = 0; // MeshGuid values that now state the mesh file's header GUID

        // Blocks that cannot be raised, as "Tag > SkinnedMesh.MeshGuid = 123 (why)". Non-empty REFUSES the
        // file: a number that is not the handle of the file beside it, a missing file, or a file stating no
        // GUID leaves no identity to write, and guessing one is the silent fallback this step retires.
        std::vector<std::string> UnknownNames;
    };

    // Raises StaticMesh / SkinnedMesh / InstancedStaticMesh `MeshGuid` from v27 (the path-derived u64 handle,
    // read back as int64) to v28 (the GUID text the mesh file's header states), in entity records and in
    // their prefab-override records. `MeshPath` is project-relative ("Cooked/Meshes/X.skmesh"); the project
    // is the nearest ancestor of `assetsRoot` under which that file exists. SHELF LIFE: deleted once no v27
    // file remains.
    MeshGuidsMigrationReport MigrateMeshGuidsV27ToV28( std::vector<Assets::EntityData>& entities,
                                                       const std::filesystem::path&     assetsRoot );

    struct TextureAssetRefsMigrationReport
    {
        int Rewritten = 0; // strings that now name a texture asset

        // WHICH ones, as "Tag > UIPanel.Sprite = assets:Textures/T.detex". Named, like every step here.
        std::vector<std::string> RewrittenNames;
        // Rewritten, but no asset of that name lies under the assets root: the reference was already
        // dangling (its cooked file is gone either way) and the operator has to see which.
        std::vector<std::string> MissingNames;
    };

    // Raises a scene from schema v23 to v24: EVERY string value, at any depth of any component payload and
    // of the scene's Settings, that reads `cooked:Textures/<p>.tex` becomes `assets:Textures/<p>.detex`.
    // Every field, not a list of sites: a texture reference is a string in whatever field holds it
    // (UIPanel.Sprite, UICanvas.Sprite, Settings.SplashSprite ...), and a list would miss the next one.
    //
    // THE MAPPING IS THE IMPORTER'S OWN: a source `Textures/<p>.<ext>` cooked to `cooked:Textures/<p>.tex`
    // and is imported into `<p>.detex` beside it (TextureImporter::AssetPathFor), so the stem and folders
    // carry over and only the root tag and extension change. `assetsRoot` is read only to NAME a target
    // that does not exist; the rewrite happens regardless, because the old string resolves to nothing.
    //
    // Idempotent (a rewritten value no longer matches), but gated on its number like every step above.
    // SHELF LIFE: deleted once no v23 file remains.
    TextureAssetRefsMigrationReport MigrateTextureAssetRefsV23ToV24( std::optional<rfl::Generic>&     settings,
                                                                     std::vector<Assets::EntityData>& entities,
                                                                     const std::filesystem::path&     assetsRoot );

    // What MigrateSiblingOrderV24ToV25 did to one file.
    struct SiblingOrderMigrationReport
    {
        size_t Indexed   = 0; // records that received a siblingIndex
        size_t Reordered = 0; // records whose position in the file moved when sorted by id
    };

    // Stamps every record's `siblingIndex` with its place among its siblings AS THE v24 LOADER BUILT IT,
    // then sorts the records by id. The v24 loader attached non-prefab records in file order (pass 2) and
    // prefab instances after all of them, in file order (pass 3); roots were made in that same order. The
    // index mirrors exactly that, so the loaded hierarchy is identical before and after. A parent that no
    // record answers to counted as a root then and counts as one here.
    //
    // Idempotent in effect (a v25 file already carries the indices and is already sorted), but gated on
    // its number like every step above. SHELF LIFE: deleted once no v24 scene remains.
    SiblingOrderMigrationReport MigrateSiblingOrderV24ToV25( std::vector<Assets::EntityData>& entities );

    // What MigrateAnimGraphV20ToV21 did, returned rather than logged, like every report above.
    struct AnimGraphMigrationReport
    {
        int Entities = 0; // entities whose "Animation" payload carried a non-empty GraphJson
        int Empty    = 0; // entities whose GraphJson was present but empty — the key is dropped, no file
        int Rejected = 0; // present, non-empty and NOT parseable as a graph — named below, nothing dropped

        // Named, not counted: a rejected blob is an authored state machine that did NOT reach a file, and
        // the operator has to see which entity (§1.4 — nothing is dropped silently). The blob is LEFT IN
        // PLACE in that case, so the work is still in the file and a fixed tool can have another go.
        std::vector<std::string> RejectedNames;

        std::vector<AnimGraphFile> Graphs; // files for the tool to write; empty when nothing moved
    };

    // Raises a scene from schema v20 to v21: the state machine leaves the entity's "Animation" payload for
    // a `.danimgraph` of its own, and the payload names it under "Graph".
    //
    // PURE — no GPU, no filesystem, no global state. The graph file's BYTES are part of the return value
    // rather than a side effect.
    //
    // THE FILE IS NAMED AFTER THE GRAPH, NOT AFTER THE ENTITY OR THE SCENE, and that is the decision the
    // whole step exists to make possible: two entities that carried byte-identical blobs (which is exactly
    // what copying a character produced) converge on ONE file and genuinely share it afterwards. The caller
    // is what notices a name claimed twice with DIFFERENT content — `Graphs` may legitimately contain the
    // same RelativePath twice with equal Json, and main.cpp's map is where that is collapsed, on the terms
    // it already collapses cloud materials.
    //
    // A graph with an empty `Name` is named after the ENTITY instead, because "" is not a filename and
    // silently inventing "Graph.danimgraph" for every such blob would collide them into one.
    //
    // Idempotent: a payload with no "GraphJson" key is not touched, so a second pass over a converted file
    // does nothing at all.
    //
    // SHELF LIFE: this raises v20 to v21 and nothing else. It is deleted once no v20 file remains.
    AnimGraphMigrationReport MigrateAnimGraphV20ToV21( std::vector<Assets::EntityData>& entities );

    // Everything that ran, so the caller can say which FILE moved and how far.
    //
    // `File` and not `Scene` since И11: the same report comes back from MigratePrefab, because a
    // `.deprefab` is raised by the same chain. A prefab has no scene-wide Settings block, so the
    // settings-only steps below simply do not run for one and their flags stay false - that is a
    // property of the file, stated once in MigrateFileTree, not a second report shape.
    struct FileMigrationReport
    {
        // Non-empty: the tree states a generation ABOVE the head this tool knows, so it was written by a
        // build this one predates. NOTHING was run and NOTHING was stamped - the caller must report the
        // refusal and leave the file alone.
        //
        // WHY A REFUSAL AND NOT A NO-OP. Every gate below is `stated < step`, so a v18 tree entering a
        // v17 tool matches no step and used to fall through to the unconditional stamp at the bottom:
        // the tool wrote v17 over a v18 number while the payloads stayed v18, and printed "already at
        // scene v17" over a file it had just mis-labelled. That is §1.4's silent substitution with the
        // successful-looking answer attached. The pair is the file's identity, so a pair this tool
        // cannot place is named rather than guessed at.
        std::string Refused;

        bool                        SkyRaised = false; // the sky schema was below kSceneVersionSky
        SkyMigrationReport          Sky;
        bool                        UnitsRaised = false; // the world unit was below kUnitVersion
        UnitMigrationReport         Units;
        bool                        TonemapperRaised = false; // the schema was below kSceneVersionTonemap
        TonemapMigrationReport      Tonemap;
        bool                        CloudNoiseRaised = false; // the schema was below kSceneVersionCloudNoise
        CloudNoiseMigrationReport   CloudNoise;
        bool                        CloudSpeciesRaised = false; // the schema was below kSceneVersionCloudSpecies
        CloudSpeciesMigrationReport CloudSpecies;
        bool                        CloudTypeRaised = false; // the schema was below kSceneVersionCloudType
        CloudTypeMigrationReport    CloudType;
        bool                        CloudSetRaised = false; // the schema was below kSceneVersionCloudSet
        CloudSetMigrationReport     CloudSet;
        // the schema was below kSceneVersionTerrainMaterial
        bool                           TerrainMaterialRaised = false;
        TerrainMaterialMigrationReport TerrainMaterial;
        bool                        MaterialPathRaised = false; // the schema was below kSceneVersionMaterialPath
        MaterialPathMigrationReport MaterialPath;
        // the schema was below kSceneVersionGravityUnits
        bool                        GravityUnitsRaised = false;
        GravityUnitsMigrationReport GravityUnits;
        // the schema was below kSceneVersionUIVisibility
        bool                        UIVisibilityRaised = false;
        UIVisibilityMigrationReport UIVisibility;
        // the schema was below kSceneVersionSSRUnits
        bool                    SSRUnitsRaised = false;
        SSRUnitsMigrationReport SSRUnits;
        // the schema was below kSceneVersionCloudMaterial
        bool                         CloudMaterialRaised = false;
        CloudMaterialMigrationReport CloudMaterial;
        // the schema was below kSceneVersionDebugView
        bool                     DebugViewRaised = false;
        DebugViewMigrationReport DebugView;
        // the schema was below kSceneVersionScriptRoot
        bool                      ScriptRootRaised = false;
        ScriptRootMigrationReport ScriptRoot;
        // the schema was below kSceneVersionServiceAssetRoot
        bool                            ServiceAssetRootRaised = false;
        ServiceAssetRootMigrationReport ServiceAssetRoot;
        // the schema was below kSceneVersionGrassGeneration
        bool                           GrassGenerationRaised = false;
        GrassGenerationMigrationReport GrassGeneration;
        // the schema was below kSceneVersionTextKeySigil
        bool                        TextKeySigilRaised = false;
        TextKeySigilMigrationReport TextKeySigil;
        // the schema was below kSceneVersionAnimGraphAsset
        bool                     AnimGraphRaised = false;
        AnimGraphMigrationReport AnimGraph;
        // the schema was below kSceneVersionRetiredKeys
        bool                       EditMeshRaised = false; // the schema was below kSceneVersionEditMesh
        EditMeshMigrationReport    EditMesh;
        bool                             ProceduralTerrainRaised = false; // below kSceneVersionProceduralTerrain
        ProceduralTerrainMigrationReport ProceduralTerrain;
        bool                             TextureAssetRefsRaised = false; // below kSceneVersionTextureAssetRefs
        TextureAssetRefsMigrationReport  TextureAssetRefs;
        bool                             SiblingOrderRaised = false; // below kSceneVersionSiblingOrder
        bool                             TextHeaderRaised   = false; // below kSceneVersionTextHeader
        SiblingOrderMigrationReport      SiblingOrder;
        bool                       RetiredKeysRaised = false;
        RetiredKeysMigrationReport RetiredKeys;
        bool                             MaterialGuidsRaised = false; // below kSceneVersionMaterialGuids
        MaterialGuidsMigrationReport     MaterialGuids;
        bool                             MeshGuidsRaised = false; // below kSceneVersionMeshGuids
        MeshGuidsMigrationReport         MeshGuids;

        bool Changed() const
        {
            return SkyRaised || UnitsRaised || TonemapperRaised || CloudNoiseRaised || CloudSpeciesRaised ||
                   CloudTypeRaised || CloudSetRaised || TerrainMaterialRaised || MaterialPathRaised ||
                   GravityUnitsRaised || UIVisibilityRaised || SSRUnitsRaised || CloudMaterialRaised ||
                   DebugViewRaised || ScriptRootRaised || ServiceAssetRootRaised || GrassGenerationRaised ||
                   TextKeySigilRaised || AnimGraphRaised || EditMeshRaised || ProceduralTerrainRaised ||
                   TextureAssetRefsRaised || SiblingOrderRaised || TextHeaderRaised || RetiredKeysRaised ||
                   MaterialGuidsRaised || MeshGuidsRaised;
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
    // the repository and writes the result back - the contract's "data migrates once, and is written back
    // in the new form" is only true if something actually writes it back.
    //
    // The three migrations are independent (no sky field is a length, no length lives under
    // "SkyAtmosphere", and the tonemapper touches neither), so the order below is the order they were
    // written and nothing depends on it.
    //
    // `assetsRoot` is what the v7 -> v8 material-path step measures against, and it DEFAULTS to the live
    // content root so that the loader, the migrator tool and the six suites that already call this need no
    // change. The default is evaluated at the call site, which is the only place that knows whether a
    // project has been opened; the step underneath it takes the root explicitly and is tested that way.
    FileMigrationReport
    //
    // `sourceFile` is the file the tree was read from. Only the v22 -> v23 step reads it (its tiles are
    // files named after it); a Terrain block migrated without one is NAMED and left as it was.
    MigrateScene( SceneSerialized&             scene,
                  const std::filesystem::path& assetsRoot = Common::Constants::Path::ASSETS_PATH,
                  const std::filesystem::path& sourceFile = {}, const LegacyMaterialIdMap& legacyIds = {} );

    // What MigratePrefab did to one `.deprefab`, on top of the chain's own report.
    struct PrefabMigrationOutcome
    {
        FileMigrationReport Steps; // what the shared chain did; all-false for the two cases below

        bool AlreadyCurrent = false; // both integers already at the head; nothing to do
        bool StampOnly      = false; // the unstamped pre-Д28 case: stamped, entities deliberately untouched

        // Non-empty: this pair is one no build of this engine writes, or is above the head. Nothing was
        // changed. Distinct from Steps.Refused only in that it also covers the half-stamped pairs below.
        std::string Refused;

        int FoundSceneVersion = 0; // what the tree stated before the step (absent = 0), for the report
        int FoundUnitVersion  = 0;
    };

    // Raises a parsed `.deprefab` to the current generation and stamps it - through the SAME step chain
    // MigrateScene runs, which is the whole point of И11.
    //
    // WHY ONE CHAIN AND NOT A PREFAB ONE. A prefab's payload is `std::vector<Assets::EntityData>` - the
    // scene's own Entities, written by the same ComponentRegistry - so every entity-level schema step is
    // literally the same step. Before this, prefabs shared the scene's two integers but had a migrator
    // with ONE stamp-only step, so raising Core::kSceneVersion left every existing prefab at the old
    // number with no route forward: the loader refused it and so did its migrator. A second numbering
    // scheme would have removed the coupling and replaced it with a second table of steps to keep in
    // hand-sync with the first, for payloads that are the same payloads. So: shared number, shared chain,
    // and adding a step to MigrateFileTree covers both file classes at once.
    //
    // THE FOUR THINGS THE PAIR (SceneVersion, UnitVersion) CAN BE, and what each gets:
    //
    //   (head, head)      current - nothing runs.
    //   (0, 0)            UNSTAMPED, i.e. pre-Д28, when no prefab stated a version at all. STAMP ONLY,
    //                     and this is the one place prefabs and scenes legitimately differ: a `.desce`
    //                     at v0 provably predates v1, because scenes were stamped from the beginning,
    //                     while a `.deprefab` at v0 could be from ANY generation up to the one Д28
    //                     landed in. Running the chain on it would be guessing which - and the sky and
    //                     unit steps are the two that cannot be re-run safely, so the guess costs a
    //                     hundred-fold scale error rather than a wasted pass. The file is stamped and
    //                     the report says stamp-only, so an operator whose prefab then looks wrong knows
    //                     exactly which assumption to disbelieve.
    //   (1..head-1, kUnitVersion)  a stamped older generation: the chain raises it, step by step.
    //   anything else     refused by its own numbers - see below.
    //
    // WHY A UnitVersion THAT IS NOT THE HEAD IS REFUSED RATHER THAN MIGRATED. Both integers are stamped
    // together by the one writer of prefab text (Assets::WritePrefabJson), so a pair stating one and not
    // the other is a file no build produces. The static_assert under this comment is what stops that
    // reasoning from silently outliving the fact it rests on.
    //
    // PURE - no GPU, no filesystem, no global state, like every step it calls. Any `.demat` the v11 ->
    // v12 step produces comes back inside `Steps` for the caller to write, exactly as it does for a
    // scene: a prefab can carry a VolumetricCloud entity like any other.
    static_assert( kUnitVersion == 1,
                   "the world unit has moved: a prefab can now legitimately state a UnitVersion below the "
                   "head, so MigratePrefab must run the unit step instead of refusing the pair - and the "
                   "(0,0) stamp-only case has to be re-argued at the same time" );

    PrefabMigrationOutcome
    MigratePrefab( PrefabData&                  prefab,
                   const std::filesystem::path& assetsRoot = Common::Constants::Path::ASSETS_PATH,
                   const std::filesystem::path& sourceFile = {}, const LegacyMaterialIdMap& legacyIds = {} );

} // namespace Desert::Migration
