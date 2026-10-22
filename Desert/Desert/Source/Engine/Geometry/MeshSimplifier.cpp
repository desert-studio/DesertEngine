#include "MeshSimplifier.hpp"

#include <meshoptimizer.h>

#include <algorithm>
#include <unordered_map>

namespace Desert::Geometry
{
    namespace
    {
        // The topology census a stalled reduction reports, over POSITION-welded edges (a normal/UV seam is
        // not a border): edges used by one triangle (open) and by three or more (non-manifold).
        std::string EdgeCensus( const std::vector<uint32_t>& unwelded, const float* positions,
                                std::size_t vertexCount )
        {
            std::vector<uint32_t> indices( unwelded.size() );
            meshopt_generateShadowIndexBuffer( indices.data(), unwelded.data(), unwelded.size(), positions,
                                               vertexCount, sizeof( float ) * 3, sizeof( float ) * 3 );
            std::unordered_map<uint64_t, uint32_t> uses;
            uses.reserve( indices.size() );
            for ( std::size_t i = 0; i + 2 < indices.size(); i += 3 )
                for ( int k = 0; k < 3; ++k )
                {
                    const uint32_t a = indices[i + k], b = indices[i + ( k + 1 ) % 3];
                    ++uses[( uint64_t( std::min( a, b ) ) << 32 ) | std::max( a, b )];
                }
            std::size_t border = 0, nonManifold = 0;
            for ( const auto& [edge, n] : uses )
            {
                border += n == 1;
                nonManifold += n > 2;
            }
            return std::to_string( uses.size() ) + " edges, " + std::to_string( border ) + " open, " +
                   std::to_string( nonManifold ) + " non-manifold";
        }

        // A UV chart border is a real discontinuity of the texture, a hard normal is not: corners of one
        // position that disagree on UV are tagged Protect, so Permissive still crosses normal seams but never
        // collapses a chart border into a neighbouring chart (a triangle spanning two charts samples whatever
        // lies between them in the atlas). UE's QEM keeps UV seams the same way: wedges of different UVs are
        // not merged, they slide along the seam.
        std::vector<unsigned char> ProtectUVSeams( const float* positions, std::size_t vertexCount,
                                                   const float* attributes )
        {
            static constexpr std::size_t kU = 3, kV = 4;
            std::vector<unsigned int>    remap( vertexCount );
            meshopt_generatePositionRemap( remap.data(), positions, vertexCount, sizeof( float ) * 3 );
            std::vector<unsigned char> flags( vertexCount, 0 );
            for ( std::size_t v = 0; v < vertexCount; ++v )
            {
                const std::size_t r = remap[v];
                if ( r == v )
                    continue;
                const float* a = attributes + v * kSimplifyAttributeCount;
                const float* b = attributes + r * kSimplifyAttributeCount;
                if ( a[kU] != b[kU] || a[kV] != b[kV] )
                    flags[v] = flags[r] = meshopt_SimplifyVertex_Protect;
            }
            // A third corner can disagree with the second but agree with the representative: tag every
            // corner of a position whose representative is tagged.
            for ( std::size_t v = 0; v < vertexCount; ++v )
                flags[v] |= flags[remap[v]];
            return flags;
        }
    } // namespace

    SimplifyResult SimplifyMesh( const float* positions, std::size_t vertexCount,
                                 const std::vector<uint32_t>& indices, float targetRatio, float targetError,
                                 const float* attributes )
    {
        SimplifyResult out;
        if ( !positions || vertexCount == 0 || indices.size() < 3 )
        {
            out.Indices = indices;
            return out;
        }

        const float       ratio            = std::clamp( targetRatio, 0.0f, 1.0f );
        const std::size_t targetIndexCount = static_cast<std::size_t>( indices.size() * ratio ) / 3 * 3;

        // Normal seams (same position and UV, different normal) are not barriers: Permissive lets a collapse
        // cross them and keeps each surviving corner on a wedge of its own side, as UE's QEM collapses
        // positions and lets the wedges ride along. UV chart borders stay protected (ProtectUVSeams). Prune removes a disconnected part whole once it
        // is the cheapest thing left, so a mesh of many small shells still reaches its triangle target.
        static constexpr unsigned kOptions = meshopt_SimplifyPermissive | meshopt_SimplifyPrune;

        std::vector<uint32_t> lod( indices.size() );
        float                 error = 0.0f;
        std::size_t           count = 0;
        if ( attributes )
        {
            // Normals live in [-1, 1], UVs mostly in [0, 1]: inside meshopt's recommended weight range,
            // normals weigh a little more - a lit silhouette shows a bent normal before a slid UV.
            static constexpr float kWeights[kSimplifyAttributeCount] = { 0.08f, 0.08f, 0.08f, 0.04f, 0.04f };
            const std::vector<unsigned char> protect = ProtectUVSeams( positions, vertexCount, attributes );
            count = meshopt_simplifyWithAttributes( lod.data(), indices.data(), indices.size(), positions,
                                                    vertexCount, sizeof( float ) * 3, attributes,
                                                    sizeof( float ) * kSimplifyAttributeCount, kWeights,
                                                    kSimplifyAttributeCount, protect.data(),
                                                    targetIndexCount, targetError, kOptions, &error );
        }
        else
        {
            count = meshopt_simplify( lod.data(), indices.data(), indices.size(), positions, vertexCount,
                                      sizeof( float ) * 3, targetIndexCount, targetError, kOptions, &error );
        }
        lod.resize( count );

        if ( count > targetIndexCount )
            out.StopReason = error >= targetError ? std::string( "error limit reached" )
                                                  : "no legal collapse left; source: " +
                                                        EdgeCensus( indices, positions, vertexCount ) +
                                                        "; result: " + EdgeCensus( lod, positions, vertexCount );

        out.Indices = std::move( lod );
        out.Error   = error;
        return out;
    }
} // namespace Desert::Geometry
