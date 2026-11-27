#pragma once

#include <Engine/Assets/Prefab/PrefabData.hpp>
#include <Engine/Assets/TextAssetHeaderStamp.hpp>
#include <Engine/Core/Serialize/ForeignKeys.hpp>
#include <Engine/Core/Serialize/WorldPartitionFormat.hpp>

#include <Common/Json/Carry.hpp>



#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::Core
{
    // THE GENERATION OF .desce THIS ENGINE WRITES. The saver stamps it; the loader raises an older file to it
    // IN MEMORY (Core/SceneUpgrade/SceneUpgrade.hpp, UE's FCustomVersion upgrade on load: "upgraded vX->vY in memory,
    // save to keep"), and refuses a NEWER one, or one below kSceneUpgradeFloor, by name with nothing created for it.
    // Tools/SceneMigrator is the resave: the same UpgradeSceneRecords, then the file written back.
    //
    // HISTORY - WHY THE OLD STEPS ARE NOT HERE (they still are not: the steps below the floor move data between
    // files, so they stay in the tool). There used to be eight of them, one per schema step, and each was
    // written with a shelf life it never got: "deleted once no v<n> file remains". Eight accumulated steps
    // is eight expiries that were never enforced, and DEV_CONTRACT §4.3 - "the runtime knows nothing about
    // the old format" - had been false for every one of them. The steps were not deleted; they MOVED, whole,
    // to Tools/SceneMigrator, which is where a conversion belongs: it runs once, over files, and writes the
    // result back, which is the only thing that ever makes a migration expire (§4.6). An old file is
    // converted by one run of that tool instead of by every scene load of every build, forever.
    //
    // BUMPING THIS. Raising the number is one edit here plus one step file Core/SceneUpgrade/Steps/V<N><Name> and
    // its row in Core/SceneUpgrade/SceneUpgradeSteps.cpp, whose static_assert refuses a number no row reaches
    // (UpgradeSceneRecords walks that table): the loader raises an older file in memory with it, and
    // Tools/SceneMigrator - the resave - writes the same result back. A step that must write OTHER files stays in the
    // tool, below the floor.
    //
    // AND IT IS NOT ONLY SCENES. A `.deprefab` carries the same Assets::EntityData and states THIS number
    // (Engine/Assets/Prefab/PrefabData.hpp), so raising it moves prefabs too. An entity-level step added to
    // Tools/SceneMigrator raises both, because both enter the same chain, and the one run of the tool over
    // the tree converts both, because it collects both extensions. That is the whole of what И11 had to
    // build: before it, prefabs shared this number with a migrator that had no steps, so a bump left every
    // existing prefab at the old generation with nothing able to read it.
    //
    // AND IT IS WHAT MAKES RETIREMENT WORK. Since K11 a key this build does not declare is PRESERVED across
    // a save (Serialize/ForeignKeys.hpp) - so the only thing that can ever remove one is a deliberate
    // decision, and the only place that decision can be written down is a migration step, which names the
    // key, drops it from the FILES, and is made compulsory by this number moving. The loader therefore
    // needs no list of dead keys and must never grow one: "retired" is a fact about a conversion that has
    // already happened, not a rule the runtime carries.
    //
    // v35 (WP16): a partitioned world keeps one file per entity (Serialize/ExternalEntities.hpp). The typed
    // tree did not change; the FILE LAYOUT did, and an older build reading the header alone would load a world
    // with no entities in it - so the number moves and that build refuses the file by name.
    //
    // v36 (SET1): SceneSettings' grade and shadow fields moved to an Unbound PostProcessVolume entity and to
    // the DirectionalLight (Tools/SceneMigrator, MigrateSceneSettingsHomesV35ToV36).
    // v37 (PFX1): a prefab instance's record states its root's Translation/Rotation/Scale itself, and the
    // root's override no longer does (Tools/SceneMigrator, MigrateInstanceTransformsV36ToV37).
    // v38 (LS-16): the landscape's look is its Landscape Material and its layer infos only; the root's
    // LandscapeMaterial block no longer states GrassMode/RockMode/SnowMode (Tools/SceneMigrator,
    // MigrateLandscapeLayerModesV37ToV38).
    // v39 (SAVE1): every key a component block states is one the build declares, in its declared type; the
    // corpus's four hand-authoring slips are settled in the files (Tools/SceneMigrator,
    // MigrateUndeclaredKeysV38ToV39), and the corpus is the saver's canonical text.
    // v40 (SPAWN1): Camera.IsMainCamera is Camera.AutoActivateForPlayer, default false; only a scene's sole
    // camera keeps it set (Tools/SceneMigrator, MigratePlayerViewFlagV39ToV40).
    // v42 (PHYS-A1): RigidBody / CharacterController state CollisionProfile (a name in the project's
    // Config/CollisionProfiles.json), chosen by body Type (Tools/SceneMigrator, MigrateCollisionProfilesV41ToV42).
    // v43 (WIND-SRC): VolumetricCloud.WindDirection / WindSpeed are one WindSource record per scene
    // (Tools/SceneMigrator, MigrateWindSourceV42ToV43).
    // v44 (TOD-SPLIT): the sky's five clock keys are a TimeOfDay block on the same record (Tools/SceneMigrator,
    // MigrateTimeOfDayComponentV43ToV44).
    // v45 (GP3): SceneSettings states PlayerController and RespawnDelay (Tools/SceneMigrator,
    // MigrateGameModeSettingsV44ToV45).
    // v46 (SOUND-ASSET): a sound is a `.desound` asset named by GUID — AudioSource.Clip (a file path) is
    // AudioSource.Sound {Guid, Path}, and a sequence's Audio section names the sound's GUID (Tools/SceneMigrator,
    // MigrateSoundRefsV45ToV46).
    // v47 (VFX-08): ParticleEmitter.Blend is gone; a sprite composites by its Material's blend mode
    // (Tools/SceneMigrator, MigrateParticleSpriteMaterialsV46ToV47).
    // v48 (SSR1B): PostProcessSettings.EnableSSR defaults to true; every PostProcessVolume states true where
    // v47 stated the old default false (Tools/SceneMigrator, MigrateSSRDefaultOnV47ToV48).
    // v49 (SHADOW-BIAS): DirectionLight.ShadowBias is world centimetres applied by the caster, not a fraction
    // of a cascade's NDC depth; a stated older number is re-stated as the new default (Tools/SceneMigrator,
    // MigrateShadowBiasCentimetresV48ToV49).
    // v50 (WP-FAR-7): a partitioned world states its HLOD layers and the one its cells use (WorldPartition's
    // HLODLayers / DefaultHLODLayer); every partitioned world states the Instancing layer it had implicitly
    // (Tools/SceneMigrator, MigrateHLODLayersV49ToV50).
    // v51 (VFX-HOST): ParticleEmitter is gone; each block became a `.dfx` system played by a VFX block
    // (Tools/SceneMigrator, MigrateParticleEmittersToVFX).
    // v52 (SCENE-DEPS): Header.Dependencies lists every asset the scene references, unique and sorted, gathered
    // by the one rule the save applies too (Core::GatherSceneDependencies; Tools/SceneMigrator,
    // MigrateSceneDependenciesV51ToV52).
    // v53 (RMODE-IFACE): a volume states its render methods — GlobalIllumination (GIMode) is
    // GlobalIlluminationMethod and EnableSSR is ReflectionsMethod, on the method ladders of
    // Common/Settings/RenderMethods.hpp (Tools/SceneMigrator, MigrateRenderMethodsV52ToV53). v54 (SCR-DETAILS): a
    // script slot's Props are only the values that differ from the script's declared defaults
    // (Tools/SceneMigrator, MigrateScriptDefaultsV53ToV54). v56 (LIGHT-UNITS): point / spot lights state
    // IntensityUnits (UE ELightUnits) and the exposure range is EV100 (Tools/SceneMigrator,
    // MigrateLightUnitsV55ToV56); v55 is the SourceAngle step. v57 (LIGHT-UNITS-2): the Physical Atmosphere
    // sky takes its sun from the atmosphere sun light (UE) and manual exposure is EV100 (Tools/SceneMigrator,
    // MigrateSkySunFromLightV56ToV57). v58 (TOD1): the TimeOfDay clock has UE SunPosition's calendar - date,
    // longitude, time zone, daylight saving (Tools/SceneMigrator, MigrateSunCalendarV57ToV58). v59..v63 are
    // Windows' steps. v64 (GI-METHOD-RENUMBER): GlobalIlluminationMethod is stated on today's ladder, which lost
    // ReflectiveShadowMaps (3) - every method above it is one number lower (Tools/SceneMigrator,
    // MigrateGIMethodLadderV63ToV64). v65 (RENDER-LOOK round 05): the display transfer is the sRGB OETF and
    // PostProcessSettings lost Gamma (Tools/SceneMigrator, MigrateDisplayTransferV64ToV65). v66 (WP22): a World
    // Partition grid states its Name (what a RuntimeGrid block names) and Priority (Tools/SceneMigrator,
    // MigrateGridNamesV65ToV66). v67 (WP-FAR-6B): a landscape tile's HeightFile is a content key ("assets:...")
    // (Tools/SceneMigrator, MigrateLandscapeHeightKeysV66ToV67). v68 (WP22c): the HLOD layer is stated per GRID
    // (UE FRuntimePartitionDesc::HLODSetups), not once for the world - the world's DefaultHLODLayer moved onto
    // every grid (Tools/SceneMigrator, MigrateGridHLODLayerV67ToV68). v69 (PATH1): every asset reference is a
    // content key - the assets-relative and project-relative spellings are restated (Tools/SceneMigrator,
    // MigrateAssetKeysV68ToV69). v70 (WP17): a ROOT record of an external-entities scene states no siblingIndex -
    // a streamed world persists no root order, as UE's OFPA (Tools/SceneMigrator,
    // MigrateRootSiblingIndexV69ToV70).
    // v71 (VOLFOG): ExponentialHeightFog states UE's Volumetric Fog group and every light its
    // VolumetricScatteringIntensity. Additive: the step rewrites nothing, a v70 record loads with the declared
    // defaults and the save states them; the number moves so an older build refuses a file whose fog it would
    // silently drop (Tools/SceneMigrator, kSceneVersionVolumetricFog).
    // v72 (TRACED-METHOD-AXIS): a volume's GlobalIlluminationMethod / ReflectionsMethod is a request FAMILY
    // (Common::Scalability::GIRequest / ReflectionRequest) - SoftwareTraced and HardwareTraced are one Traced, the
    // tracer the machine's and the device's (Tools/SceneMigrator, MigrateTracedRequestV71ToV72).
    // v73 (GTX-TIER-LADDER): GI comes from the method alone - the lighting composite's screen-space gather is
    // gone and GIRequest ScreenSpace was then declared, not implemented (SSGIRenderer implements it since
    // SSGI2-GTAO, no scene version); a volume that asked for it asks for Off, and
    // GIIntensity (read by that gather only) is dropped (Core/SceneUpgrade/Steps/V73ComposedGI.hpp,
    // MigrateComposedGIV72ToV73).
    //
    // A SUBSYSTEM THAT OWNS PART OF THE FORMAT COUNTS ITS OWN CHANGES (MIGRATE-SUBSYS, owner 2026-10-22; UE
    // FCustomVersion). This number is no longer raised for World Partition: its block and its streamed records are
    // versioned by kWorldPartitionVersion under Assets::kWorldPartitionSchemaTag, and Tools/SceneMigrator runs
    // that subsystem's steps by the number the file states under that tag. The World Partition steps already taken
    // (v50, v66, v68, v70) stay where they were written - a file below v70 still climbs them by this number.
    // v74 (POST-LOOK-LAND): UE's grade - Filmic is the default tonemapper (ACES stays an operator); a volume
    // that stated ACES (the former default) states Filmic (Core/SceneUpgrade/Steps/V74FilmicDefault.hpp,
    // MigrateFilmicDefaultV73ToV74). The white balance, colour grading, film curve and exposure compensation fields are
    // additive (declared defaults are the identity).
    // v75 (FPS-GRASS-VISBUF): PostProcessSettings.GrassVisibilityBuffer (bool) is GrassRaster, a
    // Core::GrassRasterPath (Scalability 0, GBuffer 1, VisibilityBuffer 2): the renderer's choice is the level's
    // Foliage.GrassVisibilityBuffer unless a volume overrides it. false (the former default) -> Scalability,
    // true -> VisibilityBuffer (Core/SceneUpgrade/Steps/V75GrassRasterPath.hpp, MigrateGrassRasterPathV74ToV75).
    // v76 (WP-LANDSCAPE-SPLINES): UE 5.5 ALandscapeSplineActor - every landscape spline is its own entity, a
    // LandscapeSpline block {Landscape: the root's id, Splines}, spatially loaded by its band. The root's
    // Landscape block loses Splines: each connected group of its control points becomes one LandscapeSpline record
    // (Core/SceneUpgrade/Steps/V76LandscapeSplineEntities.hpp, MigrateLandscapeSplineEntitiesV75ToV76).
    // v77 (NG-AREA-LIGHTS): UE SourceLength - PointLight / SpotLight state SourceLength (cm; 0 = a sphere), the
    // capsule ("tube") emitter along the light's forward (Core/SceneUpgrade/Steps/V77TubeSourceLength.hpp,
    // MigrateTubeSourceLengthV76ToV77). v78 (BISTRO-NIGHT-LOOK): UE has one exposure offset - PostProcessVolume
    // Settings.AutoExposureKey is gone, its log2(key / 0.18) added to AutoExposureBias
    // (Core/SceneUpgrade/Steps/V78ExposureBiasOnly.hpp, MigrateExposureKeyV77ToV78).
    // v79 (MIG-BISTRO): GIRequest Lightmaps (1), never implemented and walked down to Off by every build, is
    // re-stated as Off (Core/SceneUpgrade/Steps/V79UnbuiltLightmaps.hpp, MigrateUnbuiltLightmapsV78ToV79).
    // v80 (MIG-BISTRO): the WaterBody block v2 / v3 upgrade - TargetWaveMaskDepth (the depth transform derives the
    // damping) and FallFoam (the plunge boil) are dropped (Core/SceneUpgrade/Steps/V80WaterBodyRetiredKeys.hpp,
    // MigrateWaterBodyRetiredKeysV79ToV80).
    inline constexpr int kSceneVersion = 80;

    // The reflected asset-reference format's own generation, stated under Assets::kAssetRefSchemaTag (its steps:
    // Core/SceneUpgrade/Steps/AssetRef1ReflectedRefs.hpp, MigrateReflectedAssetRefsV0ToV1). v1 (STATICMESH-FORMAT):
    // static meshes and materials are named {Guid, Path} by reflected fields, the Static Mesh's references are its
    // reflected MeshHandle / MaterialSlots. A file stating no tag is v0.
    inline constexpr int kAssetRefVersion = 1;

    // The light components' own format generation, stated under Assets::kLightSchemaTag (its steps:
    // Core/SceneUpgrade/Steps/Light1Rotation.hpp, MigrateLightRotationV0ToV1). v1 (PORT-SUN-ROTATION): a directional
    // light is aimed by its Rotation (forward -Z), as in UE; before it, its Translation was the direction it travelled.
    // The step restates every directional light's Translation as a Rotation and zeroes the Translation. A file stating
    // no tag is v0.
    inline constexpr int kLightVersion = 1;

    // The post-process settings' own format generation, stated under Assets::kPostProcessSchemaTag (its steps:
    // Core/Serialize/SceneUpgrade.hpp, MigratePostProcessBloomV0ToV1). v1 (RENDER-BLOOM-UE): UE's Gaussian bloom
    // and histogram auto exposure as PostProcessSettings fields - EnableBloom and AutoExposureSpeed are gone. A
    // file stating no tag is v0.
    inline constexpr int kPostProcessVersion = 1;

    // World-unit generation of a .desce file. One world unit is a CENTIMETRE (Common/Core/Units.hpp).
    // Bump this only if the world unit changes again - and then, as above, add the step to SceneMigrator
    // and run it, because nothing converts a file at load any more.
    inline constexpr int kUnitVersion = 1;

    // The on-disk shape of a .desce file, and the ONLY definition of it: the loader parses into this, the
    // saver writes it, and Tools/SceneMigrator rewrites it in place. It lives here rather than inside
    // SceneSerializer.cpp because a migration whose input is "the parsed tree" needs the tree's type, and a
    // second copy of this struct anywhere is a format that can silently fork.
    struct SceneSerialized
    {
        // First, so it is the document's first member and readable without the body (TextAssetHeader.hpp).
        // It is where the file states its two generations (SCNE, UNIT) - since v26 nowhere else.
        std::optional<Common::Content::TextAssetHeaderSerialized> Header;
        std::string                                               SceneName;
        std::vector<Assets::EntityData> Entities;
        // Scene-wide settings - reflected, so the whole block round-trips through the generic serializer.
        std::optional<Common::Json::Value> Settings;
        // Absent = this world is not partitioned. See WorldPartitionSerialized.
        std::optional<WorldPartitionSerialized> WorldPartition;
    };

    // True when the parsed tree is at BOTH current generations, which is the only thing the loader accepts.
    //
    // An ABSENT integer is version 0 and not "current": every stamp this engine writes states both numbers,
    // so a file missing one was written by something older. Reading it as current is the substitution §1.4
    // forbids - it would load a metres-era scene as though it were centimetres and put the world a hundred
    // times too small on screen with nothing said about it.
    [[nodiscard]] inline bool SceneIsAtCurrentVersion( const SceneSerialized& scene )
    {
        return Assets::StatedVersion( scene.Header, Assets::kSceneSchemaTag ) == kSceneVersion &&
               Assets::StatedVersion( scene.Header, Assets::kUnitSchemaTag ) == kUnitVersion &&
               Assets::StatedVersion( scene.Header, Assets::kWorldPartitionSchemaTag ) == kWorldPartitionVersion &&
               Assets::StatedVersion( scene.Header, Assets::kAssetRefSchemaTag ) == kAssetRefVersion &&
               Assets::StatedVersion( scene.Header, Assets::kLightSchemaTag ) == kLightVersion &&
               Assets::StatedVersion( scene.Header, Assets::kPostProcessSchemaTag ) == kPostProcessVersion;
    }

    // The subsystem versions a .desce / .deprefab of this build states, and the reading context built on them.
    [[nodiscard]] inline std::span<const Common::Content::SubsystemVersion> SceneTextSubsystems()
    {
        static const std::array<Common::Content::SubsystemVersion, 6> versions = {
             Common::Content::SubsystemVersion{ Assets::kSceneSchemaTag, static_cast<uint32_t>( kSceneVersion ) },
             Common::Content::SubsystemVersion{ Assets::kUnitSchemaTag, static_cast<uint32_t>( kUnitVersion ) },
             Common::Content::SubsystemVersion{ Assets::kWorldPartitionSchemaTag,
                                                static_cast<uint32_t>( kWorldPartitionVersion ) },
             Common::Content::SubsystemVersion{ Assets::kAssetRefSchemaTag,
                                                static_cast<uint32_t>( kAssetRefVersion ) },
             Common::Content::SubsystemVersion{ Assets::kLightSchemaTag, static_cast<uint32_t>( kLightVersion ) },
             Common::Content::SubsystemVersion{ Assets::kPostProcessSchemaTag,
                                                static_cast<uint32_t>( kPostProcessVersion ) } };
        return versions;
    }

    // The refusal, as a string: which file, what it is, what this engine needs, and the exact command that
    // fixes it. PURE - no logging, no filesystem, no globals - so the loader can log it and a test can read
    // it without a GPU or a scene.
    //
    // It names the COMMAND and not just the tool, because "your scene is too old" without the fix is a dead
    // end for whoever hits it: the file is usually somebody's autosave or a scene saved outside the
    // repository, and they have one action to take.
    [[nodiscard]] std::string RefuseSceneVersion( std::string_view source, int foundSceneVersion,
                                                  int foundUnitVersion, int foundWorldPartitionVersion,
                                                  int foundAssetRefVersion, int foundLightVersion,
                                                  int foundPostProcessVersion );

    // Same, read straight off the parsed tree (absent integers become 0 - see SceneIsAtCurrentVersion).
    [[nodiscard]] std::string RefuseSceneVersion( std::string_view source, const SceneSerialized& scene );

    // Reads the JSON of a .desce and hands back the tree ONLY if this engine will load it: readable, and at
    // both current generations. Otherwise the error is the message the loader logs, already naming the file,
    // what it is, what is needed and the command that converts it.
    //
    // WHY THIS IS SEPARATE FROM THE LOADER, AND WHY CALLERS ASK IT FIRST. Opening a scene DESTROYS the one
    // that is open - every caller of SceneSerializer::DeserializeFromJson clears the scene before it hands
    // the text over. If the refusal only happened inside the loader, opening an old file would empty the
    // editor and then decline to fill it, which is a worse answer than the migration this replaced: the
    // user would lose the scene they had to a file that never loaded. So the question "will this load?" is
    // answerable without a Scene, and the callers ask it while there is still something to protect.
    //
    // PURE - no filesystem (the caller has already read the text), no globals, no scene. The parse it does
    // is the same one the loader does; the loader calls THIS rather than repeating the checks, so there is
    // one statement of what "loadable" means and one wording of the refusal.
    //
    // THE ONE PARSE OF A LOAD. The text is parsed once, into the document; the typed tree is read off that
    // document, and BOTH come back: the loader instantiates the typed tree and keeps the document for the
    // saver's foreign-key merge. A caller that asks first hands the result on to SceneSerializer::Deserialize
    // rather than the text, so asking costs no second parse.
    struct LoadableScene
    {
        Common::Json::TextDocument
                        Document; // the file as parsed - every key, known or not, every number at its width
        SceneSerialized Scene;    // the typed view of that same document
        // Non-empty: the file was at an older generation and was upgraded in memory
        // (Core/SceneUpgrade/SceneUpgrade.hpp); the one line the loader logs. The document above is then the upgraded
        // tree's.
        std::string Upgraded;
    };
    [[nodiscard]] Common::ResultStr<LoadableScene> ParseLoadableScene( std::string_view   source,
                                                                       const std::string& json );

    // What a save writes: the tree this build states (`fresh`), with every key of the loaded document this
    // build does not state merged back where it stood (ForeignKeys.hpp). No loaded document (a scene that
    // never came from a file) = `fresh` as it is. PURE - the saver calls it, and the corpus suite proves a
    // committed scene comes back byte for byte through the same function.
    //
    // `fresh` is written by the TYPED writer (Json::Write), where a UUID is a uint64 - never through
    // Json::Value, which has no unsigned 64-bit number and would spell an id >= 2^63 negative. Fails only
    // when the typed writer's output is not JSON (a NaN or an infinity in the scene), naming the reason.
    [[nodiscard]] Common::ResultStr<Common::Json::TextDocument>
    ComposeSceneDocument( const SceneSerialized& fresh, const std::optional<Common::Json::TextDocument>& loaded,
                          const Serialize::KeyIsOurs&          entityKeyIsOurs,
                          const Common::Json::MemberKeyIsOurs& componentKeyIsOurs = {} );

} // namespace Desert::Core
