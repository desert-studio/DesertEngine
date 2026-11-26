#pragma once

// THE WORLD PARTITION BLOCK OF A .desce (WorldPartitionSerialized and what it states), split out of SceneFormat.hpp
// (BUILD-MOD-3) so the partition's rules, its streamer and its editor panels name the partition's format without
// the scene document's (entity records, prefab data, the scene parse). SceneFormat.hpp includes this; the block is
// still a member of SceneSerialized and still one format, versioned by kWorldPartitionVersion below.

#include <Engine/Assets/AssetGuidRef.hpp>

#include <Common/Json/Document.hpp>


#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <glm/glm.hpp>

namespace Desert::Core
{
    // World Partition's own format generation, stated under Assets::kWorldPartitionSchemaTag. v1 (MIGRATE-SUBSYS):
    // the counter exists - the block is exactly what scene schema v70 states; the step (Tools/SceneMigrator,
    // kWorldPartitionVersionTagged) states the tag and changes nothing else. The next World Partition change is
    // v2 HERE, with its step in SceneMigrator's World Partition chain, never a kSceneVersion bump.
    // v2 (WP-DATA-LAYERS): the block may state `DataLayers` (DataLayerSerialized) and a record a `DataLayers`
    // component. Additive - a v1 file states neither and reads the same - so the step (Tools/SceneMigrator,
    // kWorldPartitionVersionDataLayers) states the tag and changes nothing else; the number moves so an older
    // build refuses a world whose layers it would silently ignore.
    // v3 (WP-DATA-LAYERS-RUNTIME): a layer states its `Type` (DataLayerType: Editor or Runtime) and a Runtime
    // layer its `InitialRuntimeState`; the block states `DataLayersLogicOperator`. Additive - a v2 file reads
    // every layer as an Editor layer and the operator as Or, which is what it meant - so the step
    // (Tools/SceneMigrator, kWorldPartitionVersionRuntimeDataLayers) states the tag and the defaults; the number
    // moves so a v2 build refuses a world whose runtime layers it would stream as if they were not there. v4
    // (WP-DATA-LAYER-OUTLINER): a layer states its `Parent` (UE UDataLayerInstance::Parent), its `DebugColor` (UE
    // UDataLayerAsset::DebugColor) and `InitiallyVisible` (UE bIsInitiallyVisible). Additive - a v3 file's layers
    // are top-level, grey and visible, which the read gives every missing key and the rewrite states - so the step
    // (Tools/SceneMigrator, kWorldPartitionVersionDataLayerOutliner) changes nothing else; the number moves so a
    // v3 build refuses a world whose nesting it would read flat (a child streaming past its parent).
    // v5 (WP-HASH-SET): UE's Runtime Hash Set - `Grids` is `RuntimePartitions` (RuntimePartitionSerialized: Name,
    // Type LHGrid / Persistent, the LHGrid block, HLODSetups), a grid's one HLODLayer and the layers' ParentLayer
    // chain are the partition's ordered HLOD setups (Tools/SceneMigrator, MigrateRuntimePartitionsV4ToV5).
    // v6 (WP-LOADER-ADAPTERS): a record may state a `LocationVolume` component (ECS::LocationVolumeComponent, UE
    // ALocationVolume: an editor loader's box, left out by the cook). Additive - a v5 file states none and reads
    // the same - so the step (Tools/SceneMigrator, kWorldPartitionVersionLocationVolumes) states the tag and
    // changes nothing else; the number moves so a v5 build refuses a world whose volumes it would cook as
    // entities. v7 (WP21-WHOLE; UE ALevelInstance::DesiredRuntimeBehavior): a prefab states its `RuntimeBehavior`
    // (Assets::PrefabRuntimeBehavior: Partitioned or Standalone) and an instance record may override it with its
    // own `RuntimeBehavior`. A Standalone instance is expanded with its body kept under its root - one composite,
    // one cell, one HLOD source, carried by the root (Core::WorldCells::ExpandPrefabInstances). The step (Tools/
    // SceneMigrator, kWorldPartitionVersionPrefabRuntimeBehavior) states Partitioned on every prefab (what each
    // meant) and Standalone on every nested instance record inside a prefab (a nested body was always kept whole
    // under its root, and a Partitioned nested instance now opens into the outer world).
    // v8 (WP-HLOD-BUILDERS): an HLOD layer's builder is a registered one (UE UHLODLayer::HLODBuilderClass) with
    // its own settings object (UE HLODBuilderSettings): the layer states `Settings`, read by its builder alone,
    // and a Custom layer its `Builder`. MeshSimplify's SimplifyTrianglePercent is its builder's
    // Settings.TrianglePercent (Tools/SceneMigrator, MigrateHLODBuilderSettingsV7ToV8); the number moves so a v7
    // build refuses a layer it cannot build. v9 (WP-HLOD-MATBAKE): an HLOD layer may state `MergeMaterials`
    // (HLODMaterialMergeSerialized). Additive - a v8 layer states none and builds one section per material, as it
    // did - so the step (Tools/SceneMigrator, kWorldPartitionVersionHLODMergeMaterials) states the tag and changes
    // nothing else; the number moves so a v8 build refuses a world whose merged material it would silently drop.
    // v10 (WP-DEBUG-DRAW): a runtime partition states its `DebugColor` (UE FSpatialHashRuntimeGrid::DebugColor) -
    // the gameplay debugger's World Partition category colours a partition's cells by it, and a data layer's cells
    // by the layer's DebugColor (v4). The step (Tools/SceneMigrator, StateRuntimePartitionDebugColorsV9ToV10)
    // states SeededDebugColor of each partition's name; nothing else changes. v11 (WP-MAP-VIEW): the block may
    // state `MiniMap` (WorldPartitionMiniMapSerialized, UE AWorldPartitionMiniMap): the texture Build Minimap
    // captured and the world rectangle it spans. Additive - a v10 file states none and the map simply has no
    // picture under its grid - so the step (Tools/SceneMigrator, kWorldPartitionVersionMiniMap) states the tag and
    // changes nothing else; the number moves because the read is strict and a v10 build would refuse the key
    // without naming the migrator. v12 (WP-INT-HLOD; UE UHLODLayer::HLODBuilderSettings): every built-in HLOD
    // builder reads its own settings object, as UE's UHLODBuilderMeshMergeSettings /
    // UHLODBuilderMeshSimplifySettings / UHLODBuilderMeshApproximateSettings do. A mesh layer's merged material
    // (HLODMaterialMergeSerialized, UE FMeshMergingSettings::bMergeMaterials + MaterialSettings) is its builder's
    // Settings.MergeMaterials, no longer a layer member (Tools/SceneMigrator, MigrateHLODMergeMaterialsV11ToV12),
    // and HLODLayerType gains MeshApproximate (WP-HLOD-APPROX), whose builder's Settings are
    // HLODApproximationSettings. The number moves so a v11 build refuses a layer whose merged material it would
    // read from the wrong place. v13 (WP-SETTINGS-UI): a record may state an `HLODLayer` component
    // (ECS::HLODLayerComponent, UE AActor::HLODLayer) naming one of the world's HLODLayers: its composite's part
    // of its cell's HLOD is built by that layer instead of its grid's (Rules::CompositeHLODLayer). Additive - a
    // v12 record states none and is built as before - so the step (Tools/SceneMigrator,
    // kWorldPartitionVersionEntityHLODLayer) states the tag and changes nothing else; the number moves so a v12
    // build refuses a world whose per-entity layers it would silently build by the grid's. v14 (IMPOSTORS-HLOD;
    // UE ImpostorBaker): HLODLayerType gains Impostor (octahedral impostor cards, HLODImpostorSettings). Additive -
    // a v13 world states none - so the step (Tools/SceneMigrator, kWorldPartitionVersionImpostorLayers) states the
    // tag and changes nothing else; the number moves because the enum read is strict and a v13 build would refuse
    // an Impostor layer without naming the migrator.
    inline constexpr int kWorldPartitionVersion = 14;

    // THIS WORLD IS PARTITIONED, AND THE NUMBERS THAT CANNOT BE DERIVED.
    //
    // PRESENCE IS THE SWITCH. A world is partitioned or it is not, and the absence of this block is how a
    // file says "not". There is no `Enabled` flag, because a flag would make `{"Enabled":false}` and no
    // block at all two spellings of one state - and the moment a state has two spellings, the two drift.
    // It is a `std::optional` member below for the same reason the mesh mirrors' `CastShadows` is one:
    // reflect-cpp omits a `nullopt` field entirely, so none of the `.desce` files in this repository
    // changes a single byte. The suite Desert/Tests/Engine/WorldPartition asserts that over the corpus.
    //
    // WHY IT IS A PROPERTY OF THE WORLD AND NOT AN ENGINE SETTING: owner decision 2026-09-18 - a world is
    // partitioned or it is not, and this is never an engine mode. A scene of twenty-four entities must not
    // pay for a grid.
    //
    // WHAT A HUMAN STATES, AND WHAT IS DERIVED. Cell boundaries, grid LEVELS, which level and cell a
    // composite is in, and what is always loaded are all functions of the scene's own contents (owner
    // decisions 2026-09-18 and O1 2026-09-23; Rules::PlanWorldPartition). What cannot be derived is how
    // big a cell is and how far away one starts to load - per grid.
    //
    // THE RUNTIME HASH IS A SET OF RUNTIME PARTITIONS (WP-HASH-SET, World Partition format v5) - UE 5.5's
    // UWorldPartitionRuntimeHashSet, the form UE creates new worlds with: the world states a LIST of partitions
    // (FRuntimePartitionDesc), each with a Name a record streams on by its RuntimeGrid block (UE AActor::
    // RuntimeGrid), a TYPE (FRuntimePartitionDesc::Class) whose settings decide how its content streams, and its
    // own HLOD setups (FRuntimePartitionDesc::HLODSetups). A record that names no partition streams on the FIRST
    // one, UE's main partition. v1..v4 stated a bare list of square grids (`Grids`); the migrator restated each as
    // an LHGrid partition (Tools/SceneMigrator, MigrateRuntimePartitionsV4ToV5).
    //
    // DATA LAYERS (WP-DATA-LAYERS, the O4 reservation taken up) are a SIBLING of `RuntimePartitions` in this
    // object - `DataLayers`, a list of named layers (DataLayerSerialized) - plus a membership list on the record
    // (the `DataLayers` component, ECS::DataLayersComponent). This step brings UE's EDITOR data layers: they
    // decide what of the world the editor holds and nothing of the cook or Play. A RUNTIME layer (WP v3,
    // WP-DATA-LAYERS-RUNTIME: DataLayerSerialized::Type) streams by the layer's runtime state: the set of runtime
    // layers a composite belongs to is the third coordinate of its cell beside level and square
    // (Rules::PlannedCell::DataLayers), with a file and an index row of its own.

    // THE KIND OF A RUNTIME PARTITION - UE's URuntimePartition subclasses (WorldPartition/RuntimeHashSet/
    // RuntimePartition*.h), the pattern and not the letter: FRuntimePartitionDesc::Class, stated by name.
    //   LHGrid     - URuntimePartitionLHGrid: the square hierarchical grid. A composite lands in the lowest level
    //                whose cell (CellSize * 2^L) holds its footprint, and a cell streams in within LoadingRange
    //                of a source. Its settings are the partition's `LHGrid` block.
    //   Persistent - URuntimePartitionPersistent: one always-loaded set. What streams on it is resident from the
    //                first frame of Play to the last, whatever the sources - a player, a game mode, a manager
    //                that must never stream out. It states no settings and no HLOD setups.
    // A further kind is an enumerator here, its settings block on RuntimePartitionSerialized and its class in
    // Rules (IRuntimePartition, Rules::RuntimePartitionClass): what plans, cooks and streams a world asks the
    // partition, never its type.
    enum class RuntimePartitionType
    {
        LHGrid,
        Persistent,
    };

    // The settings of an LHGrid partition (UE URuntimePartitionLHGrid::CellSize + URuntimePartition::LoadingRange
    // / Priority).
    struct RuntimePartitionLHGridSerialized
    {
        // Edge length of one level-0 cell in WORLD UNITS, and a world unit is a CENTIMETRE
        // (Common/Core/Units.hpp). Level L cells are CellSize * 2^L. The default is UE's own main grid,
        // 128 m, in our units; it is the number a world gets when switched on, nothing depends on it.
        //
        // CELLS ARE SQUARE AND THE GRID IS TWO-DIMENSIONAL - X and Z, unbounded in Y. That is the PATTERN
        // of UE's grid and not its letter (owner, 2026-09-22): the acceptance criterion is running ACROSS a
        // map, so the axis a player leaves behind is horizontal.
        float CellSize = 12800.0f;

        // How far from a streaming source a cell of this grid starts to load, in world units. UE's main
        // grid default, 256 m. Read by the streaming query (WP4); the partition itself does not depend
        // on it, which is why a cell's contents never change when it is edited.
        float LoadingRange = 25600.0f;

        // UE URuntimePartition::Priority: among the cells one source wants at one source priority, this
        // partition's load first when higher. It never changes WHICH cells are wanted (QueryStreamingCells).
        int Priority = 0;

        bool operator==( const RuntimePartitionLHGridSerialized& ) const = default;
    };

    // ONE LEVEL OF A PARTITION'S HLOD HIERARCHY - UE FRuntimePartitionHLODSetup ("one for each layer in the
    // hierarchy"). Setup 0's HLODs stand in for the partition's own cells; setup k+1's are built from, and stand
    // in for, setup k's HLOD cells (UE HLOD1 of HLOD0). The ORDER of the list is the chain - nowhere else states
    // it (v1..v4's HLODLayerSerialized::ParentLayer was restated as this order). How setup k's HLODs stream is its
    // layer's (HLODLayerSerialized::CellSize / LoadingRange: spatially loaded, or resident throughout).
    // In UE a setup lists SEVERAL layers because an actor picks its own HLOD layer (AActor::HLODLayer); here the
    // partition builds every cell's HLOD with the setup's layer, so a setup names exactly one - a second name
    // would be read by nothing.
    struct RuntimePartitionHLODSetupSerialized
    {
        // UE FRuntimePartitionHLODSetup::Name: unique within the partition, non-empty (the panel's row, the log).
        std::string Name;
        // Names a layer of WorldPartitionSerialized::HLODLayers (Rules::ValidateHLODLayers).
        std::string HLODLayer;

        bool operator==( const RuntimePartitionHLODSetupSerialized& ) const = default;
    };

    // ONE RUNTIME PARTITION - UE FRuntimePartitionDesc (Name, Class, MainLayer, HLODSetups).
    struct RuntimePartitionSerialized
    {
        // UE FRuntimePartitionDesc::Name. What a record names in its RuntimeGrid block to stream on this
        // partition (UE AActor::RuntimeGrid); a record that names none streams on the FIRST partition.
        // Required, non-empty and unique within the world (Rules::CheckRuntimeGrids refuses the list otherwise):
        // a name is the only way a record reaches a partition, so two of one name would be a guess.
        std::string Name;

        // UE FRuntimePartitionDesc::Class.
        RuntimePartitionType Type = RuntimePartitionType::LHGrid;

        // UE FRuntimePartitionDesc::MainLayer of class URuntimePartitionLHGrid: required on an LHGrid partition
        // and refused on any other type (Rules::CheckRuntimeGrids), so a number nobody reads cannot sit in a
        // file looking like a setting.
        std::optional<RuntimePartitionLHGridSerialized> LHGrid;

        // UE FRuntimePartitionDesc::HLODSetups, the hierarchy's levels in order
        // (RuntimePartitionHLODSetupSerialized). ABSENT = this partition's cells have no HLOD and a far cell draws
        // nothing; optional so a partition with none writes no key and an empty list is not a second spelling of
        // "none". Refused on Persistent: what it holds is never away, so nothing would ever stand in for it.
        std::optional<std::vector<RuntimePartitionHLODSetupSerialized>> HLODSetups;

        // UE FSpatialHashRuntimeGrid::DebugColor (WP v10, WP-DEBUG-DRAW): the colour the gameplay debugger's World
        // Partition category paints this grid's cells in when it colours by grid - an on-screen RGB, 0..1. A new
        // grid gets SeededDebugColor of its name (the panel's Add partition, the v9 -> v10 step), as UE seeds a new
        // layer's colour; after that it is the author's.
        glm::vec3 DebugColor = glm::vec3( 0.5f );

        // Compared, not just written: the editor's World Partition panel keeps the plan it painted and
        // the partition that plan was made from, and a plan is remade when the two stop matching. Without
        // an equality the panel would have to rebuild every frame (a whole serialisation of the world) or
        // trust a notification, and an undo that no notification reached would leave the old picture up.
        bool operator==( const RuntimePartitionSerialized& ) const = default;
    };

    // WHAT STANDS IN FOR A CELL WHILE IT IS AWAY - UE's UHLODLayer (WorldPartition/HLOD/HLODLayer.h), the
    // pattern and not the letter: a named layer with a builder TYPE and an optional PARENT layer. Every type is
    // built by a BUILDER found by name in the process's registry (Rules::HLODBuilderRegistry,
    // WorldPartitionHLODBuilders.hpp; UE UHLODLayer::GetHLODBuilderClass), which also reads the layer's Settings:
    //   Instancing   - UHLODBuilderInstancing: the cell's own meshes, one instanced component per distinct
    //                  (mesh, materials, flags). Nothing is simplified (WorldPartitionHLODRules.hpp).
    //   MeshMerge    - UHLODBuilderMeshMerge: every mesh the cell draws merged into ONE mesh in world space,
    //                  one section per distinct material; geometry unchanged, one draw per material.
    //   MeshSimplify - UHLODBuilderMeshSimplify: the merged mesh, then simplified (QEM, the editor's own
    //                  Simplify - Geometry/MeshCore/DynamicMesh/MeshSimplification.hpp) down to
    //                  SimplifyTrianglePercent of its triangles. What a far cell should cost.
    //   MeshApproximate - UHLODBuilderMeshApproximate (UE IGeometryProcessing_ApproximateActors): the merged mesh
    //                  voxel-wrapped into ONE closed shell (fast winding number -> Marching Cubes), gaps closed
    //                  morphologically, inner cavities dropped, simplified to a geometric tolerance, auto-UV'd and
    //                  textured by a capture of the source meshes (BaseColor, Normal, ORM) - one mesh and one
    //                  material per cell, what a dense quarter costs from far away (HLODApproximate.hpp).
    // The built mesh is stored INSIDE the cell's HLOD file, as a StaticMesh record's EditMesh: UE writes its
    // merged UStaticMesh into the HLOD actor's own package for the same reason - it is derived from the cell
    // and belongs to nobody else.
    enum class HLODLayerType
    {
        Instancing,
        MeshMerge,
        MeshSimplify,
        MeshApproximate,
        Custom,
        // Impostor - UE ImpostorBaker's octahedral impostors as an HLOD builder (Fortnite's far trees): every
        //            gathered batch keeps its instances, its mesh becomes ONE camera-facing card per instance
        //            shaded from an atlas of FramesPerSide^2 photos of the mesh (Core::HLODImpostorSettings,
        //            REQUIRED Settings; Core::HLODBuilderImpostor, Programs/Surface/ImpostorSurface.shader). Last,
        //            so no stored number of the older types moves.
        Impostor,
    };

    // A MESH LAYER'S MERGED MATERIAL - UE FMeshMergingSettings::bMergeMaterials with its FMaterialProxySettings
    // (TextureSize, bNormalMap, bMetallicMap / bRoughnessMap / bAmbientOcclusionMap, bEmissiveMap), the pattern:
    // the built mesh gets ONE UV atlas (MeshMerge: the source UV islands packed by UVPacker; MeshSimplify: AutoUV
    // on the simplified mesh) and ONE section, and Build HLODs bakes every source material into that atlas
    // (Editor/Core/HLODMaterialBake.hpp) as M_<key>.demat over T_<key>_{BaseColor, ORM, Normal,
    // Emissive}.detex beside the built HLODs - one draw per built mesh instead of one per material. BaseColor is
    // always baked; a channel switched off is not baked and the material keeps that input's constant default
    // (StandardSurface: flat normal, roughness 0.5, metallic 0, no emission).
    struct HLODMaterialMergeSerialized
    {
        // The atlas's side in texels (UE TextureSize, square), a power of two in [64, 8192]
        // (Rules::ValidateHLODLayers).
        int  TextureSize = 1024;
        bool NormalMap   = true;  // UE bNormalMap: T_<key>_Normal (tangent space of the built mesh)
        bool ORMMap      = true;  // UE bAmbientOcclusionMap + bRoughnessMap + bMetallicMap: T_<key>_ORM
        bool EmissiveMap = false; // UE bEmissiveMap: T_<key>_Emissive, scaled into 8 bits by EmissiveIntensity

        bool operator==( const HLODMaterialMergeSerialized& ) const = default;
    };

    // How a MeshApproximate shell's normals are split - UE FMeshApproximationSettings::bEstimateHardNormals.
    enum class HLODApproximationNormals
    {
        Smooth,    // one normal per vertex: the shell shades as one smooth surface
        HardEdges, // split where two faces meet sharper than HardNormalAngle (walls and roofs keep their edges)
    };

    // UE FMeshApproximationSettings (Engine/Classes/Engine/MeshMerging.h) - a MeshApproximate layer's builder
    // Settings (UE UHLODBuilderMeshApproximateSettings::MeshApproximationSettings, WP v12): the fields the
    // approximation reads, in centimetres where UE states metres. Every field is read by
    // Core::BuildApproximateShell or the capture bake (Editor HLODApproximateBake); ranges are refused by
    // Rules::ValidateHLODApproximation.
    struct HLODApproximationSettings
    {
        // UE ApproximationAccuracy: the voxel size of the wrap, cm (> 0). The shell is within about this of the
        // source surface, and the capture accepts a photo texel whose surface lies within it.
        float WorldSpaceApproximationAccuracy = 100.0f;
        // UE ClampVoxelDimension: the most voxels along the cell's longest side, 8..1024; a cell larger than
        // ClampVoxelDimension x accuracy is wrapped at the coarser size this allows.
        int32_t ClampVoxelDimension = 1024;
        // UE WindingThreshold: the fast winding number the solid starts at (0.5 = inside a closed surface).
        float WindingThreshold = 0.5f;
        // UE bFillGaps + GapDistance: a morphological close (dilate, then contract) by GapDistance cm, so a
        // street narrower than 2 x GapDistance between two houses is filled and the quarter is one shell.
        bool  FillGaps    = true;
        float GapDistance = 10.0f;
        // UE GeometricDeviation (SimplifyMethod GeometricTolerance): the simplified shell stays within this many
        // cm of the unsimplified one (> 0).
        float GeometricDeviation = 10.0f;
        // UE MaterialSettings.TextureSize: the baked BaseColor / Normal / ORM side, texels (power of two,
        // 64..8192); also the auto-UV packing resolution.
        int32_t TextureSize = 1024;
        // UE RenderCaptureResolution: the side of each capture photo of the source meshes, pixels (64..8192).
        int32_t CaptureResolution = 2048;
        // UE bEstimateHardNormals + HardNormalAngle (degrees, (0, 180)).
        HLODApproximationNormals NormalMode      = HLODApproximationNormals::HardEdges;
        float                    HardNormalAngle = 60.0f;

        bool operator==( const HLODApproximationSettings& ) const = default;
    };

    struct HLODLayerSerialized
    {
        std::string   Name;
        HLODLayerType Type = HLODLayerType::Instancing;
        // UE UHLODLayer::HLODBuilderClass (WP v8): the name a builder was registered under. REQUIRED on Custom and
        // REFUSED on every other type (whose builder is its type's), and never a built-in type's name, so one
        // builder has one spelling (Rules::ValidateHLODLayers).
        std::optional<std::string> Builder;
        // UE UHLODLayer::HLODBuilderSettings (WP v8): the builder's own settings object (UE: the
        // UHLODBuilderSettings subclass its GetSettingsClass names), read and checked by that builder alone
        // (IHLODBuilder::ValidateSettings) - MeshMerge's is {"MergeMaterials": {...}}
        // (HLODMaterialMergeSerialized, optional), MeshSimplify's {"TrianglePercent": ..., "MergeMaterials":
        // {...}}, MeshApproximate's the HLODApproximationSettings (WP v12). A builder that reads none refuses any,
        // so a number nobody reads cannot sit in a file looking like a setting. Absent = the layer states none.
        std::optional<Common::Json::Value> Settings;
        // UE UHLODLayer::bIsSpatiallyLoaded / CellSize / LoadingRange (WP-FAR-8): BOTH stated = the HLODs this
        // layer builds are SPATIALLY LOADED - each lives in a square cell of CellSize cm (by the centre of what it
        // stands in for) and is streamed by the same residency as a grid cell, within LoadingRange cm of a
        // source; the NEXT setup's HLOD (RuntimePartitionHLODSetupSerialized: the order is the chain) stands in
        // for that cell while the cell is not resident. BOTH absent = the layer's HLODs are resident for the whole
        // session (the top of a chain). One without the other, a spatially loaded layer that is the last setup of
        // a partition (a hole beyond its range) and an always-loaded layer that is not (a next setup never drawn)
        // are refused by Rules::ValidateHLODLayers.
        std::optional<float> CellSize;
        std::optional<float> LoadingRange;

        // Settings is an untyped value, compared as Json::Same compares one (its Write text); the rest
        // member-wise.
        bool operator==( const HLODLayerSerialized& other ) const
        {
            const bool sameSettings =
                 Settings.has_value() == other.Settings.has_value() &&
                 ( !Settings.has_value() || Common::Json::Same( *Settings, *other.Settings ) );
            return Name == other.Name && Type == other.Type && Builder == other.Builder && sameSettings &&
                   CellSize == other.CellSize && LoadingRange == other.LoadingRange;
        }
    };

    // WHAT A DATA LAYER IS FOR - UE EDataLayerType (WorldPartition/DataLayer/DataLayerType.h).
    enum class DataLayerType
    {
        // Organises the world while it is authored; nothing to the cook or to Play (see DataLayerSerialized).
        Editor,
        // Its members stream by the layer's runtime state (DataLayerRuntimeState): the set of runtime layers a
        // composite belongs to is the third coordinate of the cell it lands in (Rules::PlannedCell::DataLayers).
        Runtime,
    };

    // HOW FAR A RUNTIME LAYER'S CONTENT IS ALONG - UE EDataLayerRuntimeState, in the same order (each state includes
    // the one before it, so "the furthest" and "the least" of several are their max and min).
    enum class DataLayerRuntimeState
    {
        Unloaded,  // its cells are not read: not a byte of them leaves the disk
        Loaded,    // its cells in range are read and held in memory, and are not entities
        Activated, // its cells in range are entities
    };

    // HOW A CELL OF SEVERAL RUNTIME LAYERS COMBINES THEIR STATES - UE EWorldPartitionDataLayersLogicOperator
    // (UWorldPartition::DataLayersLogicOperator). Or: the cell is as far along as its FURTHEST layer (UE's default);
    // And: as its least.
    enum class WorldPartitionDataLayersLogicOperator
    {
        Or,
        And,
    };

    // ONE DATA LAYER OF A WORLD - UE's UDataLayerInstance (WorldPartition/DataLayer/DataLayerInstance.h), the
    // pattern and not the letter: UE names a layer by a UDataLayerAsset and keeps an instance of it per world;
    // here the world states the layer by name, and an entity names the layers it belongs to (its `DataLayers`
    // component), as an actor lists its DataLayerAssets.
    //
    // AN EDITOR LAYER (UE EDataLayerType::Editor): it organises the world while it is authored and is nothing to
    // the cook or to Play - every member is cooked and streams as if the layer were not there. What the editor
    // holds of the world follows it: a composite whose anchor belongs to layers, none of them loaded in the
    // editor, stays on disk wherever the loaded regions are (UE UDataLayerManager::ResolveIsLoadedInEditor - an
    // actor is loaded when ANY of its valid layers is; an actor in no layer is never held back by one).
    struct DataLayerSerialized
    {
        // UE UDataLayerInstance::GetDataLayerShortName. What an entity's DataLayers component names; required,
        // non-empty and unique within the world (Rules::CheckDataLayers refuses the list otherwise).
        std::string Name;

        // UE UDataLayerInstance::bIsInitiallyLoadedInEditor: whether the layer is loaded when the world opens in
        // the editor. The WORLD's default; what one user loaded or unloaded since is that user's state
        // (EditorRegions::DataLayerOverrides, kept beside the loaded regions - never in the world).
        bool InitiallyLoadedInEditor = true;

        // UE UDataLayerAsset::DataLayerType (WP v3). Editor as above; a RUNTIME layer (UE EDataLayerType::Runtime)
        // also splits the cook and streams in Play and in a packaged game by its runtime state - a composite
        // whose anchor belongs to runtime layers lands in a cell of that set of layers (Rules::PlannedCell::
        // DataLayers), whose file and index row are its own, and residency reads the cell only while the
        // layers' state wants it (Rules::DataLayerUnitStates).
        DataLayerType Type = DataLayerType::Editor;

        // UE UDataLayerInstance::InitialRuntimeState: the runtime state the layer starts Play and a packaged game
        // in (the game moves it: WorldStreamer::SetDataLayerRuntimeState). REQUIRED on a Runtime layer and
        // REFUSED on an Editor one (Rules::CheckDataLayers), so a state nobody reads cannot sit in a file looking
        // like a setting.
        std::optional<DataLayerRuntimeState> InitialRuntimeState;

        // UE UDataLayerInstance::Parent (WP v4): the layer this one nests under, by name; absent = a top-level
        // layer. A member of a child layer is a member of its parent too, the way UE resolves it: the parent gates
        // the child - it is loaded in the editor and visible only while every ancestor is (Rules::
        // EffectiveDataLayerFlags), and a runtime child is never further along than its parent (Rules::
        // EffectiveDataLayerStates). Must name ANOTHER layer of the world and close no cycle, and a Runtime
        // layer's parent is a Runtime layer (UE UDataLayerInstance::CanBeChildOf) - Rules::CheckDataLayers.
        std::optional<std::string> Parent;

        // UE UDataLayerAsset::DebugColor (WP v4): the layer's colour, sRGB 0..1 - its swatch in the Data Layers
        // outliner and the mark on its members' rows there. A layer made in the editor gets
        // Rules::SeededDataLayerColor of its name; the default is what a v3 layer reads as.
        std::array<float, 3> DebugColor = { 0.5f, 0.5f, 0.5f };

        // UE UDataLayerInstance::bIsInitiallyVisible (WP v4): whether the layer's members are drawn in the editor
        // when the world opens. The WORLD's default; hiding or showing the layer afterwards is the session's
        // (Editor DataLayerEditor::SetVisible, UE's transient bIsVisible) and is never written. Visibility is not
        // loading: a hidden layer's members stay loaded and in the Outliner, only not drawn (UE bHiddenEdLayer).
        bool InitiallyVisible = true;

        bool operator==( const DataLayerSerialized& ) const = default;
    };

    // THE COLOUR A NEW GRID OR DATA LAYER IS GIVEN (UE FColor::MakeRandomSeededColor, which seeds a new data layer's
    // DebugColor): a hue from the FNV-1a hash of @p name - FNV and not std::hash, whose value is the standard
    // library's choice and would give one world two colours on two platforms - at full value and 0.75 saturation,
    // so two names read apart on the debugger's dark map.
    [[nodiscard]] inline glm::vec3 SeededDebugColor( std::string_view name )
    {
        std::uint32_t hash = 2166136261u;
        for ( const char c : name )
        {
            hash ^= static_cast<std::uint8_t>( c );
            hash *= 16777619u;
        }
        const float hue        = static_cast<float>( hash % 360u ) / 60.0f; // sextant 0..6
        const float saturation = 0.75f;
        const float x          = 1.0f - std::abs( std::fmod( hue, 2.0f ) - 1.0f );
        const float low        = 1.0f - saturation;
        const float mid        = low + saturation * x;
        switch ( static_cast<int>( hue ) )
        {
            case 0:
                return { 1.0f, mid, low };
            case 1:
                return { mid, 1.0f, low };
            case 2:
                return { low, 1.0f, mid };
            case 3:
                return { low, mid, 1.0f };
            case 4:
                return { mid, low, 1.0f };
            default:
                return { 1.0f, low, mid };
        }
    }

    // THE WORLD SEEN FROM ABOVE (WP v4; UE AWorldPartitionMiniMap + UWorldPartitionMiniMapBuilder): Build Minimap
    // loads the world one BuilderCellSize square at a time, captures each from straight above through an
    // orthographic camera - the G-buffer's BaseColor, UE's SCS_BaseColor capture - and stitches them into one
    // texture, `<stem>_MiniMap.detex` beside the scene. The World Partition map draws it under its cell grid.
    // Editor-only, as UE's minimap actor is: nothing at runtime reads it, and HLODBuildChecksum leaves it out, so
    // building a minimap never makes an HLOD stale.
    struct WorldPartitionMiniMapSerialized
    {
        // UE MiniMapTexture: the captured texture (its header GUID, and its path relative to the assets root).
        Assets::AssetGuidRef Texture;
        // UE MiniMapWorldBounds, X/Z in cm: the rectangle the texture spans. Texel row 0 lies along MinZ, column 0
        // along MinX (the capture looks down -Y with image up = world -Z).
        float MinX = 0.0f;
        float MinZ = 0.0f;
        float MaxX = 0.0f;
        float MaxZ = 0.0f;
        // UE WorldUnitsPerPixel: centimetres of world per texel. Build Minimap reads it (the next build keeps it).
        float WorldUnitsPerPixel = 50.0f;
        // UE UWorldPartitionBuilder IterativeCellSize: the side (cm) of the square of the world Build Minimap
        // loads and captures at once, so a world never sits in memory whole. Build Minimap reads it.
        float BuilderCellSize = 102400.0f;

        bool operator==( const WorldPartitionMiniMapSerialized& ) const = default;
    };

    struct WorldPartitionSerialized
    {
        // UE UWorldPartitionRuntimeHashSet::RuntimePartitions (RuntimePartitionSerialized).
        std::vector<RuntimePartitionSerialized> RuntimePartitions;
        // The world's HLOD layers (UE: the UHLODLayer assets a world's partitions name). Which layers a cell's
        // HLODs are built with is its PARTITION's choice (RuntimePartitionSerialized::HLODSetups). Optional, so a
        // world with no layers writes no key (reflect-cpp omits a nullopt) and an empty list is not a second
        // spelling of "none".
        std::optional<std::vector<HLODLayerSerialized>> HLODLayers;
        // Side of the square region (cm) whose cells a packaged build ships in ONE on-demand content chunk
        // (UE: a World Partition cell's content lands in a chunk the runtime mounts as the cell streams in).
        // ABSENT = the world ships in the base chunk, as before - no migrator, the key is simply not written.
        // Present must be > 0 and finite (ChunkLayoutOf refuses anything else).
        std::optional<float> ChunkRegionSize;
        // The world's data layers (UE: the UDataLayerInstance list of the world's AWorldDataLayers, one per
        // UDataLayerAsset). Which layers an entity belongs to is its own `DataLayers` component. Optional, so a
        // world with no layers writes no key and an empty list is not a second spelling of "none".
        std::optional<std::vector<DataLayerSerialized>> DataLayers;
        // UE UWorldPartition::DataLayersLogicOperator (WP v3): how a cell of several runtime layers combines
        // their runtime states (Rules::EffectiveDataLayerState). The world's, as in UE; Or unless stated.
        WorldPartitionDataLayersLogicOperator DataLayersLogicOperator = WorldPartitionDataLayersLogicOperator::Or;
        // UE AWorldPartitionMiniMap (WP v4): absent until Build Minimap has run on the world.
        std::optional<WorldPartitionMiniMapSerialized> MiniMap;

        bool operator==( const WorldPartitionSerialized& ) const = default;
    };

} // namespace Desert::Core
