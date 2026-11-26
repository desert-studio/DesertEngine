#include "PackedPrefab.hpp"

#include <Engine/Core/Serialize/WorldPartitionHLODRules.hpp>
#include <Engine/Core/Serialize/WorldPartitionRules.hpp>

#include <Common/Json/Json.hpp>

#include <cstddef>
#include <cstdint>
#include <format>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <Common/Json/ReadWrite.hpp>

namespace Desert::Assets
{
    namespace
    {
        [[nodiscard]] std::string JoinWords( const std::vector<std::string>& words, std::string_view separator )
        {
            std::string joined;
            for ( std::size_t i = 0; i < words.size(); ++i )
            {
                if ( i > 0 )
                    joined += separator;
                joined += words[i];
            }
            return joined;
        }

        // The facts of a StaticMesh block as the file states them: an absent field is the component's default,
        // exactly as the loader reads it.
        [[nodiscard]] StaticMeshPackFacts FactsOf( const StaticMeshComponentSer& mesh )
        {
            StaticMeshPackFacts facts;
            facts.OutlineDraw              = mesh.OutlineDraw.value_or( false );
            facts.ForcedLOD                = mesh.ForcedLOD.value_or( -1 );
            facts.LODBias                  = mesh.LODBias.value_or( 0 );
            facts.ReceiveShadows           = mesh.ReceiveShadows.value_or( true );
            facts.ReceivesDecals           = mesh.ReceivesDecals.value_or( true );
            facts.TranslucencySortPriority = mesh.TranslucencySortPriority.value_or( 0 );
            facts.HiddenSubmeshes          = mesh.HiddenSubmeshes.value_or( 0 );
            facts.MaxDrawDistance          = mesh.MaxDrawDistance.value_or( 0.0f );
            facts.EditMesh                 = mesh.EditMesh.has_value();
            return facts;
        }

        // The ISM key of a StaticMesh block that draws: the fold's identity (mesh, slots, primitive, both shadow
        // flags) in the instanced block's own spelling, the shape BuildInstancingHLOD writes and ComponentRegistry
        // reads. nullopt when the block names neither an asset nor a primitive (it draws nothing).
        [[nodiscard]] std::optional<InstancedStaticMeshComponentSer>
        GroupKeyOf( const StaticMeshComponentSer& mesh )
        {
            const bool namesAsset = mesh.MeshHandle.has_value() &&
                                    ( !mesh.MeshHandle->Guid.empty() || !mesh.MeshHandle->Path.empty() );
            if ( !namesAsset && !mesh.Primitive.has_value() )
                return std::nullopt;

            InstancedStaticMeshComponentSer key;
            if ( mesh.MeshHandle.has_value() && !mesh.MeshHandle->Guid.empty() )
                key.MeshGuid = mesh.MeshHandle->Guid;
            if ( mesh.MeshHandle.has_value() && !mesh.MeshHandle->Path.empty() )
                key.MeshPath = mesh.MeshHandle->Path;
            if ( mesh.MaterialSlots.has_value() && !mesh.MaterialSlots->empty() )
            {
                key.MaterialGuids = std::vector<std::string>{};
                key.MaterialPaths = std::vector<std::string>{};
                for ( const AssetGuidRef& slot : *mesh.MaterialSlots )
                {
                    key.MaterialGuids->push_back( slot.Guid );
                    key.MaterialPaths->push_back( slot.Path );
                }
            }
            key.Primitive = mesh.Primitive;
            // Absent = true on both sides; written only when false, as the registry writes them.
            if ( !mesh.CastShadows.value_or( true ) )
                key.CastShadows = false;
            if ( !mesh.CastContactShadow.value_or( true ) )
                key.CastContactShadow = false;
            return key;
        }
    } // namespace

    std::vector<std::string> StaticMeshPackBlockers( const StaticMeshPackFacts& facts )
    {
        // Each one is a knob somebody deliberately moved, so losing it silently is losing an authoring decision.
        std::vector<std::string> blockers;
        if ( facts.OutlineDraw )
            blockers.emplace_back( "a persistent outline" );
        if ( facts.ForcedLOD >= 0 )
            blockers.emplace_back( "a forced LOD" );
        if ( facts.LODBias != 0 )
            blockers.emplace_back( "a LOD bias" );
        if ( !facts.ReceiveShadows )
            blockers.emplace_back( "receive-shadows off" );
        if ( !facts.ReceivesDecals )
            blockers.emplace_back( "receives-decals off" );
        if ( facts.TranslucencySortPriority != 0 )
            blockers.emplace_back( "a translucency sort priority" );
        if ( facts.HiddenSubmeshes != 0 )
            blockers.emplace_back( "hidden submeshes" );
        if ( facts.MaxDrawDistance > 0.0f )
            blockers.emplace_back( "a max draw distance" );
        if ( facts.EditMesh )
            blockers.emplace_back(
                 "a mesh built in the editor (an instanced mesh names an asset or a primitive)" );
        return blockers;
    }

    std::string PrefabSourceHash( std::span<const EntityData> records )
    {
        // The body as the one writer spells it, so two bodies that write the same text are the same source.
        const std::string text = Common::Json::Write( std::vector<EntityData>( records.begin(), records.end() ) );
        std::uint64_t     hash = 14695981039346656037ULL; // FNV-1a 64 offset basis
        for ( const char c : text )
        {
            hash ^= static_cast<std::uint8_t>( c );
            hash *= 1099511628211ULL; // FNV-1a 64 prime
        }
        return std::format( "{:016x}", hash );
    }

    PackedPrefabSer PackPrefab( std::span<const EntityData> records )
    {
        PackedPrefabSer packed;
        packed.SourceHash = PrefabSourceHash( records );
        if ( records.empty() )
        {
            packed.Refused = "the prefab holds no entity";
            return packed;
        }

        // Every record's world matrix by the loader's own composition (Rules::Detail::ComposeWorld), then taken
        // into the root's frame: the instance root carries the placement, the instances everything below it.
        std::unordered_map<Common::UUID, std::size_t> byId;
        for ( std::size_t record = 0; record < records.size(); ++record )
        {
            const auto& id = records[record].id;
            if ( id.has_value() && !id->IsNull() )
                byId.emplace( *id, record );
        }
        const std::vector<glm::mat4> world  = Core::Rules::Detail::ComposeWorld( records, byId );
        const glm::mat4              toRoot = glm::inverse( world.front() );

        std::vector<std::string>           refusals;
        std::map<std::string, std::size_t> groupOf;     // canonical text of the key -> index into Groups
        std::vector<std::string>           firstSource; // per group: the tag of the record that opened it
        Common::Json::Issues               issues;

        for ( std::size_t index = 0; index < records.size(); ++index )
        {
            const EntityData& record = records[index];
            const std::string name   = record.Tag.value_or( std::format( "record {}", index ) );

            if ( record.PrefabPath.has_value() && !record.PrefabPath->empty() )
            {
                refusals.push_back( std::format( "{} nests the prefab '{}'", name, *record.PrefabPath ) );
                continue;
            }

            // EVERY COMPONENT THE RECORD STATES, asked of the record rather than listed here: a list of "things a
            // packed prop must not carry" is wrong the day somebody adds a component, and wrong in the direction
            // that loses data. Visibility is the one exception, and only while it says VISIBLE (the fold's rule).
            std::vector<std::string> blockers;
            for ( const auto& [key, payload] : record.Components )
            {
                if ( key == Core::Rules::kPrimitiveComponent )
                    continue;
                if ( key == Core::Rules::kVisibilityComponent &&
                     Core::Rules::Detail::AuthoredVisible( record, issues ) )
                    continue;
                blockers.push_back( std::format( "a {} component", key ) );
            }

            std::optional<InstancedStaticMeshComponentSer> key;
            if ( const auto block = Core::Rules::Detail::PayloadOf( record, Core::Rules::kPrimitiveComponent );
                 block.has_value() )
            {
                StaticMeshComponentSer mesh;
                const std::size_t      before = issues.size();
                block->ReadValue( mesh, issues );
                if ( issues.size() != before )
                    blockers.emplace_back( "a StaticMesh block that does not read" );
                else
                {
                    for ( std::string& blocker : StaticMeshPackBlockers( FactsOf( mesh ) ) )
                        blockers.push_back( std::move( blocker ) );
                    key = GroupKeyOf( mesh );
                }
            }

            if ( !blockers.empty() )
            {
                refusals.push_back( std::format( "{} carries {}", name, JoinWords( blockers, ", " ) ) );
                continue;
            }
            if ( !key.has_value() )
                continue; // draws nothing: a group node, or the root

            const std::string text    = Common::Json::Write( *key );
            const auto [where, fresh] = groupOf.emplace( text, packed.Groups.size() );
            if ( fresh )
            {
                PackedPrefabGroupSer group;
                group.Block                    = std::move( *key );
                group.Block.InstanceTransforms = std::vector<std::array<float, 16>>{};
                packed.Groups.push_back( std::move( group ) );
                firstSource.push_back( name );
            }
            packed.Groups[where->second].Block.InstanceTransforms->push_back(
                 Core::Rules::Detail::Flatten( toRoot * world[index] ) );
        }

        if ( !refusals.empty() )
        {
            packed.Groups.clear();
            packed.Refused = std::format( "{} of {} entities cannot be packed: {}", refusals.size(),
                                          records.size(), JoinWords( refusals, "; " ) );
            return packed;
        }
        if ( packed.Groups.empty() )
        {
            packed.Refused = "nothing in the prefab draws a static mesh, so there is nothing to pack";
            return packed;
        }

        // Named after the prop a person recognises, with the count, as Collapse names its ISM.
        for ( std::size_t group = 0; group < packed.Groups.size(); ++group )
            packed.Groups[group].Name = std::format( "{} x{}", firstSource[group],
                                                     packed.Groups[group].Block.InstanceTransforms->size() );
        return packed;
    }

    std::size_t PackedInstanceCount( const PackedPrefabSer& packed )
    {
        std::size_t count = 0;
        for ( const PackedPrefabGroupSer& group : packed.Groups )
            count += group.Block.InstanceTransforms.has_value() ? group.Block.InstanceTransforms->size() : 0;
        return count;
    }
} // namespace Desert::Assets
