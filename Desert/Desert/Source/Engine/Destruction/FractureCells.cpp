#include <Engine/Destruction/FractureCells.hpp>

#include <voro++.hh>

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <numbers>

namespace Desert::Destruction
{
    namespace
    {
        /// voro++'s cell arrays -> a ConvexCell. `offset` is what voro++ adds to its cell-relative vertices
        /// (the site for a Voronoi cell, the origin for a cell clipped in absolute coordinates).
        ConvexCell ExtractCell( voro::voronoicell_base& cell, const glm::dvec3& offset,
                                std::vector<int>* neighbors )
        {
            std::vector<double> xyz;
            std::vector<int>    faceVertices;
            cell.vertices( offset.x, offset.y, offset.z, xyz );
            cell.face_vertices( faceVertices );

            ConvexCell out;
            out.Vertices.reserve( xyz.size() / 3 );
            for ( size_t i = 0; i + 2 < xyz.size(); i += 3 )
                out.Vertices.emplace_back( xyz[i], xyz[i + 1], xyz[i + 2] );

            glm::dvec3 centre( 0.0 );
            for ( const glm::dvec3& v : out.Vertices )
                centre += v;
            centre /= static_cast<double>( std::max<size_t>( out.Vertices.size(), 1 ) );

            // face_vertices: [count, i0 .. i(count-1)] per face. The winding is made outward here by the
            // face normal against the centre rather than trusted, so every method yields the same orientation.
            for ( size_t at = 0; at < faceVertices.size(); )
            {
                const int        count = faceVertices[at++];
                std::vector<int> face( faceVertices.begin() + static_cast<std::ptrdiff_t>( at ),
                                       faceVertices.begin() + static_cast<std::ptrdiff_t>( at + count ) );
                at += static_cast<size_t>( count );
                if ( face.size() < 3 )
                    continue;
                glm::dvec3 normal( 0.0 );
                glm::dvec3 faceCentre( 0.0 );
                for ( size_t k = 0; k < face.size(); ++k )
                {
                    const glm::dvec3& a = out.Vertices[face[k]];
                    const glm::dvec3& b = out.Vertices[face[( k + 1 ) % face.size()]];
                    normal += glm::cross( a, b ); // Newell
                    faceCentre += a;
                }
                faceCentre /= static_cast<double>( face.size() );
                if ( glm::dot( normal, faceCentre - centre ) < 0.0 )
                    std::reverse( face.begin(), face.end() );
                out.Faces.push_back( std::move( face ) );
            }
            if ( neighbors )
                out.Neighbors = *neighbors;
            else
                out.Neighbors.assign( out.Faces.size(), -1 );
            return out;
        }

        /// pre_container::guess_optimal (voro++ 0.4.6 pre_container.cc) for a known site count: a grid of
        /// about `optimal_particles` sites per block. UE calls the same arithmetic (Voronoi.cpp:105).
        void GuessOptimalGrid( int sites, const glm::dvec3& size, int& nx, int& ny, int& nz )
        {
            const double volume = std::max( size.x * size.y * size.z, 1e-12 );
            const double ilscale =
                 std::cbrt( static_cast<double>( std::max( sites, 1 ) ) / ( voro::optimal_particles * volume ) );
            nx = std::max( 1, static_cast<int>( size.x * ilscale + 1 ) );
            ny = std::max( 1, static_cast<int>( size.y * ilscale + 1 ) );
            nz = std::max( 1, static_cast<int>( size.z * ilscale + 1 ) );
        }

        glm::dvec3 UnitDirection( FractureRandom& random )
        {
            const double z   = random.Range( -1.0, 1.0 );
            const double phi = random.Range( 0.0, 2.0 * std::numbers::pi );
            const double r   = std::sqrt( std::max( 0.0, 1.0 - z * z ) );
            return { r * std::cos( phi ), r * std::sin( phi ), z };
        }

        void InitBox( voro::voronoicell& cell, const CellBounds& b )
        {
            cell.init( b.Min.x, b.Max.x, b.Min.y, b.Max.y, b.Min.z, b.Max.z );
        }
    } // namespace

    std::vector<glm::dvec3> GenerateUniformSites( const CellBounds& bounds, int count, FractureRandom& random )
    {
        std::vector<glm::dvec3> sites;
        sites.reserve( static_cast<size_t>( std::max( count, 0 ) ) );
        for ( int i = 0; i < count; ++i )
        {
            const double x = random.Range( bounds.Min.x, bounds.Max.x );
            const double y = random.Range( bounds.Min.y, bounds.Max.y );
            const double z = random.Range( bounds.Min.z, bounds.Max.z );
            sites.emplace_back( x, y, z );
        }
        return sites;
    }

    std::vector<glm::dvec3> GenerateClusteredSites( const CellBounds& bounds, int clusters, int sitesPerCluster,
                                                    double minRadius, double maxRadius, FractureRandom& random )
    {
        std::vector<glm::dvec3>       sites;
        const std::vector<glm::dvec3> centres = GenerateUniformSites( bounds, clusters, random );
        for ( const glm::dvec3& centre : centres )
        {
            sites.push_back( centre );
            for ( int i = 0; i < sitesPerCluster; ++i )
            {
                const glm::dvec3 direction = UnitDirection( random );
                const double     distance  = random.Range( minRadius, maxRadius );
                sites.push_back( glm::clamp( centre + direction * distance, bounds.Min, bounds.Max ) );
            }
        }
        return sites;
    }

    std::vector<ConvexCell> ComputeVoronoiCells( const std::vector<glm::dvec3>& sites, const CellBounds& bounds )
    {
        std::vector<ConvexCell> cells;
        if ( sites.empty() )
            return cells;
        int nx = 1, ny = 1, nz = 1;
        GuessOptimalGrid( static_cast<int>( sites.size() ), bounds.Max - bounds.Min, nx, ny, nz );
        voro::container container( bounds.Min.x, bounds.Max.x, bounds.Min.y, bounds.Max.y, bounds.Min.z,
                                   bounds.Max.z, nx, ny, nz, false, false, false, 8 );
        for ( size_t i = 0; i < sites.size(); ++i )
            container.put( static_cast<int>( i ), sites[i].x, sites[i].y, sites[i].z );

        // Computed per site and then emitted in SITE order: the container walks its grid blocks, and the
        // result must not depend on how the grid happened to be sized.
        std::vector<std::optional<ConvexCell>> bySite( sites.size() );
        voro::c_loop_all                       loop( container );
        voro::voronoicell_neighbor             cell;
        if ( loop.start() )
        {
            do
            {
                if ( !container.compute_cell( cell, loop ) )
                    continue;
                double x = 0, y = 0, z = 0;
                loop.pos( x, y, z );
                std::vector<int> neighbors;
                cell.neighbors( neighbors );
                bySite[static_cast<size_t>( loop.pid() )] = ExtractCell( cell, { x, y, z }, &neighbors );
            } while ( loop.inc() );
        }
        for ( auto& c : bySite )
            if ( c )
                cells.push_back( std::move( *c ) );
        return cells;
    }

    std::vector<ConvexCell> ComputePlanarCells( const std::vector<CutPlane>& planes, const CellBounds& bounds )
    {
        // Each plane splits every current region in two; voronoicell::plane(n, rsq) keeps 2 n.p < rsq, so the
        // plane n.p = d with the region n.p < d is plane(n, 2d), and the other side is plane(-n, -2d).
        std::vector<std::unique_ptr<voro::voronoicell>> regions;
        regions.push_back( std::make_unique<voro::voronoicell>() );
        InitBox( *regions[0], bounds );
        for ( const CutPlane& plane : planes )
        {
            const double                                    d = glm::dot( plane.Normal, plane.Point );
            std::vector<std::unique_ptr<voro::voronoicell>> next;
            for ( auto& region : regions )
            {
                // voro++ cells own raw arrays and have no copy constructor (a copy would share them); operator=
                // deep-copies.
                auto below = std::make_unique<voro::voronoicell>();
                auto above = std::make_unique<voro::voronoicell>();
                *below     = *region;
                *above     = *region;
                if ( below->plane( plane.Normal.x, plane.Normal.y, plane.Normal.z, 2.0 * d ) )
                    next.push_back( std::move( below ) );
                if ( above->plane( -plane.Normal.x, -plane.Normal.y, -plane.Normal.z, -2.0 * d ) )
                    next.push_back( std::move( above ) );
            }
            regions = std::move( next );
        }
        std::vector<ConvexCell> cells;
        for ( auto& region : regions )
            if ( region->volume() > 0.0 )
                cells.push_back( ExtractCell( *region, glm::dvec3( 0.0 ), nullptr ) );
        return cells;
    }

    std::vector<ConvexCell> ComputeBrickCells( const BrickSettings& settings, const CellBounds& bounds )
    {
        // FractureEngineFracturing.cpp:844-900: brick CENTRES tiled from Min until they pass Max plus half a
        // brick, Stretcher offsetting alternate courses (and alternate rows in depth) by half a length.
        std::vector<ConvexCell> cells;
        const glm::dvec3        dim( settings.Length, settings.Depth, settings.Height );
        if ( dim.x <= 0.0 || dim.y <= 0.0 || dim.z <= 0.0 )
            return cells;
        const glm::dvec3 half    = dim * 0.5;
        const glm::dvec3 extents = ( bounds.Max + half + 1e-8 ) - bounds.Min;
        bool             oddY    = false;
        for ( double yy = 0.0; yy <= extents.y; yy += dim.y )
        {
            bool oddLine = false;
            for ( double zz = half.z; zz <= extents.z; zz += dim.z )
            {
                for ( double xx = 0.0; xx <= extents.x; xx += dim.x )
                {
                    const bool       shifted = settings.Bond == BrickBond::Stretcher ? ( oddLine ^ oddY ) : oddY;
                    const glm::dvec3 centre  = bounds.Min + glm::dvec3( shifted ? xx : xx + half.x, yy, zz );
                    CellBounds       brick{ glm::max( centre - half, bounds.Min ),
                                      glm::min( centre + half, bounds.Max ) };
                    if ( brick.Max.x - brick.Min.x <= 1e-9 || brick.Max.y - brick.Min.y <= 1e-9 ||
                         brick.Max.z - brick.Min.z <= 1e-9 )
                        continue;
                    voro::voronoicell cell;
                    InitBox( cell, brick );
                    cells.push_back( ExtractCell( cell, glm::dvec3( 0.0 ), nullptr ) );
                }
                oddLine = !oddLine;
            }
            oddY = !oddY;
        }
        return cells;
    }
} // namespace Desert::Destruction
