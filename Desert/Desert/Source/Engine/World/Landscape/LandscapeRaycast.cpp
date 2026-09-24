#include <Engine/World/Landscape/LandscapeRaycast.hpp>

#include <Common/Core/Logger.hpp>

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace Desert::World::Landscape
{
    namespace
    {
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

        // Smallest root of a·s² + b·s + c in [lo, hi]. The two roots come from the cancellation-free form
        // (q, then q/a and c/q), which also degrades gracefully as a → 0 — the flat-twist cell, where the
        // bilinear patch is a plane and the equation is linear.
        std::optional<double> FirstRoot( double a, double b, double c, double lo, double hi )
        {
            if ( a == 0.0 )
            {
                if ( b == 0.0 )
                    return c == 0.0 ? std::optional<double>( lo ) : std::nullopt;
                const double s = -c / b;
                return ( s >= lo && s <= hi ) ? std::optional<double>( s ) : std::nullopt;
            }
            const double disc = b * b - 4.0 * a * c;
            if ( disc < 0.0 )
                return std::nullopt;
            const double q  = -0.5 * ( b + std::copysign( std::sqrt( disc ), b ) );
            double       r1 = q / a;
            double       r2 = q != 0.0 ? c / q : r1;
            if ( r1 > r2 )
                std::swap( r1, r2 );
            if ( r1 >= lo && r1 <= hi )
                return r1;
            if ( r2 >= lo && r2 <= hi )
                return r2;
            return std::nullopt;
        }

        struct TileHit
        {
            double   T     = 0.0;
            double   Fx    = 0.0; // within the cell, [0, 1]
            double   Fz    = 0.0;
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
            const auto lastX = static_cast<double>( sx - 1u );
            const auto lastZ = static_cast<double>( sz - 1u );

            const auto h = [&]( uint32_t x, uint32_t z ) -> double
            { return static_cast<double>( LandscapeHeightCm( tile.Sample( x, z ), frame.ZScale ) ); };

            // The vertical slab bounds the traversal to where the ray is between the tile's lowest and
            // highest sample: the bilinear patch never leaves the range of its four corners.
            const auto [lowIt, highIt] = std::minmax_element( tile.Samples().begin(), tile.Samples().end() );
            const auto low             = static_cast<double>( LandscapeHeightCm( *lowIt, frame.ZScale ) );
            const auto high            = static_cast<double>( LandscapeHeightCm( *highIt, frame.ZScale ) );

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
                const double e   = h( cx + 1u, cz ) - h00;
                const double f   = h( cx, cz + 1u ) - h00;
                const double k   = h00 + h( cx + 1u, cz + 1u ) - h( cx + 1u, cz ) - h( cx, cz + 1u );

                // Re-origined at ta so s stays small and the quadratic well conditioned.
                const double ax = ray.Gx0 + ray.Ux * ta - cxd;
                const double az = ray.Gz0 + ray.Uz * ta - czd;
                const double y0 = ray.Y0 + ray.Dy * ta;
                // height(s) - y(s), with height = h00 + e·fx + f·fz + k·fx·fz and fx = ax + Ux·s, fz = az + Uz·s.
                const double qa = k * ray.Ux * ray.Uz;
                const double qb = e * ray.Ux + f * ray.Uz + k * ( ax * ray.Uz + az * ray.Ux ) - ray.Dy;
                const double qc = h00 + e * ax + f * az + k * ax * az - y0;

                // A root on the shared edge belongs to both cells and both give the same height there, so a
                // hair of slack at either end loses nothing and keeps rounding from dropping it between them.
                const double slack = 1e-9 * std::max( 1.0, std::abs( tb ) );
                const auto   s     = FirstRoot( qa, qb, qc, -slack, ( tb - ta ) + slack );
                if ( !s )
                    continue;
                TileHit hit;
                hit.T     = std::clamp( ta + *s, tMin, tMax );
                hit.Fx    = std::clamp( ax + ray.Ux * *s, 0.0, 1.0 );
                hit.Fz    = std::clamp( az + ray.Uz * *s, 0.0, 1.0 );
                hit.CellX = cx;
                hit.CellZ = cz;
                return hit;
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
            if ( entry.Heights == nullptr )
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
            const auto            spacing = static_cast<double>( frame.SpacingCm );

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

            const auto h = [&]( uint32_t x, uint32_t z ) -> double
            { return static_cast<double>( LandscapeHeightCm( entry.Heights->Sample( x, z ), frame.ZScale ) ); };
            const double h00 = h( hit->CellX, hit->CellZ );
            const double h10 = h( hit->CellX + 1u, hit->CellZ );
            const double h01 = h( hit->CellX, hit->CellZ + 1u );
            const double h11 = h( hit->CellX + 1u, hit->CellZ + 1u );
            const double k   = h00 - h10 - h01 + h11;
            // Slope per centimetre along X and Z: the bilinear patch's own partial derivatives.
            const double dhdx = ( ( h10 - h00 ) + k * hit->Fz ) / spacing;
            const double dhdz = ( ( h01 - h00 ) + k * hit->Fx ) / spacing;

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
