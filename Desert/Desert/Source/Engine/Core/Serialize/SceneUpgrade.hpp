#pragma once

#include <Engine/Assets/Prefab/PrefabData.hpp>
#include <Engine/Core/Serialize/SceneFormat.hpp>

#include <Common/Content/TextAssetHeader.hpp>

#include <cstddef>
#include <functional>
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
// Tools/SceneMigrator, and a file below the floor is refused by name with that one command. Every step at or
// above a floor - the scene's v71..v77, World Partition's v13/v14, the asset-reference and light chains whole -
// is HERE and only here: the migrator runs its below-floor chain, then this one (UpgradeSceneRecords).
//
// A STEP THAT NEEDS ANOTHER FILE'S CONTENT DOES NOT OPEN IT. The asset-reference step reads a material's header
// GUID through the MaterialGuidLookup its caller passes: the loader's reads the mounted content
// (ContentMaterialGuidLookup), the migrator's its own assets root. The transform is one copy.
//
// SELF-VERSIONED BLOCKS upgrade in their own reader, not here (UE: a struct's Serialize reads its own custom
// version): a timeline block states its TMLN number inside itself, so a TMLN v1 block - in a scene's UIAnim, a
// .dseq, an .anim - is raised by the one reader every host goes through (Animation::Timeline::ReadSequence,
// TimelineInterpShift.hpp). A file NEWER than
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

    //  78 - BLOOM AND AUTO EXPOSURE AS UE (RENDER-BLOOM-UE). A PostProcessVolume's Settings lose EnableBloom
    //  (false
    //       -> BloomIntensity 0, true keeps the intensity) and AutoExposureSpeed (-> AutoExposureSpeedUp =
    //       AutoExposureSpeedDown); BloomThreshold <= 0 -> -1. A record also states what the v77 defaults gave it
    //       (bloom off, threshold 2, speed 1.5, auto exposure off) and the 0 / 95 histogram window it was graded
    //       under; an override is a delta and re-states only its own keys. A Settings that is not an object, or a
    //       key of the wrong type, is refused (MigrateBloomUEV77ToV78). This step is the PostProcessVolume block's
    //       upgrade from COMPONENT Version 1 to Version( 2 ) (ECS/PostProcessVolumeComponent.hpp).
    inline constexpr int kSceneVersionBloomUE = 78;

    static_assert( kSceneVersionBloomUE == kSceneVersion,
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

    // THE REFLECTED ASSET-REFERENCE FORMAT'S OWN CHAIN (Assets::kAssetRefSchemaTag), run after the scene's and
    // World Partition's.
    //   1 - STATIC MESHES AND MATERIALS BY {Guid, Path} (STATICMESH-FORMAT). The Static Mesh block's MeshGuid +
    //       MeshPath become its reflected `MeshHandle` {Guid, Path} and MaterialGuids + MaterialPaths its
    //       `MaterialSlots` [{Guid, Path}] (the GUIDs the file already states); every reflected material field
    //       stated as a bare key (LandscapeMaterial.Material, VolumetricCloud.Material,
    //       Destructible.InteriorMaterial, UIPanel.Material) becomes {Guid, Path}, the GUID the `.demat` header
    //       states (the caller's MaterialGuidLookup). Records and prefab overrides alike
    //       (MigrateReflectedAssetRefsV0ToV1).
    inline constexpr int kAssetRefVersionReflected = 1;
    static_assert( kAssetRefVersionReflected == kAssetRefVersion,
                   "the asset-reference version moved: its step belongs in SceneUpgrade (UpgradeSceneRecords)" );

    // THE LIGHT COMPONENTS' OWN CHAIN (Assets::kLightSchemaTag), run after the asset-reference chain.
    //   1 - A DIRECTIONAL LIGHT IS AIMED BY ITS ROTATION (PORT-SUN-ROTATION), as in UE: the light travels along
    //       the entity's world forward axis (-Z) and its Translation is only where it sits. Every record with a
    //       DirectionLight block whose Translation is a direction (longer than 1e-4, the old engine's gate) gets
    //       the Rotation that aims its world forward axis along normalize(Translation) - the parents' stated
    //       Rotations divided out - and a zero Translation. A light whose Translation was no direction emitted
    //       nothing before; it keeps its Rotation and is counted. A prefab override that states a Translation
    //       beside a DirectionLight block is converted the same way, its parent rotation taken as identity, and
    //       counted (MigrateLightRotationV0ToV1). Scenes and prefabs alike.
    inline constexpr int kLightVersionRotation = 1;
    static_assert( kLightVersionRotation == kLightVersion,
                   "the light version moved: its step belongs in SceneUpgrade (UpgradeSceneRecords)" );

    // THE FLOORS: the oldest generation of each tag this engine raises in memory. The asset-reference and light
    // chains are here whole (floor 0: a file that states no number).
    inline constexpr int kSceneUpgradeFloor          = kSceneVersionRootSiblingIndex;
    inline constexpr int kWorldPartitionUpgradeFloor = kWorldPartitionVersionHLODBuilderSettings;
    inline constexpr int kAssetRefUpgradeFloor       = 0;
    inline constexpr int kLightUpgradeFloor          = 0;

    // The header GUID (text) of the material an asset key ("assets:Materials/M.demat") names, or nullopt when
    // that file states none. Where the file is read from is the caller's: the step never opens a file.
    using MaterialGuidLookup = std::function<std::optional<std::string>( const std::string& key )>;

    // The loader's lookup: the key through the mounted content roots (Common::AssetHandle::PathForStableKey),
    // the header read VFS-first (Assets::ReadTextHeaderGuid) - the identity every load of that material adopts.
    [[nodiscard]] MaterialGuidLookup ContentMaterialGuidLookup();

    // What MigrateReflectedAssetRefsV0ToV1 did to one file.
    struct ReflectedAssetRefsReport
    {
        std::size_t              MeshBlocks     = 0; // Static Mesh blocks (records and overrides) restated
        std::size_t              MaterialFields = 0; // bare material keys restated as {Guid, Path}
        std::vector<std::string> Refused;            // a material file with no header GUID, or a wrong shape
    };

    // What MigrateLightRotationV0ToV1 did to one file.
    struct SunRotationReport
    {
        std::size_t Aimed      = 0; // records whose Translation became their Rotation
        std::size_t Overrides  = 0; // prefab overrides converted with an identity parent rotation
        std::size_t Undirected = 0; // lights whose Translation was no direction; Rotation kept as stated
    };

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

    // What MigrateBloomUEV77ToV78 did to one file.
    struct BloomUEReport
    {
        std::size_t              Restated = 0; // Settings blocks re-stated
        std::size_t              BloomOff = 0; // EnableBloom false re-stated as BloomIntensity 0
        std::vector<std::string> Refused;      // a Settings block that is not an object, or a mistyped key
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
    BloomUEReport MigrateBloomUEV77ToV78( std::vector<Assets::EntityData>& entities );
    // PURE given @p materialGuid, which is asked once per distinct key.
    ReflectedAssetRefsReport MigrateReflectedAssetRefsV0ToV1( std::vector<Assets::EntityData>& entities,
                                                              const MaterialGuidLookup&        materialGuid );
    SunRotationReport        MigrateLightRotationV0ToV1( std::vector<Assets::EntityData>& entities );

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
        bool                          BloomUERaised           = false;
        BloomUEReport                 BloomUE;
        bool                          EntityHLODLayerRaised   = false;
        bool                          ImpostorLayersRaised    = false;
        bool                          ReflectedAssetRefsRaised = false;
        ReflectedAssetRefsReport      ReflectedAssetRefs;
        bool                          SunRotationRaised = false;
        SunRotationReport             SunRotation;

        std::string Refused;

        [[nodiscard]] bool AnyRaised() const
        {
            return VolumetricFogRaised || TracedRequestRaised || ComposedGIRaised || FilmicDefaultRaised ||
                   GrassRasterPathRaised || LandscapeSplineEntitiesRaised || TubeSourceLengthRaised || BloomUERaised ||
                   EntityHLODLayerRaised || ImpostorLayersRaised || ReflectedAssetRefsRaised || SunRotationRaised;
        }
    };

    // THE CHAIN, over the records of one file whose generations are @p from. Every tag must be at or above its
    // floor and at or below its head (else Refused names the numbers); then every step whose number is above the
    // stated one runs, in order. Does not touch a header: the caller restates it (RestateCurrentGenerations) or,
    // for the migrator, stamps it. @p name names the file in a refusal; @p materialGuid answers the
    // asset-reference step (asked only when the file states asset references v0). PURE given it.
    [[nodiscard]] SceneUpgradeReport UpgradeSceneRecords( std::vector<Assets::EntityData>& entities,
                                                          const SceneGenerations& from, UpgradeTarget target,
                                                          std::string_view name, const MaterialGuidLookup& materialGuid );

    // The header's Versions become this build's (Kind, Guid and Dependencies are kept): what an upgraded tree
    // states, so every later check of it reads current.
    void RestateCurrentGenerations( Common::Content::TextAssetHeaderSerialized& header );

    // A scene read by the engine: the chain from its header's numbers, then the header restated. A tree already
    // current is left untouched (AnyRaised false, From == CurrentGenerations()). Materials are looked up
    // through ContentMaterialGuidLookup.
    [[nodiscard]] SceneUpgradeReport UpgradeScene( SceneSerialized& scene );

    // The one log line of an in-memory upgrade: "'<source>': upgraded v71->v77 in memory (...), save to keep".
    [[nodiscard]] std::string DescribeUpgrade( std::string_view source, const SceneUpgradeReport& report );
} // namespace Desert::Core
