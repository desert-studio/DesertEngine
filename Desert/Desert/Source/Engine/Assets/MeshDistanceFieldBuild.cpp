// Ported from UE 5.8 Engine/Source/Developer/MeshUtilities/Private/MeshDistanceFieldUtilities.cpp:300-581
// (GenerateSignedDistanceFieldVolumeData: per-mesh resolution from the voxel density and the per-mesh cap, the
// positive-extent bounds, the per-mip indirection layout with a one-voxel border, the band-limited 8-bit encoding,
// brick validity, the volume -> virtual UV transform), adapted: UE asks Embree for each sample's distance (a point
// query) and its sign (2 x 49 hemisphere rays, "inside" when more than a quarter hit backfaces). There is no
// Embree here, so the distance is computed on the mip's sample lattice the way SDFGen does it (C. Batty, public
// domain): exact point-triangle distances in a one-voxel band around every triangle, then fast sweeping that
// carries each sample's closest triangle to its neighbours - bounded by the encoding band, so the cost follows the
// surface and not the volume. The sign is a crossing-parity vote along the three lattice axes: a sample is inside
// along an axis when an odd number of surfaces lies on BOTH sides of it on that axis line, and inside overall when
// two of the three axes agree - which keeps an open sheet (a facade without a back, a ground plane) from turning
// the half-space behind it negative, the case UE's backface quarter guards against.
#include <Engine/Assets/MeshDistanceField.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace Desert::Assets
{
    namespace
    {
        // The sample lattice of one mip: (dims * 7 + 1)^3 points, point j at Origin + j * Voxel (brick b's sample
        // i is lattice point b * 7 + i, so neighbouring bricks share their border plane exactly as UE's do).
        struct SdfLattice
        {
            glm::ivec3           N{ 0 };
            glm::dvec3           Origin{ 0.0 };
            glm::dvec3           Voxel{ 1.0 };
            std::vector<float>   Distance; // unsigned, local units, capped at the trace distance
            std::vector<int>     Closest;  // triangle index, -1 = none within the band
            std::vector<uint8_t> InsideVotes;

            [[nodiscard]] size_t Index( const int i, const int j, const int k ) const
            {
                return ( static_cast<size_t>( k ) * N.y + j ) * N.x + i;
            }
            [[nodiscard]] glm::dvec3 Point( const int i, const int j, const int k ) const
            {
                return Origin + glm::dvec3( i, j, k ) * Voxel;
            }
        };

        // Ericson, Real-Time Collision Detection 5.1.5: the closest point of triangle abc to p.
        double PointTriangleDistance( const glm::dvec3& p, const glm::dvec3& a, const glm::dvec3& b,
                                      const glm::dvec3& c )
        {
            const glm::dvec3 ab = b - a, ac = c - a, ap = p - a;
            const double     d1 = glm::dot( ab, ap ), d2 = glm::dot( ac, ap );
            if ( d1 <= 0.0 && d2 <= 0.0 )
                return glm::length( ap );
            const glm::dvec3 bp = p - b;
            const double     d3 = glm::dot( ab, bp ), d4 = glm::dot( ac, bp );
            if ( d3 >= 0.0 && d4 <= d3 )
                return glm::length( bp );
            const double vc = d1 * d4 - d3 * d2;
            if ( vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0 )
                return glm::length( p - ( a + ab * ( d1 / ( d1 - d3 ) ) ) );
            const glm::dvec3 cp = p - c;
            const double     d5 = glm::dot( ab, cp ), d6 = glm::dot( ac, cp );
            if ( d6 >= 0.0 && d5 <= d6 )
                return glm::length( cp );
            const double vb = d5 * d2 - d1 * d6;
            if ( vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0 )
                return glm::length( p - ( a + ac * ( d2 / ( d2 - d6 ) ) ) );
            const double va = d3 * d6 - d5 * d4;
            if ( va <= 0.0 && ( d4 - d3 ) >= 0.0 && ( d5 - d6 ) >= 0.0 )
                return glm::length( p - ( b + ( c - b ) * ( ( d4 - d3 ) / ( ( d4 - d3 ) + ( d5 - d6 ) ) ) ) );
            const double denom = 1.0 / ( va + vb + vc );
            return glm::length( p - ( a + ab * ( vb * denom ) + ac * ( vc * denom ) ) );
        }

        // SDFGen's tie-broken orientation (makelevelset3.cpp): exactly one of two triangles sharing an edge claims
        // a lattice line through that edge, so a crossing is never counted twice or not at all.
        int Orientation( const double x1, const double y1, const double x2, const double y2, double& twiceArea )
        {
            twiceArea = y1 * x2 - x1 * y2;
            if ( twiceArea > 0.0 )
                return 1;
            if ( twiceArea < 0.0 )
                return -1;
            if ( y2 > y1 )
                return 1;
            if ( y2 < y1 )
                return -1;
            if ( x1 > x2 )
                return 1;
            if ( x1 < x2 )
                return -1;
            return 0;
        }

        bool PointInTriangle2D( const double x0, const double y0, double x1, double y1, double x2, double y2,
                                double x3, double y3, double& a, double& b, double& c )
        {
            x1 -= x0;
            x2 -= x0;
            x3 -= x0;
            y1 -= y0;
            y2 -= y0;
            y3 -= y0;
            const int signA = Orientation( x2, y2, x3, y3, a );
            if ( signA == 0 )
                return false;
            if ( Orientation( x3, y3, x1, y1, b ) != signA )
                return false;
            if ( Orientation( x1, y1, x2, y2, c ) != signA )
                return false;
            const double sum = a + b + c;
            if ( sum == 0.0 )
                return false;
            a /= sum;
            b /= sum;
            c /= sum;
            return true;
        }

        struct SdfMesh
        {
            std::vector<glm::dvec3> Positions;
            std::vector<glm::uvec3> Triangles;
        };

        double TriangleDistance( const SdfMesh& mesh, const int t, const glm::dvec3& p )
        {
            const glm::uvec3& tri = mesh.Triangles[t];
            return PointTriangleDistance( p, mesh.Positions[tri.x], mesh.Positions[tri.y], mesh.Positions[tri.z] );
        }

        void ExactBand( const SdfMesh& mesh, SdfLattice& lattice )
        {
            for ( int t = 0; t < static_cast<int>( mesh.Triangles.size() ); ++t )
            {
                const glm::uvec3& tri = mesh.Triangles[t];
                glm::dvec3        lo =
                     glm::min( glm::min( mesh.Positions[tri.x], mesh.Positions[tri.y] ), mesh.Positions[tri.z] );
                glm::dvec3 hi =
                     glm::max( glm::max( mesh.Positions[tri.x], mesh.Positions[tri.y] ), mesh.Positions[tri.z] );
                const glm::ivec3 i0 =
                     glm::clamp( glm::ivec3( glm::floor( ( lo - lattice.Origin ) / lattice.Voxel ) ) - 1,
                                 glm::ivec3( 0 ), lattice.N - 1 );
                const glm::ivec3 i1 =
                     glm::clamp( glm::ivec3( glm::ceil( ( hi - lattice.Origin ) / lattice.Voxel ) ) + 1,
                                 glm::ivec3( 0 ), lattice.N - 1 );
                for ( int k = i0.z; k <= i1.z; ++k )
                    for ( int j = i0.y; j <= i1.y; ++j )
                        for ( int i = i0.x; i <= i1.x; ++i )
                        {
                            const size_t idx = lattice.Index( i, j, k );
                            const double d   = TriangleDistance( mesh, t, lattice.Point( i, j, k ) );
                            if ( d < lattice.Distance[idx] )
                            {
                                lattice.Distance[idx] = static_cast<float>( d );
                                lattice.Closest[idx]  = t;
                            }
                        }
            }
        }

        void CheckNeighbour( const SdfMesh& mesh, SdfLattice& lattice, const int i, const int j, const int k,
                             const int ni, const int nj, const int nk, const double trace )
        {
            const int t = lattice.Closest[lattice.Index( ni, nj, nk )];
            if ( t < 0 )
                return;
            const size_t idx = lattice.Index( i, j, k );
            if ( lattice.Closest[idx] == t )
                return;
            const double d = TriangleDistance( mesh, t, lattice.Point( i, j, k ) );
            // Bounded by the band: a sample farther than the trace distance encodes as saturated anyway, and
            // leaving it without a closest triangle is what stops the sweep from walking the whole volume.
            if ( d < lattice.Distance[idx] && d <= trace )
            {
                lattice.Distance[idx] = static_cast<float>( d );
                lattice.Closest[idx]  = t;
            }
        }

        void Sweep( const SdfMesh& mesh, SdfLattice& lattice, const int di, const int dj, const int dk,
                    const double trace )
        {
            const glm::ivec3 n  = lattice.N;
            const int        i0 = di > 0 ? 1 : n.x - 2, i1 = di > 0 ? n.x : -1;
            const int        j0 = dj > 0 ? 1 : n.y - 2, j1 = dj > 0 ? n.y : -1;
            const int        k0 = dk > 0 ? 1 : n.z - 2, k1 = dk > 0 ? n.z : -1;
            for ( int k = k0; k != k1; k += dk )
                for ( int j = j0; j != j1; j += dj )
                    for ( int i = i0; i != i1; i += di )
                    {
                        CheckNeighbour( mesh, lattice, i, j, k, i - di, j, k, trace );
                        CheckNeighbour( mesh, lattice, i, j, k, i, j - dj, k, trace );
                        CheckNeighbour( mesh, lattice, i, j, k, i - di, j - dj, k, trace );
                        CheckNeighbour( mesh, lattice, i, j, k, i, j, k - dk, trace );
                        CheckNeighbour( mesh, lattice, i, j, k, i - di, j, k - dk, trace );
                        CheckNeighbour( mesh, lattice, i, j, k, i, j - dj, k - dk, trace );
                        CheckNeighbour( mesh, lattice, i, j, k, i - di, j - dj, k - dk, trace );
                    }
        }

        // One axis of the parity vote: count the surface crossings of every lattice line along @p axis, then
        // vote "inside" for each point with an odd count on both sides of it.
        void VoteAxis( const SdfMesh& mesh, SdfLattice& lattice, const int axis )
        {
            const int u = ( axis + 1 ) % 3, v = ( axis + 2 ) % 3;
            const int nu = lattice.N[u], nv = lattice.N[v], na = lattice.N[axis];
            // crossings[(row) * (na + 1) + slot]: slot s counts crossings between lattice points s-1 and s.
            std::vector<uint16_t> crossings( static_cast<size_t>( nu ) * nv * ( na + 1 ), 0 );
            for ( const glm::uvec3& tri : mesh.Triangles )
            {
                const glm::dvec3 p[3] = { mesh.Positions[tri.x], mesh.Positions[tri.y], mesh.Positions[tri.z] };
                const double     lu   = std::min( { p[0][u], p[1][u], p[2][u] } );
                const double     hu   = std::max( { p[0][u], p[1][u], p[2][u] } );
                const double     lv   = std::min( { p[0][v], p[1][v], p[2][v] } );
                const double     hv   = std::max( { p[0][v], p[1][v], p[2][v] } );
                const int        iu0 =
                     std::max( 0, static_cast<int>( std::ceil( ( lu - lattice.Origin[u] ) / lattice.Voxel[u] ) ) );
                const int iu1 = std::min(
                     nu - 1, static_cast<int>( std::floor( ( hu - lattice.Origin[u] ) / lattice.Voxel[u] ) ) );
                const int iv0 =
                     std::max( 0, static_cast<int>( std::ceil( ( lv - lattice.Origin[v] ) / lattice.Voxel[v] ) ) );
                const int iv1 = std::min(
                     nv - 1, static_cast<int>( std::floor( ( hv - lattice.Origin[v] ) / lattice.Voxel[v] ) ) );
                for ( int iv = iv0; iv <= iv1; ++iv )
                    for ( int iu = iu0; iu <= iu1; ++iu )
                    {
                        const double pu = lattice.Origin[u] + iu * lattice.Voxel[u];
                        const double pv = lattice.Origin[v] + iv * lattice.Voxel[v];
                        double       a = 0.0, b = 0.0, c = 0.0;
                        if ( !PointInTriangle2D( pu, pv, p[0][u], p[0][v], p[1][u], p[1][v], p[2][u], p[2][v], a,
                                                 b, c ) )
                            continue;
                        const double at   = a * p[0][axis] + b * p[1][axis] + c * p[2][axis];
                        const double s    = std::ceil( ( at - lattice.Origin[axis] ) / lattice.Voxel[axis] );
                        const int    slot = static_cast<int>( std::clamp( s, 0.0, static_cast<double>( na ) ) );
                        auto& count       = crossings[( static_cast<size_t>( iv ) * nu + iu ) * ( na + 1 ) + slot];
                        if ( count < std::numeric_limits<uint16_t>::max() )
                            ++count;
                    }
            }
            for ( int iv = 0; iv < nv; ++iv )
                for ( int iu = 0; iu < nu; ++iu )
                {
                    const uint16_t* row   = &crossings[( static_cast<size_t>( iv ) * nu + iu ) * ( na + 1 )];
                    uint32_t        total = 0;
                    for ( int s = 0; s <= na; ++s )
                        total += row[s];
                    if ( total == 0 )
                        continue;
                    uint32_t before = 0;
                    for ( int ia = 0; ia < na; ++ia )
                    {
                        before += row[ia];
                        if ( ( before & 1u ) == 0 || ( ( total - before ) & 1u ) == 0 )
                            continue;
                        glm::ivec3 ijk( 0 );
                        ijk[axis] = ia;
                        ijk[u]    = iu;
                        ijk[v]    = iv;
                        ++lattice.InsideVotes[lattice.Index( ijk.x, ijk.y, ijk.z )];
                    }
                }
        }

        uint8_t Quantize( const double localDistance, const double localToVolume, const glm::dvec2 scaleBias )
        {
            const double volumeDistance = localDistance * localToVolume;
            const double rescaled       = ( volumeDistance - scaleBias.y ) / scaleBias.x;
            return static_cast<uint8_t>( std::clamp( std::floor( rescaled * 255.0 + 0.5 ), 0.0, 255.0 ) );
        }
    } // namespace

    Common::ResultStr<MeshDistanceFieldData>
    BuildMeshDistanceField( const MeshDistanceFieldBuildInput&    input,
                            const MeshDistanceFieldBuildSettings& settings )
    {
        if ( input.Indices.size() % 3 != 0 )
            return Common::MakeFormattedError<MeshDistanceFieldData>(
                 "distance field: {} indices are not whole triangles", input.Indices.size() );
        if ( !( settings.VoxelDensity > 0.0f ) || settings.MaxPerMeshResolution < kDistanceFieldUniqueBrickSize ||
             !( settings.ResolutionScale > 0.0f ) )
            return Common::MakeFormattedError<MeshDistanceFieldData>(
                 "distance field: settings out of range (density {}, max resolution {}, scale {})",
                 settings.VoxelDensity, settings.MaxPerMeshResolution, settings.ResolutionScale );

        SdfMesh mesh;
        mesh.Positions.reserve( input.Positions.size() );
        for ( const glm::vec3& p : input.Positions )
            mesh.Positions.emplace_back( p );
        for ( size_t i = 0; i < input.Indices.size(); i += 3 )
        {
            const glm::uvec3 tri( input.Indices[i], input.Indices[i + 1], input.Indices[i + 2] );
            if ( tri.x >= mesh.Positions.size() || tri.y >= mesh.Positions.size() ||
                 tri.z >= mesh.Positions.size() )
                return Common::MakeFormattedError<MeshDistanceFieldData>(
                     "distance field: triangle {} names vertex {} of {}", i / 3,
                     std::max( { tri.x, tri.y, tri.z } ), mesh.Positions.size() );
            mesh.Triangles.push_back( tri );
        }

        MeshDistanceFieldData out;
        if ( mesh.Triangles.empty() )
            return Common::MakeSuccess( std::move( out ) ); // no surface: every mip empty, nothing to sample

        glm::dvec3 lo( std::numeric_limits<double>::max() ), hi( std::numeric_limits<double>::lowest() );
        for ( const glm::uvec3& tri : mesh.Triangles )
            for ( int c = 0; c < 3; ++c )
            {
                lo = glm::min( lo, mesh.Positions[tri[c]] );
                hi = glm::max( hi, mesh.Positions[tri[c]] );
            }
        // Positive extents, so a plane still has a volume (UE: extent at least one unit).
        const glm::dvec3 centre = ( lo + hi ) * 0.5;
        const glm::dvec3 extent = glm::max( ( hi - lo ) * 0.5, glm::dvec3( 1.0 ) );
        lo                      = centre - extent;
        hi                      = centre + extent;
        const glm::dvec3 size   = hi - lo;

        const uint32_t perMeshMax =
             settings.ResolutionScale <= 1.0f ? settings.MaxPerMeshResolution / 2 : settings.MaxPerMeshResolution;
        const int        maxBlocks     = static_cast<int>( std::min<uint32_t>(
             ( perMeshMax + kDistanceFieldUniqueBrickSize / 2 ) / kDistanceFieldUniqueBrickSize,
             kDistanceFieldMaxIndirectionDim - 1 ) );
        const double     voxelsPerUnit = static_cast<double>( settings.VoxelDensity ) * settings.ResolutionScale;
        const double     localToVolume = 1.0 / std::max( { extent.x, extent.y, extent.z } );
        const glm::dvec3 desired  = size * voxelsPerUnit / static_cast<double>( kDistanceFieldUniqueBrickSize );
        const glm::ivec3 mip0Dims = glm::clamp( glm::ivec3( glm::round( desired ) ), glm::ivec3( 1 ),
                                                glm::ivec3( std::max( 1, maxBlocks ) ) );

        out.LocalSpaceMeshBounds      = { glm::vec3( lo ), glm::vec3( hi ) };
        const glm::dvec3 volumeExtent = extent * localToVolume;
        constexpr int    kUnique      = static_cast<int>( kDistanceFieldUniqueBrickSize );
        constexpr int    kBrick       = static_cast<int>( kDistanceFieldBrickSize );
        constexpr int    kBorder      = static_cast<int>( kDistanceFieldObjectBorder );

        for ( uint32_t mipIndex = 0; mipIndex < kDistanceFieldNumMips; ++mipIndex )
        {
            const int        div  = 1 << mipIndex;
            const glm::ivec3 dims = ( mip0Dims + div - 1 ) / div;
            // A one-voxel border around the mesh box, for gradients by central differences at its faces.
            const glm::dvec3 texelSize   = size / glm::dvec3( dims * kUnique - 2 * kBorder );
            const glm::dvec3 volumeMin   = lo - texelSize;
            const glm::dvec3 volumeSize  = size + 2.0 * texelSize;
            const glm::dvec3 indirection = volumeSize / glm::dvec3( dims );
            const glm::dvec3 voxel       = indirection / static_cast<double>( kUnique );
            const double     maxEncoding = glm::length( voxel * localToVolume ) * kDistanceFieldBandSizeInVoxels;
            const double     trace       = maxEncoding / localToVolume;
            const glm::dvec2 scaleBias( 2.0 * maxEncoding, -maxEncoding );

            SdfLattice lattice;
            lattice.N           = dims * kUnique + 1;
            lattice.Origin      = volumeMin;
            lattice.Voxel       = voxel;
            const size_t points = static_cast<size_t>( lattice.N.x ) * lattice.N.y * lattice.N.z;
            lattice.Distance.assign( points, static_cast<float>( trace ) );
            lattice.Closest.assign( points, -1 );
            lattice.InsideVotes.assign( points, 0 );
            ExactBand( mesh, lattice );
            for ( const int dk : { 1, -1 } )
                for ( const int dj : { 1, -1 } )
                    for ( const int di : { 1, -1 } )
                        Sweep( mesh, lattice, di, dj, dk, trace );
            for ( int axis = 0; axis < 3; ++axis )
                VoteAxis( mesh, lattice, axis );

            SparseDistanceFieldMip& mip        = out.Mips[mipIndex];
            mip.IndirectionDimensions          = glm::uvec3( dims );
            mip.DistanceFieldToVolumeScaleBias = glm::vec2( scaleBias );
            mip.IndirectionTable.assign( static_cast<size_t>( dims.x ) * dims.y * dims.z,
                                         kDistanceFieldInvalidBrick );
            std::vector<uint8_t> brick( kDistanceFieldBrickVoxels );
            for ( int bz = 0; bz < dims.z; ++bz )
                for ( int by = 0; by < dims.y; ++by )
                    for ( int bx = 0; bx < dims.x; ++bx )
                    {
                        uint8_t lowest = 255, highest = 0;
                        for ( int z = 0; z < kBrick; ++z )
                            for ( int y = 0; y < kBrick; ++y )
                                for ( int x = 0; x < kBrick; ++x )
                                {
                                    const size_t idx =
                                         lattice.Index( bx * kUnique + x, by * kUnique + y, bz * kUnique + z );
                                    const double  sign = lattice.InsideVotes[idx] >= 2 ? -1.0 : 1.0;
                                    const uint8_t q =
                                         Quantize( sign * lattice.Distance[idx], localToVolume, scaleBias );
                                    brick[( z * kBrick + y ) * kBrick + x] = q;
                                    lowest                                 = std::min( lowest, q );
                                    highest                                = std::max( highest, q );
                                }
                        // UE keeps a brick only when it is not saturated at one end: all 255 is "outside, at
                        // least the band away" and all 0 "inside, at least the band deep" - both read back as
                        // the mip's maximum encoded distance without a brick.
                        if ( lowest < 255 && highest > 0 )
                        {
                            mip.IndirectionTable[( static_cast<size_t>( bz ) * dims.y + by ) * dims.x + bx] =
                                 mip.BrickCount();
                            mip.BrickData.insert( mip.BrickData.end(), brick.begin(), brick.end() );
                        }
                    }
            // Account for the border voxels: [-VolumePositionExtent, VolumePositionExtent] -> [uvMin, uvMin +
            // uvSize].
            const glm::dvec3 uvMin     = glm::dvec3( kBorder ) / glm::dvec3( dims * kUnique );
            const glm::dvec3 uvSize    = glm::dvec3( dims * kUnique - 2 * kBorder ) / glm::dvec3( dims * kUnique );
            const glm::dvec3 uvScale   = uvSize / ( 2.0 * volumeExtent );
            mip.VolumeToVirtualUVScale = glm::vec3( uvScale );
            mip.VolumeToVirtualUVAdd   = glm::vec3( volumeExtent * uvScale + uvMin );
        }
        return Common::MakeSuccess( std::move( out ) );
    }
} // namespace Desert::Assets
