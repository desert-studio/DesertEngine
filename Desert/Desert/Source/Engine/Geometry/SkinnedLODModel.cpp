#include "SkinnedLODModel.hpp"

#include <Engine/Geometry/MeshLOD.hpp>

#include <Common/Core/Logger.hpp>

#include <algorithm>
#include <array>
#include <unordered_map>

namespace Desert
{
    namespace
    {
        // UE's skeletal LOD reduction default shape (FSkeletalMeshOptimizationSettings::MaxBonesPerVertex): the
        // authored influences at LOD 0, two at LOD 1, one beyond - a far character's skinning is one bone a
        // vertex.
        [[nodiscard]] uint32_t LODMaxInfluences( const std::size_t level )
        {
            if ( level == 0 )
                return static_cast<uint32_t>( SkinnedVertex::MAX_BONE_INFLUENCES );
            return level == 1 ? 2u : 1u;
        }

        // @p vertex with its @p maxInfluences heaviest influences, renormalised (UE FSkinWeightInfo reduction).
        [[nodiscard]] SkinnedVertex ReduceInfluences( const SkinnedVertex& vertex, const uint32_t maxInfluences )
        {
            std::array<std::size_t, SkinnedVertex::MAX_BONE_INFLUENCES> order{};
            for ( std::size_t k = 0; k < order.size(); ++k )
                order[k] = k;
            std::stable_sort( order.begin(), order.end(), [&]( const std::size_t a, const std::size_t b )
                              { return vertex.BoneWeights[a] > vertex.BoneWeights[b]; } );
            SkinnedVertex reduced = vertex;
            reduced.BoneIDs.fill( 0u );
            reduced.BoneWeights.fill( 0.0f );
            float sum = 0.0f;
            for ( std::size_t k = 0; k < std::min<std::size_t>( maxInfluences, order.size() ); ++k )
            {
                reduced.BoneIDs[k]     = vertex.BoneIDs[order[k]];
                reduced.BoneWeights[k] = std::max( vertex.BoneWeights[order[k]], 0.0f );
                sum += reduced.BoneWeights[k];
            }
            if ( sum <= 0.0f )
                return vertex; // an unweighted vertex stays as authored
            for ( float& weight : reduced.BoneWeights )
                weight /= sum;
            return reduced;
        }

        [[nodiscard]] std::vector<uint32_t> RequiredBoneList( const std::vector<SkinnedVertex>& vertices,
                                                              const std::size_t first, const std::size_t count )
        {
            std::vector<uint32_t> bones;
            for ( std::size_t v = first; v < first + count; ++v )
                for ( std::size_t k = 0; k < SkinnedVertex::MAX_BONE_INFLUENCES; ++k )
                    if ( vertices[v].BoneWeights[k] > 0.0f )
                        bones.push_back( vertices[v].BoneIDs[k] );
            std::sort( bones.begin(), bones.end() );
            bones.erase( std::unique( bones.begin(), bones.end() ), bones.end() );
            return bones;
        }

    } // namespace

    SkinnedLODModel BuildSkinnedLODModel( const std::vector<SkinnedVertex>& vertices,
                                          const std::vector<Index>& indices, const std::vector<Submesh>& submeshes,
                                          const std::vector<MeshVertexStreams>& streams,
                                          const std::vector<MorphTarget>&       morphTargets )
    {
        SkinnedLODModel       chain{ .Vertices   = vertices,
                                     .Streams    = streams,
                                     .Morphs     = morphTargets,
                                     .GpuIndices = {},
                                     .Submeshes  = submeshes,
                                     .LODs       = {} };
        const auto            authored      = static_cast<uint32_t>( vertices.size() );
        std::vector<uint32_t> authoredBones = RequiredBoneList( vertices, 0u, vertices.size() );
        chain.LODs.push_back( { .FirstVertex   = 0u,
                                .VertexCount   = authored,
                                .RequiredBones = static_cast<uint32_t>( authoredBones.size() ),
                                .MaxInfluences = LODMaxInfluences( 0 ),
                                .Bones         = std::move( authoredBones ) } );

        std::vector<Vertex> positions;
        positions.reserve( vertices.size() );
        for ( const SkinnedVertex& vertex : vertices )
            positions.push_back( vertex.StaticVertex );
        // Where the levels come from, stated once per build: the cook's chain (the .skmesh's baked LODs, the
        // derived data) or generated here for sections no cook decided (a procedural or pre-cook mesh).
        std::size_t cookedSections = 0, cookedLevels = 0;
        for ( const Submesh& submesh : chain.Submeshes )
            if ( submesh.LodChainCooked )
            {
                ++cookedSections;
                cookedLevels = std::max( cookedLevels, submesh.BakedLODs.size() );
            }
        chain.GpuIndices =
             Geometry::BuildLODIndexBuffer( positions, indices, chain.Submeshes, Geometry::UnbakedLODChain() );
        if ( cookedSections == chain.Submeshes.size() )
        {
            LOG_INFO( "[SkinnedLOD] {} vertices: LOD chain loaded from the cooked derived data ({} section(s), "
                      "{} generated level(s) + LOD 0); nothing reduced at load",
                      vertices.size(), cookedSections, cookedLevels );
        }
        else
        {
            LOG_INFO( "[SkinnedLOD] {} vertices: {} of {} section(s) have no cooked chain - their LODs were "
                      "GENERATED AT LOAD (re-import the mesh to cook them)",
                      vertices.size(), chain.Submeshes.size() - cookedSections, chain.Submeshes.size() );
        }
        for ( Submesh& submesh : chain.Submeshes )
            std::vector<std::vector<Index>>().swap( submesh.BakedLODs );

        std::size_t levels = 1;
        for ( const Submesh& submesh : chain.Submeshes )
            levels = std::max( levels, submesh.LODs.size() );
        if ( levels <= 1 || vertices.empty() )
            return chain;

        // Morph arrays padded to the authored count (the asset may stop short: a zero delta), so an appended
        // vertex's delta is its source's.
        for ( MorphTarget& target : chain.Morphs )
        {
            target.DeltaPositions.resize( vertices.size(), glm::vec3( 0.0f ) );
            if ( !target.DeltaNormals.empty() )
                target.DeltaNormals.resize( vertices.size(), glm::vec3( 0.0f ) );
        }
        // The levels as BuildLODIndexBuffer wrote them (over the authored vertices): the source of every copy.
        const std::vector<Index>               source       = chain.GpuIndices;
        const std::vector<Submesh>             sourceRanges = chain.Submeshes;
        std::unordered_map<uint32_t, uint32_t> copies;
        for ( std::size_t level = 1; level < levels; ++level )
        {
            const uint32_t maxInfluences = LODMaxInfluences( level );
            const auto     first         = static_cast<uint32_t>( chain.Vertices.size() );
            for ( std::size_t s = 0; s < chain.Submeshes.size(); ++s )
            {
                Submesh&       submesh = chain.Submeshes[s];
                const Submesh& from    = sourceRanges[s];
                if ( from.LODs.empty() )
                    continue;
                const LODRange read = from.LODs[std::min( level, from.LODs.size() - 1u )];
                // A level the submesh lacks gets its own triangles at the end of the buffer.
                if ( level >= from.LODs.size() )
                {
                    submesh.LODs.push_back(
                         { static_cast<uint32_t>( chain.GpuIndices.size() * 3u ), read.IndexCount } );
                    chain.GpuIndices.insert( chain.GpuIndices.end(), read.IndexCount / 3u, Index{ 0u, 0u, 0u } );
                }
                const LODRange write = submesh.LODs[level];
                copies.clear();
                const auto copyOf = [&]( const uint32_t local ) -> uint32_t
                {
                    const auto [found, fresh] = copies.try_emplace( local, 0u );
                    if ( fresh )
                    {
                        const std::size_t at = static_cast<std::size_t>( from.VertexOffset ) + local;
                        found->second        = static_cast<uint32_t>( chain.Vertices.size() ) - from.VertexOffset;
                        chain.Vertices.push_back( ReduceInfluences( vertices[at], maxInfluences ) );
                        if ( !streams.empty() )
                            chain.Streams.push_back( streams[std::min( at, streams.size() - 1u )] );
                        for ( MorphTarget& target : chain.Morphs )
                        {
                            target.DeltaPositions.push_back( target.DeltaPositions[at] );
                            if ( !target.DeltaNormals.empty() )
                                target.DeltaNormals.push_back( target.DeltaNormals[at] );
                        }
                    }
                    return found->second;
                };
                for ( uint32_t t = 0; t < read.IndexCount / 3u; ++t )
                {
                    const Index& tri = source[read.IndexOffset / 3u + t];
                    chain.GpuIndices[write.IndexOffset / 3u + t] =
                         Index{ copyOf( tri.V1 ), copyOf( tri.V2 ), copyOf( tri.V3 ) };
                }
            }
            const auto            count      = static_cast<uint32_t>( chain.Vertices.size() ) - first;
            std::vector<uint32_t> levelBones = RequiredBoneList( chain.Vertices, first, count );
            chain.LODs.push_back( { .FirstVertex   = first,
                                    .VertexCount   = count,
                                    .RequiredBones = static_cast<uint32_t>( levelBones.size() ),
                                    .MaxInfluences = maxInfluences,
                                    .Bones         = std::move( levelBones ) } );
        }
        return chain;
    }
} // namespace Desert
