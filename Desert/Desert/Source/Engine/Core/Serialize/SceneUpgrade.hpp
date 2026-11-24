#pragma once

#include <Engine/Assets/Prefab/PrefabData.hpp>
#include <Engine/Core/Serialize/SceneFormat.hpp>

#include <Common/Content/TextAssetHeader.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// UPGRADE ON LOAD (SCENE-UPGRADE-ON-LOAD; UE: FCustomVersion + Serialize upgrading an older package as it loads,
// ResavePackages running the very same code to write it back). A .desce / .deprefab OLDER than this engine is
// raised IN MEMORY by the steps below when it is parsed (Core::ParseLoadableScene, Assets::ParseLoadablePrefab),
// and the next save writes the current generation - the file on disk is untouched until somebody saves it.
// Tools/SceneMigrator is the resave: it reads, calls THESE functions, and writes; it carries no copy of them.
//
// THE OLDEST LOADABLE GENERATION (UE: VER_UE4_OLDEST_LOADABLE_PACKAGE). The steps below kSceneUpgradeFloor move
// data between FILES (prefab files, landscape tiles, mesh headers, .defoliage / .desound / .vfx assets minted
// beside the scene) or read shapes the engine's tree no longer has a member for; they stay in
// Tools/SceneMigrator, and a file below the floor is refused by name with that one command. A file NEWER than
// this engine (any tag above the head) is refused as before: nothing is ever loaded "as best we can".
//
// ADDING A STEP. Raise Core::kSceneVersion (or the subsystem's own number), add a kSceneVersion<Name> below
// equal to it, the step function here, and one `if` in UpgradeSceneRecords. Nothing else: the loader, the
// prefab reader and the migrator all reach it through that one function.
namespace Desert::Core
{
    //  70 - the floor: the oldest scene generation this engine raises in memory (kSceneUpgradeFloor). The steps
    //       up to it are Tools/SceneMigrator's (MigrateRootSiblingIndexV69ToV70 the last).
    inline constexpr int kSceneVersionRootSiblingIndex = 70;

    //  71 - VOLUMETRIC FOG (VOLFOG, UE's Volumetric Fog group and ULightComponent::VolumetricScatteringIntensity).
    //       ExponentialHeightFog and the lights gain fields that are ADDITIVE with defaults that reproduce the old
    //       picture, so the step has no transform: the record is read with the declared defaults and the next
    //       save states them. Scenes and prefabs alike.
    inline constexpr int kSceneVersionVolumetricFog = 71;

    //  72 - A VIEW ASKS FOR "TRACED", NOT FOR A TRACER (TRACED-METHOD-AXIS). PostProcessSettings
    //       GlobalIlluminationMethod is a GIRequest (Off 0, Lightmaps 1, ScreenSpace 2, ProbeVolume 3, Traced 4,
    //       PathTraced 5) and ReflectionsMethod a ReflectionRequest (Off 0, Captures 1, ScreenSpace 2, Traced 3,
    //       PathTraced 4). A v64..v71 file stated the rendered ladders: GI SoftwareTraced 4 / HardwareTraced 5 /
    //       PathTraced 6, reflections SoftwareTraced 3 / HardwareTraced 4 / PathTraced 5. Both traced numbers
    //       become Traced and PathTraced moves down one; a value outside the old ladder is refused, naming the
    //       entity (MigrateTracedRequestV71ToV72). The record and every prefab override.
    inline constexpr int kSceneVersionTracedRequest = 72;

    //  73 - GI COMES FROM THE METHOD ALONE (GTX-TIER-LADDER). A volume that stated GIRequest ScreenSpace (2) asks
    //       for Off (0) - the GI it rendered in effect - and GIIntensity, read by nothing now, is dropped
    //       (MigrateComposedGIV72ToV73). The record and every prefab override.
    inline constexpr int kSceneVersionComposedGI = 73;

    //  74 - FILMIC BY DEFAULT (POST-LOOK-LAND, UE parity). A volume's Tonemapper ACES (0) - the former default -
    //       is Filmic (2); Reinhard stays (MigrateFilmicDefaultV73ToV74). The record and every override.
    inline constexpr int kSceneVersionFilmicDefault = 74;

    //  75 - THE GRASS RASTER PATH IS THE RENDERER'S (FPS-GRASS-VISBUF). The volume's bool
    //       PostProcessSettings.GrassVisibilityBuffer becomes GrassRaster, a Core::GrassRasterPath (Scalability 0,
    //       GBuffer 1, VisibilityBuffer 2): false -> Scalability, true -> VisibilityBuffer; a value that is not a
    //       bool is refused, naming the entity (MigrateGrassRasterPathV74ToV75). The record and every override.
    inline constexpr int kSceneVersionGrassRasterPath = 75;

    //  76 - ONE ENTITY PER LANDSCAPE SPLINE (WP-LANDSCAPE-SPLINES; UE 5.5 ALandscapeSplineActor). A Landscape
    //       block's Splines {ControlPoints, Segments} leaves the root: every connected group of its control points
    //       becomes one record {LandscapeSpline: {Landscape: the root's id, Splines: the group, re-indexed}}, Tag
    //       "Landscape Spline", its id FNV-1a over "LandscapeSpline:<root id>:<group>" so every upgrade of the
    //       same file states the same identity. A prefab (or override) whose Landscape states control points is
    //       refused naming it (MigrateLandscapeSplineEntitiesV75ToV76).
    inline constexpr int kSceneVersionLandscapeSplineEntities = 76;

    //  77 - TUBE LIGHTS (NG-AREA-LIGHTS; UE ULocalLightComponent::SourceLength). Every PointLight / SpotLight
    //  record
    //       that does not state SourceLength states 0 - a sphere, the meaning every older light had
    //       (MigrateTubeSourceLengthV76ToV77). A prefab override is a delta and is left alone.
    inline constexpr int kSceneVersionTubeSourceLength = 77;

    static_assert( kSceneVersionTubeSourceLength == kSceneVersion,
                   "the last upgrade step and the engine's scene version must be the same generation - a bump of "
                   "Core::kSceneVersion needs its step here (SceneUpgrade.hpp, UpgradeSceneRecords)" );

    // World Partition's own chain (Assets::kWorldPartitionSchemaTag). v12 is the floor (the last step that moves a
    // value, MigrateHLODMergeMaterialsV11ToV12, is Tools/SceneMigrator's). v13 and v14 are additive: the block is
    // read with the declared defaults and the next save states them.
    inline constexpr int kWorldPartitionVersionHLODBuilderSettings = 12;
    // 13 - HLOD LAYER PER ENTITY (WP-HLOD-LAYER-COMPONENT): a record that states no HLODLayer component is built
    // by
    //      its partition's layer, as it was.
    inline constexpr int kWorldPartitionVersionEntityHLODLayer = 13;
    // 14 - IMPOSTOR HLOD LAYERS: a v13 world states no Impostor layer.
    inline constexpr int kWorldPartitionVersionImpostorLayers = 14;
    static_assert( kWorldPartitionVersionImpostorLayers == kWorldPartitionVersion,
                   "World Partition's version moved: its step (or its additive note) belongs in SceneUpgrade" );

    // THE FLOORS: the oldest generation of each tag this engine raises in memory. The asset-reference (v0 -> v1
    // reads mesh files) and light (v0 -> v1) chains have no in-memory step yet, so their floor is the head.
    inline constexpr int kSceneUpgradeFloor          = kSceneVersionRootSiblingIndex;
    inline constexpr int kWorldPartitionUpgradeFloor = kWorldPartitionVersionHLODBuilderSettings;
    inline constexpr int kAssetRefUpgradeFloor       = kAssetRefVersion;
    inline constexpr int kLightUpgradeFloor          = kLightVersion;

    // What MigrateLandscapeSplineEntitiesV75ToV76 did to one file.
    struct LandscapeSplineEntitiesReport
    {
        std::size_t RootsCleared   = 0;   // Landscape blocks (and overrides) whose Splines key was taken out
        std::size_t SplinesCreated = 0;   // LandscapeSpline records added, one per connected group
        std::vector<std::string> Refused; // a Splines value of the wrong shape, or splines in a prefab
    };

    // What MigrateTracedRequestV71ToV72 did to one file.
    struct TracedRequestReport
    {
        std::size_t              Restated = 0; // GlobalIlluminationMethod / ReflectionsMethod numbers changed
        std::vector<std::string> Refused;      // a Settings block or a value that is not on the old ladder
    };

    // What MigrateComposedGIV72ToV73 did to one file.
    struct ComposedGIReport
    {
        std::size_t              Restated = 0; // GlobalIlluminationMethod ScreenSpace re-stated as Off
        std::size_t              Dropped  = 0; // GIIntensity keys removed
        std::vector<std::string> Refused;      // a Settings block that is not an object
    };

    // What MigrateFilmicDefaultV73ToV74 did to one file.
    struct FilmicDefaultReport
    {
        std::size_t              Restated = 0; // Tonemapper ACES re-stated as Filmic
        std::vector<std::string> Refused;      // a Settings block that is not an object
    };

    // What MigrateGrassRasterPathV74ToV75 did to one file.
    struct GrassRasterPathReport
    {
        std::size_t              Restated = 0; // GrassVisibilityBuffer bools re-stated as GrassRaster
        std::vector<std::string> Refused;      // a Settings block that is not an object, or a non-bool value
    };

    // The steps. Each is PURE (no filesystem, no globals) and edits the records in place.
    TracedRequestReport   MigrateTracedRequestV71ToV72( std::vector<Assets::EntityData>& entities );
    ComposedGIReport      MigrateComposedGIV72ToV73( std::vector<Assets::EntityData>& entities );
    FilmicDefaultReport   MigrateFilmicDefaultV73ToV74( std::vector<Assets::EntityData>& entities );
    GrassRasterPathReport MigrateGrassRasterPathV74ToV75( std::vector<Assets::EntityData>& entities );
    // @p createEntities: true for a scene, false for a prefab (whose control points are refused).
    LandscapeSplineEntitiesReport
    MigrateLandscapeSplineEntitiesV75ToV76( std::vector<Assets::EntityData>& entities, bool createEntities );
    // Returns how many PointLight / SpotLight records it stated SourceLength 0 in.
    std::size_t MigrateTubeSourceLengthV76ToV77( std::vector<Assets::EntityData>& entities );

    // The generations a file states (absent = 0), one per tag.
    struct SceneGenerations
    {
        int Scene          = 0;
        int Unit           = 0;
        int WorldPartition = 0;
        int AssetRef       = 0;
        int Light          = 0;

        [[nodiscard]] bool operator==( const SceneGenerations& ) const = default;
    };
    [[nodiscard]] SceneGenerations
    StatedGenerations( const std::optional<Common::Content::TextAssetHeaderSerialized>& header );
    // This build's generations (the head of every tag).
    [[nodiscard]] SceneGenerations CurrentGenerations();

    enum class UpgradeTarget
    {
        Scene,  // a spline becomes its own entity
        Prefab, // a spline is refused: it cannot become an entity of an instance
    };

    // What one upgrade did. Refused non-empty: the records are in an unspecified state and the caller loads and
    // writes nothing.
    struct SceneUpgradeReport
    {
        SceneGenerations From;

        bool                          VolumetricFogRaised = false;
        bool                          TracedRequestRaised = false;
        TracedRequestReport           TracedRequest;
        bool                          ComposedGIRaised = false;
        ComposedGIReport              ComposedGI;
        bool                          FilmicDefaultRaised = false;
        FilmicDefaultReport           FilmicDefault;
        bool                          GrassRasterPathRaised = false;
        GrassRasterPathReport         GrassRasterPath;
        bool                          LandscapeSplineEntitiesRaised = false;
        LandscapeSplineEntitiesReport LandscapeSplineEntities;
        bool                          TubeSourceLengthRaised  = false;
        std::size_t                   TubeSourceLengthsStated = 0;
        bool                          EntityHLODLayerRaised   = false;
        bool                          ImpostorLayersRaised    = false;

        std::string Refused;

        [[nodiscard]] bool AnyRaised() const
        {
            return VolumetricFogRaised || TracedRequestRaised || ComposedGIRaised || FilmicDefaultRaised ||
                   GrassRasterPathRaised || LandscapeSplineEntitiesRaised || TubeSourceLengthRaised ||
                   EntityHLODLayerRaised || ImpostorLayersRaised;
        }
    };

    // THE CHAIN, over the records of one file whose generations are @p from. Every tag must be at or above its
    // floor and at or below its head (else Refused names the numbers); then every step whose number is above the
    // stated one runs, in order. Does not touch a header: the caller restates it (RestateCurrentGenerations) or,
    // for the migrator, stamps it. @p name names the file in a refusal. PURE.
    [[nodiscard]] SceneUpgradeReport UpgradeSceneRecords( std::vector<Assets::EntityData>& entities,
                                                          const SceneGenerations& from, UpgradeTarget target,
                                                          std::string_view name );

    // The header's Versions become this build's (Kind, Guid and Dependencies are kept): what an upgraded tree
    // states, so every later check of it reads current.
    void RestateCurrentGenerations( Common::Content::TextAssetHeaderSerialized& header );

    // A scene read by the engine: the chain from its header's numbers, then the header restated. A tree already
    // current is left untouched (AnyRaised false, From == CurrentGenerations()). PURE.
    [[nodiscard]] SceneUpgradeReport UpgradeScene( SceneSerialized& scene );

    // The one log line of an in-memory upgrade: "'<source>': upgraded v71->v77 in memory (...), save to keep".
    [[nodiscard]] std::string DescribeUpgrade( std::string_view source, const SceneUpgradeReport& report );
} // namespace Desert::Core
