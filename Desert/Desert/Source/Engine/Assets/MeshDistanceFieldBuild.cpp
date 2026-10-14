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

#include <algorithm>
#include <cmath>
#include <cstring>
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
        // counts as a hit but never as a backface (UE IsHitTwoSided). Embree is replaced by a triangle BVH below.

        // UE FRandomStream (seed 0): MutateSeed, then a float in [0, 1) from the seed's top 23 bits.
        struct UeRandomStream
        {
            uint32_t Seed = 0;
            float    GetFraction()
            {
                Seed                  = Seed * 196314165u + 907633515u;
                const uint32_t bits   = 0x3F800000u | ( Seed >> 9 );
                float          result = 0.0f;
                std::memcpy( &result, &bits, sizeof( result ) );
                return result - 1.0f;
            }
        };

        // UE MeshUtilities::GenerateStratifiedUniformHemisphereSamples(49) twice, the second set mirrored in Z
        // (GenerateSignedDistanceFieldVolumeData, bUsePointQuery): 7 x 7 jittered strata per hemisphere, each
        // mapped by UniformSampleHemisphere (phi = 2 pi u, cos theta = v).
        constexpr int           kDfSignSamplesPerHemisphere = 49;
        std::vector<glm::dvec3> SignDirections()
        {
            const int dim = static_cast<int>( std::sqrt( static_cast<float>( kDfSignSamplesPerHemisphere ) ) );
            UeRandomStream          random;
            std::vector<glm::dvec3> directions;
            for ( const double zSign : { 1.0, -1.0 } )
                for ( int x = 0; x < dim; ++x )
                    for ( int y = 0; y < dim; ++y )
                    {
                        const float  u1 = random.GetFraction();
                        const float  u2 = random.GetFraction();
                        const double phi =
                             2.0 * 3.14159265358979323846 * ( ( x + u1 ) / static_cast<float>( dim ) );
                        const double cosTheta = ( y + u2 ) / static_cast<float>( dim );
                        const double sinTheta = std::sqrt( std::max( 0.0, 1.0 - cosTheta * cosTheta ) );
                        directions.emplace_back( std::cos( phi ) * sinTheta, std::sin( phi ) * sinTheta,
                                                 zSign * cosTheta );
                    }
            return directions;
        }

        // A bounding-volume hierarchy over the triangle soup (median split on the widest centroid axis, up to
        // kDfLeafTriangles per leaf): what Embree's scene is to UE's builder. Closest hit only.
        constexpr int kDfLeafTriangles = 4;
        struct DfBvhNode
        {
            double Lo[3];
            double Hi[3];
            int    First = 0; // leaf: first entry in Order; inner: index of the left child (right = left + 1)
            int    Count = 0; // > 0: leaf
        };

        struct DfBvh
        {
            std::vector<DfBvhNode>  Nodes;
            std::vector<int>        Order;
            std::vector<glm::dvec3> A, E1, E2, Normal; // per triangle: vertex a, b - a, c - a, (b - a) x (c - a)
            // The leaves' triangles in leaf order, 9 doubles each (a, b - a, c - a): the ray loop reads them as
            // plain scalars - it runs ~100 times per band sample and the editor's builds are unoptimised.
            std::vector<double> Packed;
        };

        void DfBvhSplit( DfBvh& bvh, const std::vector<glm::dvec3>& centroids, const int node, const int first,
                         const int count )
        {
            glm::dvec3 lo( std::numeric_limits<double>::max() ), hi( std::numeric_limits<double>::lowest() );
            glm::dvec3 clo = lo, chi = hi;
            for ( int i = first; i < first + count; ++i )
            {
                const int t = bvh.Order[i];
                for ( const glm::dvec3& p : { bvh.A[t], bvh.A[t] + bvh.E1[t], bvh.A[t] + bvh.E2[t] } )
                {
                    lo = glm::min( lo, p );
                    hi = glm::max( hi, p );
                }
                clo = glm::min( clo, centroids[t] );
                chi = glm::max( chi, centroids[t] );
            }
            for ( int c = 0; c < 3; ++c )
            {
                bvh.Nodes[node].Lo[c] = lo[c];
                bvh.Nodes[node].Hi[c] = hi[c];
            }
            const glm::dvec3 spread = chi - clo;
            const int axis = spread.x >= spread.y && spread.x >= spread.z ? 0 : ( spread.y >= spread.z ? 1 : 2 );
            if ( count <= kDfLeafTriangles || spread[axis] <= 0.0 )
            {
                bvh.Nodes[node].First = first;
                bvh.Nodes[node].Count = count;
                return;
            }
            const int half = count / 2;
            std::nth_element( bvh.Order.begin() + first, bvh.Order.begin() + first + half,
                              bvh.Order.begin() + first + count, [&]( const int l, const int r )
                              { return centroids[l][axis] < centroids[r][axis]; } );
            const int left = static_cast<int>( bvh.Nodes.size() );
            bvh.Nodes.resize( bvh.Nodes.size() + 2 );
            bvh.Nodes[node].First = left;
            bvh.Nodes[node].Count = 0;
            DfBvhSplit( bvh, centroids, left, first, half );
            DfBvhSplit( bvh, centroids, left + 1, first + half, count - half );
        }

        DfBvh DfBvhBuild( const SdfMesh& mesh )
        {
            DfBvh        bvh;
            const size_t n = mesh.Triangles.size();
            bvh.A.resize( n );
            bvh.E1.resize( n );
            bvh.E2.resize( n );
            bvh.Normal.resize( n );
            bvh.Order.resize( n );
            std::vector<glm::dvec3> centroids( n );
            for ( size_t t = 0; t < n; ++t )
            {
                const glm::uvec3& tri = mesh.Triangles[t];
                bvh.A[t]              = mesh.Positions[tri.x];
                bvh.E1[t]             = mesh.Positions[tri.y] - bvh.A[t];
                bvh.E2[t]             = mesh.Positions[tri.z] - bvh.A[t];
                bvh.Normal[t]         = glm::cross( bvh.E1[t], bvh.E2[t] );
                centroids[t]          = bvh.A[t] + ( bvh.E1[t] + bvh.E2[t] ) / 3.0;
                bvh.Order[t]          = static_cast<int>( t );
            }
            bvh.Nodes.reserve( 2 * n / kDfLeafTriangles + 2 );
            bvh.Nodes.resize( 1 );
            DfBvhSplit( bvh, centroids, 0, 0, static_cast<int>( n ) );
            bvh.Packed.resize( n * 9 );
            for ( size_t i = 0; i < n; ++i )
            {
                const int t = bvh.Order[i];
                for ( int c = 0; c < 3; ++c )
                {
                    bvh.Packed[i * 9 + c]     = bvh.A[t][c];
                    bvh.Packed[i * 9 + 3 + c] = bvh.E1[t][c];
                    bvh.Packed[i * 9 + 6 + c] = bvh.E2[t][c];
                }
            }
            return bvh;
        }

        // Slab test: the entry distance of the ray into the node's box, or a negative value when it misses within
        // [0, tMax].
        double DfBoxEntry( const DfBvhNode& node, const double o[3], const double inv[3], const double tMax )
        {
            double t0 = 0.0, t1 = tMax;
            for ( int c = 0; c < 3; ++c )
            {
                const double l = ( node.Lo[c] - o[c] ) * inv[c];
                const double h = ( node.Hi[c] - o[c] ) * inv[c];
                const double n = l < h ? l : h;
                const double f = l < h ? h : l;
                t0             = n > t0 ? n : t0;
                t1             = f < t1 ? f : t1;
                if ( t0 > t1 )
                    return -1.0;
            }
            return t0;
        }

        // The closest triangle the ray o + t d, t in [0, tMax], crosses from either side (Moller-Trumbore), or -1.
        int DfClosestHit( const DfBvh& bvh, const double o[3], const double d[3], const double tMax )
        {
            const double inv[3] = { 1.0 / d[0], 1.0 / d[1], 1.0 / d[2] };
            double       best   = tMax;
            int          hit    = -1;
            int          stack[64];
            int          top = 0;
            if ( DfBoxEntry( bvh.Nodes[0], o, inv, best ) < 0.0 )
                return -1;
            stack[top++] = 0;
            while ( top > 0 )
            {
                const DfBvhNode& node = bvh.Nodes[stack[--top]];
                if ( node.Count > 0 )
                {
                    const double* tri = bvh.Packed.data() + static_cast<size_t>( node.First ) * 9;
                    for ( int i = 0; i < node.Count; ++i, tri += 9 )
                    {
                        const double* a  = tri;
                        const double* e1 = tri + 3;
                        const double* e2 = tri + 6;
                        // p = d x e2, det = e1 . p
                        const double px  = d[1] * e2[2] - d[2] * e2[1];
                        const double py  = d[2] * e2[0] - d[0] * e2[2];
                        const double pz  = d[0] * e2[1] - d[1] * e2[0];
                        const double det = e1[0] * px + e1[1] * py + e1[2] * pz;
                        if ( det == 0.0 )
                            continue;
                        const double invDet = 1.0 / det;
                        const double sx = o[0] - a[0], sy = o[1] - a[1], sz = o[2] - a[2];
                        const double u = ( sx * px + sy * py + sz * pz ) * invDet;
                        if ( u < 0.0 || u > 1.0 )
                            continue;
                        // q = s x e1
                        const double qx = sy * e1[2] - sz * e1[1];
                        const double qy = sz * e1[0] - sx * e1[2];
                        const double qz = sx * e1[1] - sy * e1[0];
                        const double v  = ( d[0] * qx + d[1] * qy + d[2] * qz ) * invDet;
                        if ( v < 0.0 || u + v > 1.0 )
                            continue;
                        const double tHit = ( e2[0] * qx + e2[1] * qy + e2[2] * qz ) * invDet;
                        if ( tHit >= 0.0 && tHit <= best )
                        {
                            best = tHit;
                            hit  = bvh.Order[node.First + i];
                        }
                    }
                    continue;
                }
                const double l = DfBoxEntry( bvh.Nodes[node.First], o, inv, best );
                const double r = DfBoxEntry( bvh.Nodes[node.First + 1], o, inv, best );
                // Nearer child on top of the stack, so a close hit shrinks the farther child's test.
                if ( l >= 0.0 && r >= 0.0 )
                {
                    stack[top++] = l <= r ? node.First + 1 : node.First;
                    stack[top++] = l <= r ? node.First : node.First + 1;
                }
                else if ( l >= 0.0 )
                    stack[top++] = node.First;
                else if ( r >= 0.0 )
                    stack[top++] = node.First + 1;
            }
            return hit;
        }

        // UE's vote for one sample: rays start pulled back by 1e-4 of the trace distance (a sample exactly on a
        // triangle - box corners - still sees it) and reach the trace distance. Inside when Hit > 0 and HitBack >
        // a quarter of the rays; the loop stops as soon as the remaining rays cannot change that outcome.
        bool DfInsideByRays( const SdfMesh& mesh, const DfBvh& bvh, const std::vector<glm::dvec3>& directions,
                             const glm::dvec3& p, const double trace )
        {
            const int total   = static_cast<int>( directions.size() );
            int       hitBack = 0;
            for ( int i = 0; i < total; ++i )
            {
                const glm::dvec3& dir  = directions[i];
                const double      d[3] = { dir.x, dir.y, dir.z };
                const double      pull = 1.0e-4 * trace;
                const double      o[3] = { p.x - pull * d[0], p.y - pull * d[1], p.z - pull * d[2] };
                const int         hit  = DfClosestHit( bvh, o, d, trace );
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
        if ( input.TriangleTwoSided.size() != input.Indices.size() / 3 )
            return Common::MakeFormattedError<MeshDistanceFieldData>(
                 "distance field: {} two-sided flags for {} triangles", input.TriangleTwoSided.size(),
                 input.Indices.size() / 3 );
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
            mesh.TwoSided.push_back( input.TriangleTwoSided[i / 3] );
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

        const DfBvh                   bvh        = DfBvhBuild( mesh );
        const std::vector<glm::dvec3> directions = SignDirections();
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
            ExactBand( mesh, lattice );
            for ( const int dk : { 1, -1 } )
                for ( const int dj : { 1, -1 } )
                    for ( const int di : { 1, -1 } )
                        Sweep( mesh, lattice, di, dj, dk, trace );
            lattice.Inside.assign( points, 0 );
            for ( int k = 0; k < lattice.N.z; ++k )
                for ( int j = 0; j < lattice.N.y; ++j )
                    for ( int i = 0; i < lattice.N.x; ++i )
                    {
                        // UE traces only where the closest surface lies within the trace distance (bTraceRays).
                        const size_t idx = lattice.Index( i, j, k );
                        if ( lattice.Closest[idx] >= 0 )
                            lattice.Inside[idx] =
                                 DfInsideByRays( mesh, bvh, directions, lattice.Point( i, j, k ), trace ) ? 1 : 0;
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
            const glm::dvec3 uvMin     = glm::dvec3( kBorder ) / glm::dvec3( dims * kUnique );
            const glm::dvec3 uvSize    = glm::dvec3( dims * kUnique - 2 * kBorder ) / glm::dvec3( dims * kUnique );
            const glm::dvec3 uvScale   = uvSize / ( 2.0 * volumeExtent );
            mip.VolumeToVirtualUVScale = glm::vec3( uvScale );
            mip.VolumeToVirtualUVAdd   = glm::vec3( volumeExtent * uvScale + uvMin );
        }
        return Common::MakeSuccess( std::move( out ) );
    }
} // namespace Desert::Assets
