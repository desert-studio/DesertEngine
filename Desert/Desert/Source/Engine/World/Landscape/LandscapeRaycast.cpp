#include <Engine/World/Landscape/LandscapeRaycast.hpp>

#include <Common/Core/GlslAsCpp.hpp>
#include <Common/Core/Logger.hpp>

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

namespace Desert::World::Landscape
{
    namespace
    {
        // Shaders/Common/LandscapeHeight.glslh COMPILED AS C++, for LandscapeInUpperTriangle: the ray picks
        // a cell's triangle by the rule the shader and SampleLandscapeHeight pick it by.
        using glm::floor;
        using glm::min;

        DESERT_GLSL_AS_CPP_BEGIN // see the header: GLSL has no `inline`, so these are statics
#include <Common/LandscapeHeight.glslh>
             DESERT_GLSL_AS_CPP_END

             // The ray in one tile's grid space: g(t) = G0 + U·t, in samples; Y relative to the tile's base.
             struct GridRay
        {
            double Gx0 = 0.0;
            double Gz0 = 0.0;
            double Ux  = 0.0;
            double Uz  = 0.0;
            double Y0  = 0.0;
            double Dy  = 0.0;
        };

        // Narrows [tMin, tMax] to where p0 + u·t lies in [lo, hi]. False when that is empty. A ray parallel
        // to the slab keeps the interval only if it runs inside it, edges included.
        bool ClipSlab( double p0, double u, double lo, double hi, double& tMin, double& tMax )
        {
            if ( u == 0.0 )
                return p0 >= lo && p0 <= hi;
            double a = ( lo - p0 ) / u;
            double b = ( hi - p0 ) / u;
            if ( a > b )
                std::swap( a, b );
            tMin = std::max( tMin, a );
            tMax = std::min( tMax, b );
            return tMin <= tMax;
        }

        // Every t in (tMin, tMax) where p0 + u·t crosses an integer grid line.
        void AppendCrossings( double p0, double u, double tMin, double tMax, std::vector<double>& out )
        {
            if ( u == 0.0 )
                return;
            const double a     = p0 + u * tMin;
            const double b     = p0 + u * tMax;
            const double first = std::ceil( std::min( a, b ) );
            const double last  = std::floor( std::max( a, b ) );
            for ( double line = first; line <= last; line += 1.0 )
            {
                const double t = ( line - p0 ) / u;
                if ( t > tMin && t < tMax )
                    out.push_back( t );
            }
        }

        // The root of c + b·s in [lo, hi], if there is one. b == 0 is a ray running parallel to the
        // triangle: it hits only if it lies in it, and then at the start of the piece.
        std::optional<double> LinearRoot( double b, double c, double lo, double hi )
        {
            if ( b == 0.0 )
                return c == 0.0 ? std::optional<double>( lo ) : std::nullopt;
            const double s = -c / b;
            return ( s >= lo && s <= hi ) ? std::optional<double>( s ) : std::nullopt;
        }

        struct TileHit
        {
            double   T     = 0.0;
            double   Fx    = 0.0; // within the cell, [0, 1]
            double   Fz    = 0.0;
            double   DhDfx = 0.0; // the hit triangle's slope, cm per cell
            double   DhDfz = 0.0;
            uint32_t CellX = 0u;
            uint32_t CellZ = 0u;
        };

        std::optional<TileHit> RaycastTile( const LandscapeTileData& tile, const LandscapeFrame& frame,
                                            const GridRay& ray, double maxDistance )
        {
            const uint32_t sx = tile.SamplesX();
            const uint32_t sz = tile.SamplesZ();
            if ( sx < kLandscapeMinTileSamples || sz < kLandscapeMinTileSamples )
                return std::nullopt;
            const double lastX = static_cast<double>( sx - 1u );
            const double lastZ = static_cast<double>( sz - 1u );

            const auto h = [&]( uint32_t x, uint32_t z ) -> double
            { return static_cast<double>( LandscapeHeightCm( tile.Sample( x, z ), frame.ZScale ) ); };

            // The vertical slab bounds the traversal to where the ray is between the tile's lowest and
            // highest sample: a cell's triangles never leave the range of its four corners.
            const auto [lowIt, highIt] = std::minmax_element( tile.Samples().begin(), tile.Samples().end() );
            const double low           = static_cast<double>( LandscapeHeightCm( *lowIt, frame.ZScale ) );
            const double high          = static_cast<double>( LandscapeHeightCm( *highIt, frame.ZScale ) );

            double tMin = 0.0;
            double tMax = maxDistance;
            if ( !ClipSlab( ray.Gx0, ray.Ux, 0.0, lastX, tMin, tMax ) ||
                 !ClipSlab( ray.Gz0, ray.Uz, 0.0, lastZ, tMin, tMax ) ||
                 !ClipSlab( ray.Y0, ray.Dy, low, high, tMin, tMax ) )
                return std::nullopt;

            // The segments between consecutive grid-line crossings, in ray order: each lies in exactly one
            // cell, found from its midpoint so a crossing that rounds onto the wrong side of a line cannot
            // hand the segment to a neighbour. The first root found is the nearest in this tile.
            std::vector<double> ts{ tMin, tMax };
            AppendCrossings( ray.Gx0, ray.Ux, tMin, tMax, ts );
            AppendCrossings( ray.Gz0, ray.Uz, tMin, tMax, ts );
            std::sort( ts.begin(), ts.end() );

            for ( size_t i = 0; i + 1u < ts.size(); ++i )
            {
                const double ta  = ts[i];
                const double tb  = ts[i + 1u];
                const double tm  = 0.5 * ( ta + tb );
                const double cxd = std::clamp( std::floor( ray.Gx0 + ray.Ux * tm ), 0.0, lastX - 1.0 );
                const double czd = std::clamp( std::floor( ray.Gz0 + ray.Uz * tm ), 0.0, lastZ - 1.0 );
                const auto   cx  = static_cast<uint32_t>( cxd );
                const auto   cz  = static_cast<uint32_t>( czd );

                const double h00 = h( cx, cz );
                const double h10 = h( cx + 1u, cz );
                const double h01 = h( cx, cz + 1u );
                const double h11 = h( cx + 1u, cz + 1u );

                // Re-origined at ta so s stays small.
                const double ax  = ray.Gx0 + ray.Ux * ta - cxd;
                const double az  = ray.Gz0 + ray.Uz * ta - czd;
                const double y0  = ray.Y0 + ray.Dy * ta;
                const double len = tb - ta;

                // The cell's diagonal fx = fz cuts the segment at most once; each piece lies in one triangle,
                // found — like the cell — from its midpoint, by the shader's own rule.
                std::array<double, 3> cuts{ 0.0, len, len };
                size_t                pieces = 1u;
                if ( ray.Ux != ray.Uz )
                {
                    const double sd = -( ax - az ) / ( ray.Ux - ray.Uz );
                    if ( sd > 0.0 && sd < len )
                    {
                        cuts   = { 0.0, sd, len };
                        pieces = 2u;
                    }
                }
                // A root on a shared edge belongs to both sides and both give the same height there, so a
                // hair of slack at either end loses nothing and keeps rounding from dropping it between them.
                const double slack = 1e-9 * std::max( 1.0, std::abs( tb ) );
                for ( size_t p = 0; p < pieces; ++p )
                {
                    const double sm    = 0.5 * ( cuts[p] + cuts[p + 1u] );
                    const bool   upper = LandscapeInUpperTriangle( static_cast<float>( ax + ray.Ux * sm ),
                                                                   static_cast<float>( az + ray.Uz * sm ) );
                    // height = h00 + A·fx + B·fz on the triangle (LandscapeTriangle's two planes).
                    const double A = upper ? h11 - h01 : h10 - h00;
                    const double B = upper ? h01 - h00 : h11 - h10;
                    const double c = h00 + A * ax + B * az - y0;
                    const double b = A * ray.Ux + B * ray.Uz - ray.Dy;
                    const auto   s = LinearRoot( b, c, cuts[p] - slack, cuts[p + 1u] + slack );
                    if ( !s )
                        continue;
                    TileHit hit;
                    hit.T     = std::clamp( ta + *s, tMin, tMax );
                    hit.Fx    = std::clamp( ax + ray.Ux * *s, 0.0, 1.0 );
                    hit.Fz    = std::clamp( az + ray.Uz * *s, 0.0, 1.0 );
                    hit.DhDfx = A;
                    hit.DhDfz = B;
                    hit.CellX = cx;
                    hit.CellZ = cz;
                    return hit;
                }
            }
            return std::nullopt;
        }
    } // namespace

    std::optional<LandscapeRayHit> RaycastLandscape( std::span<const LandscapeRayTile> tiles,
                                                     const glm::vec3& origin, const glm::vec3& direction,
                                                     float maxDistance )
    {
        std::optional<LandscapeRayHit> best;
        for ( size_t index = 0; index < tiles.size(); ++index )
        {
            const LandscapeRayTile& entry = tiles[index];
            if ( !entry.Heights )
            {
                LOG_ERROR( "[Landscape] raycast: tile {} of {} has no heights and is skipped", index,
                           tiles.size() );
                continue;
            }
            const auto valid = ValidateLandscapeFrame( entry.Frame );
            if ( !valid.IsSuccess() )
            {
                LOG_ERROR( "[Landscape] raycast: tile {} of {} is skipped: {}", index, tiles.size(),
                           valid.GetError() );
                continue;
            }
            const LandscapeFrame& frame   = entry.Frame;
            const double          spacing = static_cast<double>( frame.SpacingCm );

            GridRay ray;
            ray.Gx0 = ( static_cast<double>( origin.x ) - frame.OriginX ) / spacing;
            ray.Gz0 = ( static_cast<double>( origin.z ) - frame.OriginZ ) / spacing;
            ray.Ux  = static_cast<double>( direction.x ) / spacing;
            ray.Uz  = static_cast<double>( direction.z ) / spacing;
            ray.Y0  = static_cast<double>( origin.y ) - frame.BaseY;
            ray.Dy  = static_cast<double>( direction.y );

            const double limit = best ? static_cast<double>( best->Distance ) : static_cast<double>( maxDistance );
            const auto   hit   = RaycastTile( *entry.Heights, frame, ray, limit );
            if ( !hit || ( best && hit->T >= static_cast<double>( best->Distance ) ) )
                continue;

            // Slope per centimetre along X and Z: the hit triangle's own plane.
            const double dhdx = hit->DhDfx / spacing;
            const double dhdz = hit->DhDfz / spacing;

            LandscapeRayHit out;
            out.Distance = static_cast<float>( hit->T );
            out.Point    = glm::vec3( glm::dvec3( origin ) + glm::dvec3( direction ) * hit->T );
            out.Normal =
                 glm::normalize( glm::vec3( static_cast<float>( -dhdx ), 1.0f, static_cast<float>( -dhdz ) ) );
            out.Tile = index;
            out.Uv =
                 glm::vec2( static_cast<float>( ( hit->CellX + hit->Fx ) / ( entry.Heights->SamplesX() - 1u ) ),
                            static_cast<float>( ( hit->CellZ + hit->Fz ) / ( entry.Heights->SamplesZ() - 1u ) ) );
            best = out;
        }
        return best;
    }
} // namespace Desert::World::Landscape
