#pragma once
#include <unordered_map>
#include <unordered_set>

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
#include <Engine/Animation/SkeletonReference.hpp>
#include <filesystem>
#include <span>
#include <string_view>
#include <Engine/Core/Serialize/SceneFormat.hpp>

// The `.deprefab` payload and its gate. A prefab's entities ARE Core::SceneSerialized::Entities - the
// same struct, written by the same ComponentRegistry - so it is raised by the SAME step chain rather
// than by a second one that would have to be kept in step by hand (И11). See MigratePrefab.
#include <Engine/Assets/Prefab/PrefabFormat.hpp>

#include <Common/Core/Constants.hpp>
#include <Common/Core/ResultStr.hpp>
#include <Engine/Assets/AssetGuidRef.hpp>
#include <Engine/Assets/Serialization/VFXSystem.hpp>
#include <glm/glm.hpp>

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

    //  37 - A PREFAB INSTANCE STATES WHERE IT STANDS (PFX1). A scene record naming a prefab file carries its
    //       root's Translation/Rotation/Scale; the root's override stops stating them. Before, the one fact
    //       the World Partition planner needs about an instance lived only in an override addressed by ids
    //       the `.deprefab` resolves (MigrateInstanceTransformsV36ToV37). Scene-only; a prefab only gains
    //       the stamp.
    inline constexpr int kSceneVersionInstanceTransforms = 37;

    //  38 - THE LANDSCAPE HAS NO BUILT-IN LAYERS (LS-16). The root's LandscapeMaterial block loses GrassMode,
    //       RockMode and SnowMode: the ground is drawn by the Landscape Material and the painted layer infos
    //       alone, as UE's is (MigrateLandscapeLayerModesV37ToV38). Scenes and prefabs alike.
    inline constexpr int kSceneVersionNoLandscapeLayerModes = 38;

    //  39 - EVERY KEY A BLOCK STATES IS ONE THE BUILD DECLARES, IN THE TYPE IT DECLARES (SAVE1). Four
    //       hand-authoring slips the lenient reader used to skip are settled in the files
    //       (MigrateUndeclaredKeysV38ToV39): UIToggle.On and UIButton.CornerRadius were never fields of
    //       either component (UIToggleData has stated its state as `Value` since it was introduced, and
    //       UIButtonData never had a corner radius), so the build never read them and they go; a
    //       UIButton.Action stated as a NAME was a message name (the only string an action carries), so it
    //       becomes SendEvent + that name as the Action Target; a light's Falloff stated as an enumerator
    //       NAME becomes that enumerator's number, which is how every other light in the corpus states it.
    //       Scenes and prefabs alike.
    inline constexpr int kSceneVersionNoUndeclaredKeys = 39;

    //  40 - THE PLAYER'S VIEW IS CHOSEN, NOT DEFAULTED (SPAWN1). Camera.IsMainCamera defaulted to true on
    //       every camera, so a scene with two cameras had two "main" ones and the first in registry order
    //       won. It becomes Camera.AutoActivateForPlayer (UE's bAutoActivate for the player), default false
    //       (MigratePlayerViewFlagV39ToV40): a scene with exactly ONE camera keeps that camera's stated
    //       value (a missing key was the old default, true); in a scene with any other count every camera
    //       states false, and the level names its view through a Default Pawn or by setting one camera.
    //       A prefab override's IsMainCamera goes: the flag belongs to the level's own cameras. Scenes and
    //       prefabs alike.
    inline constexpr int kSceneVersionPlayerViewFlag = 40;

    //  41 - UI ANIMATION IS A TIMELINE SEQUENCE (ANIM-I9). The UIAnim block's own key model (Tracks of
    //       {Property, Keys{Time, Value, Easing}}, Duration, Loop, Playing) becomes {Sequence: the TMLN block
    //       hosted as UIAnimation, Loop: LoopMode, AutoPlay} (MigrateUIAnimationsV40ToV41, through
    //       Timeline::LiftUIAnimation — key times rounded onto the tick grid and REPORTED, easings baked into
    //       keys). The one widget binding names the owning record's UUID. A UIAnim in a prefab override is
    //       refused by name: an override that restates a clip has no v40 whole to lift. Scenes and prefabs alike.
    inline constexpr int kSceneVersionUIAnimationSequences = 41;

    //  42 - THE SCENE'S WIND IS A SOURCE (WIND-SRC, UE AWindDirectionalSource). VolumetricCloud.WindDirection /
    //       WindSpeed are gone; foliage, clouds, cloth and hair read one query, ECS::WindAt, over WindSource
    //       entities (MigrateWindSourceV41ToV42). A scene whose first ENABLED cloud layer had wind (speed above
    //       zero, a non-zero direction; missing keys were the old defaults [1, 0, 0] and 3000 cm/s) gains ONE
    //       directional WindSource record with that direction and speed; a scene with no wind gains nothing. A
    //       second layer's different wind cannot be kept (one scene, one wind) and is REPORTED. A prefab loses
    //       the keys and gains no record (a source is the level's, not a prefab's); prefab overrides lose them.
    inline constexpr int kSceneVersionWindSource = 42;

    //  43 - TIME OF DAY IS ITS OWN COMPONENT (TOD-SPLIT), as UE's SunPosition / SunSky is its own actor. The
    //       five clock keys a SkyAtmosphere block stated (DriveSunFromTimeOfDay, TimeOfDay, DayLengthSeconds,
    //       Latitude, NorthOffset) leave the sky and become a TimeOfDay block on the SAME record
    //       (MigrateTimeOfDayComponentV42ToV43); a sky that stated none of them gets no clock, which is what
    //       it had (the old default did not drive the sun). A prefab override that states one of them on a
    //       SkyAtmosphere is refused by name: the override restates part of a block that no longer holds the
    //       key. Scenes and prefabs alike.
    inline constexpr int kSceneVersionTimeOfDayComponent = 43;


    //  44 - A PARTICLE SPRITE COMPOSITES BY ITS MATERIAL (VFX-08). ParticleEmitter.Blend (0 Additive, 1
    //  AlphaBlend,
    //       missing = AlphaBlend) is removed: how a sprite composites is the blend mode of the material it draws
    //       with (UE BLEND_Additive), and ParticleEmitter.Material names that material (empty = the engine's
    //       translucent sprite template ParticleSpriteDefault). MigrateParticleSpriteMaterialsV43ToV44: an
    //       Additive emitter that names no material gets the shipped additive one (kParticleAdditiveMaterial*);
    //       an AlphaBlend one stays empty. Prefab overrides alike (an override stating AlphaBlend just loses the
    //       key, counted). Scenes and prefabs alike.
    inline constexpr int kSceneVersionParticleSpriteMaterial = 44;

    // The engine's additive particle sprite material: Editor/Resources/Engine/Materials/M_ParticleAdditive.demat.
    inline constexpr const char* kParticleAdditiveMaterialGuid = "6f2b9c41d8e04a57b3a1c0e9f5d27b86";
    inline constexpr const char* kParticleAdditiveMaterialPath =
         "engine:Engine/Materials/M_ParticleAdditive.demat";


    //  45 - A COLLIDER CAN BE A TRIGGER (GP4), UE's Overlap response with bGenerateOverlapEvents. Every Collider
    //       block states IsTrigger and the four overlap-filter keys (OverlapStatic, OverlapKinematic,
    //       OverlapDynamic, OverlapCharacters) with their defaults, written by MigrateTriggerColliderV44ToV45;
    //       a collider that stated none of them is a solid collider, which is what it was, and the corpus stays
    //       the saver's canonical text. Prefab overrides are left alone: an override states only what differs.
    //       Scenes and prefabs alike.
    inline constexpr int kSceneVersionTriggerCollider = 45;


    //  46 - THE GAME MODE HAS ITS RULES (GP3, UE AGameModeBase PlayerControllerClass + the respawn). SceneSettings
    //       gains PlayerController (a prefab handle, unset) and RespawnDelay (seconds, the struct's default)
    //       right after DefaultPawn, where the saver writes them (MigrateGameModeSettingsV45ToV46); a key the
    //       file already states is kept. Scene-only: a prefab has no settings block and gains only the stamp.
    inline constexpr int kSceneVersionGameModeSettings = 46;

    static_assert( kSceneVersionGameModeSettings == kSceneVersion,
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

    // MigrateGameModeSettingsV45ToV46: the number of keys it added to the scene's settings block (0..2).
    int MigrateGameModeSettingsV45ToV46( SceneSerialized& scene );

    // The keys MigrateLandscapeLayerModesV37ToV38 takes out of every LandscapeMaterial block.
    inline constexpr std::array<const char*, 3> kRetiredLandscapeLayerModeKeys = { "GrassMode", "RockMode",
                                                                                   "SnowMode" };

    // Takes kRetiredLandscapeLayerModeKeys out of every LandscapeMaterial block of @p entities (records and
    // their prefab overrides). Returns how many keys went. PURE.
    std::size_t MigrateLandscapeLayerModesV37ToV38( std::vector<Assets::EntityData>& entities );

    // What MigrateUndeclaredKeysV38ToV39 did to one file.
    struct UndeclaredKeysReport
    {
        std::size_t TogglesOnDropped         = 0; // UIToggle.On
        std::size_t ButtonCornerRadiiDropped = 0; // UIButton.CornerRadius
        std::size_t ButtonActionNamesMoved   = 0; // UIButton.Action "<name>" -> SendEvent + OnClickMessage
        std::size_t FalloffNamesNumbered     = 0; // PointLight/SpotLight.Falloff "<name>" -> its number

        // One line per block the step cannot settle without guessing (named entity, key, value). Non-empty
        // refuses the file.
        std::vector<std::string> Refused;

        [[nodiscard]] std::size_t Rewritten() const
        {
            return TogglesOnDropped + ButtonCornerRadiiDropped + ButtonActionNamesMoved + FalloffNamesNumbered;
        }
    };

    // Settles the four slips kSceneVersionNoUndeclaredKeys names in every block of @p entities (records and
    // their prefab overrides). A UIButton whose Action is a name while its OnClickMessage already states a
    // DIFFERENT non-empty target, and a Falloff name that is no LightFalloff enumerator, are refused. PURE.
    UndeclaredKeysReport MigrateUndeclaredKeysV38ToV39( std::vector<Assets::EntityData>& entities );

    // What MigratePlayerViewFlagV39ToV40 did to one file.
    struct PlayerViewFlagReport
    {
        std::size_t Cameras          = 0;     // Camera blocks on the file's own records
        bool        KeptOne          = false; // exactly one camera: its stated (or defaulted) value kept
        std::size_t OverridesDropped = 0;     // IsMainCamera keys taken out of prefab overrides
    };

    // Renames Camera.IsMainCamera -> AutoActivateForPlayer on every record of @p entities under the rule
    // kSceneVersionPlayerViewFlag states, and drops the key from prefab overrides. PURE.
    PlayerViewFlagReport MigratePlayerViewFlagV39ToV40( std::vector<Assets::EntityData>& entities );

    // What MigrateWindSourceV41ToV42 did to one file.
    struct WindSourceReport
    {
        std::size_t CloudWinds       = 0;     // cloud blocks whose WindDirection / WindSpeed were taken out
        bool        Created          = false; // a WindSource record was added (scenes only)
        std::size_t Disagreeing      = 0;     // further cloud layers whose wind differed from the one kept
        std::size_t OverridesDropped = 0;     // wind keys taken out of prefab overrides
    };

    // Moves the cloud layer's wind into a WindSource record under the rule kSceneVersionWindSource states.
    // @p createSource: true for a scene, false for a prefab. The record's id is derived from the id of the cloud
    // layer whose wind it keeps, so a re-run states the same identity and the file's name never reaches it. PURE.
    WindSourceReport MigrateWindSourceV41ToV42( std::vector<Assets::EntityData>& entities, bool createSource );

    // What MigrateUIAnimationsV40ToV41 did to one file.
    struct UIAnimationsReport
    {
        std::size_t              Clips       = 0; // UIAnim blocks lifted
        std::size_t              RoundedKeys = 0; // keys whose time was not on the tick grid (UILiftReport)
        std::vector<std::string> Refused;         // one line per clip that could not be lifted; nothing written
    };

    // Lifts every record's v40 UIAnim block into the v41 {Sequence, Loop, AutoPlay} form under the rule
    // kSceneVersionUIAnimationSequences states; refuses a UIAnim in a prefab override. PURE.
    UIAnimationsReport MigrateUIAnimationsV40ToV41( std::vector<Assets::EntityData>& entities );

    // What MigrateTimeOfDayComponentV42ToV43 did to one file.
    struct TimeOfDayComponentReport
    {
        std::size_t              Clocks    = 0; // TimeOfDay blocks created from a sky's clock keys
        std::size_t              KeysMoved = 0; // clock keys taken out of SkyAtmosphere blocks
        std::vector<std::string> Refused;       // one line per record or override that could not move
    };

    // Moves the five clock keys of every record's SkyAtmosphere block into a TimeOfDay block on the same
    // record under the rule kSceneVersionTimeOfDayComponent states; refuses a clock key in a prefab
    // override. PURE.
    TimeOfDayComponentReport MigrateTimeOfDayComponentV42ToV43( std::vector<Assets::EntityData>& entities );

    // What MigrateParticleSpriteMaterialsV43ToV44 did to one file.
    struct ParticleSpriteMaterialsReport
    {
        std::size_t Emitters          = 0; // ParticleEmitter blocks on the file's own records
        std::size_t MovedToAdditive   = 0; // Blend Additive -> Material = the shipped additive material
        std::size_t OverridesAdditive = 0; // the same, in prefab overrides
        std::size_t OverridesDropped  = 0; // prefab overrides that stated Blend AlphaBlend: the key goes
    };

    // Removes ParticleEmitter.Blend under the rule kSceneVersionParticleSpriteMaterial states. PURE.
    ParticleSpriteMaterialsReport
    MigrateParticleSpriteMaterialsV43ToV44( std::vector<Assets::EntityData>& entities );

    // What MigrateTriggerColliderV44ToV45 did to one file.
    struct TriggerColliderReport
    {
        std::size_t Colliders = 0; // Collider blocks that gained at least one key
        std::size_t KeysAdded = 0;
    };

    // Writes the trigger keys' defaults into every record's Collider block that does not state them, under the
    // rule kSceneVersionTriggerCollider states. Never refuses. PURE.
    TriggerColliderReport MigrateTriggerColliderV44ToV45( std::vector<Assets::EntityData>& entities );

    // VFX-03, NOT YET CHAINED (see REMAINDER-VFX-03: it is chained, with the next scene version, in the
    // same commit that makes the renderer draw VFXComponent and deletes ParticleEmitterComponent - chained
    // earlier, the loader's migration would turn every emitter into a block nothing draws).
    //
    // A ParticleEmitter block becomes a `.dfx` system of ONE emitter whose module stack reproduces it, and the
    // block becomes `VFX { System: {Guid, Path}, AutoActivate: true }`. The file is written next to the owner
    // under VFX/<owner>_<entity uuid>.dfx (relative to the assets root), its GUID MigrationGuidForPath of that
    // path, so a rerun writes the same bytes; emitters of one file with the same numbers share the first's file.
    // System seed 0: VFXWorld seeds an emitter from (system seed, entity uuid, emitter index), and the old
    // component's seed was (0, uuid, 0), so every random stream stays the same.
    inline constexpr const char* kVFXConvertedCategory = "Converted";

    // The v42 ParticleEmitter block's numbers, member for member, with ECS::ParticleEmitterData's v42 defaults
    // (an absent key meant the default); frozen here because the component is deleted.
    struct ParticleEmitterV42
    {
        bool                 Enabled          = true;
        int                  MaxParticles     = 2000;
        float                SpawnRate        = 200.0f;
        bool                 Looping          = true;
        bool                 WorldSpace       = true;
        float                Lifetime         = 3.0f;
        float                LifetimeVariance = 0.2f;
        float                StartSpeed       = 200.0f;
        float                SpeedVariance    = 0.3f;
        glm::vec3            Direction        = glm::vec3( 0.0f, 1.0f, 0.0f );
        float                ConeAngle        = 45.0f;
        glm::vec3            Gravity          = glm::vec3( 0.0f, -200.0f, 0.0f );
        float                StartSize        = 25.0f;
        float                SizeCurvePower   = 1.0f;
        float                EndSize          = 6.0f;
        glm::vec3            StartColor       = glm::vec3( 1.0f, 0.6f, 0.15f );
        glm::vec3            EndColor         = glm::vec3( 0.6f, 0.1f, 0.0f );
        float                StartAlpha       = 1.0f;
        float                EndAlpha         = 0.0f;
        Assets::AssetGuidRef Material; ///< empty = the default sprite template
    };

    // Reads a ParticleEmitter block; a key of the wrong type is an error naming it.
    Common::ResultStr<ParticleEmitterV42> ReadParticleEmitterV42( const rfl::Generic::Object& block );

    // The size-over-life curve's key count when SizeCurvePower != 1 (Linear keys on t^power, so the curve is
    // exact at every key; between keys it is the chord, error <= range * max|f''| / (8 * segments^2)).
    inline constexpr int kVFXConvertedSizeSegments = 16;

    // The system the emitter becomes (no header: the caller stamps it). Material: see REMAINDER-VFX-03 (the
    // `.dfx` sprite renderer gains its Material in VFXS 2).
    Assets::Serialization::VFXSystemData VFXSystemFromParticleEmitter( const ParticleEmitterV42& emitter );

    struct ParticleEmittersToVFXReport
    {
        std::size_t Emitters = 0; // ParticleEmitter blocks on the file's own records, now VFX blocks
        std::size_t Shared   = 0; // of those, how many reuse a file an earlier identical emitter minted
        std::vector<std::pair<std::filesystem::path, std::string>> NewSystems; // absolute path, canonical text
        // A prefab override stating ParticleEmitter keys (a partial block over the prefab's emitter, which this
        // file does not hold) or an unreadable block: the whole file is refused, naming each.
        std::vector<std::string> Refused;
    };

    ParticleEmittersToVFXReport MigrateParticleEmittersToVFX( std::vector<Assets::EntityData>& entities,
                                                              const std::string&               ownerName,
                                                              const std::filesystem::path&     assetsRoot );

    // What MigrateUIAnimationTimelinesV1ToV2 did to one file.
    struct UIAnimationTimelinesReport
    {
        std::size_t              Clips         = 0; // UIAnim blocks whose TMLN v1 sequence was shifted
        std::size_t              SamplesProved = 0; // (component, tick) samples equal under both rules
        std::vector<std::string> Refused;           // one line per block that could not be shifted
    };

    // TMLN v1 -> v2 (ANIM-FMT): every UIAnim block whose Sequence states TMLN v1 has each key's mode moved to
    // the segment leaving it (ClipInterpShift.hpp), proved bit for bit, rewritten by the one writer. Keyed on
    // the block's own number, not the scene's: the timeline block states its meaning itself. PURE.
    UIAnimationTimelinesReport MigrateUIAnimationTimelinesV1ToV2( std::vector<Assets::EntityData>& entities );

    // What MigrateInstanceTransformsV36ToV37 did to one scene.
    struct InstanceTransformsReport
    {
        int Stated = 0; // instance records that now state their root transform

        // Instances that cannot be raised, as "Entities[id=N] > 'Prefabs/X.deprefab': why". Non-empty REFUSES
        // the file: without the prefab's root there is nothing to take the transform from.
        std::vector<std::string> UnknownNames;
    };

    // Every entity record naming a prefab file that does not state its root transform gains it: each of
    // Translation, Rotation and Scale from the instance's root override when that override states it (and
    // the override loses it, Assets::TakeRootTransformOverride), otherwise from the prefab's root record,
    // otherwise the TransformComponent default the loader would have left (0, 0, 1). The prefab file is found
    // the way a mesh file is: under the nearest ancestor of `assetsRoot` that holds it. PURE but for reading
    // the prefab files.
    InstanceTransformsReport MigrateInstanceTransformsV36ToV37( std::vector<Assets::EntityData>& entities,
                                                                const std::filesystem::path&     assetsRoot );

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

    // The SKEL 3 text of a SKEL 1 or 2 `.skeleton`: header GUID, signature, bones, PreviewMesh and
    // CompatibleSkeletons kept (SKEL 1 stated neither: null, []); the dead `Import` provenance dropped (SKEL 3).
    // A file that does not state SKEL 1 or 2 is an error naming what it states. PURE - no filesystem access.
    Common::ResultStr<std::string> MigrateSkeletonToV3( const std::string& text );

    // The ANGR 3 text of an ANGR 2 `.danimgraph`: everything kept, the Output Pose node of the graph and of every
    // implemented layer graph placed where the v2 editor drew it (Animation::Graph::DefaultOutputPosePosition:
    // one column right of the rightmost node, level with the node wired into it). A file that does not state
    // ANGR 2, or whose body does not read, is an error naming why. PURE - no filesystem access.
    Common::ResultStr<std::string> MigrateAnimGraphV2ToV3( const std::string& text );

    // ANIM-SKELREF: one .skeleton as the TargetSkeleton step matches it - header GUID, path relative to its
    // `Assets` root (ReadSkeletonCandidate's form) and every bone name.
    struct TargetSkeletonRig
    {
        std::string                     Guid;
        std::string                     Path;
        std::unordered_set<std::string> Bones;
    };
    Common::ResultStr<TargetSkeletonRig> ReadTargetSkeletonRig( const std::filesystem::path& path,
                                                                const std::string&           text );

    // ANGR 3 -> 4, CRIG 2 -> 3, RTGT 3 -> 4 (ANIM-SKELREF): the file gains TargetSkeleton {Guid, Path} - the one
    // rig of @p rigs the file's own statements fit: every bone name it states of its target (keys "Bone",
    // "BoneName",
    // "*Bone", a Kind "Bone" space's "Target"; never under a "Source*" key) is a bone of the rig, and every clip
    // it plays ("Clip", matched by name in @p clipRigs: clip Name -> its Skeleton GUID) is authored on it. No
    // evidence, or not exactly one fitting rig, is an error naming the candidates. The result is re-read and
    // re-written by the engine's own reader/writer of the kind. @p tag is "ANGR", "CRIG" or "RTGT". PURE.
    Common::ResultStr<std::string>
    StateTargetSkeleton( const std::string& text, const std::string& tag,
                         const std::vector<TargetSkeletonRig>&               rigs,
                         const std::unordered_map<std::string, std::string>& clipRigs );

    // SKEL-TREE (Engine/Animation/SkeletonReference.hpp): what the two raises below resolve a legacy bone hash
    // against - one .skeleton's header GUID, Signature and path (relative to its `Assets` root, the form an
    // AssetGuidRef states). A file that does not read as the current SKEL is an error naming it.
    Common::ResultStr<Animation::SkeletonCandidate> ReadSkeletonCandidate( const std::filesystem::path& path,
                                                                           const std::string&           text );

    // MeshBinary 3/4 -> 5: the 64-byte header's bone hash becomes the 80-byte header's SkeletonGuid (same rule);
    // the table and payloads shift behind the longer prefix, v3 gains the empty Colors/UV1 rows. The result is
    // judged by the engine's DecodeMeshBinary and re-stated by EncodeMeshBinary. PURE.
    Common::ResultStr<std::string>
    MigrateMeshBinaryToV5( std::string_view path, std::string_view bytes,
                           std::span<const Animation::SkeletonCandidate> skeletons );

    // MSAS SRCE 2 -> 3 (a `.stmesh` / `.skmesh` mesh source asset): the skin's bone signature (U64) becomes its
    // skeleton's GUID (Hi, Lo; same rule as above) and the header's dependencies gain it after the materials; a
    // static source changes its SRCE version only. Every other section and byte is kept. The result is judged
    // by the engine's DecodeMeshSourceAsset. A source that does not state SRCE 2 is an error naming what it
    // states. PURE.
    Common::ResultStr<std::string>
    MigrateMeshSourceToV3( std::string_view path, std::string_view bytes,
                           std::span<const Animation::SkeletonCandidate> skeletons );

    // The v3 text of a v2 `.defoliage`: every v2 number kept, CullDistance at UE's default {0, 0} (never
    // culled), the header's GUID kept. A file that does not state FOLT 2 is an error naming what it states.
    // PURE - no filesystem access.
    Common::ResultStr<std::string> MigrateFoliageTypeV2ToV3( const std::string& text );

    // The v4 text of a v3 `.defoliage`: every v3 number kept, Wind at Strength 0 (the instances stand still,
    // as every v3 field drew), the header's GUID kept. A file that does not state FOLT 3 is an error naming
    // what it states. PURE - no filesystem access.
    Common::ResultStr<std::string> MigrateFoliageTypeV3ToV4( const std::string& text );

    // The v5 text of a v4 `.defoliage`: every v4 value kept, IncludeInHLOD true (UE's default; every v4 field
    // stood in its cell's HLOD), the header's GUID kept. A file that does not state FOLT 4 is an error naming
    // what it states. PURE - no filesystem access.
    Common::ResultStr<std::string> MigrateFoliageTypeV4ToV5( const std::string& text );

    // The v6 text of a v5 `.defoliage`: every v5 value kept, Kind Mesh (FOLT 5 had no other kind), the header's
    // GUID kept. A file that does not state FOLT 5 is an error naming what it states. PURE - no filesystem
    // access.
    Common::ResultStr<std::string> MigrateFoliageTypeV5ToV6( const std::string& text );

    // The v7 text of a v6 `.defoliage`: every v6 value kept, Procedural at UE UFoliageType's defaults (FOLT 6
    // had no procedural simulation), the header's GUID kept. A file that does not state FOLT 6 is an error naming
    // what it states. PURE - no filesystem access.
    Common::ResultStr<std::string> MigrateFoliageTypeV6ToV7( const std::string& text );

    // The v8 text of a v7 `.defoliage`: every v7 value kept except Wind.DirectionDegrees, which leaves (the
    // direction is the scene's WindSource, read through ECS::WindAt; the type keeps Strength, Speed, Height),
    // the header's GUID kept. A file that does not state FOLT 7 is an error naming what it states. PURE - no
    // filesystem access.
    Common::ResultStr<std::string> MigrateFoliageTypeV7ToV8( const std::string& text );

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

        bool                     InstanceTransformsRaised = false; // below kSceneVersionInstanceTransforms
        InstanceTransformsReport InstanceTransforms;

        bool        LandscapeLayerModesRaised  = false; // below kSceneVersionNoLandscapeLayerModes
        std::size_t LandscapeLayerModesDropped = 0;

        bool                 UndeclaredKeysRaised = false; // below kSceneVersionNoUndeclaredKeys
        UndeclaredKeysReport UndeclaredKeys;

        bool                 PlayerViewFlagRaised = false; // below kSceneVersionPlayerViewFlag
        PlayerViewFlagReport PlayerViewFlag;

        bool               UIAnimationsRaised = false; // below kSceneVersionUIAnimationSequences
        UIAnimationsReport UIAnimations;

        bool             WindSourceRaised = false; // below kSceneVersionWindSource
        WindSourceReport WindSource;
        bool                     TimeOfDayComponentRaised = false; // below kSceneVersionTimeOfDayComponent
        TimeOfDayComponentReport TimeOfDayComponent;
        bool                     TriggerColliderRaised = false; // below kSceneVersionTriggerCollider
        TriggerColliderReport    TriggerCollider;
        bool                     GameModeSettingsRaised = false; // below kSceneVersionGameModeSettings
        int                      GameModeKeysAdded      = 0;

        // TMLN v1 -> v2 (ANIM-FMT): gated by each UIAnim block's own TMLN number, at any scene version.
        bool ParticleSpriteMaterialsRaised = false; // below kSceneVersionParticleSpriteMaterial
        ParticleSpriteMaterialsReport ParticleSpriteMaterials;

        bool                       UIAnimationTimelinesRaised = false;
        UIAnimationTimelinesReport UIAnimationTimelines;

        bool Changed() const
        {
            return PathOnlyMeshGuidsRaised || FoliageTypesRaised || LandscapeLayerRefsRaised ||
                   ExternalEntitiesRaised || SceneSettingsHomesRaised || InstanceTransformsRaised ||
                   LandscapeLayerModesRaised || UndeclaredKeysRaised || PlayerViewFlagRaised ||
                   UIAnimationsRaised || UIAnimationTimelinesRaised || WindSourceRaised ||
                   TimeOfDayComponentRaised || ParticleSpriteMaterialsRaised ||
                   TriggerColliderRaised ||
                   GameModeSettingsRaised;
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
