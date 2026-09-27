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
// ReceiveShadows and OutlineDraw (editor/forward-only flags). HiddenSubmeshes is different — dropping it would
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

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
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
    };

    struct HLODNotInstanced
    {
        std::size_t   Record = kNoRecord;
        HLODExclusion Reason = HLODExclusion::Unreadable;
    };

    // One instanced component of the HLOD: the ISM block, ready to be a record's "InstancedStaticMesh".
    struct HLODBatch
    {
        Assets::InstancedStaticMeshComponentSer Block;
        std::vector<std::size_t>                Sources; // records that contributed, in member order
    };

    struct InstancingHLOD
    {
        std::vector<HLODBatch>        Batches;
        std::vector<HLODNotInstanced> NotInstanced;
        std::size_t                   Instances = 0;
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
    [[nodiscard]] inline InstancingHLOD BuildInstancingHLOD( std::span<const Assets::EntityData> records,
                                                             std::span<const glm::mat4>          world,
                                                             std::span<const std::size_t>        members,
                                                             Common::Json::Issues&               issues )
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
                add( std::move( ism ), index, std::move( instances ) );
            }
        }
        return hlod;
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

    // The record of batch @p batch: an entity with only the InstancedStaticMesh block, which ComponentRegistry
    // loads as it loads any authored ISM. What the cook writes to the HLOD file and editor Play instantiates.
    [[nodiscard]] inline Assets::EntityData HLODRecord( const HLODBatch& batch, Common::UUID id,
                                                        std::size_t index )
    {
        Assets::EntityData record;
        record.id                                                  = id;
        record.Tag                                                 = "HLOD " + std::to_string( index );
        record.Components[std::string( kInstancePointsComponent )] = Common::Json::FromStruct( batch.Block );
        return record;
    }
} // namespace Desert::Core::Rules
