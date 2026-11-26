#include <Engine/Core/Serialize/WorldPartitionHLODMember.hpp>

#include <algorithm>
#include <format>
#include <map>
#include <utility>

namespace Desert::Core::Rules
{
    namespace
    {
        // The asset key the checksum hashes for one {Guid, Path} reference: the GUID text when stated, else the
        // path. Empty when neither is.
        void AddContentKey( std::vector<std::string>& content, const std::string& guid, const std::string& path )
        {
            if ( !guid.empty() )
                content.push_back( guid );
            else if ( !path.empty() )
                content.push_back( path );
        }

        void AddKeyContent( std::vector<std::string>& content, const Assets::InstancedStaticMeshComponentSer& key )
        {
            AddContentKey( content, key.MeshGuid.value_or( std::string{} ),
                           key.MeshPath.value_or( std::string{} ) );
            const std::size_t guids = key.MaterialGuids.has_value() ? key.MaterialGuids->size() : 0;
            const std::size_t paths = key.MaterialPaths.has_value() ? key.MaterialPaths->size() : 0;
            for ( std::size_t slot = 0; slot < std::max( guids, paths ); ++slot )
                AddContentKey( content, slot < guids ? ( *key.MaterialGuids )[slot] : std::string{},
                               slot < paths ? ( *key.MaterialPaths )[slot] : std::string{} );
        }
    } // namespace

    Common::ResultStr<HLODMember> ReadHLODMember( std::span<const Assets::EntityData> records,
                                                  std::span<const glm::mat4> world, std::size_t index,
                                                  const HLODMemberSources& sources )
    {
        (void)world; // the record's world matrix is the instance of a StaticMesh batch (Instances absent)
        const Assets::EntityData& record = records[index];
        HLODMember                member;
        const auto                done = [&]() -> Common::ResultStr<HLODMember>
        {
            std::sort( member.Content.begin(), member.Content.end() );
            member.Content.erase( std::unique( member.Content.begin(), member.Content.end() ),
                                  member.Content.end() );
            member.Relevant = !member.Batches.empty() || !member.Holes.empty();
            return Common::MakeSuccess( std::move( member ) );
        };

        if ( !Detail::AuthoredVisible( record, member.Issues ) )
            return done();

        // FO-6: a foliage field whose type is left out of the HLOD (UE bIncludeInHLOD false).
        if ( const auto type = Detail::PayloadOf( record, kFoliageComponent );
             type.has_value() && sources.Foliage )
        {
            std::string guid;
            std::string path;
            type->ReadInto( "FoliageTypeGuid", guid, member.Issues );
            type->ReadInto( "FoliageTypePath", path, member.Issues );
            AddContentKey( member.Content, guid, path );
            const auto included = sources.Foliage( guid, path );
            if ( !included )
            {
                member.Holes.push_back( HLODExclusion::Unreadable );
                return done();
            }
            if ( !included.GetValue() )
            {
                member.FoliageLeftOut = true;
                return done();
            }
        }

        // A prefab instance never reaches here: the cook, Build HLODs, the map check and Play all read the world
        // ExpandPrefabInstances made (WP21, WP21-PLAY). One that does is a caller that skipped the expansion.
        if ( record.PrefabPath.has_value() && !record.PrefabPath->empty() )
            return Common::MakeError<HLODMember>( "an unexpanded instance of '" + *record.PrefabPath + "'" );

        // A record its module's own HLOD builder draws (HLODBuilderComponents: a tile's terrain from its height
        // file, a spline's meshes bent along it), not a mesh block.
        for ( const HLODBuilderComponent& row : HLODBuilderComponents() )
            if ( Detail::PayloadOf( record, row.Component ).has_value() )
            {
                member.OwnBuilder = &row;
                member.Holes.push_back( row.Reason );
                return done();
            }
        if ( Detail::PayloadOf( record, kSkinnedMeshComponent ).has_value() )
            member.Holes.push_back( HLODExclusion::SkinnedMesh );

        if ( const auto block = Detail::PayloadOf( record, kPrimitiveComponent ); block.has_value() )
        {
            Assets::StaticMeshComponentSer mesh;
            const std::size_t              before = member.Issues.size();
            block->ReadValue( mesh, member.Issues );
            if ( member.Issues.size() != before )
                member.Holes.push_back( HLODExclusion::Unreadable );
            else if ( mesh.EditMesh.has_value() )
                member.Holes.push_back( HLODExclusion::EditorMesh );
            else if ( mesh.HiddenSubmeshes.value_or( 0 ) != 0 )
                member.Holes.push_back( HLODExclusion::HiddenSubmeshes );
            else if ( ( mesh.MeshHandle.has_value() &&
                        ( !mesh.MeshHandle->Guid.empty() || !mesh.MeshHandle->Path.empty() ) ) ||
                      mesh.Primitive.has_value() )
            {
                // The Static Mesh states {Guid, Path} pairs (asset references v1); the instanced key keeps its
                // own GUID / path fields.
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
                key.Primitive = mesh.Primitive;
                // Absent = true on both sides; written only when false, as the registry writes it.
                if ( !mesh.CastShadows.value_or( true ) )
                    key.CastShadows = false;
                AddKeyContent( member.Content, key );
                if ( Detail::DrawsOnlyWithCustomShaders( key, sources.CustomShader ) )
                    member.Holes.push_back( HLODExclusion::CustomShaderMaterial );
                else
                    member.Batches.push_back( HLODMemberBatch{ std::move( key ), std::nullopt } );
            }
        }

        if ( const auto block = Detail::PayloadOf( record, kInstancePointsComponent ); block.has_value() )
        {
            Assets::InstancedStaticMeshComponentSer ism;
            const std::size_t                       before = member.Issues.size();
            block->ReadValue( ism, member.Issues );
            if ( member.Issues.size() != before )
            {
                member.Holes.push_back( HLODExclusion::Unreadable );
                return done();
            }
            if ( !ism.MeshGuid.has_value() && !ism.MeshPath.has_value() && !ism.Primitive.has_value() )
                return done();
            std::vector<std::array<float, 16>> instances =
                 ism.InstanceTransforms.value_or( std::vector<std::array<float, 16>>{} );
            ism.InstanceTransforms.reset();
            if ( ism.CastShadows.value_or( true ) )
                ism.CastShadows.reset();
            AddKeyContent( member.Content, ism );
            if ( Detail::DrawsOnlyWithCustomShaders( ism, sources.CustomShader ) )
            {
                member.Holes.push_back( HLODExclusion::CustomShaderMaterial );
                return done();
            }
            if ( !instances.empty() )
                member.Batches.push_back( HLODMemberBatch{ std::move( ism ), std::move( instances ) } );
        }
        return done();
    }

    InstancingHLOD BuildInstancingHLOD( std::span<const Assets::EntityData> records,
                                        std::span<const glm::mat4> world, std::span<const std::size_t> members,
                                        const CustomShaderSource& customShader, Common::Json::Issues& issues,
                                        const FoliageHLODSource& foliage )
    {
        InstancingHLOD                     hlod;
        std::map<std::string, std::size_t> batchOf; // canonical text of the batch key -> index into Batches
        const HLODMemberSources            sources{ customShader, foliage };

        for ( const std::size_t index : members )
        {
            auto read = ReadHLODMember( records, world, index, sources );
            if ( !read )
            {
                const Assets::EntityData& record = records[index];
                issues.push_back( Common::Json::Issue{
                     record.id.has_value()
                          ? std::format( "Entities[id={}]", static_cast<std::uint64_t>( *record.id ) )
                          : std::format( "Entities[{}]", index ),
                     "a record of an expanded world (ExpandPrefabInstances)", read.GetError() } );
                continue;
            }
            HLODMember member = read.ExtractValue();
            issues.insert( issues.end(), member.Issues.begin(), member.Issues.end() );
            if ( member.FoliageLeftOut )
                hlod.FoliageLeftOut++;
            for ( const HLODExclusion hole : member.Holes )
                hlod.NotInstanced.push_back( { index, hole } );
            for ( HLODMemberBatch& batch : member.Batches )
            {
                std::vector<std::array<float, 16>> instances =
                     batch.Instances.has_value()
                          ? std::move( *batch.Instances )
                          : std::vector<std::array<float, 16>>{ Detail::Flatten( world[index] ) };
                const std::string text    = Common::Json::Write( batch.Key );
                const auto [where, fresh] = batchOf.emplace( text, hlod.Batches.size() );
                if ( fresh )
                {
                    HLODBatch built;
                    built.Block                    = std::move( batch.Key );
                    built.Block.InstanceTransforms = std::vector<std::array<float, 16>>{};
                    hlod.Batches.push_back( std::move( built ) );
                }
                HLODBatch& into = hlod.Batches[where->second];
                if ( into.Sources.empty() || into.Sources.back() != index )
                    into.Sources.push_back( index );
                hlod.Instances += instances.size();
                into.Block.InstanceTransforms->insert( into.Block.InstanceTransforms->end(), instances.begin(),
                                                       instances.end() );
            }
        }
        return hlod;
    }
} // namespace Desert::Core::Rules
