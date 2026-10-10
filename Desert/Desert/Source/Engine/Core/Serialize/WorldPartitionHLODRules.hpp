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
//   * A StaticMesh block naming a mesh asset (MeshGuid/MeshPath) or a Primitive: one instance at the
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
//   PrefabInstance  — the instance's body is in the .deprefab, not in this file (see UnplacedPrefabInstances).
//   Unreadable      — a mesh block of the wrong shape; the Issue says where, as it does for the loader.
//   CustomShaderMaterial — every material the batch would bind draws with its own (DSL) shader. The ISM draw
//                     path is the batched lit one and skips such a component whole (MeshECSSystem, "Instanced
//                     Static Mesh doesn't support custom-shader materials"), so an instance written for it
//                     would be a hole nobody named. Asked of a CustomShaderSource, because which shader a
//                     material names is in the material's file, not in this one.
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

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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
        PrefabInstance,
        Unreadable,
        CustomShaderMaterial,
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
    struct HLODBatch
    {
        Assets::InstancedStaticMeshComponentSer       Block;
        std::optional<Assets::StaticMeshComponentSer> Mesh;
        std::vector<std::size_t>                      Sources; // records that contributed, in member order
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

            if ( record.PrefabPath.has_value() && !record.PrefabPath->empty() )
            {
                hlod.NotInstanced.push_back( { index, HLODExclusion::PrefabInstance } );
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
                else if ( mesh.MeshGuid.has_value() || mesh.MeshPath.has_value() || mesh.Primitive.has_value() )
                {
                    Assets::InstancedStaticMeshComponentSer key;
                    key.MeshGuid      = mesh.MeshGuid;
                    key.MeshPath      = mesh.MeshPath;
                    key.MaterialGuids = mesh.MaterialGuids;
                    key.MaterialPaths = mesh.MaterialPaths;
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

    // Refused, by name: a layer without a name or named twice, a ParentLayer that names no layer or itself or
    // closes a loop, a SimplifyTrianglePercent missing on MeshSimplify, outside (0, 1], or stated on another
    // type, and a DefaultHLODLayer that names no layer.
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
                return Common::MakeError<bool>( "HLOD layer " + std::to_string( i ) + " has no Name" );
            for ( std::size_t j = 0; j < i; ++j )
                if ( layers[j].Name == layer.Name )
                    return Common::MakeError<bool>( "HLOD layer '" + layer.Name + "' is named twice" );
            const bool simplify = layer.Type == HLODLayerType::MeshSimplify;
            if ( simplify && !layer.SimplifyTrianglePercent.has_value() )
                return Common::MakeError<bool>( "HLOD layer '" + layer.Name +
                                                "' is MeshSimplify and states no SimplifyTrianglePercent" );
            if ( !simplify && layer.SimplifyTrianglePercent.has_value() )
                return Common::MakeError<bool>(
                     "HLOD layer '" + layer.Name +
                     "' states SimplifyTrianglePercent, which only MeshSimplify reads" );
            if ( simplify && !( *layer.SimplifyTrianglePercent > 0.0f && *layer.SimplifyTrianglePercent <= 1.0f ) )
                return Common::MakeError<bool>( "HLOD layer '" + layer.Name + "': SimplifyTrianglePercent " +
                                                std::to_string( *layer.SimplifyTrianglePercent ) +
                                                " is outside (0, 1]" );
            // Walk the parent chain: every link must resolve, and it must end within the list's length.
            const HLODLayerSerialized* at = &layer;
            for ( std::size_t steps = 0; at->ParentLayer.has_value(); ++steps )
            {
                const HLODLayerSerialized* parent = FindHLODLayer( partition, *at->ParentLayer );
                if ( parent == nullptr )
                    return Common::MakeError<bool>( "HLOD layer '" + at->Name + "' names ParentLayer '" +
                                                    *at->ParentLayer + "', which is no layer of this world" );
                if ( steps >= layers.size() || parent == &layer )
                    return Common::MakeError<bool>( "HLOD layer '" + layer.Name +
                                                    "': its ParentLayer chain loops back on itself" );
                at = parent;
            }
        }
        if ( partition.DefaultHLODLayer.has_value() &&
             FindHLODLayer( partition, *partition.DefaultHLODLayer ) == nullptr )
            return Common::MakeError<bool>( "DefaultHLODLayer '" + *partition.DefaultHLODLayer +
                                            "' is no layer of this world's HLODLayers" );
        return Common::MakeSuccess( true );
    }

    // The layer a cell's HLOD is built with: the world's DefaultHLODLayer, null when it states none (cells then
    // have no HLOD). Call after ValidateHLODLayers.
    [[nodiscard]] inline const HLODLayerSerialized* CellHLODLayer( const WorldPartitionSerialized& partition )
    {
        return partition.DefaultHLODLayer.has_value() ? FindHLODLayer( partition, *partition.DefaultHLODLayer )
                                                      : nullptr;
    }

    // WHAT A MESH LAYER HANDS THE GEOMETRY BUILDER: the instanced parts the Instancing pass already chose (so a
    // merged HLOD leaves out exactly what an instanced one would, for the same named reasons), each an ISM
    // block naming its mesh and materials with WORLD instance matrices. The builder merges every instance into
    // one mesh in world space, one section per distinct material, and for MeshSimplify keeps
    // `TrianglePercent` of the triangles (UHLODBuilderMeshMerge / UHLODBuilderMeshSimplify).
    struct HLODMeshRequest
    {
        HLODLayerType                                            Type            = HLODLayerType::MeshMerge;
        float                                                    TrianglePercent = 1.0f;
        std::span<const Assets::InstancedStaticMeshComponentSer> Parts;
    };

    // The geometry side, which reads mesh assets and runs the simplifier, so it is given to the rules rather
    // than linked into them (as CustomShaderSource is). Answers a StaticMesh block with the built EditMesh and
    // the material slot lists, or an error naming the part that could not be read. AN EMPTY BUILDER IS A
    // STATED CONDITION: a mesh layer then REFUSES the build by name (BuildCellHLOD) rather than falling back
    // to instancing.
    using HLODMeshBuilder =
         std::function<Common::ResultStr<Assets::StaticMeshComponentSer>( const HLODMeshRequest& request )>;

    // THE HLOD OF ONE CELL BY ITS LAYER. Instancing: BuildInstancingHLOD. MeshMerge / MeshSimplify: the same
    // pass chooses the parts and the holes, then every batch that casts shadows becomes one built mesh and every
    // batch that does not becomes another (a merged mesh has one CastShadows), each standing in for the union
    // of its batches' sources. Refused when a mesh layer has no builder or the builder refuses.
    [[nodiscard]] inline Common::ResultStr<InstancingHLOD>
    BuildCellHLOD( const HLODLayerSerialized& layer, std::span<const Assets::EntityData> records,
                   std::span<const glm::mat4> world, std::span<const std::size_t> members,
                   const CustomShaderSource& customShader, Common::Json::Issues& issues,
                   const FoliageHLODSource& foliage, const HLODMeshBuilder& meshBuilder )
    {
        InstancingHLOD instanced = BuildInstancingHLOD( records, world, members, customShader, issues, foliage );
        if ( layer.Type == HLODLayerType::Instancing || instanced.Batches.empty() )
            return Common::MakeSuccess( std::move( instanced ) );
        if ( !meshBuilder )
            return Common::MakeError<InstancingHLOD>( "HLOD layer '" + layer.Name +
                                                      "' builds meshes and this cook was given no HLOD mesh "
                                                      "builder; cook it where meshes can be read (the editor)" );
        InstancingHLOD merged;
        merged.NotInstanced   = std::move( instanced.NotInstanced );
        merged.Instances      = instanced.Instances;
        merged.FoliageLeftOut = instanced.FoliageLeftOut;
        for ( const bool shadows : { true, false } )
        {
            std::vector<Assets::InstancedStaticMeshComponentSer> parts;
            std::vector<std::size_t>                             sources;
            for ( HLODBatch& batch : instanced.Batches )
            {
                if ( batch.Block.CastShadows.value_or( true ) != shadows )
                    continue;
                parts.push_back( batch.Block );
                sources.insert( sources.end(), batch.Sources.begin(), batch.Sources.end() );
            }
            if ( parts.empty() )
                continue;
            HLODMeshRequest request;
            request.Type            = layer.Type;
            request.TrianglePercent = layer.SimplifyTrianglePercent.value_or( 1.0f );
            request.Parts           = parts;
            auto built              = meshBuilder( request );
            if ( !built )
                return Common::MakeError<InstancingHLOD>( "HLOD layer '" + layer.Name + "': " + built.GetError() );
            HLODBatch batch;
            batch.Mesh = built.ExtractValue();
            if ( !shadows )
                batch.Mesh->CastShadows = false;
            // Sources in member order, once each (a record may feed several batches).
            for ( const std::size_t member : members )
                if ( std::find( sources.begin(), sources.end(), member ) != sources.end() )
                    batch.Sources.push_back( member );
            merged.Batches.push_back( std::move( batch ) );
        }
        return Common::MakeSuccess( std::move( merged ) );
    }

    // The id of batch @p batch of the HLOD named @p name in world @p world: a function of the three, as a file's
    // GUID is, so a re-cook (and a second Play of the same scene) reproduces it. Zero is a possible hash and is
    // the null id; the caller refuses it, and an id that meets another record's.
    [[nodiscard]] inline Common::UUID HLODRecordId( std::string_view world, std::string_view name,
                                                    std::size_t batch )
    {
        const std::string identity =
             std::string( world ) + "\nHLOD\n" + std::string( name ) + "\n" + std::to_string( batch );
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
        record.Tag                                                 = "HLOD " + std::to_string( index );
        if ( batch.Mesh.has_value() )
            record.Components[std::string( kPrimitiveComponent )] = Common::Json::FromStruct( *batch.Mesh );
        else
            record.Components[std::string( kInstancePointsComponent )] = Common::Json::FromStruct( batch.Block );
        return record;
    }
} // namespace Desert::Core::Rules
