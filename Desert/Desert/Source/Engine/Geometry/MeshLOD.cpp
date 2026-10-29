#include "MeshLOD.hpp"

#include "MeshSimplifier.hpp"

#include <cstdint>

namespace Desert::Geometry
{
    std::vector<std::vector<Index>> SimplifyLODLevels( const float* positions, uint32_t vertexCount,
                                                       const std::vector<Index>& localTris,
                                                       const float*              attributes )
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
            const auto lod = SimplifyMesh( positions, vertexCount, flat, ratio, kNoErrorLimit, attributes );
            LOG_INFO( "[MeshLOD] ratio {:.2f}: target {} tris, result {} of {} tris, error {:.4f}{}", ratio,
                      static_cast<std::size_t>( flat.size() * ratio ) / 3, lod.Indices.size() / 3, flat.size() / 3,
                      lod.Error, attributes ? "" : " (positions only)" );
            if ( !lod.StopReason.empty() )
                LOG_WARN( "[MeshLOD] ratio {:.2f} stopped short of its target: {}", ratio, lod.StopReason );
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

    void AppendSimplifyAttributes( const glm::vec3& normal, const glm::vec2& uv, std::vector<float>& out )
    {
        out.push_back( normal.x );
        out.push_back( normal.y );
        out.push_back( normal.z );
        out.push_back( uv.x );
        out.push_back( uv.y );
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
                std::vector<float> attrs;
                pos.reserve( static_cast<size_t>( sm.VertexCount ) * 3 );
                attrs.reserve( static_cast<size_t>( sm.VertexCount ) * kSimplifyAttributeCount );
                for ( uint32_t v = 0; v < sm.VertexCount; ++v )
                {
                    const auto& vertex = vertices[sm.VertexOffset + v];
                    pos.push_back( vertex.Position.x );
                    pos.push_back( vertex.Position.y );
                    pos.push_back( vertex.Position.z );
                    AppendSimplifyAttributes( vertex.Normal, vertex.TexCoord, attrs );
                }
                levels = SimplifyLODLevels( pos.data(), sm.VertexCount, localTris, attrs.data() );
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
