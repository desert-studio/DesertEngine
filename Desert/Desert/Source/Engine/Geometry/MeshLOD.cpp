#include "MeshLOD.hpp"

#include "MeshSimplifier.hpp"

#include <cstdint>

namespace Desert::Geometry
{
    namespace
    {
        // Ratios for the auto LOD chain (LOD0 = original). A level that cannot beat the previous one yields an
        // empty set so the assembler reuses that range, keeping LOD selection monotonic.
        constexpr float kLODRatios[] = { 0.5f, 0.25f, 0.1f };
    } // namespace

    std::vector<std::vector<Index>> SimplifyLODLevels( const float* positions, uint32_t vertexCount,
                                                       const std::vector<Index>& localTris )
    {
        std::vector<std::vector<Index>> levels;
        levels.reserve( sizeof( kLODRatios ) / sizeof( kLODRatios[0] ) );

        std::vector<uint32_t> flat;
        flat.reserve( localTris.size() * 3 );
        for ( const auto& t : localTris )
        {
            flat.push_back( t.V1 );
            flat.push_back( t.V2 );
            flat.push_back( t.V3 );
        }

        std::size_t prevCount = flat.size(); // each level must strictly decrease
        for ( float ratio : kLODRatios )
        {
            const auto         lod = SimplifyMesh( positions, vertexCount, flat, ratio );
            std::vector<Index> tris;
            if ( lod.Indices.size() >= 3 && lod.Indices.size() < prevCount )
            {
                tris.reserve( lod.Indices.size() / 3 );
                for ( std::size_t i = 0; i + 2 < lod.Indices.size(); i += 3 )
                    tris.push_back( { lod.Indices[i], lod.Indices[i + 1], lod.Indices[i + 2] } );
                prevCount = lod.Indices.size();
            }
            levels.push_back( std::move( tris ) ); // empty => reuse previous at assembly
        }
        return levels;
    }

    std::vector<Index> BuildLODIndexBuffer( const std::vector<Vertex>& vertices,
                                            const std::vector<Index>&  baseIndices,
                                            std::vector<Submesh>&      submeshes )
    {
        std::vector<Index> gpuIndices = baseIndices;

        for ( auto& sm : submeshes )
        {
            sm.LODs.clear();
            sm.LODs.push_back( { sm.IndexOffset, sm.IndexCount } ); // LOD0 = original

            const uint32_t triCount = sm.IndexCount / 3;
            if ( triCount < 8 || sm.VertexCount == 0 )
                continue; // too small to bother

            // Cooked LODs win; otherwise generate them now (unchanged pre-baking behaviour). Both are
            // submesh-local triangles (indices relative to VertexOffset -> same baseVertex).
            std::vector<std::vector<Index>> levels;
            if ( !sm.BakedLODs.empty() )
            {
                levels = sm.BakedLODs;
            }
            else
            {
                std::vector<Index> localTris;
                localTris.reserve( triCount );
                const uint32_t triStart = sm.IndexOffset / 3;
                for ( uint32_t t = 0; t < triCount; ++t )
                    localTris.push_back( baseIndices[triStart + t] );

                std::vector<float> pos;
                pos.reserve( static_cast<size_t>( sm.VertexCount ) * 3 );
                for ( uint32_t v = 0; v < sm.VertexCount; ++v )
                {
                    const auto& p = vertices[sm.VertexOffset + v].Position;
                    pos.push_back( p.x );
                    pos.push_back( p.y );
                    pos.push_back( p.z );
                }
                levels = SimplifyLODLevels( pos.data(), sm.VertexCount, localTris );
            }

            for ( const auto& lvl : levels )
            {
                const uint32_t lvlIndexCount = static_cast<uint32_t>( lvl.size() * 3 );
                if ( lvl.empty() || lvlIndexCount >= sm.LODs.back().IndexCount )
                {
                    sm.LODs.push_back( sm.LODs.back() ); // no improvement -> reuse previous (monotonic)
                    continue;
                }
                const uint32_t offset = static_cast<uint32_t>( gpuIndices.size() * 3 );
                for ( const auto& tri : lvl )
                    gpuIndices.push_back( tri );
                sm.LODs.push_back( { offset, lvlIndexCount } );
            }
        }

        return gpuIndices;
    }
} // namespace Desert::Geometry
