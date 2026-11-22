#pragma once
// Ported from UE 5.8 Engine/Source/Runtime/Engine/Private/WorldPartition/HLOD/HLODBuilder.cpp:164-260
// (UHLODBuilder::BatchInstances, the static-mesh half), adapted: records of a cooked cell instead of
// UActorComponents, a batch keyed by the canonical text of its InstancedStaticMesh block instead of
// FISMComponentDescriptor::GetTypeHash, and every source that cannot be batched returned with its reason instead
// of logged as a warning.
//
// THE INSTANCING HLOD OF ONE CELL (WP10): WHAT STANDS IN FOR A CELL WHILE THE CELL ITSELF IS NOT LOADED.
//
// UE's first HLOD type is Instancing: the HLOD of a cell draws the very meshes the cell's actors draw, one
// instanced component per distinct (mesh, materials, render flags), with one instance per source component at
// that component's world transform. Nothing is simplified or merged — it is the same geometry, drawn in a few
// draw calls from one small file instead of the cell's full records. That is exactly what this file builds,
// out of the same records the cook splits into cells: a list of InstancedStaticMesh blocks per cell, in the
// shape ComponentRegistry reads (Assets::InstancedStaticMeshComponentSer), so an HLOD record is loaded by the
// same code as any authored ISM.
//
// ── WHAT IS BATCHED ──────────────────────────────────────────────────────────────────────────────
//
//   * A StaticMesh block naming a mesh asset (MeshHandle {Guid, Path}) or a Primitive: one instance at the
//     record's WORLD matrix (Detail::ComposeWorld, the loader's own composition).
//   * An InstancedStaticMesh block: each of its instances, whose matrices are WORLD already
//     (Detail::AppendInstancePoints states why).
//
// The batch key is everything an ISM carries that changes the picture: mesh GUID and path, material GUIDs and
// paths, primitive, CastShadows. What a StaticMesh carries and an ISM cannot is NOT carried: ForcedLOD/LODBias
// (an HLOD is seen from beyond its cell's loading range, where the automatic pick is the one wanted),
// ReceiveShadows and OutlineDraw (editor/forward-only flags), TranslucencySortPriority (the translucency
// pass sorts the static queue only; an ISM is never in it). HiddenSubmeshes is different — dropping it would
// draw geometry the author hid — so a record with hidden submeshes is not batched and says so.
//
// ── WHAT IS NOT, AND WHY IT IS RETURNED RATHER THAN DROPPED ──────────────────────────────────────
//
// A record that draws something and gets no instance is a HOLE in the distant picture when its cell unloads.
// WP11 has to know every one of them, so each is listed with its reason (UE logs the same cases and moves on):
//   EditorMesh      — a mesh built in the editor (StaticMesh.EditMesh): there is no asset to instance; UE
//                     refuses a private/transient mesh for the same reason.
//   HiddenSubmeshes — see above.
//   SkinnedMesh     — a skinned mesh; UE batches only INSTANCED skinned meshes, and there are none here.
//   Unreadable      — a mesh block of the wrong shape; the Issue says where, as it does for the loader.
//   CustomShaderMaterial — every material the batch would bind draws with its own (DSL) shader. The ISM draw
//                     path is the batched lit one and skips such a component whole (MeshECSSystem, "Instanced
//                     Static Mesh doesn't support custom-shader materials"), so an instance written for it
//                     would be a hole nobody named. Asked of a CustomShaderSource, because which shader a
//                     material names is in the material's file, not in this one.
//   LandscapeTile   — a landscape tile (UE: ALandscapeStreamingProxy). It draws terrain and has no mesh block, so
//                     reading it as "draws nothing" made the far terrain vanish with the log saying "every
//                     drawing record has its stand-in" (WP-FAR-1, live). UE stands a landscape HLOD in for it
//                     (ULandscapeHLODBuilder, a simplified mesh of the proxy). BuildCellHLOD
//                     (WorldPartitionHLODBuilders.hpp) hands every such tile to the registry's landscape builder
//                     (WP-FAR-6), which makes it a built mesh of the tile's own heights; only a registry with no
//                     landscape builder keeps it a NAMED hole.
//   LandscapeSpline — a landscape spline entity (UE: ALandscapeSplineActor). Its meshes are bent along its
//                     splines, not stated in a mesh block (UE: USplineMeshComponents, which the HLOD builds into
//                     its mesh). BuildCellHLOD (WorldPartitionHLODBuilders.hpp) hands every such record to the
//                     registry's landscape spline builder (HLODBuilderLandscapeSplines), which makes the bent
//                     meshes one built WORLD-space mesh; only a registry with no spline builder keeps it a NAMED
//                     hole, and a spline that lays no mesh needs no stand-in.
// A record that draws nothing — no mesh block, a StaticMesh naming neither asset nor primitive, an ISM
// without instances, `Visibility.Visible == false` — needs no stand-in and is neither batched nor listed.
//
// PURE: a function of the records and the list of member indices, so the cook, a suite and a later editor
// preview get one answer. Deterministic: batches in the order their first source appears in @p members,
// instances in member order.
#include <Engine/Assets/Prefab/PrefabData.hpp>
#include <Engine/Core/Serialize/WorldPartitionRules.hpp>

#include <Common/Json/Document.hpp>
#include <Common/Json/Json.hpp>
#include <Common/Utilities/PakFile.hpp>


#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <Common/Core/Core.hpp>
#include <Common/Json/ReadWrite.hpp>
#include <Common/Json/DocumentStruct.hpp>

namespace Desert::Core::Rules
{
    inline constexpr std::string_view kVisibilityComponent  = "Visibility";
    inline constexpr std::string_view kVisibleField         = "Visible";
    inline constexpr std::string_view kSkinnedMeshComponent = "SkinnedMesh";

    // Written into the index by name (rfl spells an enum by its enumerator), so the list is closed: a reader
    // refuses a reason it does not know.
    enum class HLODExclusion
    {
        EditorMesh,
        HiddenSubmeshes,
        SkinnedMesh,
        Unreadable,
        CustomShaderMaterial,
        LandscapeTile,
        LandscapeSpline,
    };

    // WHAT THE BUILDER MAY KNOW ABOUT A MATERIAL FROM OUTSIDE THE FILE: whether the material a slot names (by the
    // GUID its block states — null when none — and the path beside it) draws with its own shader rather than the
    // Lit surface. AN EMPTY SOURCE IS A STATED CONDITION, as for AssetBoundsSource: no material is then known to
    // be custom, which is all a caller with no registry (a suite over one file) can say.
    using CustomShaderSource =
         std::function<bool( const Common::Content::AssetGuid& guid, std::string_view path )>;

    // WHETHER A FOLIAGE TYPE STANDS IN ITS CELL'S HLOD (UE UFoliageType::bIncludeInHLOD, FOLT 5), asked by the
    // {FoliageTypeGuid, FoliageTypePath} text a record's Foliage block states, because the flag is in the type's
    // `.defoliage`, not in this file. An error (the type does not resolve or read) makes the record an Unreadable
    // hole, named. AN EMPTY SOURCE IS A STATED CONDITION: every type is in the HLOD, UE's default.
    using FoliageHLODSource =
         std::function<Common::ResultStr<bool>( std::string_view guid, std::string_view path )>;
    inline constexpr std::string_view kFoliageComponent = "Foliage";

    struct HLODNotInstanced
    {
        std::size_t   Record = kNoRecord;
        HLODExclusion Reason = HLODExclusion::Unreadable;
    };

    // One component of the HLOD. An Instancing layer's batch is the ISM block, ready to be a record's
    // "InstancedStaticMesh". A MeshMerge / MeshSimplify layer's batch is the built mesh instead (`Mesh`, a
    // StaticMesh block carrying the merged geometry as its EditMesh, in world space) and its `Block` is unused.
    // Which machine quality draws a batch (IMPOSTORS-HLOD): Always, or one side of an Impostor layer's pair - the
    // card, drawn while Scalability's ViewDistance.HLODImpostors is 1, and the same instances of the source mesh,
    // drawn while it is 0. The pair stands in for the same records; the HLOD switch shows exactly one side.
    enum class HLODBatchVariant : std::uint8_t
    {
        Always,
        ImpostorCard,
        ImpostorFallback,
    };

    struct HLODBatch
    {
        Assets::InstancedStaticMeshComponentSer       Block;
        std::optional<Assets::StaticMeshComponentSer> Mesh;
        std::vector<std::size_t>                      Sources; // records that contributed, in member order
        HLODBatchVariant                              Variant = HLODBatchVariant::Always;
    };

    struct InstancingHLOD
    {
        std::vector<HLODBatch>        Batches;
        std::vector<HLODNotInstanced> NotInstanced;
        std::size_t                   Instances = 0;
        // Foliage records left out because their type says so (IncludeInHLOD false). Not holes: the author chose
        // that a far cell draws none of the type.
        std::size_t FoliageLeftOut = 0;
    };

    namespace Detail
    {
        [[nodiscard]] inline std::array<float, 16> Flatten( const glm::mat4& matrix )
        {
            // Column-major, as ComponentRegistry writes an ISM matrix (a memcpy of glm::mat4).
            std::array<float, 16> flat{};
            std::memcpy( flat.data(), &matrix[0][0], sizeof( float ) * 16 );
            return flat;
        }

        // True when the ISM path would skip @p key whole: it names at least one material slot and every slot's
        // material draws with its own shader. One lit slot is enough for the ISM path to draw (it binds the
        // first lit slot), and a component naming no material draws with the default lit one.
        [[nodiscard]] inline bool DrawsOnlyWithCustomShaders( const Assets::InstancedStaticMeshComponentSer& key,
                                                              const CustomShaderSource& customShader )
        {
            if ( !customShader )
                return false;
            // Plain pointers taken once: the static analyser forgets a has_value() check inside the loop.
            const auto* const guidList = key.MaterialGuids ? &*key.MaterialGuids : nullptr;
            const auto* const pathList = key.MaterialPaths ? &*key.MaterialPaths : nullptr;
            const std::size_t guids    = guidList != nullptr ? guidList->size() : 0;
            const std::size_t paths    = pathList != nullptr ? pathList->size() : 0;
            const std::size_t slots = std::max( guids, paths );
            for ( std::size_t slot = 0; slot < slots; ++slot )
            {
                Common::Content::AssetGuid guid;
                if ( slot < guids && !( *guidList )[slot].empty() )
                    if ( const auto parsed = Common::Content::AssetGuidFromText( ( *guidList )[slot] ) )
                        guid = parsed.GetValue();
                const std::string_view path =
                     slot < paths ? std::string_view( ( *pathList )[slot] ) : std::string_view();
                if ( guid.IsNull() && path.empty() )
                    return false; // an empty slot draws with the default lit material
                if ( !customShader( guid, path ) )
                    return false;
            }
            return slots > 0;
        }

        // False when the record is authored invisible: then it draws nothing and needs no stand-in.
        [[nodiscard]] inline bool AuthoredVisible( const Assets::EntityData& record, Common::Json::Issues& issues )
        {
            const auto block = BlockOf( record, kVisibilityComponent, issues );
            if ( !block.has_value() )
                return true;
            bool visible = true;
            block->ReadInto( kVisibleField, visible, issues );
            return visible;
        }
    } // namespace Detail

    // THE INSTANCING HLOD of the records @p members (one cell's, in ResidencyUnitMembers order). @p world is
    // every record's world matrix (Detail::ComposeWorld over the whole file, indexed like @p records).
    [[nodiscard]] inline InstancingHLOD
    BuildInstancingHLOD( std::span<const Assets::EntityData> records, std::span<const glm::mat4> world,
                         std::span<const std::size_t> members, const CustomShaderSource& customShader,
                         Common::Json::Issues& issues, const FoliageHLODSource& foliage = {} )
    {
        InstancingHLOD                     hlod;
        std::map<std::string, std::size_t> batchOf; // canonical text of the batch key -> index into Batches

        const auto add = [&]( Assets::InstancedStaticMeshComponentSer key, std::size_t record,
                              std::vector<std::array<float, 16>> instances )
        {
            if ( instances.empty() )
                return;
            key.InstanceTransforms.reset();
            const std::string text    = Common::Json::Write( key );
            const auto [where, fresh] = batchOf.emplace( text, hlod.Batches.size() );
            if ( fresh )
            {
                HLODBatch batch;
                batch.Block                    = std::move( key );
                batch.Block.InstanceTransforms = std::vector<std::array<float, 16>>{};
                hlod.Batches.push_back( std::move( batch ) );
            }
            HLODBatch& batch = hlod.Batches[where->second];
            if ( batch.Sources.empty() || batch.Sources.back() != record )
                batch.Sources.push_back( record );
            hlod.Instances += instances.size();
            batch.Block.InstanceTransforms->insert( batch.Block.InstanceTransforms->end(), instances.begin(),
                                                    instances.end() );
        };

        for ( const std::size_t index : members )
        {
            const Assets::EntityData& record = records[index];
            if ( !Detail::AuthoredVisible( record, issues ) )
                continue;

            // FO-6: a foliage field whose type is left out of the HLOD (UE bIncludeInHLOD false).
            if ( const auto type = Detail::PayloadOf( record, kFoliageComponent ); type.has_value() && foliage )
            {
                std::string guid;
                std::string path;
                type->ReadInto( "FoliageTypeGuid", guid, issues );
                type->ReadInto( "FoliageTypePath", path, issues );
                const auto included = foliage( guid, path );
                if ( !included )
                {
                    hlod.NotInstanced.push_back( { index, HLODExclusion::Unreadable } );
                    continue;
                }
                if ( !included.GetValue() )
                {
                    hlod.FoliageLeftOut++;
                    continue;
                }
            }

            // A prefab instance never reaches here: the cook, Build HLODs and Play all build from the world
            // ExpandPrefabInstances made (WP21, WP21-PLAY). One that does is a caller that skipped the expansion -
            // named, not turned into a hole.
            if ( record.PrefabPath.has_value() && !record.PrefabPath->empty() )
            {
                issues.push_back( Common::Json::Issue{
                     record.id.has_value()
                          ? std::format( "Entities[id={}]", static_cast<std::uint64_t>( *record.id ) )
                          : std::format( "Entities[{}]", index ),
                     "a record of an expanded world (ExpandPrefabInstances)",
                     "an unexpanded instance of '" + *record.PrefabPath + "'" } );
                continue;
            }
            // A landscape tile draws terrain from its height file, not from a mesh block (the reason above).
            if ( Detail::PayloadOf( record, kLandscapeTileComponent ).has_value() )
            {
                hlod.NotInstanced.push_back( { index, HLODExclusion::LandscapeTile } );
                continue;
            }
            // A landscape spline draws meshes bent along its splines, not a mesh block (the reason above).
            if ( Detail::PayloadOf( record, kLandscapeSplineComponent ).has_value() )
            {
                hlod.NotInstanced.push_back( { index, HLODExclusion::LandscapeSpline } );
                continue;
            }
            if ( Detail::PayloadOf( record, kSkinnedMeshComponent ).has_value() )
                hlod.NotInstanced.push_back( { index, HLODExclusion::SkinnedMesh } );

            if ( const auto block = Detail::PayloadOf( record, kPrimitiveComponent ); block.has_value() )
            {
                Assets::StaticMeshComponentSer mesh;
                const std::size_t              before = issues.size();
                block->ReadValue( mesh, issues );
                if ( issues.size() != before )
                    hlod.NotInstanced.push_back( { index, HLODExclusion::Unreadable } );
                else if ( mesh.EditMesh.has_value() )
                    hlod.NotInstanced.push_back( { index, HLODExclusion::EditorMesh } );
                else if ( mesh.HiddenSubmeshes.value_or( 0 ) != 0 )
                    hlod.NotInstanced.push_back( { index, HLODExclusion::HiddenSubmeshes } );
                else if ( ( mesh.MeshHandle.has_value() &&
                            ( !mesh.MeshHandle->Guid.empty() || !mesh.MeshHandle->Path.empty() ) ) ||
                          mesh.Primitive.has_value() )
                {
                    // The Static Mesh states {Guid, Path} pairs (asset references v1); the instanced key keeps
                    // its own GUID / path fields.
                    Assets::InstancedStaticMeshComponentSer key;
                    if ( mesh.MeshHandle.has_value() && !mesh.MeshHandle->Guid.empty() )
                        key.MeshGuid = mesh.MeshHandle->Guid;
                    if ( mesh.MeshHandle.has_value() && !mesh.MeshHandle->Path.empty() )
                        key.MeshPath = mesh.MeshHandle->Path;
                    if ( mesh.MaterialSlots.has_value() && !mesh.MaterialSlots->empty() )
                    {
                        key.MaterialGuids = std::vector<std::string>{};
                        key.MaterialPaths = std::vector<std::string>{};
                        for ( const Assets::AssetGuidRef& slot : *mesh.MaterialSlots )
                        {
                            key.MaterialGuids->push_back( slot.Guid );
                            key.MaterialPaths->push_back( slot.Path );
                        }
                    }
                    key.Primitive     = mesh.Primitive;
                    // Absent = true on both sides; written only when false, as the registry writes it.
                    if ( !mesh.CastShadows.value_or( true ) )
                        key.CastShadows = false;
                    if ( Detail::DrawsOnlyWithCustomShaders( key, customShader ) )
                        hlod.NotInstanced.push_back( { index, HLODExclusion::CustomShaderMaterial } );
                    else
                        add( std::move( key ), index, { Detail::Flatten( world[index] ) } );
                }
            }

            if ( const auto block = Detail::PayloadOf( record, kInstancePointsComponent ); block.has_value() )
            {
                Assets::InstancedStaticMeshComponentSer ism;
                const std::size_t                       before = issues.size();
                block->ReadValue( ism, issues );
                if ( issues.size() != before )
                {
                    hlod.NotInstanced.push_back( { index, HLODExclusion::Unreadable } );
                    continue;
                }
                if ( !ism.MeshGuid.has_value() && !ism.MeshPath.has_value() && !ism.Primitive.has_value() )
                    continue;
                std::vector<std::array<float, 16>> instances =
                     ism.InstanceTransforms.value_or( std::vector<std::array<float, 16>>{} );
                if ( ism.CastShadows.value_or( true ) )
                    ism.CastShadows.reset();
                if ( Detail::DrawsOnlyWithCustomShaders( ism, customShader ) )
                {
                    hlod.NotInstanced.push_back( { index, HLODExclusion::CustomShaderMaterial } );
                    continue;
                }
                add( std::move( ism ), index, std::move( instances ) );
            }
        }
        return hlod;
    }

    // ── HLOD LAYERS (WP-FAR-7) ───────────────────────────────────────────────────────────────────────

    // The layer of @p partition named @p name, or null.
    [[nodiscard]] inline const HLODLayerSerialized* FindHLODLayer( const WorldPartitionSerialized& partition,
                                                                   std::string_view                name )
    {
        if ( !partition.HLODLayers.has_value() )
            return nullptr;
        for ( const HLODLayerSerialized& layer : *partition.HLODLayers )
            if ( layer.Name == name )
                return &layer;
        return nullptr;
    }

    // THE LAYER THE HLODs OF @p layer ARE GATHERED INTO (UE HLOD1 of HLOD0): the layer of the setup after the one
    // that names @p layer, in any partition (RuntimePartitionHLODSetupSerialized: the order is the chain), or null
    // when @p layer is the last setup wherever it is named. ValidateHLODLayers refuses a layer two partitions
    // follow with different layers, so the answer does not depend on which partition is read first.
    [[nodiscard]] inline const HLODLayerSerialized* NextHLODLayer( const WorldPartitionSerialized& partition,
                                                                   std::string_view                layer )
    {
        for ( const RuntimePartitionSerialized& runtime : partition.RuntimePartitions )
        {
            if ( !runtime.HLODSetups.has_value() )
                continue;
            const std::vector<RuntimePartitionHLODSetupSerialized>& setups = *runtime.HLODSetups;
            for ( std::size_t k = 0; k + 1 < setups.size(); ++k )
                if ( setups[k].HLODLayer == layer )
                    return FindHLODLayer( partition, setups[k + 1].HLODLayer );
        }
        return nullptr;
    }

    // The built-in layer types and the names their builders are registered under (Rules::HLODBuilderRegistry; UE
    // UHLODLayer::GetHLODBuilderClass maps a type to UHLODBuilderInstancing / MeshMerge / MeshSimplify the same
    // way). Custom is not here: its builder is the one its layer names.
    inline constexpr std::array<std::pair<HLODLayerType, std::string_view>, 5> kBuiltInHLODBuilders = { {
         { HLODLayerType::Instancing, "Instancing" },
         { HLODLayerType::MeshMerge, "MeshMerge" },
         { HLODLayerType::MeshSimplify, "MeshSimplify" },
         { HLODLayerType::MeshApproximate, "MeshApproximate" },
         { HLODLayerType::Impostor, "Impostor" },
    } };

    // The name of the builder @p layer is built by: its type's (kBuiltInHLODBuilders), or a Custom layer's
    // Builder. Empty for a Custom layer stating none (ValidateHLODLayers refuses it).
    [[nodiscard]] inline std::string_view HLODBuilderNameOf( const HLODLayerSerialized& layer )
    {
        if ( layer.Type == HLODLayerType::Custom )
            return layer.Builder.has_value() ? std::string_view( *layer.Builder ) : std::string_view();
        for ( const auto& [type, name] : kBuiltInHLODBuilders )
            if ( type == layer.Type )
                return name;
        return {};
    }

    // Refused, by name: a layer without a name or named twice, a Custom layer stating no Builder or a built-in
    // type's name as its Builder, a Builder on any other type (each builder refuses
    // its own Settings - MergeMaterials, TrianglePercent, the approximation - HLODLayerBuilderIssues), a CellSize
    // without a LoadingRange; and per
    // partition's HLOD setups (UE FRuntimePartitionDesc::HLODSetups): setups on a partition whose content is never
    // away, a setup without a name or named twice, a layer that is no layer of the world or named twice in one
    // partition, a chain with a hole (a spatially loaded last setup, an always-loaded setup before another), ranges
    // that do not grow along the chain, and one layer followed by different layers in two partitions. The layer's
    // builder and Settings are the REGISTRY's to judge (HLODLayerBuilderIssues, WorldPartitionHLODBuilders.hpp): a
    // world naming a builder this process has not registered still loads, edits and plays its saved HLODs - only
    // Build HLODs is refused.
    // A MeshApproximate layer's settings, refused by name outside the ranges its readers take
    // (HLODApproximationSettings states each); Core::BuildApproximateShell refuses through this same check.
    [[nodiscard]] inline Common::BoolResultStr ValidateHLODApproximation( const HLODApproximationSettings& settings )
    {
        if ( !( settings.WorldSpaceApproximationAccuracy > 0.0f ) )
            return Common::MakeError<bool>( std::format( "WorldSpaceApproximationAccuracy {:f} cm is not > 0",
                                                         settings.WorldSpaceApproximationAccuracy ) );
        if ( settings.ClampVoxelDimension < 8 || settings.ClampVoxelDimension > 1024 )
            return Common::MakeError<bool>(
                 std::format( "ClampVoxelDimension {} is outside 8..1024", settings.ClampVoxelDimension ) );
        if ( !( settings.WindingThreshold >= -10.0f && settings.WindingThreshold <= 10.0f ) )
            return Common::MakeError<bool>(
                 std::format( "WindingThreshold {:f} is outside -10..10", settings.WindingThreshold ) );
        if ( settings.FillGaps && !( settings.GapDistance > 0.0f ) )
            return Common::MakeError<bool>(
                 std::format( "FillGaps with GapDistance {:f} cm, which is not > 0", settings.GapDistance ) );
        if ( !( settings.GeometricDeviation > 0.0f ) )
            return Common::MakeError<bool>(
                 std::format( "GeometricDeviation {:f} cm is not > 0", settings.GeometricDeviation ) );
        const auto powerOfTwo = []( int32_t value ) { return value > 0 && ( value & ( value - 1 ) ) == 0; };
        if ( settings.TextureSize < 64 || settings.TextureSize > 8192 || !powerOfTwo( settings.TextureSize ) )
            return Common::MakeError<bool>(
                 std::format( "TextureSize {} is not a power of two in 64..8192", settings.TextureSize ) );
        if ( settings.CaptureResolution < 64 || settings.CaptureResolution > 8192 )
            return Common::MakeError<bool>(
                 std::format( "CaptureResolution {} is outside 64..8192", settings.CaptureResolution ) );
        if ( !( settings.HardNormalAngle > 0.0f && settings.HardNormalAngle < 180.0f ) )
            return Common::MakeError<bool>(
                 std::format( "HardNormalAngle {:f} is outside (0, 180)", settings.HardNormalAngle ) );
        return BOOLSUCCESS;
    }

    // Refused, by name: a layer without a name or named twice, and what the long comment above lists (the
    // builder, the spatial loading, the partitions' HLOD setups).
    [[nodiscard]] inline Common::BoolResultStr ValidateHLODLayers( const WorldPartitionSerialized& partition )
    {
        const std::vector<HLODLayerSerialized>  none;
        const std::vector<HLODLayerSerialized>& layers =
             partition.HLODLayers.has_value() ? *partition.HLODLayers : none;
        if ( partition.HLODLayers.has_value() && layers.empty() )
            return Common::MakeError<bool>( "HLODLayers is stated and empty; a world with no layers states none" );
        for ( std::size_t i = 0; i < layers.size(); ++i )
        {
            const HLODLayerSerialized& layer = layers[i];
            if ( layer.Name.empty() )
                return Common::MakeError<bool>( std::format( "HLOD layer {} has no Name", i ) );
            for ( std::size_t j = 0; j < i; ++j )
                if ( layers[j].Name == layer.Name )
                    return Common::MakeError<bool>( "HLOD layer '" + layer.Name + "' is named twice" );
            // THE BUILDER'S NAME (UE UHLODLayer::HLODBuilderClass, read only for EHLODLayerType::Custom).
            const bool custom = layer.Type == HLODLayerType::Custom;
            if ( custom && ( !layer.Builder.has_value() || layer.Builder->empty() ) )
                return Common::MakeError<bool>(
                     std::format( "HLOD layer '{}' is Custom and names no Builder", layer.Name ) );
            if ( !custom && layer.Builder.has_value() )
                return Common::MakeError<bool>(
                     std::format( "HLOD layer '{}' is {} and names Builder '{}', which only a Custom layer reads",
                                  layer.Name, HLODBuilderNameOf( layer ), *layer.Builder ) );
            if ( custom )
                for ( const auto& builtIn : kBuiltInHLODBuilders )
                    if ( *layer.Builder == builtIn.second )
                        return Common::MakeError<bool>(
                             std::format( "HLOD layer '{}' is Custom and names the built-in builder '{}'; state "
                                          "Type {} instead",
                                          layer.Name, builtIn.second, builtIn.second ) );
            // SPATIAL LOADING (WP-FAR-8): both numbers or neither; positive and finite.
            if ( layer.CellSize.has_value() != layer.LoadingRange.has_value() )
                return Common::MakeError<bool>( "HLOD layer '" + layer.Name +
                                                "' states one of CellSize and LoadingRange; a spatially loaded "
                                                "layer states both, an always-loaded one neither" );
            if ( layer.CellSize.has_value() &&
                 !( std::isfinite( *layer.CellSize ) && *layer.CellSize > 0.0f &&
                    std::isfinite( *layer.LoadingRange ) && *layer.LoadingRange > 0.0f ) )
                return Common::MakeError<bool>( std::format(
                     "HLOD layer '{}': CellSize {:f} and LoadingRange {:f} must both be positive and finite",
                     layer.Name, *layer.CellSize, *layer.LoadingRange ) );
        }
        for ( const RuntimePartitionSerialized& runtime : partition.RuntimePartitions )
        {
            if ( !runtime.HLODSetups.has_value() )
                continue;
            const std::vector<RuntimePartitionHLODSetupSerialized>& setups = *runtime.HLODSetups;
            if ( setups.empty() )
                return Common::MakeError<bool>(
                     "partition '" + runtime.Name +
                     "' states HLODSetups and none; a partition without HLODs states no "
                     "HLODSetups" );
            if ( !RuntimePartitionClass( runtime.Type ).CanBeAway() )
                return Common::MakeError<bool>( "partition '" + runtime.Name +
                                                "' states HLODSetups, but what streams on it is never away, so no "
                                                "HLOD would ever stand in for it" );
            const RuntimePartitionLHGridSerialized* grid =
                 RuntimePartitionClass( runtime.Type ).CellGrid( runtime );
            for ( std::size_t k = 0; k < setups.size(); ++k )
            {
                const RuntimePartitionHLODSetupSerialized& setup = setups[k];
                if ( setup.Name.empty() )
                    return Common::MakeError<bool>(
                         std::format( "partition '{}': HLOD setup {} has no Name", runtime.Name, k ) );
                for ( std::size_t j = 0; j < k; ++j )
                {
                    if ( setups[j].Name == setup.Name )
                        return Common::MakeError<bool>( std::format(
                             "partition '{}': HLOD setup '{}' is named twice", runtime.Name, setup.Name ) );
                    // A layer twice in one chain would stand in for its own HLODs: the chain loops.
                    if ( setups[j].HLODLayer == setup.HLODLayer )
                        return Common::MakeError<bool>(
                             std::format( "partition '{}': HLOD layer '{}' is named by setups '{}' and '{}'; the "
                                          "chain would loop back on itself",
                                          runtime.Name, setup.HLODLayer, setups[j].Name, setup.Name ) );
                }
                const HLODLayerSerialized* layer = FindHLODLayer( partition, setup.HLODLayer );
                if ( layer == nullptr )
                    return Common::MakeError<bool>(
                         std::format( "partition '{}': HLOD setup '{}' names HLODLayer '{}', which is no layer of "
                                      "this world's HLODLayers",
                                      runtime.Name, setup.Name, setup.HLODLayer ) );
                const bool last    = k + 1 == setups.size();
                const bool spatial = layer->LoadingRange.has_value();
                // NO HOLE AT ANY DISTANCE: beyond a spatially loaded layer's range only the next setup stands in,
                // so the chain must go on; and an always-loaded layer's own HLODs never leave, so a setup after it
                // is never drawn - a setting nobody sees.
                if ( spatial && last )
                    return Common::MakeError<bool>( std::format(
                         "partition '{}': the last HLOD setup '{}' builds with spatially loaded layer '{}': "
                         "beyond "
                         "its LoadingRange nothing would stand in for its cells; add a setup after it",
                         runtime.Name, setup.Name, layer->Name ) );
                if ( !spatial && !last )
                    return Common::MakeError<bool>( std::format(
                         "partition '{}': HLOD setup '{}' builds with always-loaded layer '{}', so setup '{}' "
                         "after "
                         "it would never be drawn; state the layer's CellSize and LoadingRange or drop the later "
                         "setups",
                         runtime.Name, setup.Name, layer->Name, setups[k + 1].Name ) );
                // What stands in must reach further than what it stands in for, or the band where the one has
                // left and the other has not arrived is a hole: setup 0 beyond the partition's cells, each further
                // spatially loaded setup beyond the one before.
                if ( spatial && k == 0 && grid != nullptr && !( *layer->LoadingRange > grid->LoadingRange ) )
                    return Common::MakeError<bool>( std::format( "partition '{}' (LoadingRange {:f}) builds its "
                                                                 "first HLOD setup '{}' with spatially loaded "
                                                                 "layer '{}' of LoadingRange {:f}; the layer must "
                                                                 "load further out than the cells it stands "
                                                                 "in for",
                                                                 runtime.Name, grid->LoadingRange, setup.Name,
                                                                 layer->Name, *layer->LoadingRange ) );
                if ( k > 0 )
                {
                    const HLODLayerSerialized* before = FindHLODLayer( partition, setups[k - 1].HLODLayer );
                    if ( spatial && !( *layer->LoadingRange > *before->LoadingRange ) )
                        return Common::MakeError<bool>( std::format(
                             "partition '{}': HLOD setup '{}' (layer '{}', LoadingRange {:f}) follows layer '{}' "
                             "of "
                             "LoadingRange {:f}; a later setup must load further out than the one before",
                             runtime.Name, setup.Name, layer->Name, *layer->LoadingRange, before->Name,
                             *before->LoadingRange ) );
                }
                // ONE HIERARCHY OF LAYERS PER WORLD: the HLODs of a layer are gathered into one next layer, so a
                // layer two partitions follow with different layers would make its parent a guess.
                const HLODLayerSerialized* next =
                     last ? nullptr : FindHLODLayer( partition, setups[k + 1].HLODLayer );
                for ( const RuntimePartitionSerialized& other : partition.RuntimePartitions )
                {
                    if ( !other.HLODSetups.has_value() )
                        continue;
                    const std::vector<RuntimePartitionHLODSetupSerialized>& theirs = *other.HLODSetups;
                    for ( std::size_t j = 0; j < theirs.size(); ++j )
                    {
                        if ( theirs[j].HLODLayer != setup.HLODLayer )
                            continue;
                        const HLODLayerSerialized* theirNext =
                             j + 1 < theirs.size() ? FindHLODLayer( partition, theirs[j + 1].HLODLayer ) : nullptr;
                        if ( theirNext != next )
                            return Common::MakeError<bool>( std::format(
                                 "HLOD layer '{}' is followed by '{}' in partition '{}' and by '{}' in partition "
                                 "'{}'; one layer's HLODs are gathered into one next layer",
                                 setup.HLODLayer, next != nullptr ? next->Name : std::string( "nothing" ),
                                 runtime.Name, theirNext != nullptr ? theirNext->Name : std::string( "nothing" ),
                                 other.Name ) );
                    }
                }
            }
        }
        return Common::MakeSuccess( true );
    }

    // The layer a cell's FIRST HLOD is built with: its PARTITION's first HLOD setup (UE FRuntimePartitionDesc::
    // HLODSetups[0]), null when the partition states none (its cells then have no HLOD). The later setups are the
    // chain above it (NextHLODLayer). Call after ValidateHLODLayers.
    [[nodiscard]] inline const HLODLayerSerialized* GridHLODLayer( const WorldPartitionSerialized&   partition,
                                                                   const RuntimePartitionSerialized& runtime )
    {
        if ( !runtime.HLODSetups.has_value() || runtime.HLODSetups->empty() )
            return nullptr;
        return FindHLODLayer( partition, runtime.HLODSetups->front().HLODLayer );
    }

    // Whether any partition of the world states an HLOD setup, i.e. whether any cell has an HLOD to build or load.
    [[nodiscard]] inline bool AnyGridHLODLayer( const WorldPartitionSerialized& partition )
    {
        for ( const RuntimePartitionSerialized& runtime : partition.RuntimePartitions )
            if ( GridHLODLayer( partition, runtime ) != nullptr )
                return true;
        return false;
    }

    // The layer that builds composite @p composite of @p plan (WP-SETTINGS-UI, UE UHLODLayer::GetHLODLayer: the
    // actor's own HLODLayer, else the default): its anchor's HLODLayer block when it names a layer of the world
    // (PlannedComposite::HLODLayer), else its partition's first HLOD setup's. Null when neither states one. Call after ValidateHLODLayers.
    [[nodiscard]] inline const HLODLayerSerialized* CompositeHLODLayer( const WorldPartitionSerialized& partition,
                                                                        const PlannedComposite&         composite )
    {
        if ( composite.HLODLayer.has_value() && partition.HLODLayers.has_value() &&
             *composite.HLODLayer < partition.HLODLayers->size() )
            return &( *partition.HLODLayers )[*composite.HLODLayer];
        return GridHLODLayer( partition, partition.RuntimePartitions.at( composite.Grid ) );
    }

    // ONE PART OF A CELL'S HLOD: the members one layer builds (UE FWorldPartitionHLODUtilities::CreateHLODActors
    // groups a cell's actors by their HLOD layer and builds each group with its layer's builder).
    struct CellHLODPart
    {
        const HLODLayerSerialized* Layer = nullptr;
        std::vector<std::size_t>   Members; // record indices, in the order of the cell's members
    };

    // THE PARTS OF CELL @p cell OF @p plan: @p members (ResidencyUnitMembers of the cell) grouped by the layer that
    // builds their composite (CompositeHLODLayer), in the order each layer is first met. The parts make ONE HLOD of
    // the cell, streamed by its GRID's layer (WorldPartitionHLODHierarchy.hpp): in UE an actor's own layer also
    // decides where its HLOD actor streams; here the cell's HLOD is one residency unit (one HLOD per cell unit,
    // HLODRuntime), so the entity's layer decides how its part is BUILT - Instancing, MeshMerge, MeshSimplify and its
    // triangle share - and the parents above the cell are its grid layer's chain. Empty when the grid states no
    // layer and no composite names one. Refused, by the entity's path, when the grid states no layer and a composite
    // names one (that cell has no HLOD to put the part in), and for a member of no composite of the cell.
    [[nodiscard]] inline Common::ResultStr<std::vector<CellHLODPart>>
    CellHLODParts( const WorldPartitionSerialized& partition, const WorldPartitionPlan& plan, const PlannedCell& cell,
                   std::span<const std::size_t> members )
    {
        using Result                              = std::vector<CellHLODPart>;
        const RuntimePartitionSerialized&   grid  = partition.RuntimePartitions.at( cell.Grid );
        const HLODLayerSerialized* const    gridL = GridHLODLayer( partition, grid );
        std::map<std::size_t, const HLODLayerSerialized*> layerOf; // record -> the layer that builds it
        for ( const std::size_t c : cell.Composites )
        {
            const PlannedComposite&          composite = plan.Composites.at( c );
            const HLODLayerSerialized* const layer     = CompositeHLODLayer( partition, composite );
            if ( gridL == nullptr && layer != nullptr )
                return Common::MakeError<Result>( std::format(
                     "Entities[{}].{}.Layer names HLOD layer '{}', and its partition '{}' builds no HLOD; give the "
                     "partition an HLOD setup (World Partition > HLOD setups) or clear the entity's HLOD Layer",
                     composite.Anchor, kHLODLayerComponent, layer->Name, grid.Name ) );
            for ( const std::size_t member : composite.Members )
                layerOf[member] = layer;
        }
        Result parts;
        if ( gridL == nullptr )
            return Common::MakeSuccess( std::move( parts ) );
        for ( const std::size_t member : members )
        {
            const auto found = layerOf.find( member );
            if ( found == layerOf.end() )
                return Common::MakeError<Result>(
                     std::format( "record {} of a cell of partition '{}' is in none of the cell's composites", member,
                                  grid.Name ) );
            const auto part = std::find_if( parts.begin(), parts.end(),
                                            [&]( const CellHLODPart& p ) { return p.Layer == found->second; } );
            if ( part != parts.end() )
                part->Members.push_back( member );
            else
                parts.push_back( CellHLODPart{ found->second, { member } } );
        }
        return Common::MakeSuccess( std::move( parts ) );
    }

    // WHAT A MESH LAYER HANDS THE GEOMETRY BUILDER: the instanced parts the Instancing pass already chose (so a
    // merged HLOD leaves out exactly what an instanced one would, for the same named reasons), each an ISM
    // block naming its mesh and materials with WORLD instance matrices. The builder merges every instance into
    // one mesh in world space, one section per distinct material (UHLODBuilderMeshMerge), and when
    // `TrianglePercent` is stated keeps that share of the triangles (UHLODBuilderMeshSimplify, its settings'
    // target - Core::HLODMeshSimplifySettings).
    // With `MergeMaterials` (the mesh builder's Settings.MergeMaterials, WP v12) the mesh is ONE section over one UV
    // atlas, its material baked by the editor's Build HLODs (HLODMeshBuilder.hpp, BuildHLODMeshForBake).
    struct HLODMeshRequest
    {
        std::optional<float>                                     TrianglePercent; // absent = merge only
        std::optional<HLODMaterialMergeSerialized>               MergeMaterials;
        // MeshApproximate only (its builder's Settings, UE UHLODBuilderMeshApproximateSettings): stated = the
        // parts are wrapped into ONE closed shell (Core::BuildApproximateShell) instead of merged.
        std::optional<HLODApproximationSettings>                 Approximation;
        std::span<const Assets::InstancedStaticMeshComponentSer> Parts;
    };

    // The geometry side (Core::BuildHLODMesh), which reads mesh assets and runs the simplifier, so it is given to
    // the merge pass (MergeHLODBatches) rather than linked into the rules (as CustomShaderSource is). Answers a
    // StaticMesh block with the built EditMesh and the material slot lists, or an error naming the part that could
    // not be read.
    using HLODMeshBuilder =
         std::function<Common::ResultStr<Assets::StaticMeshComponentSer>( const HLODMeshRequest& request )>;

    // THE LANDSCAPE HLOD OF ONE TILE (WP-FAR-6): UE ULandscapeHLODBuilder
    // (Engine/Source/Runtime/Landscape/Private/LandscapeHLODBuilder.cpp), the pattern: a landscape streaming proxy
    // is not batched with the cell's meshes; it gets its own HLOD component, a mesh of the proxy's heights at a
    // reduced LOD, so the far terrain is never a hole. Here the tile record (index @p Tile into @p Records, @p
    // World its composed matrices) is answered with a StaticMesh block whose EditMesh is the tile in WORLD space.
    // THE RESOLUTION IS THE LANDSCAPE LOD THE HLOD IS SEEN AT, on every layer type (WP-FAR-6B): UE's
    // ComputeRequiredLandscapeLOD (AutomaticLOD policy) projects the tile's bounding sphere from
    // `MinVisibleDistance` - the nearest the HLOD is ever drawn, FHLODBuildContext::MinVisibleDistance, which is
    // the source cell's grid LoadingRange - and keeps the first landscape LOD whose screen size the tile no longer
    // exceeds. A heightfield is simplified by skipping samples (a landscape LOD), not by the layer's edge
    // collapses, so MeshSimplify's TrianglePercent does not apply to it (UE: the landscape builder ignores the
    // layer's mesh settings too).
    struct LandscapeHLODRequest
    {
        std::span<const Assets::EntityData> Records;
        std::span<const glm::mat4>          World;
        std::size_t                         Tile               = kNoRecord;
        float                               MinVisibleDistance = 0.0f; // cm
    };
    // The geometry side (it reads the tile's height file: Core::BuildLandscapeHLODMesh), given to the landscape
    // builder (Rules::HLODBuilderLandscape) as HLODMeshBuilder is given to the merge. An error (the height file is
    // missing, the root does not resolve) refuses the HLOD build by name.
    using LandscapeHLODBuilder =
         std::function<Common::ResultStr<Assets::StaticMeshComponentSer>( const LandscapeHLODRequest& request )>;

    // THE SPLINE MESHES OF ONE LANDSCAPE SPLINE RECORD (UE: the USplineMeshComponents of an ALandscapeSplineActor
    // merged into its cell's HLOD): record @p Spline of @p Records answered with a StaticMesh block whose EditMesh
    // is every segment's bent meshes in WORLD space (the splines are world cm), or nullopt when the spline lays no
    // mesh and so needs no stand-in.
    struct LandscapeSplineHLODRequest
    {
        std::span<const Assets::EntityData> Records;
        std::span<const glm::mat4>          World;
        std::size_t                         Spline = kNoRecord;
    };

    // The id of batch @p batch of the HLOD named @p name in world @p world: a function of the three, as a file's
    // GUID is, so a re-cook (and a second Play of the same scene) reproduces it. Zero is a possible hash and is
    // the null id; the caller refuses it, and an id that meets another record's.
    [[nodiscard]] inline Common::UUID HLODRecordId( std::string_view world, std::string_view name,
                                                    std::size_t batch )
    {
        const std::string identity = std::format( "{}\nHLOD\n{}\n{}", world, name, batch );
        return Common::UUID( Common::Utils::PakContentHash( identity.data(), identity.size() ) );
    }

    // The record of batch @p batch: an entity with only the InstancedStaticMesh block (or, for a built mesh, only
    // the StaticMesh block at the identity transform), which ComponentRegistry loads as it loads any authored one.
    // What the cook writes to the HLOD file and editor Play instantiates.
    [[nodiscard]] inline Assets::EntityData HLODRecord( const HLODBatch& batch, Common::UUID id,
                                                        std::size_t index )
    {
        Assets::EntityData record;
        record.id                                                  = id;
        record.Tag                                                 = std::format( "HLOD {}", index );
        if ( batch.Mesh.has_value() )
            record.Components[std::string( kPrimitiveComponent )] = Common::Json::FromStruct( *batch.Mesh );
        else
            record.Components[std::string( kInstancePointsComponent )] = Common::Json::FromStruct( batch.Block );
        return record;
    }
} // namespace Desert::Core::Rules
