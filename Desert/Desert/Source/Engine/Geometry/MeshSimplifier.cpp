#include "MeshSimplifier.hpp"

#include <meshoptimizer.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <unordered_map>

namespace Desert::Geometry
{
    namespace
    {
        // The original vertices ("wedges") that share one welded position, CSR-packed per canonical vertex,
        // plus the chart each wedge lives in: wedges joined by an ORIGINAL triangle share a chart, so an
        // unwelded normal or UV split separates two charts.
        struct WedgeTable
        {
            std::vector<uint32_t> Offsets; // canonical -> [Offsets[c], Offsets[c + 1]) into Wedges
            std::vector<uint32_t> Wedges;
            std::vector<uint32_t> Chart; // per original vertex
        };

        uint32_t FindRoot( std::vector<uint32_t>& parent, uint32_t v )
        {
            while ( parent[v] != v )
            {
                parent[v] = parent[parent[v]];
                v         = parent[v];
            }
            return v;
        }

        WedgeTable BuildWedges( const std::vector<uint32_t>& indices, const std::vector<uint32_t>& welded,
                                std::size_t vertexCount )
        {
            WedgeTable t;
            t.Chart.resize( vertexCount );
            std::iota( t.Chart.begin(), t.Chart.end(), 0u );
            for ( std::size_t i = 0; i + 2 < indices.size(); i += 3 )
            {
                const uint32_t a = FindRoot( t.Chart, indices[i] );
                const uint32_t b = FindRoot( t.Chart, indices[i + 1] );
                t.Chart[b]       = a;
                const uint32_t c = FindRoot( t.Chart, indices[i + 2] );
                t.Chart[c]       = a;
            }
            for ( uint32_t v = 0; v < vertexCount; ++v )
                t.Chart[v] = FindRoot( t.Chart, v );

            constexpr uint32_t    kUnseen = std::numeric_limits<uint32_t>::max();
            std::vector<uint32_t> canonicalOf( vertexCount, kUnseen );
            t.Offsets.assign( vertexCount + 1, 0u );
            for ( std::size_t i = 0; i < indices.size(); ++i )
                if ( canonicalOf[indices[i]] == kUnseen )
                {
                    canonicalOf[indices[i]] = welded[i];
                    ++t.Offsets[welded[i] + 1];
                }
            for ( std::size_t c = 0; c < vertexCount; ++c )
                t.Offsets[c + 1] += t.Offsets[c];
            t.Wedges.resize( t.Offsets[vertexCount] );
            std::vector<uint32_t> fill( t.Offsets.begin(), t.Offsets.end() - 1 );
            for ( uint32_t v = 0; v < vertexCount; ++v )
                if ( canonicalOf[v] != kUnseen )
                    t.Wedges[fill[canonicalOf[v]]++] = v;
            return t;
        }

        // Gives every corner of a welded triangle back an original wedge: the triangle picks the chart that
        // fits its three corners best (a corner outside it costs a seam), and inside the chart each corner
        // takes the wedge whose normal is closest to the face - so a hard edge keeps its hard normals.
        void RestoreWedges( std::vector<uint32_t>& tris, const WedgeTable& t, const float* positions,
                            const float* attributes )
        {
            constexpr float kSeamCost = 4.0f; // > the worst normal cost (2): staying in one chart wins
            for ( std::size_t i = 0; i + 2 < tris.size(); i += 3 )
            {
                const uint32_t c[3]    = { tris[i], tris[i + 1], tris[i + 2] };
                float          face[3] = { 0.0f, 0.0f, 0.0f };
                {
                    const float* p0    = positions + std::size_t( c[0] ) * 3;
                    const float* p1    = positions + std::size_t( c[1] ) * 3;
                    const float* p2    = positions + std::size_t( c[2] ) * 3;
                    const float  e1[3] = { p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2] };
                    const float  e2[3] = { p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2] };
                    face[0]            = e1[1] * e2[2] - e1[2] * e2[1];
                    face[1]            = e1[2] * e2[0] - e1[0] * e2[2];
                    face[2]            = e1[0] * e2[1] - e1[1] * e2[0];
                    const float len    = std::sqrt( face[0] * face[0] + face[1] * face[1] + face[2] * face[2] );
                    for ( float& f : face )
                        f = len > 0.0f ? f / len : 0.0f;
                }
                auto normalCost = [&]( uint32_t w )
                {
                    if ( !attributes )
                        return 0.0f;
                    const float* n = attributes + std::size_t( w ) * kSimplifyAttributeCount;
                    return 1.0f - ( n[0] * face[0] + n[1] * face[1] + n[2] * face[2] );
                };
                // The best wedge of corner k for a chart, and its cost.
                auto bestIn = [&]( int k, uint32_t chart, float& cost )
                {
                    uint32_t best = c[k];
                    cost          = std::numeric_limits<float>::max();
                    for ( uint32_t j = t.Offsets[c[k]]; j < t.Offsets[c[k] + 1]; ++j )
                    {
                        const uint32_t w  = t.Wedges[j];
                        const float    wc = normalCost( w ) + ( t.Chart[w] == chart ? 0.0f : kSeamCost );
                        if ( wc < cost )
                        {
                            cost = wc;
                            best = w;
                        }
                    }
                    return best;
                };

                uint32_t chosen[3] = { c[0], c[1], c[2] };
                float    bestTotal = std::numeric_limits<float>::max();
                for ( int k = 0; k < 3; ++k )
                    for ( uint32_t j = t.Offsets[c[k]]; j < t.Offsets[c[k] + 1]; ++j )
                    {
                        const uint32_t chart = t.Chart[t.Wedges[j]];
                        float          total = 0.0f;
                        uint32_t       pick[3];
                        for ( int m = 0; m < 3; ++m )
                        {
                            float cost = 0.0f;
                            pick[m]    = bestIn( m, chart, cost );
                            total += cost;
                        }
                        if ( total < bestTotal )
                        {
                            bestTotal = total;
                            std::copy( pick, pick + 3, chosen );
                        }
                    }
                std::copy( chosen, chosen + 3, tris.begin() + i );
            }
        }
        // The topology census a stalled reduction reports: edges used by one triangle (open borders, which
        // meshopt may only slide along) and by three or more (non-manifold, whose vertices it locks).
        std::string EdgeCensus( const std::vector<uint32_t>& indices )
        {
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

        // Weld by position: wedges split at a normal/UV seam become one vertex, so the collapse sees the
        // real surface and is not walled in by the split (meshopt's shadow index buffer).
        std::vector<uint32_t> welded( indices.size() );
        meshopt_generateShadowIndexBuffer( welded.data(), indices.data(), indices.size(), positions, vertexCount,
                                           sizeof( float ) * 3, sizeof( float ) * 3 );

        std::vector<uint32_t> lod( indices.size() );
        float                 error = 0.0f;
        std::size_t           count = 0;
        if ( attributes )
        {
            // Normals live in [-1, 1], UVs mostly in [0, 1]: inside meshopt's recommended weight range,
            // normals weigh a little more - a lit silhouette shows a bent normal before a slid UV. On the
            // welded topology the metric reads each position's first wedge.
            static constexpr float kWeights[kSimplifyAttributeCount] = { 0.08f, 0.08f, 0.08f, 0.04f, 0.04f };
            count = meshopt_simplifyWithAttributes( lod.data(), welded.data(), welded.size(), positions,
                                                    vertexCount, sizeof( float ) * 3, attributes,
                                                    sizeof( float ) * kSimplifyAttributeCount, kWeights,
                                                    kSimplifyAttributeCount, targetIndexCount, targetError,
                                                    /*options*/ 0, &error );
        }
        else
        {
            count = meshopt_simplify( lod.data(), welded.data(), welded.size(), positions, vertexCount,
                                      sizeof( float ) * 3, targetIndexCount, targetError, /*options*/ 0, &error );
        }
        lod.resize( count );

        if ( count > targetIndexCount )
            out.StopReason = error >= targetError
                                  ? std::string( "error limit reached" )
                                  : "no legal collapse left (meshopt locks non-manifold vertices and only slides "
                                    "open borders); welded source: " +
                                         EdgeCensus( welded ) + "; unwelded source: " + EdgeCensus( indices ) +
                                         "; result: " + EdgeCensus( lod );

        RestoreWedges( lod, BuildWedges( indices, welded, vertexCount ), positions, attributes );

        out.Indices = std::move( lod );
        out.Error   = error;
        return out;
    }
} // namespace Desert::Geometry
