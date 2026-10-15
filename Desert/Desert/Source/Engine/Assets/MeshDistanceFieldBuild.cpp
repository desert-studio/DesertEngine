// Ported from UE 5.8 Engine/Source/Developer/MeshUtilities/Private/MeshDistanceFieldUtilities.cpp:300-581
// (GenerateSignedDistanceFieldVolumeData: per-mesh resolution from the voxel density and the per-mesh cap, the
// positive-extent bounds, the per-mip indirection layout with a one-voxel border, the band-limited 8-bit encoding,
// brick validity, the volume -> virtual UV transform), adapted: UE asks Embree for each sample's distance (a point
// query) and its sign (2 x 49 hemisphere rays, "inside" when more than a quarter hit backfaces). There is no
// Embree here, so the distance is computed on the mip's sample lattice the way SDFGen does it (C. Batty, public
// domain): exact point-triangle distances in a one-voxel band around every triangle, then fast sweeping that
// carries each sample's closest triangle to its neighbours - bounded by the encoding band, so the cost follows the
// surface and not the volume. The sign is UE's (the rays, see DfInsideByRays): a sample within the band is inside
// when more than a quarter of its 98 rays hit a backface first, traced against a triangle BVH in place of Embree's
// scene. A TWO-SIDED triangle (its section's material is two-sided: foliage cards, leaves, awnings) stops a ray
// but never counts as a backface, as UE's IsHitTwoSided: it bounds no volume. Samples beyond the band are not
// traced and stay outside, as in UE (bTraceRays).
#include <Engine/Assets/MeshDistanceField.hpp>
#include <Engine/Assets/StratifiedHemisphereSamples.hpp>
#include <Engine/Geometry/TriangleBvh.hpp>

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
            std::vector<uint8_t> Inside;   // 1 = inside by UE's ray vote (samples within the band only)

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
        glm::dvec3 ClosestPointOnTriangle( const glm::dvec3& p, const glm::dvec3& a, const glm::dvec3& b,
                                           const glm::dvec3& c )
        {
            const glm::dvec3 ab = b - a, ac = c - a, ap = p - a;
            const double     d1 = glm::dot( ab, ap ), d2 = glm::dot( ac, ap );
            if ( d1 <= 0.0 && d2 <= 0.0 )
                return a;
            const glm::dvec3 bp = p - b;
            const double     d3 = glm::dot( ab, bp ), d4 = glm::dot( ac, bp );
            if ( d3 >= 0.0 && d4 <= d3 )
                return b;
            const double vc = d1 * d4 - d3 * d2;
            if ( vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0 )
                return a + ab * ( d1 / ( d1 - d3 ) );
            const glm::dvec3 cp = p - c;
            const double     d5 = glm::dot( ab, cp ), d6 = glm::dot( ac, cp );
            if ( d6 >= 0.0 && d5 <= d6 )
                return c;
            const double vb = d5 * d2 - d1 * d6;
            if ( vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0 )
                return a + ac * ( d2 / ( d2 - d6 ) );
            const double va = d3 * d6 - d5 * d4;
            if ( va <= 0.0 && ( d4 - d3 ) >= 0.0 && ( d5 - d6 ) >= 0.0 )
                return b + ( c - b ) * ( ( d4 - d3 ) / ( ( d4 - d3 ) + ( d5 - d6 ) ) );
            const double denom = 1.0 / ( va + vb + vc );
            return a + ab * ( vb * denom ) + ac * ( vc * denom );
        }

        double PointTriangleDistance( const glm::dvec3& p, const glm::dvec3& a, const glm::dvec3& b,
                                      const glm::dvec3& c )
        {
            return glm::length( p - ClosestPointOnTriangle( p, a, b, c ) );
        }

        struct SdfMesh
        {
            std::vector<glm::dvec3> Positions;
            std::vector<glm::uvec3> Triangles;
            // Per triangle: 1 = its material is two-sided, so it takes no part in the inside/outside vote.
            std::vector<uint8_t> TwoSided;
        };

        double TriangleDistance( const SdfMesh& mesh, const int t, const glm::dvec3& p )
        {
            const glm::uvec3& tri = mesh.Triangles[t];
            return PointTriangleDistance( p, mesh.Positions[tri.x], mesh.Positions[tri.y], mesh.Positions[tri.z] );
        }

        // THE SIGN, AS UE (MeshDistanceFieldUtilities.cpp:211-284, FSparseMeshDistanceFieldAsyncTask::DoWork): a
        // sample within the band traces kDfSignRays rays of the band's length in a stratified sphere of directions
        // and is inside when more than a quarter of them hit a BACKFACE first. Every surface the rays reach takes
        // part - a sample under a pavement sheet's border next to a kerb's back counts both - so open sheets,
        // sheets meeting at a kerb and closed meshes are signed by one rule. A two-sided triangle stops a ray and
        // counts as a hit but never as a backface (UE IsHitTwoSided). Embree is replaced by
        // Geometry/TriangleBvh.hpp.

        // UE MeshUtilities::GenerateStratifiedUniformHemisphereSamples(49) twice from one stream at seed 0, the
        // second set mirrored in Z (GenerateSignedDistanceFieldVolumeData, bUsePointQuery): 7 x 7 jittered strata
        // per hemisphere.
        constexpr int           kDfSignSamplesPerHemisphere = 49;
        std::vector<glm::dvec3> SignDirections()
        {
            Common::Math::RandomStream random;
            std::vector<glm::dvec3>    directions;
            for ( const double zSign : { 1.0, -1.0 } )
                AppendStratifiedUniformHemisphereSamples( kDfSignSamplesPerHemisphere, random, zSign, directions );
            return directions;
        }

        // UE's vote for one sample: rays start pulled back by 1e-4 of the trace distance (a sample exactly on a
        // triangle - box corners - still sees it) and reach the trace distance. Inside when Hit > 0 and HitBack >
        // a quarter of the rays; the loop stops as soon as the remaining rays cannot change that outcome.
        bool DfInsideByRays( const SdfMesh& mesh, const Geometry::TriangleBvh& bvh,
                             const std::vector<glm::dvec3>& directions, const glm::dvec3& p, const double trace )
        {
            const int total   = static_cast<int>( directions.size() );
            int       hitBack = 0;
            for ( int i = 0; i < total; ++i )
            {
                const glm::dvec3& dir  = directions[i];
                const double      d[3] = { dir.x, dir.y, dir.z };
                const double      pull = 1.0e-4 * trace;
                const double      o[3] = { p.x - pull * d[0], p.y - pull * d[1], p.z - pull * d[2] };
                const int         hit  = bvh.ClosestHit( o, d, 0.0, trace ).Triangle;
                if ( hit >= 0 && mesh.TwoSided[hit] == 0 )
                {
                    const glm::dvec3& n = bvh.Normal[hit];
                    if ( d[0] * n.x + d[1] * n.y + d[2] * n.z > 0.0 )
                        ++hitBack;
                }
                if ( 4 * hitBack > total ) // HitBack > .25f * SampleDirections->Num()
                    return true;
                if ( 4 * ( hitBack + total - 1 - i ) <= total )
                    return false;
            }
            return false;
        }

        // A triangle outside the lattice but within the trace distance of it seeds the lattice's boundary samples
        // (its index range is clamped), and the sweep carries it inwards: a volume that is one of several over
        // the mesh (MeshDistanceFieldData::Volumes) sees its neighbours' surface across the seam. A triangle
        // farther than that cannot reach a sample within the band and is skipped.
        void ExactBand( const SdfMesh& mesh, SdfLattice& lattice, const double trace )
        {
            const glm::dvec3 reachLo = lattice.Origin - glm::dvec3( trace );
            const glm::dvec3 reachHi = lattice.Origin + glm::dvec3( lattice.N - 1 ) * lattice.Voxel + trace;
            for ( int t = 0; t < static_cast<int>( mesh.Triangles.size() ); ++t )
            {
                const glm::uvec3& tri = mesh.Triangles[t];
                glm::dvec3        lo =
                     glm::min( glm::min( mesh.Positions[tri.x], mesh.Positions[tri.y] ), mesh.Positions[tri.z] );
                glm::dvec3 hi =
                     glm::max( glm::max( mesh.Positions[tri.x], mesh.Positions[tri.y] ), mesh.Positions[tri.z] );
                if ( glm::any( glm::greaterThan( lo, reachHi ) ) || glm::any( glm::lessThan( hi, reachLo ) ) )
                    continue;
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

        uint8_t Quantize( const double localDistance, const double localToVolume, const glm::dvec2 scaleBias )
        {
            const double volumeDistance = localDistance * localToVolume;
            const double rescaled       = ( volumeDistance - scaleBias.y ) / scaleBias.x;
            return static_cast<uint8_t>( std::clamp( std::floor( rescaled * 255.0 + 0.5 ), 0.0, 255.0 ) );
        }
        // ONE VOLUME over the box [lo, hi] of mesh space, mip0Dims bricks along each axis (UE
        // GenerateSignedDistanceFieldVolumeData from the bounds on): per mip the lattice, its band, the sign by
        // rays, the bricks that are not saturated, the volume -> virtual UV transform.
        MeshDistanceFieldVolume BuildVolume( const SdfMesh& mesh, const Geometry::TriangleBvh& bvh,
                                             const std::vector<glm::dvec3>& directions, const glm::dvec3& lo,
                                             const glm::dvec3& hi, const glm::ivec3& mip0Dims )
        {
            const glm::dvec3        extent        = ( hi - lo ) * 0.5;
            const glm::dvec3        size          = hi - lo;
            const double            localToVolume = 1.0 / std::max( { extent.x, extent.y, extent.z } );
            MeshDistanceFieldVolume out;
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
                const double maxEncoding = glm::length( voxel * localToVolume ) * kDistanceFieldBandSizeInVoxels;
                const double trace       = maxEncoding / localToVolume;
                const glm::dvec2 scaleBias( 2.0 * maxEncoding, -maxEncoding );

                SdfLattice lattice;
                lattice.N           = dims * kUnique + 1;
                lattice.Origin      = volumeMin;
                lattice.Voxel       = voxel;
                const size_t points = static_cast<size_t>( lattice.N.x ) * lattice.N.y * lattice.N.z;
                lattice.Distance.assign( points, static_cast<float>( trace ) );
                lattice.Closest.assign( points, -1 );
                ExactBand( mesh, lattice, trace );
                for ( const int dk : { 1, -1 } )
                    for ( const int dj : { 1, -1 } )
                        for ( const int di : { 1, -1 } )
                            Sweep( mesh, lattice, di, dj, dk, trace );
                lattice.Inside.assign( points, 0 );
                for ( int k = 0; k < lattice.N.z; ++k )
                    for ( int j = 0; j < lattice.N.y; ++j )
                        for ( int i = 0; i < lattice.N.x; ++i )
                        {
                            // UE traces only where the closest surface lies within the trace distance
                            // (bTraceRays).
                            const size_t idx = lattice.Index( i, j, k );
                            if ( lattice.Closest[idx] >= 0 )
                                lattice.Inside[idx] =
                                     DfInsideByRays( mesh, bvh, directions, lattice.Point( i, j, k ), trace ) ? 1
                                                                                                              : 0;
                        }

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
                                        const double  sign = lattice.Inside[idx] != 0 ? -1.0 : 1.0;
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
                const glm::dvec3 uvMin = glm::dvec3( kBorder ) / glm::dvec3( dims * kUnique );
                const glm::dvec3 uvSize =
                     glm::dvec3( dims * kUnique - 2 * kBorder ) / glm::dvec3( dims * kUnique );
                const glm::dvec3 uvScale   = uvSize / ( 2.0 * volumeExtent );
                mip.VolumeToVirtualUVScale = glm::vec3( uvScale );
                mip.VolumeToVirtualUVAdd   = glm::vec3( volumeExtent * uvScale + uvMin );
            }
            return out;
        }
    } // namespace

    Common::ResultStr<MeshDistanceFieldData>
    BuildMeshDistanceField( const MeshDistanceFieldBuildInput&    input,
                            const MeshDistanceFieldBuildSettings& settings )
    {
        if ( input.Indices.size() % 3 != 0 )
            return Common::MakeFormattedError<MeshDistanceFieldData>(
                 "distance field: {} indices are not whole triangles", input.Indices.size() );
        if ( input.TriangleTwoSided.size() != input.Indices.size() / 3 )
            return Common::MakeFormattedError<MeshDistanceFieldData>(
                 "distance field: {} two-sided flags for {} triangles", input.TriangleTwoSided.size(),
                 input.Indices.size() / 3 );
        if ( !( settings.VoxelDensity > 0.0f ) || settings.MaxPerMeshResolution < kDistanceFieldUniqueBrickSize ||
             !( settings.ResolutionScale > 0.0f ) || !( settings.ChunkVoxelSize > 0.0f ) )
            return Common::MakeFormattedError<MeshDistanceFieldData>(
                 "distance field: settings out of range (density {}, max resolution {}, scale {}, chunk voxel {})",
                 settings.VoxelDensity, settings.MaxPerMeshResolution, settings.ResolutionScale,
                 settings.ChunkVoxelSize );

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
            mesh.TwoSided.push_back( input.TriangleTwoSided[i / 3] );
        }

        MeshDistanceFieldData out;
        if ( mesh.Triangles.empty() )
            return Common::MakeSuccess( std::move( out ) ); // no surface: no volume, nothing to sample

        glm::dvec3 meshLo( std::numeric_limits<double>::max() ), meshHi( std::numeric_limits<double>::lowest() );
        for ( const glm::uvec3& tri : mesh.Triangles )
            for ( int c = 0; c < 3; ++c )
            {
                meshLo = glm::min( meshLo, mesh.Positions[tri[c]] );
                meshHi = glm::max( meshHi, mesh.Positions[tri[c]] );
            }
        // Positive extents, so a plane still has a volume (UE: extent at least one unit).
        const auto positiveBox = []( glm::dvec3& lo, glm::dvec3& hi )
        {
            const glm::dvec3 centre = ( lo + hi ) * 0.5;
            const glm::dvec3 extent = glm::max( ( hi - lo ) * 0.5, glm::dvec3( 1.0 ) );
            lo                      = centre - extent;
            hi                      = centre + extent;
        };
        glm::dvec3 lo = meshLo, hi = meshHi;
        positiveBox( lo, hi );
        const glm::dvec3 size = hi - lo;

        constexpr int  kUnique = static_cast<int>( kDistanceFieldUniqueBrickSize );
        constexpr int  kBorder = static_cast<int>( kDistanceFieldObjectBorder );
        const uint32_t perMeshMax =
             settings.ResolutionScale <= 1.0f ? settings.MaxPerMeshResolution / 2 : settings.MaxPerMeshResolution;
        const int maxBlocks =
             std::max( 1, static_cast<int>( std::min<uint32_t>(
                               ( perMeshMax + kDistanceFieldUniqueBrickSize / 2 ) / kDistanceFieldUniqueBrickSize,
                               kDistanceFieldMaxIndirectionDim - 1 ) ) );
        const double     voxelsPerUnit = static_cast<double>( settings.VoxelDensity ) * settings.ResolutionScale;
        const glm::dvec3 desired       = size * voxelsPerUnit / static_cast<double>( kUnique );
        const glm::ivec3 mip0Dims =
             glm::clamp( glm::ivec3( glm::round( desired ) ), glm::ivec3( 1 ), glm::ivec3( maxBlocks ) );

        const Geometry::TriangleBvh   bvh        = Geometry::TriangleBvh::Build( mesh.Positions, mesh.Triangles );
        const std::vector<glm::dvec3> directions = SignDirections();

        // The voxel the cap leaves this mesh (the coarsest axis) against its thinnest extent: a slab thinner than
        // its voxel has no sample inside it, so its field never crosses zero and rays pass through it.
        const glm::dvec3 cappedVoxel = size / glm::dvec3( mip0Dims * kUnique - 2 * kBorder );
        const double     coarsest    = std::max( { cappedVoxel.x, cappedVoxel.y, cappedVoxel.z } );
        const glm::dvec3 meshSize    = meshHi - meshLo;
        const double     thinnest    = std::min( { meshSize.x, meshSize.y, meshSize.z } );
        const double chunkVoxel = std::max( static_cast<double>( settings.ChunkVoxelSize ), 1.0 / voxelsPerUnit );
        if ( !( coarsest > thinnest && coarsest > chunkVoxel ) )
        {
            out.Volumes.push_back( BuildVolume( mesh, bvh, directions, lo, hi, mip0Dims ) );
            return Common::MakeSuccess( std::move( out ) );
        }

        // SPATIAL VOLUMES: a grid of cells over the mesh box; a cell's volume is the part of the mesh box its
        // triangles' boxes cover within it, grown by the inside pad on every side, and a cell no triangle reaches
        // has none. Every volume is built from the whole mesh (ExactBand takes the triangles within its reach), so
        // neighbouring volumes agree across their common face.
        // THE INSIDE PAD: these meshes are thin by construction, and a cell's triangle box around a flat sheet is
        // a centimetre or two deep. Past its box a volume reads as the face sample plus the distance to the box
        // and never negative (MeshDistanceField.glslh) - so a sheet's inside (its back, within the band) existed
        // only within that centimetre and the field climbed again right under it. The global clipmap samples it
        // at voxel centres on both sides, both positive: across the sheet its trilinear field stayed as high as
        // the trace's surface expansion (half a voxel), rays stepped through the sheet in rings set by their step
        // phase and stopped on the slope beneath it, normal down (the 8 m view, GLOBAL-SDF-10). Two chunk voxels
        // of stored inside under a sheet keep that reconstruction at most ~12 cm in clipmap 0's 39 cm voxels
        // (expansion 19.5 cm) and the central difference below the hit pointing out of the sheet; a deeper pad
        // (the whole band, ~50 cm) triples the bricks of a sheet (three bricks deep instead of one) and the build.
        // The cell shrinks by the pad so a padded volume still fits the cap at the chunk voxel (a cap too small
        // for even the pad keeps one chunk voxel of cell, and its volumes' voxels grow past the chunk voxel).
        const double insidePad = 2.0 * chunkVoxel;
        const double cell      = std::max(
             static_cast<double>( maxBlocks * kUnique - 2 * kBorder ) * chunkVoxel - 2.0 * insidePad, chunkVoxel );
        const glm::ivec3        cells  = glm::max( glm::ivec3( glm::ceil( meshSize / cell ) ), glm::ivec3( 1 ) );
        const size_t            nCells = static_cast<size_t>( cells.x ) * cells.y * cells.z;
        std::vector<glm::dvec3> cellLo( nCells, glm::dvec3( std::numeric_limits<double>::max() ) );
        std::vector<glm::dvec3> cellHi( nCells, glm::dvec3( std::numeric_limits<double>::lowest() ) );
        for ( const glm::uvec3& tri : mesh.Triangles )
        {
            const glm::dvec3 tLo =
                 glm::min( glm::min( mesh.Positions[tri.x], mesh.Positions[tri.y] ), mesh.Positions[tri.z] );
            const glm::dvec3 tHi =
                 glm::max( glm::max( mesh.Positions[tri.x], mesh.Positions[tri.y] ), mesh.Positions[tri.z] );
            const glm::ivec3 c0 =
                 glm::clamp( glm::ivec3( glm::floor( ( tLo - meshLo ) / cell ) ), glm::ivec3( 0 ), cells - 1 );
            const glm::ivec3 c1 =
                 glm::clamp( glm::ivec3( glm::floor( ( tHi - meshLo ) / cell ) ), glm::ivec3( 0 ), cells - 1 );
            for ( int z = c0.z; z <= c1.z; ++z )
                for ( int y = c0.y; y <= c1.y; ++y )
                    for ( int x = c0.x; x <= c1.x; ++x )
                    {
                        const size_t     c     = ( static_cast<size_t>( z ) * cells.y + y ) * cells.x + x;
                        const glm::dvec3 boxLo = meshLo + glm::dvec3( x, y, z ) * cell;
                        cellLo[c]              = glm::min( cellLo[c], glm::max( tLo, boxLo ) );
                        cellHi[c]              = glm::max( cellHi[c], glm::min( tHi, boxLo + cell ) );
                    }
        }
        for ( size_t c = 0; c < nCells; ++c )
        {
            if ( cellLo[c].x > cellHi[c].x )
                continue; // no triangle reaches this cell
            const glm::dvec3 vLo = cellLo[c] - insidePad, vHi = cellHi[c] + insidePad;
            // Enough bricks that no axis' voxel is coarser than the chunk voxel (rounding up, unlike the single
            // volume's UE rounding - the chunk voxel is the guarantee these volumes exist for).
            const glm::dvec3 voxels = ( vHi - vLo ) / chunkVoxel + 2.0 * kBorder;
            const glm::ivec3 dims = glm::clamp( glm::ivec3( glm::ceil( voxels / static_cast<double>( kUnique ) ) ),
                                                glm::ivec3( 1 ), glm::ivec3( maxBlocks ) );
            out.Volumes.push_back( BuildVolume( mesh, bvh, directions, vLo, vHi, dims ) );
        }
        return Common::MakeSuccess( std::move( out ) );
    }
} // namespace Desert::Assets
