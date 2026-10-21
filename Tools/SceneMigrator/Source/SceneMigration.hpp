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

    //  42 - COLLISION IS A PROFILE (PHYS-A1). Bodies filter by UE-style channels and profiles from the project's
    //       Config/CollisionProfiles.json; RigidBody and CharacterController gain CollisionProfile. Its default
    //       ("PhysicsActor") fits a simulated body only, so every RigidBody STATES its profile by Type
    //       (MigrateCollisionProfilesV41ToV42): Static -> "BlockAll", Kinematic -> "BlockAllDynamic",
    //       Dynamic or no Type -> "PhysicsActor"; every CharacterController states "Pawn". A prefab override
    //       that restates RigidBody.Type gets the profile of that Type; a stated profile is kept. Scenes and
    //       prefabs alike.
    inline constexpr int kSceneVersionCollisionProfiles = 42;

    //  43 - THE SCENE'S WIND IS A SOURCE (WIND-SRC, UE AWindDirectionalSource). VolumetricCloud.WindDirection /
    //       WindSpeed are gone; foliage, clouds, cloth and hair read one query, ECS::WindAt, over WindSource
    //       entities (MigrateWindSourceV42ToV43). A scene whose first ENABLED cloud layer had wind (speed above
    //       zero, a non-zero direction; missing keys were the old defaults [1, 0, 0] and 3000 cm/s) gains ONE
    //       directional WindSource record with that direction and speed; a scene with no wind gains nothing. A
    //       second layer's different wind cannot be kept (one scene, one wind) and is REPORTED. A prefab loses
    //       the keys and gains no record (a source is the level's, not a prefab's); prefab overrides lose them.
    inline constexpr int kSceneVersionWindSource = 43;

    //  44 - TIME OF DAY IS ITS OWN COMPONENT (TOD-SPLIT), as UE's SunPosition / SunSky is its own actor. The
    //       five clock keys a SkyAtmosphere block stated (DriveSunFromTimeOfDay, TimeOfDay, DayLengthSeconds,
    //       Latitude, NorthOffset) leave the sky and become a TimeOfDay block on the SAME record
    //       (MigrateTimeOfDayComponentV43ToV44); a sky that stated none of them gets no clock, which is what
    //       it had (the old default did not drive the sun). A prefab override that states one of them on a
    //       SkyAtmosphere is refused by name: the override restates part of a block that no longer holds the
    //       key. Scenes and prefabs alike.
    inline constexpr int kSceneVersionTimeOfDayComponent = 44;

    //  45 - THE GAME MODE HAS ITS RULES (GP3, UE AGameModeBase PlayerControllerClass + the respawn). SceneSettings
    //       gains PlayerController (a prefab handle, unset) and RespawnDelay (seconds, the struct's default)
    //       right after DefaultPawn, where the saver writes them (MigrateGameModeSettingsV44ToV45); a key the
    //       file already states is kept. Scene-only: a prefab has no settings block and gains only the stamp.
    inline constexpr int kSceneVersionGameModeSettings = 45;

    //  46 - A SOUND IS AN ASSET (SOUND-ASSET, UE's USoundWave). AudioSource.Clip, an audio FILE path, becomes
    //       AudioSource.Sound {Guid, Path}: the `.desound` beside that file whose Source names it. Every
    //       sequence an UIAnim block hosts has each Audio section's Sound path replaced by that GUID, and is
    //       rewritten by the TMLN writer (which lists the sound in the sequence header's Dependencies). An
    //       audio file with no `.desound` beside it refuses the file by name — create the asset first.
    //       (MigrateSoundRefsV45ToV46.) Scenes and prefabs alike.
    inline constexpr int kSceneVersionSoundAssets = 46;

    //  47 - A PARTICLE SPRITE COMPOSITES BY ITS MATERIAL (VFX-08). ParticleEmitter.Blend (0 Additive, 1
    //  AlphaBlend,
    //       missing = AlphaBlend) is removed: how a sprite composites is the blend mode of the material it draws
    //       with (UE BLEND_Additive), and ParticleEmitter.Material names that material (empty = the engine's
    //       translucent sprite template ParticleSpriteDefault). MigrateParticleSpriteMaterialsV46ToV47: an
    //       Additive emitter that names no material gets the shipped additive one (kParticleAdditiveMaterial*);
    //       an AlphaBlend one stays empty. Prefab overrides alike (an override stating AlphaBlend just loses the
    //       key, counted). Scenes and prefabs alike.
    inline constexpr int kSceneVersionParticleSpriteMaterial = 47;

    // The engine's additive particle sprite material: Editor/Resources/Engine/Materials/M_ParticleAdditive.demat.
    inline constexpr const char* kParticleAdditiveMaterialGuid = "6f2b9c41d8e04a57b3a1c0e9f5d27b86";
    inline constexpr const char* kParticleAdditiveMaterialPath =
         "engine:Engine/Materials/M_ParticleAdditive.demat";

    //  48 - SSR IS ON BY DEFAULT (SSR1B). PostProcessSettings.EnableSSR defaults to true. The generic writer
    //       stated every field of a volume's Settings, so a v47 "EnableSSR": false is the old default, not a
    //       choice anyone can be told apart from it: every PostProcessVolume record states true
    //       (MigrateSSRDefaultOnV47ToV48); a volume without Settings gets {"EnableSSR": true}; a stated true is
    //       kept. A prefab override's EnableSSR is a delta against its prefab record and is kept as stated. A
    //       scene that must keep SSR off sets it on its volume AFTER this step, as an authored value. Scenes
    //       and prefabs alike.
    inline constexpr int kSceneVersionSSRDefaultOn = 48;

    //  49 - THE SHADOW BIAS IS CENTIMETRES AT THE CASTER (SHADOW-BIAS). DirectionLight.ShadowBias was a
    //       fraction of a cascade's NDC depth, compared at the receiver - one number that meant 26 cm in the
    //       nearest cascade and 10 m in the farthest, so it has no single length to convert to. A stated
    //       ShadowBias is RE-STATED as the new default (kShadowBiasCentimetresDefault, the component's
    //       default) and counted (MigrateShadowBiasCentimetresV48ToV49); ShadowSlopeBias and
    //       ShadowCascadeBiasDistribution are new keys with their defaults. Scenes and prefabs alike.
    inline constexpr int kSceneVersionShadowBiasCentimetres = 49;

    //  50 - A WORLD STATES ITS HLOD LAYERS (WP-FAR-7). WorldPartition gains HLODLayers (UE UHLODLayer: Name, Type
    //       Instancing / MeshMerge / MeshSimplify, ParentLayer) and DefaultHLODLayer, the layer every cell's HLOD
    //       is built with; absent = no HLOD. Every v49 partitioned world built an Instancing HLOD for every cell
    //       with nothing stated, so it now STATES that layer as its default (MigrateHLODLayersV49ToV50, through
    //       Rules::StateInstancingHLODLayer). A world that already states layers keeps them. Scenes only: a prefab
    //       has no WorldPartition block.
    inline constexpr int kSceneVersionHLODLayers = 50;

    //  51 - THE PARTICLE EMITTER IS A VFX SYSTEM (VFX-HOST). Every ParticleEmitter block becomes a `.dfx`
    //       system of one emitter whose module stack reproduces it, played by `VFX { System, AutoActivate: true }`
    //       (MigrateParticleEmittersToVFX); the component is deleted. A prefab override stating ParticleEmitter
    //       keys refuses the file. Scenes and prefabs alike.
    inline constexpr int kSceneVersionParticleEmittersToVFX = 51;

    //  52 - A SCENE STATES WHAT IT REFERENCES (SCENE-DEPS, UE's AssetRegistry dependencies). Header.Dependencies,
    //       written [] by every earlier build, lists every asset the scene names, unique and sorted: each
    //       asset a component or the Settings block names and each prefab instance's prefab, by the ONE rule
    //       the engine's save applies too (Core::GatherSceneDependencies, MigrateSceneDependenciesV51ToV52).
    //       Scenes only.
    inline constexpr int kSceneVersionSceneDependencies = 52;

    //  53 - A VIEW STATES ITS RENDER METHODS (RMODE-IFACE, RENDER-ARCH). PostProcessSettings.GlobalIllumination
    //       (GIMode: 0 Off, 1 ScreenSpace, 2 RSM) is GlobalIlluminationMethod (Common::Scalability::GIMethod, the
    //       method ladder of v53: Off 0, ScreenSpace 2, ReflectiveShadowMaps 3 — RSM is ProbeVolume 3 on today's
    //       ladder, which kSceneVersionGIMethodLadder re-states every number of) and EnableSSR
    //       (bool) is ReflectionsMethod (ReflectionMethod: false -> Off 0, true -> ScreenSpace 2); ShadowsMethod,
    //       ContactShadows and DistanceFieldShadows are new keys with their defaults
    //       (MigrateRenderMethodsV52ToV53). Both the record's volume and a prefab override's delta are renamed:
    //       the key is gone from the type either way. A value outside the old type is refused, naming the entity.
    //       Scenes and prefabs alike.
    inline constexpr int kSceneVersionRenderMethods = 53;

    //  54 - A SCRIPT SLOT HOLDS ONLY WHAT DIFFERS FROM THE SCRIPT (SCR-DETAILS, UE's delta against the CDO). Every
    //       earlier build seeded a slot's Props with ALL the script's properties, so a changed default never
    //       reached an entity that never changed the value. A stated value equal to the default its script
    //       declares (`Name = Property.Float( 300 )`, ...) is dropped; a Number the script now declares Int is
    //       re-stated Int. A value whose declaration states no literal default, a script that is not found, and
    //       a name the script no longer declares keep their values and are listed (MigrateScriptDefaultsV53ToV54).
    //       Scenes and prefabs alike.
    inline constexpr int kSceneVersionScriptDefaults = 54;

    //  55 - THE SUN STATES ITS SOURCE ANGLE (RT-SHADOWS; UE UDirectionalLightComponent::LightSourceAngle).
    //       ECS::DirectionalLightData::SourceAngle (degrees, the sun's angular diameter) drives the cascades'
    //       penumbra and the ray-traced sun shadow's cone. Every DirectionLight record that does not state it is
    //       given kSourceAngleDefault, the component's default (MigrateSourceAngleV54ToV55). A prefab override
    //       is a delta and is left alone. Scenes and prefabs alike.
    inline constexpr int kSceneVersionSourceAngle = 55;

    //  56 - LIGHTS STATE THEIR UNITS; THE EXPOSURE RANGE IS EV100 (LIGHT-UNITS, UE ELightUnits). PointLight /
    //       SpotLight gained IntensityUnits (Unitless 0, Candelas 1, Lumens 2, EV100 3) and their defaults became
    //       UE's (8 cd, InverseSquare): every block states IntensityUnits Unitless - the meaning every older value
    //       had - and a record that stated no Falloff / Intensity states the old defaults (Quadratic, 1). The
    //       volume's AutoExposureMin / Max (luminance clamps, default 0.02 / 8) are AutoExposureMinEV100 /
    //       MaxEV100 = log2(L) + 3, the same clamp (defaults now UE's -10 / 20). A DirectionLight's Intensity was
    //       always the illuminance the BRDF integrates: it is lux now by name, the number is kept. An override
    //       (a delta) only gains the units and has its stated exposure keys renamed (MigrateLightUnitsV55ToV56).
    //       v55 is SourceAngle (Windows); scenes and prefabs alike.
    inline constexpr int kSceneVersionLightUnits = 56;

    //  57 - THE PHYSICAL SKY'S SUN IS THE LIGHT; MANUAL EXPOSURE IS EV100 (LIGHT-UNITS-2, UE). The Physical
    //       Atmosphere sky stopped reading SkyAtmosphere SunColor x SunIntensity: it scatters the atmosphere sun
    //       light's Color x Intensity (lux), as UE. A scene whose physical sky had its own sun keeps its picture
    //       by UE's factors: r = SunColor*SunIntensity / (light Color*Intensity), per channel, multiplies the
    //       sky's SkyAndAerialPerspectiveLuminanceFactor (LUTs, aerial perspective, distant light, bake) and
    //       becomes the light's AtmosphereSunDiskColorScale (the disc). The volume's manual Exposure (a
    //       multiplier m, default 1) is ManualExposureEV100 = log2(key / m) + 3 at the volume's AutoExposureKey
    //       (default 0.18) - the same scale through Graphic::ManualExposureScale. A physical sky with no
    //       atmosphere sun light, or a light of zero illuminance under a lit sky, is refused
    //       (MigrateSkySunFromLightV56ToV57). Scenes and prefabs alike.
    inline constexpr int kSceneVersionSkySunFromLight = 57;

    //  58 - THE CLOCK HAS A CALENDAR (TOD1, UE SunPosition). TimeOfDay gained Longitude, TimeZone,
    //       DaylightSavingTime, Year, Month, Day, and the sun is NOAA's / UE's solar position for that date and
    //       place instead of the equinox path in solar time. Every TimeOfDay block in a record states the
    //       calendar closest to that path - 2026-03-20 at longitude 0 in UTC, no daylight saving - so an old
    //       clock keeps its sun to within the equation of time (minutes) and the equinox declination (under a
    //       degree); a block that already states a calendar key is left alone, and an override (a delta) is
    //       not touched (MigrateSunCalendarV57ToV58). Scenes and prefabs alike.
    inline constexpr int kSceneVersionSunCalendar = 58;

    //  64 - THE GI METHOD LADDER LOST A RUNG (GI-METHOD-RENUMBER). GI-PROBE-SW removed ReflectiveShadowMaps from
    //       Common::Scalability::GIMethod and every method above it moved down one number: the ladder a v53..v63
    //       file was written on is Off 0, Lightmaps 1, ScreenSpace 2, ReflectiveShadowMaps 3, ProbeVolume 4,
    //       SoftwareTraced 5, HardwareTraced 6, PathTraced 7; today's is Off 0, Lightmaps 1, ScreenSpace 2,
    //       ProbeVolume 3, SoftwareTraced 4, HardwareTraced 5, PathTraced 6. Every stated
    //       PostProcessVolume.Settings.GlobalIlluminationMethod - a record's and a prefab override's delta - is
    //       re-stated by that table; ReflectiveShadowMaps 3 becomes ProbeVolume 3 (the off-screen bounce that
    //       replaced it), the number unchanged. A value outside the old ladder is refused, naming the entity
    //       (MigrateGIMethodLadderV63ToV64). v59..v63 are Windows' steps. Scenes and prefabs alike.
    inline constexpr int kSceneVersionGIMethodLadder = 64;

    //  65 - THE DISPLAY TRANSFER IS THE sRGB OETF (RENDER-LOOK round 05). The composite encodes with the sRGB
    //  curve,
    //       as UE does for its sRGB output device (GammaCorrectionCommon.ush LinearToSrgb), in place of
    //       pow(1 / Gamma): the transfer belongs to the display, not to a volume's look, so
    //       Core::PostProcessSettings lost Gamma. Every stated PostProcessVolume.Settings.Gamma - a record's and a
    //       prefab override's delta - is dropped, whatever its value: no value of it has a meaning now
    //       (MigrateDisplayTransferV64ToV65). Scenes and prefabs alike.
    inline constexpr int kSceneVersionDisplayTransfer = 65;

    //  66 - A WORLD PARTITION GRID HAS A NAME (WP22). An entity chooses its grid by name (RuntimeGrid block, UE
    //       AActor::RuntimeGrid), so every grid states one, unique in the world, and a Priority (default 0). Every
    //       v65 world partitioned everything on its first grid, so that grid becomes "Main" (what an entity with
    //       no RuntimeGrid streams on, as before) and every further grid "Grid<index>" - no record names those, so
    //       none of them gains a member (MigrateGridNamesV65ToV66). Scenes only: a prefab has no WorldPartition.
    inline constexpr int kSceneVersionGridNames = 66;

    //  67 - A LANDSCAPE TILE'S HeightFile IS A CONTENT KEY (WP-FAR-6B). The record names its height file as
    //       "assets:<dir>/<scene>_Landscape/<id>.dlht" (Common::AssetHandle::StableKeyForPath, UE's /Game/...
    //       package path), not as the project-relative folder ("Content/..."): a package mounts the assets root
    //       under its own name, so only the key resolves there, and the load refuses any other spelling
    //       (World::Landscape::ReadLandscapeTileByKey). The old spelling is relative to the PROJECT; the step
    //       finds the project by the .deproj above the scene and measures against its AssetsRoot. A spelling
    //       that is not under it is refused, naming the entity (MigrateLandscapeHeightKeysV66ToV67). Scenes
    //       only: a landscape tile is a scene's.
    inline constexpr int kSceneVersionLandscapeHeightKeys = 67;

    //  68 - EVERY ASSET REFERENCE IS A CONTENT KEY (PATH1). Sixteen asset types (materials, cloud volumes,
    //       rigs, graphs, themes, retargets, prefabs, foliage types, fractures, waves, input contexts, physics,
    //       sounds, VFX systems, world parameters, weather presets) wrote the path relative to the assets root
    //       ("Materials/M.demat") and the loader joined it to ASSETS_PATH; meshes were also spelled relative to
    //       the project ("Resources/Assets/Meshes/M.skmesh"). The joins are gone - the load refuses a bare
    //       spelling (ComponentRegistry FileForStoredReference) - so every string of a block (records, prefab
    //       overrides, scene settings) that is a relative file path naming a file under the assets root, by
    //       either spelling, becomes "assets:<relative>" (MigrateAssetKeysV67ToV68). A spelling that names no
    //       file is left as stated: it cannot be told from text, and its load refuses it by name. Scenes and
    //       prefabs alike.
    inline constexpr int kSceneVersionAssetKeys = 68;

    static_assert( kSceneVersionAssetKeys == kSceneVersion,
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

    // MigrateGameModeSettingsV44ToV45: the number of keys it added to the scene's settings block (0..2).
    int MigrateGameModeSettingsV44ToV45( SceneSerialized& scene );

    // What a stated pre-v49 ShadowBias becomes: ECS::DirectionalLightData::ShadowBias's default, in centimetres.
    inline constexpr float kShadowBiasCentimetresDefault = 1.0f;

    // Re-states every DirectionLight block's ShadowBias of @p entities (records and their prefab overrides)
    // as kShadowBiasCentimetresDefault. Returns how many it re-stated. PURE.
    std::size_t MigrateShadowBiasCentimetresV48ToV49( std::vector<Assets::EntityData>& entities );

    // What a pre-v55 DirectionLight is given: ECS::DirectionalLightData::SourceAngle's default, in degrees (UE's
    // LightSourceAngle default, the sun's angular diameter).
    inline constexpr float kSourceAngleDefault = 0.5357f;

    // States kSourceAngleDefault in every DirectionLight record of @p entities that has no SourceAngle (prefab
    // overrides are deltas: untouched). Returns how many it stated. PURE.
    std::size_t MigrateSourceAngleV54ToV55( std::vector<Assets::EntityData>& entities );

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

    // What MigrateWindSourceV42ToV43 did to one file.
    struct WindSourceReport
    {
        std::size_t CloudWinds       = 0;     // cloud blocks whose WindDirection / WindSpeed were taken out
        bool        Created          = false; // a WindSource record was added (scenes only)
        std::size_t Disagreeing      = 0;     // further cloud layers whose wind differed from the one kept
        std::size_t OverridesDropped = 0;     // wind keys taken out of prefab overrides
    };

    // Moves the cloud layer's wind into a WindSource record under the rule kSceneVersionWindSource states.
    // @p createSource: true for a scene, false for a prefab. The record's id is derived from @p fileName so a
    // re-run states the same identity. PURE.
    WindSourceReport MigrateWindSourceV42ToV43( std::vector<Assets::EntityData>& entities,
                                                const std::string& fileName, bool createSource );

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

    // What MigrateCollisionProfilesV41ToV42 did to one file.
    struct CollisionProfilesReport
    {
        std::size_t              Bodies     = 0; // RigidBody blocks (records and overrides) given a profile
        std::size_t              Characters = 0; // CharacterController blocks given "Pawn"
        std::vector<std::string> Refused; // one line per block whose Type is not a BodyType; nothing written
    };

    // States CollisionProfile on every RigidBody / CharacterController block under the rule
    // kSceneVersionCollisionProfiles states. PURE.
    CollisionProfilesReport MigrateCollisionProfilesV41ToV42( std::vector<Assets::EntityData>& entities );
    // What MigrateSoundRefsV45ToV46 did to one file.
    struct SoundRefsReport
    {
        std::size_t              Sources  = 0; // AudioSource blocks whose Clip became a Sound reference
        std::size_t              Sections = 0; // sequence Audio sections whose Sound path became a GUID
        std::vector<std::string> Refused;      // one line per reference with no `.desound`; nothing written
    };

    // The `.desound` whose Source names the audio file @p audioFile (absolute): searched beside it, the way
    // UE keeps the imported source beside its asset. Refuses none and more than one, by name.
    Common::ResultStr<std::pair<Common::Content::AssetGuid, std::filesystem::path>>
    SoundForAudioFile( const std::filesystem::path& audioFile );

    // Raises AudioSource.Clip and every hosted sequence's Audio Sound path to GUID references under the rule
    // kSceneVersionSoundAssets states. Paths are relative to @p assetsRoot (or absolute). Reads `.desound`s.
    SoundRefsReport MigrateSoundRefsV45ToV46( std::vector<Assets::EntityData>& entities,
                                              const std::filesystem::path&     assetsRoot );

    // What MigrateTimeOfDayComponentV43ToV44 did to one file.
    struct TimeOfDayComponentReport
    {
        std::size_t              Clocks    = 0; // TimeOfDay blocks created from a sky's clock keys
        std::size_t              KeysMoved = 0; // clock keys taken out of SkyAtmosphere blocks
        std::vector<std::string> Refused;       // one line per record or override that could not move
    };

    // Moves the five clock keys of every record's SkyAtmosphere block into a TimeOfDay block on the same
    // record under the rule kSceneVersionTimeOfDayComponent states; refuses a clock key in a prefab
    // override. PURE.
    TimeOfDayComponentReport MigrateTimeOfDayComponentV43ToV44( std::vector<Assets::EntityData>& entities );
    // What MigrateParticleSpriteMaterialsV46ToV47 did to one file.
    struct ParticleSpriteMaterialsReport
    {
        std::size_t Emitters          = 0; // ParticleEmitter blocks on the file's own records
        std::size_t MovedToAdditive   = 0; // Blend Additive -> Material = the shipped additive material
        std::size_t OverridesAdditive = 0; // the same, in prefab overrides
        std::size_t OverridesDropped  = 0; // prefab overrides that stated Blend AlphaBlend: the key goes
    };

    // Removes ParticleEmitter.Blend under the rule kSceneVersionParticleSpriteMaterial states. PURE.
    ParticleSpriteMaterialsReport
    MigrateParticleSpriteMaterialsV46ToV47( std::vector<Assets::EntityData>& entities );

    // The v51 step (kSceneVersionParticleEmittersToVFX). A ParticleEmitter block becomes a `.dfx` system of ONE
    // emitter whose module stack reproduces it, and the block becomes `VFX { System: <assets-relative path>,
    // AutoActivate: true }`. The file is written next to the owner
    // under VFX/<owner>_<entity uuid>.dfx (relative to the assets root), its GUID MigrationGuidForPath of that
    // path, so a rerun writes the same bytes; emitters of one file with the same numbers share the first's file.
    // System seed 0: VFXWorld seeds an emitter from (system seed, entity uuid, emitter index), and the old
    // component's seed was (0, uuid, 0), so every random stream stays the same.
    inline constexpr const char* kVFXConvertedCategory = "Converted";

    // The v43 ParticleEmitter block's numbers, member for member, with ECS::ParticleEmitterData's v43 defaults
    // (an absent key meant the default); frozen here because the component is deleted.
    struct ParticleEmitterV43
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
    Common::ResultStr<ParticleEmitterV43> ReadParticleEmitterV43( const rfl::Generic::Object& block );

    // The size-over-life curve's key count when SizeCurvePower != 1 (Linear keys on t^power, so the curve is
    // exact at every key; between keys it is the chord, error <= range * max|f''| / (8 * segments^2)).
    inline constexpr int kVFXConvertedSizeSegments = 16;

    // The system the emitter becomes (no header: the caller stamps it). Material: see REMAINDER-VFX-03 (the
    // `.dfx` sprite renderer gains its Material in VFXS 2).
    Assets::Serialization::VFXSystemData VFXSystemFromParticleEmitter( const ParticleEmitterV43& emitter );

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

    // What MigrateSSRDefaultOnV47ToV48 did to one file.
    struct SSRDefaultOnReport
    {
        std::size_t              Volumes = 0; // PostProcessVolume records that now state EnableSSR true
        std::vector<std::string> Refused;     // a volume whose Settings or EnableSSR is not the declared type
    };

    // States EnableSSR true on every PostProcessVolume record under the rule kSceneVersionSSRDefaultOn
    // states. PURE.
    SSRDefaultOnReport MigrateSSRDefaultOnV47ToV48( std::vector<Assets::EntityData>& entities );

    // What MigrateRenderMethodsV52ToV53 did to one file.
    struct RenderMethodsReport
    {
        std::size_t              Rewritten = 0; // GlobalIllumination / EnableSSR keys re-stated as methods
        std::vector<std::string> Refused;       // a Settings block or old value that is not the declared type
    };

    // Re-states PostProcessVolume.Settings GlobalIllumination / EnableSSR as GlobalIlluminationMethod /
    // ReflectionsMethod under the rule kSceneVersionRenderMethods states, in the record and in every prefab
    // override. PURE.
    RenderMethodsReport MigrateRenderMethodsV52ToV53( std::vector<Assets::EntityData>& entities );

    // What MigrateScriptDefaultsV53ToV54 did to one file.
    struct ScriptDefaultsReport
    {
        std::size_t              Dropped = 0; // stated values equal to their script's default, gone
        std::size_t              Retyped = 0; // Number values the script declares Int, re-stated Int
        std::vector<std::string> Kept;        // values kept with the reason (no literal default, no script, ...)
        std::vector<std::string> Refused;     // a Script block or slot that is not the declared shape
    };

    // Drops every script property value equal to the default its script declares, under the rule
    // kSceneVersionScriptDefaults states, in the record and in every prefab override. Reads the scripts under
    // @p assetsRoot (the `assets:` mount).
    ScriptDefaultsReport MigrateScriptDefaultsV53ToV54( std::vector<Assets::EntityData>& entities,
                                                        const std::filesystem::path&     assetsRoot );

    // What MigrateLightUnitsV55ToV56 did to one file.
    struct LightUnitsReport
    {
        std::size_t              Lights  = 0; // PointLight / SpotLight blocks that now state IntensityUnits
        std::size_t              Volumes = 0; // PostProcessVolume Settings re-stated in EV100
        std::vector<std::string> Refused;     // a Settings / exposure value that is not the old shape
    };

    // Raises PointLight / SpotLight and PostProcessVolume blocks to the rule kSceneVersionLightUnits states, in
    // the record and in every prefab override. PURE.
    LightUnitsReport MigrateLightUnitsV55ToV56( std::vector<Assets::EntityData>& entities );

    // What MigrateSkySunFromLightV56ToV57 did to one file.
    struct SkySunFromLightReport
    {
        std::size_t              Skies   = 0; // physical skies whose sun ratio was folded into UE's factors
        std::size_t              Volumes = 0; // PostProcessVolume Settings whose Exposure is now EV100
        std::vector<std::string> Refused;
    };

    // Raises SkyAtmosphere / DirectionLight / PostProcessVolume blocks to the rule kSceneVersionSkySunFromLight
    // states, in the record and in every prefab override (an override only has its stated Exposure re-stated).
    // PURE.
    SkySunFromLightReport MigrateSkySunFromLightV56ToV57( std::vector<Assets::EntityData>& entities );

    // What MigrateSunCalendarV57ToV58 did to one file.
    struct SunCalendarReport
    {
        std::size_t Clocks = 0; // TimeOfDay blocks that now state the equinox calendar
    };

    // States the calendar kSceneVersionSunCalendar names on every record's TimeOfDay block that states none.
    // PURE.
    SunCalendarReport MigrateSunCalendarV57ToV58( std::vector<Assets::EntityData>& entities );

    // What MigrateGIMethodLadderV63ToV64 did to one file.
    struct GIMethodLadderReport
    {
        std::size_t              Restated = 0; // GlobalIlluminationMethod values whose number changed
        std::vector<std::string> Refused;      // a Settings block or a value that is not on the old ladder
    };

    // Re-states every PostProcessVolume.Settings.GlobalIlluminationMethod by the old -> new table
    // kSceneVersionGIMethodLadder states, in the record and in every prefab override. PURE.
    GIMethodLadderReport MigrateGIMethodLadderV63ToV64( std::vector<Assets::EntityData>& entities );

    // What MigrateDisplayTransferV64ToV65 did to one file.
    struct DisplayTransferReport
    {
        std::size_t              Dropped = 0; // PostProcessVolume.Settings.Gamma keys removed
        std::vector<std::string> Refused;     // a Settings block that is not an object
    };

    // Drops every PostProcessVolume.Settings.Gamma kSceneVersionDisplayTransfer names, in the record and in every
    // prefab override. PURE.
    DisplayTransferReport MigrateDisplayTransferV64ToV65( std::vector<Assets::EntityData>& entities );

    // States the Instancing HLOD layer as the default of a partitioned world that states no layers, under the
    // rule kSceneVersionHLODLayers states. True when it wrote one. PURE.
    bool MigrateHLODLayersV49ToV50( SceneSerialized& scene );

    // Names every unnamed grid of a partitioned world (kSceneVersionGridNames); how many it named.
    std::size_t MigrateGridNamesV65ToV66( SceneSerialized& scene );

    // What MigrateAssetKeysV67ToV68 did to one file.
    struct AssetKeysReport
    {
        std::size_t              Keyed = 0; // bare spellings restated as "assets:" keys
        std::vector<std::string> Refused;   // non-empty: the file is not written
    };

    // Restates every bare relative file path naming a file under @p assetsRoot as its "assets:" key, in every
    // record, every prefab override and, when given, the scene settings (kSceneVersionAssetKeys).
    AssetKeysReport MigrateAssetKeysV67ToV68( std::vector<Assets::EntityData>& entities,
                                              std::optional<rfl::Generic>*     settings,
                                              const std::filesystem::path&     assetsRoot );

    // What MigrateLandscapeHeightKeysV66ToV67 did to one scene.
    struct LandscapeHeightKeysReport
    {
        std::size_t              Keyed = 0; // LandscapeTile.HeightFile spellings re-stated as content keys
        std::vector<std::string> Refused;   // a spelling with no project, or not under its assets root
    };

    // Re-states every LandscapeTile.HeightFile that is not an "assets:" key - a path relative to the project
    // that holds @p sourceFile (the nearest .deproj above it) - as the key of that file under the project's
    // AssetsRoot (kSceneVersionLandscapeHeightKeys), in the record and in every prefab override.
    LandscapeHeightKeysReport MigrateLandscapeHeightKeysV66ToV67( std::vector<Assets::EntityData>& entities,
                                                                  const std::filesystem::path&     sourceFile );

    // What MigrateSceneDependenciesV51ToV52 did to one scene.
    struct SceneDependenciesReport
    {
        std::size_t              Stated = 0; // GUIDs the header lists after the step
        std::vector<std::string> Refused;    // one line per prefab instance whose file states no GUID
    };

    // Header.Dependencies under the rule kSceneVersionSceneDependencies states. Prefab paths are located
    // against @p assetsRoot (LocateMeshFile's rule) and read for their header GUID.
    SceneDependenciesReport MigrateSceneDependenciesV51ToV52( SceneSerialized&             scene,
                                                              const std::filesystem::path& assetsRoot );

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

    // The SKEL 4 text of a SKEL 1, 2 or 3 `.skeleton`: header GUID, signature, bones, PreviewMesh and
    // CompatibleSkeletons kept (SKEL 1 stated neither: null, []); the dead `Import` provenance dropped (SKEL 3);
    // Sockets and BoneMasks stated empty (SKEL 4, ANIM-FIX5a: no earlier generation had either). A file that does
    // not state SKEL 1, 2 or 3 is an error naming what it states. PURE - no filesystem access.
    Common::ResultStr<std::string> MigrateSkeletonToV4( const std::string& text );

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

    // ANGR 3 -> 4, CRIG 2 -> 3 (then -> 4 by MigrateControlRigToV4), RTGT 3 -> 4 (ANIM-SKELREF): the file gains
    // TargetSkeleton {Guid, Path} - the one
    // rig of @p rigs the file's own statements fit: every bone name it states of its target (keys "Bone",
    // "BoneName",
    // "*Bone", a Kind "Bone" space's "Target"; never under a "Source*" key) is a bone of the rig, and every clip
    // it plays ("Clip", matched by name in @p clipRigs: clip Name -> its Skeleton GUID) is authored on it. No
    // evidence, or not exactly one fitting rig, is an error naming the candidates. The result is re-read and
    // re-written by the engine's own reader/writer of the kind. @p tag is "ANGR", "CRIG" or "RTGT". PURE.
    // CRIG 3 -> 4 (ANIM-FIX11): the old `Graph` becomes `Graphs[{Event "Forwards"}]`, each of its nodes gets the
    // `Position` the rig canvas used to invent for node i — (40 + 260 * (i % 4), 40 + 180 * (i / 4)) — and every
    // control without `Limits` gets an empty one. Re-read and re-written by the engine's own .derig reader and
    // writer. PURE.
    Common::ResultStr<std::string> MigrateControlRigToV4( const std::string& text );

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
    // The v6 text of a v5 `.decloudtype`: every v5 value kept, `Shape.Profile.Density` written as
    // Graphic::CloudProfileDensityBaseRamp (CLTY 5 had no altitude density, so every v5 type was dense from
    // its base up — the box H-BASE takes away), the header's GUID kept. A file that does not state CLTY 5 is
    // an error naming what it states. PURE - no filesystem access.
    Common::ResultStr<std::string> MigrateCloudTypeV5ToV6( const std::string& text );

    // The v7 text of a v6 `.decloudtype`: every v6 value kept, `Shape.BodyDiameterKm` written as the default
    // type's (FIELD-GRAIN: the exponential body-size law's D0), the header's GUID kept. A file that does not
    // state CLTY 6 is an error naming what it states. PURE - no filesystem access.
    Common::ResultStr<std::string> MigrateCloudTypeV6ToV7( const std::string& text );

    // A MATL 4 `PatchStrength` s — the slope of the ramp W = saturate(1 - 2 s u) — as the MATL 5 clear share of
    // the sky that ramp left: max(0, 1 - 1/(2s)), the fraction of the sky with W = 0 (FIELD-GRAIN-b).
    float MigratePatchStrengthV4ToV5( float strength );

    // The v5 text of a v4 `.demat`: every `PatchStrength` parameter (the cloud material's and its instances')
    // raised by MigratePatchStrengthV4ToV5, everything else and the header's GUID kept. A file that does not
    // state MATL 4 is an error naming what it states. PURE - no filesystem access.
    Common::ResultStr<std::string> MigrateMaterialV4ToV5( const std::string& text );

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

        bool                    CollisionProfilesRaised = false; // below kSceneVersionCollisionProfiles
        CollisionProfilesReport CollisionProfiles;
        bool                     WindSourceRaised = false; // below kSceneVersionWindSource
        WindSourceReport         WindSource;
        bool                     TimeOfDayComponentRaised = false; // below kSceneVersionTimeOfDayComponent
        TimeOfDayComponentReport TimeOfDayComponent;
        bool                     GameModeSettingsRaised = false; // below kSceneVersionGameModeSettings
        int                      GameModeKeysAdded      = 0;
        bool                     SoundRefsRaised        = false; // below kSceneVersionSoundAssets
        SoundRefsReport          SoundRefs;

        bool ParticleSpriteMaterialsRaised = false; // below kSceneVersionParticleSpriteMaterial
        ParticleSpriteMaterialsReport ParticleSpriteMaterials;

        bool               SSRDefaultOnRaised = false; // below kSceneVersionSSRDefaultOn
        SSRDefaultOnReport SSRDefaultOn;

        bool ParticleEmittersToVFXRaised = false; // below kSceneVersionParticleEmittersToVFX
        ParticleEmittersToVFXReport ParticleEmittersToVFX;

        bool        ShadowBiasCentimetresRaised = false; // below kSceneVersionShadowBiasCentimetres
        std::size_t ShadowBiasesRestated        = 0;

        bool HLODLayersRaised = false; // below kSceneVersionHLODLayers
        bool HLODLayerStated  = false; // a partitioned world was given the Instancing layer
        bool        GridNamesRaised = false; // below kSceneVersionGridNames
        std::size_t GridsNamed      = 0;     // grids given a name
        bool                      LandscapeHeightKeysRaised = false; // below kSceneVersionLandscapeHeightKeys
        LandscapeHeightKeysReport LandscapeHeightKeys;
        bool                      AssetKeysRaised = false; // below kSceneVersionAssetKeys
        AssetKeysReport           AssetKeys;

        bool                    SceneDependenciesRaised = false; // below kSceneVersionSceneDependencies
        SceneDependenciesReport SceneDependencies;

        bool                RenderMethodsRaised = false; // below kSceneVersionRenderMethods
        RenderMethodsReport RenderMethods;

        bool                 ScriptDefaultsRaised = false; // below kSceneVersionScriptDefaults
        ScriptDefaultsReport ScriptDefaults;

        bool        SourceAngleRaised  = false; // below kSceneVersionSourceAngle
        std::size_t SourceAnglesStated = 0;

        bool             LightUnitsRaised = false; // below kSceneVersionLightUnits
        LightUnitsReport LightUnits;

        bool                  SkySunFromLightRaised = false; // below kSceneVersionSkySunFromLight
        SkySunFromLightReport SkySunFromLight;

        bool              SunCalendarRaised = false; // below kSceneVersionSunCalendar
        SunCalendarReport SunCalendar;

        bool                 GIMethodLadderRaised = false; // below kSceneVersionGIMethodLadder
        GIMethodLadderReport GIMethodLadder;

        bool                  DisplayTransferRaised = false; // below kSceneVersionDisplayTransfer
        DisplayTransferReport DisplayTransfer;

        // TMLN v1 -> v2 (ANIM-FMT): gated by each UIAnim block's own TMLN number, at any scene version.
        bool                       UIAnimationTimelinesRaised = false;
        UIAnimationTimelinesReport UIAnimationTimelines;

        bool Changed() const
        {
            return PathOnlyMeshGuidsRaised || FoliageTypesRaised || LandscapeLayerRefsRaised ||
                   ExternalEntitiesRaised || SceneSettingsHomesRaised || InstanceTransformsRaised ||
                   LandscapeLayerModesRaised || UndeclaredKeysRaised || PlayerViewFlagRaised ||
                   UIAnimationsRaised || UIAnimationTimelinesRaised || CollisionProfilesRaised ||
                   WindSourceRaised || TimeOfDayComponentRaised || GameModeSettingsRaised || SoundRefsRaised ||
                   ParticleSpriteMaterialsRaised || SSRDefaultOnRaised || ShadowBiasCentimetresRaised ||
                   HLODLayersRaised || ParticleEmittersToVFXRaised || SceneDependenciesRaised ||
                   RenderMethodsRaised || ScriptDefaultsRaised || SourceAngleRaised || LightUnitsRaised ||
                   SkySunFromLightRaised || SunCalendarRaised || GIMethodLadderRaised || DisplayTransferRaised ||
                   GridNamesRaised || LandscapeHeightKeysRaised || AssetKeysRaised;
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
