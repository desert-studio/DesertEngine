// THE CARD BUILD - a port of UE's MeshCardRepresentationUtilities.cpp (MeshCardGen: InitClusteringParams,
// GenerateSurfelsForDirection, ComputeSurfelVisibility, BuildSurfelClusters, LimitClusters, SerializeLOD), with
// Embree replaced by Geometry/TriangleBvh.hpp and the voxel made per axis (MeshCardRepresentation.hpp, "WHERE THIS
// DIFFERS FROM UE"). Per direction the mesh box is cut into columns along the direction; rays walk each column
// from outside, every surface they meet facing back along the column spawns a SURFEL (a cell where the column's
// rays found that face, with the depth from which it is visible); surfels are then grown into the fewest boxes -
// CARDS - that cover them, the outermost first (near plane 0), then the best-covering inner ones.

#include <Engine/Assets/MeshCardRepresentation.hpp>
#include <Engine/Assets/StratifiedHemisphereSamples.hpp>
#include <Engine/Geometry/TriangleBvh.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace Desert::Assets
{
    namespace
    {
        // UE MeshUtilities::GenerateStratifiedUniformHemisphereSamples(32): trunc(sqrt(32)) = 5, 5 x 5 strata,
        // from a stream at seed 0.
        std::vector<glm::dvec3> HemisphereDirections( const int numSamples )
        {
            Common::Math::RandomStream random;
            std::vector<glm::dvec3>    out;
            AppendStratifiedUniformHemisphereSamples( numSamples, random, 1.0, out );
            return out;
        }

        // UE GetTangentBasisFrisvad: v.x * tangentX + v.y * tangentY + v.z * normal.
        glm::dvec3 FrisvadTransform( const glm::dvec3& n, const glm::dvec3& v )
        {
            glm::dvec3 tx, ty;
            if ( n.z < -0.9999999 )
            {
                tx = glm::dvec3( 0.0, -1.0, 0.0 );
                ty = glm::dvec3( -1.0, 0.0, 0.0 );
            }
            else
            {
                const double a = 1.0 / ( 1.0 + n.z );
                const double b = -n.x * n.y * a;
                tx             = glm::dvec3( 1.0 - n.x * n.x * a, b, -n.x );
                ty             = glm::dvec3( b, 1.0 - n.y * n.y * a, -n.y );
            }
            return v.x * tx + v.y * ty + v.z * n;
        }

        struct IntBox
        {
            glm::ivec3 Min{ std::numeric_limits<int>::max() };
            glm::ivec3 Max{ -std::numeric_limits<int>::max() };
            void       Add( const glm::ivec3& p )
            {
                Min = glm::min( Min, p );
                Max = glm::max( Max, p );
            }
            [[nodiscard]] int FaceArea() const
            {
                return ( Max.x + 1 - Min.x ) * ( Max.y + 1 - Min.y );
            }
            [[nodiscard]] glm::vec3 Center() const
            {
                return glm::vec3( Max + Min ) * 0.5f;
            }
        };

        struct Surfel
        {
            glm::ivec3 Coord{ 0 };
            int        MinRayZ          = 0;    // the card's nearest near plane that still sees this surfel
            float      Coverage         = 0.0f; // fraction of the column's rays that found it
            float      WeightedCoverage = 0.0f; // coverage weighted by its visibility from outside
        };

        // One direction's lattice: local x, y across the face, z along the ray (UE FAxisAlignedDirectionBasis,
        // with a voxel size per local axis).
        struct DirectionBasis
        {
            glm::dvec3 AxisX{ 0.0 }, AxisY{ 0.0 }, RayDirection{ 0.0 }; // mesh-space unit vectors
            glm::dvec3 Offset{ 0.0 };                                   // mesh-space origin of local (0,0,0)
            glm::ivec3 VolumeSize{ 1 };
            glm::dvec3 Voxel{ 1.0 }; // cm per local axis

            [[nodiscard]] glm::dvec3 ToMesh( const glm::dvec3& local ) const
            {
                return Offset + AxisX * ( local.x * Voxel.x ) + AxisY * ( local.y * Voxel.y ) +
                       RayDirection * ( local.z * Voxel.z );
            }
            [[nodiscard]] glm::dvec3 ToLocalCm( const glm::dvec3& mesh ) const
            {
                const glm::dvec3 p = mesh - Offset;
                return glm::dvec3( glm::dot( p, AxisX ), glm::dot( p, AxisY ), glm::dot( p, RayDirection ) );
            }
        };

        struct ClusteringParams
        {
            float                                       MinClusterCoverage      = 15.0f;
            float                                       MinOuterClusterCoverage = 0.0f;
            float                                       MinDensityPerCluster    = 0.0f;
            uint32_t                                    MaxCards                = kMaxCardsPerMesh;
            std::array<DirectionBasis, kCardDirections> Basis;
        };

        struct Cluster
        {
            IntBox           Bounds;
            std::vector<int> Surfels;
            int              NearPlane        = 0;
            float            Coverage         = 0.0f;
            float            WeightedCoverage = 0.0f;

            void Init( const int nearPlane )
            {
                *this     = Cluster{};
                NearPlane = nearPlane;
            }
            [[nodiscard]] float Density() const
            {
                return Coverage / static_cast<float>( Bounds.FaceArea() );
            }
            [[nodiscard]] bool IsValid( const ClusteringParams& params ) const
            {
                return WeightedCoverage >=
                            ( NearPlane == 0 ? params.MinOuterClusterCoverage : params.MinClusterCoverage ) &&
                       Density() > params.MinDensityPerCluster;
            }
            void AddSurfel( const std::vector<Surfel>& surfels, const int index )
            {
                const Surfel& s = surfels[index];
                if ( s.Coord.z >= NearPlane && s.MinRayZ <= NearPlane )
                {
                    Surfels.push_back( index );
                    Bounds.Add( s.Coord );
                    Coverage += s.Coverage;
                    WeightedCoverage += s.WeightedCoverage;
                }
            }
        };

        struct BuildMesh
        {
            std::vector<glm::dvec3> Positions;
            std::vector<glm::uvec3> Triangles;
            std::vector<uint8_t>    TwoSided;
            Geometry::TriangleBvh   Bvh;
        };

        glm::dvec3 AxisNormal( const uint32_t direction )
        {
            glm::dvec3 n( 0.0 );
            n[direction / 2] = ( direction & 1u ) != 0u ? 1.0 : -1.0;
            return n;
        }

        // UE InitClusteringParams with the per-axis voxel.
        void InitClusteringParams( ClusteringParams& params, const glm::dvec3& boundsMin,
                                   const glm::dvec3& boundsMax, const uint32_t maxVoxels,
                                   const MeshCardsBuildSettings& settings )
        {
            const glm::dvec3 size = boundsMax - boundsMin;
            glm::dvec3       voxel;
            glm::ivec3       sizeInVoxels;
            for ( int a = 0; a < 3; ++a )
            {
                const double thin = std::max<double>( settings.MinVoxelSize,
                                                      size[a] / std::max( 1u, settings.MinVoxelsAcrossThinAxis ) );
                voxel[a]          = std::max( std::min<double>( settings.TargetVoxelSize, thin ),
                                              size[a] / static_cast<double>( maxVoxels ) );
                sizeInVoxels[a]   = std::clamp( static_cast<int>( std::round( size[a] / voxel[a] ) ), 1,
                                                static_cast<int>( maxVoxels ) );
            }
            const glm::dvec3     center         = ( boundsMin + boundsMax ) * 0.5;
            const glm::dvec3     extent         = glm::dvec3( sizeInVoxels ) * voxel * 0.5;
            const glm::dvec3     voxelMin       = center - extent;
            const glm::dvec3     voxelMax       = center + extent;
            static constexpr int kInPlane[3][2] = { { 1, 2 }, { 0, 2 }, { 0, 1 } };
            for ( uint32_t d = 0; d < kCardDirections; ++d )
            {
                DirectionBasis& basis = params.Basis[d];
                const int       axis  = static_cast<int>( d / 2 );
                const int       ax    = kInPlane[axis][0];
                const int       ay    = kInPlane[axis][1];
                basis.AxisX           = glm::dvec3( 0.0 );
                basis.AxisY           = glm::dvec3( 0.0 );
                basis.AxisX[ax]       = 1.0;
                basis.AxisY[ay]       = 1.0;
                basis.RayDirection    = -AxisNormal( d );
                basis.Offset          = voxelMin;
                if ( ( d & 1u ) != 0u )
                    basis.Offset[axis] = voxelMax[axis];
                basis.VolumeSize = glm::ivec3( sizeInVoxels[ax], sizeInVoxels[ay], sizeInVoxels[axis] );
                basis.Voxel      = glm::dvec3( voxel[ax], voxel[ay], voxel[axis] );
            }
            const double averageFaceArea = 2.0 *
                                           ( sizeInVoxels.x * sizeInVoxels.y + sizeInVoxels.x * sizeInVoxels.z +
                                             sizeInVoxels.y * sizeInVoxels.z ) /
                                           6.0;
            params.MinDensityPerCluster = std::clamp( settings.MinDensity, 0.0f, 1.0f ) / 3.0f;
            params.MinClusterCoverage   = 15.0f;
            params.MinOuterClusterCoverage =
                 std::min( params.MinClusterCoverage, static_cast<float>( 0.5 * averageFaceArea ) );
            params.MaxCards = std::min( settings.MaxCards, kMaxCardsPerMesh );
        }

        struct SurfelSample
        {
            glm::dvec3 Position{ 0.0 };
            glm::dvec3 Normal{ 0.0 };
            int        MinRayZ = 0;
            int        CellZ   = 0;
        };

        // UE ComputeSurfelVisibility: rays over the hemisphere of the span's samples (cycled); a surfel whose rays
        // mostly hit back faces is inside the geometry and dropped.
        bool SurfelVisibility( const BuildMesh& mesh, const std::vector<SurfelSample>& samples,
                               const size_t offset, const size_t count, const std::vector<glm::dvec3>& hemisphere,
                               float& visibility )
        {
            size_t       sampleIndex = 0;
            int          hits = 0, backHits = 0;
            double       visible         = 0.0;
            const double kSurfaceRayBias = 0.1;
            for ( const glm::dvec3& local : hemisphere )
            {
                const SurfelSample& s    = samples[offset + sampleIndex];
                const glm::dvec3    dir  = FrisvadTransform( s.Normal, local );
                const double        o[3] = { s.Position.x, s.Position.y, s.Position.z };
                const double        d[3] = { dir.x, dir.y, dir.z };
                const auto hit = mesh.Bvh.ClosestHit( o, d, kSurfaceRayBias, std::numeric_limits<double>::max() );
                if ( hit.Triangle >= 0 )
                {
                    ++hits;
                    if ( glm::dot( dir, mesh.Bvh.Normal[hit.Triangle] ) > 0.0 && mesh.TwoSided[hit.Triangle] == 0 )
                        ++backHits;
                }
                else
                    visible += 1.0;
                sampleIndex = ( sampleIndex + 1 ) % count;
            }
            const double n = static_cast<double>( hemisphere.size() );
            visibility     = static_cast<float>( visible / n );
            return !( hits > 0.8 * n && backHits > 0.2 * n );
        }

        // UE GenerateSurfelsForDirection.
        void GenerateSurfels( const BuildMesh& mesh, const DirectionBasis& basis,
                              const std::vector<glm::dvec3>& hemisphere, const float normalThreshold,
                              std::vector<Surfel>& out )
        {
            constexpr uint32_t kSamples          = 32;
            constexpr uint32_t kMinSurfelSamples = 1;
            const glm::dvec3   rayDir            = basis.RayDirection;
            const double       nearPlaneOffset   = 2.0 * basis.Voxel.z;
            const int          volumeZ           = basis.VolumeSize.z;

            std::vector<SurfelSample> samples;
            std::vector<uint32_t>     perCell( volumeZ ), cellOffset( volumeZ );
            for ( int cy = 0; cy < basis.VolumeSize.y; ++cy )
                for ( int cx = 0; cx < basis.VolumeSize.x; ++cx )
                {
                    samples.clear();
                    std::fill( perCell.begin(), perCell.end(), 0u );
                    std::fill( cellOffset.begin(), cellOffset.end(), 0u );
                    for ( uint32_t si = 0; si < kSamples; ++si )
                    {
                        uint32_t reversed = si;
                        reversed = ( ( reversed >> 1 ) & 0x55555555u ) | ( ( reversed & 0x55555555u ) << 1 );
                        reversed = ( ( reversed >> 2 ) & 0x33333333u ) | ( ( reversed & 0x33333333u ) << 2 );
                        reversed = ( ( reversed >> 4 ) & 0x0F0F0F0Fu ) | ( ( reversed & 0x0F0F0F0Fu ) << 4 );
                        reversed = ( ( reversed >> 8 ) & 0x00FF00FFu ) | ( ( reversed & 0x00FF00FFu ) << 8 );
                        reversed = ( reversed >> 16 ) | ( reversed << 16 );
                        const double jx = ( si + 0.5 ) / kSamples;
                        const double jy = static_cast<double>( reversed ) / 4294967296.0;
                        // Pulled back so the ray starts outside the geometry (voxels may be smaller than the
                        // mesh).
                        const glm::dvec3 origin =
                             basis.ToMesh( glm::dvec3( cx + jx, cy + jy, 0.0 ) ) - rayDir * nearPlaneOffset;
                        const double o[3]     = { origin.x, origin.y, origin.z };
                        const double d[3]     = { rayDir.x, rayDir.y, rayDir.z };
                        int          lastHitZ = -2;
                        int          skip     = -1;
                        double       tNear    = 0.0;
                        while ( lastHitZ + 1 < volumeZ )
                        {
                            const auto hit =
                                 mesh.Bvh.ClosestHit( o, d, tNear, std::numeric_limits<double>::max(), skip );
                            if ( hit.Triangle < 0 )
                                break;
                            const int  hitZ   = static_cast<int>( std::clamp(
                                 ( hit.T - nearPlaneOffset ) / basis.Voxel.z, 0.0, double( volumeZ - 1 ) ) );
                            glm::dvec3 normal = glm::normalize( mesh.Bvh.Normal[hit.Triangle] );
                            double     nDotD  = glm::dot( -rayDir, normal );
                            if ( nDotD < 0.0 && mesh.TwoSided[hit.Triangle] != 0 )
                            {
                                nDotD  = -nDotD;
                                normal = -normal;
                            }
                            if ( nDotD >= normalThreshold && hitZ > lastHitZ + 1 )
                            {
                                SurfelSample& s = samples.emplace_back();
                                s.Position      = origin + rayDir * hit.T;
                                s.Normal        = normal;
                                s.CellZ         = hitZ;
                                s.MinRayZ       = lastHitZ >= 0 ? lastHitZ + 1 : 0;
                            }
                            lastHitZ = hitZ;
                            tNear    = std::nextafter(
                                 std::max( nearPlaneOffset + ( lastHitZ + 1 ) * basis.Voxel.z, hit.T ),
                                 std::numeric_limits<double>::infinity() );
                            skip = hit.Triangle;
                        }
                    }

                    std::sort( samples.begin(), samples.end(),
                               []( const SurfelSample& a, const SurfelSample& b )
                               {
                                   if ( a.CellZ != b.CellZ )
                                       return a.CellZ < b.CellZ;
                                   if ( a.MinRayZ != b.MinRayZ )
                                       return a.MinRayZ > b.MinRayZ;
                                   if ( a.Position.x != b.Position.x )
                                       return a.Position.x < b.Position.x;
                                   if ( a.Position.y != b.Position.y )
                                       return a.Position.y < b.Position.y;
                                   return a.Position.z < b.Position.z;
                               } );
                    for ( const SurfelSample& s : samples )
                        ++perCell[s.CellZ];
                    for ( int z = 1; z < volumeZ; ++z )
                        cellOffset[z] = cellOffset[z - 1] + perCell[z - 1];

                    for ( int z = 0; z < volumeZ; ++z )
                    {
                        const uint32_t cellCount = perCell[z];
                        const uint32_t cellOffs  = cellOffset[z];
                        uint32_t       spanBegin = 0;
                        // Continuous spans of equal MinRayZ each spawn one surfel. UE compares the samples at
                        // SurfelSamples[SampleIndex] without the cell's offset (cell 0's samples for every cell);
                        // here within the cell, as intended.
                        while ( spanBegin + 1 < cellCount )
                        {
                            uint32_t spanSize = 0;
                            for ( uint32_t i = spanBegin; i < cellCount; ++i )
                            {
                                if ( samples[cellOffs + i].MinRayZ != samples[cellOffs + spanBegin].MinRayZ )
                                    break;
                                ++spanSize;
                            }
                            if ( spanSize >= kMinSurfelSamples )
                            {
                                float      visibility = 0.0f;
                                const bool valid = SurfelVisibility( mesh, samples, cellOffs + spanBegin, spanSize,
                                                                     hemisphere, visibility );
                                const float coverage =
                                     static_cast<float>( spanSize ) / static_cast<float>( kSamples );
                                if ( valid )
                                {
                                    Surfel& surfel          = out.emplace_back();
                                    surfel.Coord            = glm::ivec3( cx, cy, z );
                                    surfel.MinRayZ          = samples[cellOffs + spanBegin].MinRayZ;
                                    surfel.Coverage         = coverage;
                                    surfel.WeightedCoverage = coverage * ( visibility + 1.0f );
                                }
                            }
                            spanBegin += spanSize;
                        }
                    }
                }
        }

        void BuildCluster( const int nearPlane, const std::vector<Surfel>& surfels,
                           const std::vector<uint8_t>& assigned, Cluster& cluster )
        {
            cluster.Init( nearPlane );
            for ( int i = 0; i < static_cast<int>( surfels.size() ); ++i )
                if ( assigned[i] == 0 )
                    cluster.AddSurfel( surfels, i );
        }

        void CommitCluster( std::vector<Cluster>& clusters, std::vector<uint8_t>& assigned,
                            const Cluster& cluster )
        {
            for ( const int i : cluster.Surfels )
                assigned[i] = 1;
            clusters.push_back( cluster );
        }
    } // namespace

    Common::ResultStr<MeshCardsBuildData> BuildMeshCards( const MeshDistanceFieldBuildInput& input,
                                                          const MeshCardsBuildSettings&      settings )
    {
        if ( input.Indices.size() % 3 != 0 || input.TriangleTwoSided.size() != input.Indices.size() / 3 )
            return Common::MakeError<MeshCardsBuildData>( "card build input: indices and sidedness disagree" );

        BuildMesh mesh;
        mesh.Positions.reserve( input.Positions.size() );
        glm::dvec3 lo( std::numeric_limits<double>::max() ), hi( std::numeric_limits<double>::lowest() );
        for ( const glm::vec3& p : input.Positions )
            mesh.Positions.emplace_back( p );
        size_t twoSidedCount = 0;
        for ( size_t i = 0; i < input.Indices.size(); i += 3 )
        {
            const glm::uvec3 tri( input.Indices[i], input.Indices[i + 1], input.Indices[i + 2] );
            if ( tri.x >= mesh.Positions.size() || tri.y >= mesh.Positions.size() ||
                 tri.z >= mesh.Positions.size() )
                return Common::MakeError<MeshCardsBuildData>( "card build input: index out of range" );
            mesh.Triangles.push_back( tri );
            mesh.TwoSided.push_back( input.TriangleTwoSided[i / 3] );
            twoSidedCount += input.TriangleTwoSided[i / 3] != 0 ? 1 : 0;
            for ( int c = 0; c < 3; ++c )
            {
                lo = glm::min( lo, mesh.Positions[tri[c]] );
                hi = glm::max( hi, mesh.Positions[tri[c]] );
            }
        }

        MeshCardsBuildData out;
        if ( mesh.Triangles.empty() )
            return Common::MakeSuccess( out );
        mesh.Bvh = Geometry::TriangleBvh::Build( mesh.Positions, mesh.Triangles );
        // UE FEmbreeScene: SectionNumTwoSidedTriangles * 4 >= SectionNumTriangles.
        out.MostlyTwoSided = twoSidedCount * 4 >= mesh.Triangles.size();

        // UE BuildMeshCards: a box never empty (infinitely thin planes), one centimetre of margin.
        const glm::dvec3 center    = ( lo + hi ) * 0.5;
        const glm::dvec3 extent    = glm::max( ( hi - lo ) * 0.5 + 1.0, glm::dvec3( 1.0 ) );
        const glm::dvec3 boundsMin = center - extent;
        const glm::dvec3 boundsMax = center + extent;
        out.BoundsMin              = glm::vec3( boundsMin );
        out.BoundsMax              = glm::vec3( boundsMax );

        // UE InitSurfelScene: halve the lattice while the surfels exceed the budget.
        const std::vector<glm::dvec3>                    hemisphere = HemisphereDirections( 32 );
        ClusteringParams                                 params;
        std::array<std::vector<Surfel>, kCardDirections> surfels;
        uint32_t                                         maxVoxels = std::max( 1u, settings.MaxVoxels );
        for ( ;; )
        {
            InitClusteringParams( params, boundsMin, boundsMax, maxVoxels, settings );
            size_t total = 0;
            for ( uint32_t d = 0; d < kCardDirections; ++d )
            {
                surfels[d].clear();
                GenerateSurfels( mesh, params.Basis[d], hemisphere,
                                 std::clamp( settings.NormalThreshold, 0.0f, 1.0f ), surfels[d] );
                total += surfels[d].size();
            }
            maxVoxels /= 2;
            if ( total <= settings.TargetSurfels || maxVoxels <= 1 )
                break;
        }

        // UE BuildSurfelClusters.
        std::array<std::vector<Cluster>, kCardDirections> clusters;
        for ( uint32_t d = 0; d < kCardDirections; ++d )
        {
            std::vector<uint8_t> assigned( surfels[d].size(), 0 );
            Cluster              temp;
            BuildCluster( 0, surfels[d], assigned, temp );
            if ( temp.IsValid( params ) )
                CommitCluster( clusters[d], assigned, temp );
            // Two-sided is assumed foliage: the simpler box projection only.
            if ( out.MostlyTwoSided )
                continue;
            for ( ;; )
            {
                Cluster best;
                best.Init( -1 );
                for ( int nearPlane = 1; nearPlane < params.Basis[d].VolumeSize.z; ++nearPlane )
                {
                    BuildCluster( nearPlane, surfels[d], assigned, temp );
                    if ( temp.IsValid( params ) && temp.WeightedCoverage > best.WeightedCoverage )
                        best = temp;
                }
                if ( !best.IsValid( params ) )
                    break;
                CommitCluster( clusters[d], assigned, best );
            }
        }

        // UE LimitClusters.
        size_t clusterCount = 0;
        for ( auto& list : clusters )
        {
            std::sort( list.begin(), list.end(),
                       []( const Cluster& a, const Cluster& b )
                       {
                           if ( a.WeightedCoverage != b.WeightedCoverage )
                               return a.WeightedCoverage > b.WeightedCoverage;
                           const glm::vec3 ca = a.Bounds.Center(), cb = b.Bounds.Center();
                           if ( ca.x != cb.x )
                               return ca.x < cb.x;
                           if ( ca.y != cb.y )
                               return ca.y < cb.y;
                           return ca.z < cb.z;
                       } );
            clusterCount += list.size();
        }
        while ( clusterCount > params.MaxCards )
        {
            float    smallest  = std::numeric_limits<float>::max();
            uint32_t direction = 0;
            for ( uint32_t d = 0; d < kCardDirections; ++d )
                if ( !clusters[d].empty() && clusters[d].back().WeightedCoverage < smallest )
                {
                    smallest  = clusters[d].back().WeightedCoverage;
                    direction = d;
                }
            clusters[direction].pop_back();
            --clusterCount;
        }

        // UE SerializeLOD: each cluster's voxels with half a voxel of near/far margin, clamped to the mesh box
        // (10 cm of depth margin left for LOD movement), back in mesh space.
        constexpr double kMarginZ = 10.0;
        for ( uint32_t d = 0; d < kCardDirections; ++d )
        {
            const DirectionBasis& basis = params.Basis[d];
            glm::dvec3            localLo( std::numeric_limits<double>::max() ),
                 localHi( std::numeric_limits<double>::lowest() );
            for ( int corner = 0; corner < 8; ++corner )
            {
                const glm::dvec3 p( ( corner & 1 ) ? boundsMax.x : boundsMin.x,
                                    ( corner & 2 ) ? boundsMax.y : boundsMin.y,
                                    ( corner & 4 ) ? boundsMax.z : boundsMin.z );
                const glm::dvec3 l = basis.ToLocalCm( p );
                localLo            = glm::min( localLo, l );
                localHi            = glm::max( localHi, l );
            }
            for ( const Cluster& cluster : clusters[d] )
            {
                glm::dvec3 cMin = ( glm::dvec3( cluster.Bounds.Min ) - glm::dvec3( 0.0, 0.0, 0.5 ) ) * basis.Voxel;
                glm::dvec3 cMax = ( glm::dvec3( cluster.Bounds.Max ) + glm::dvec3( 1.0, 1.0, 1.5 ) ) * basis.Voxel;
                cMin.x          = std::max( cMin.x, localLo.x );
                cMin.y          = std::max( cMin.y, localLo.y );
                cMin.z          = std::max( cMin.z, localLo.z - kMarginZ );
                cMax.x          = std::min( cMax.x, localHi.x );
                cMax.y          = std::min( cMax.y, localHi.y );
                cMax.z          = std::min( cMax.z, localHi.z + kMarginZ );
                const glm::dvec3 a =
                     basis.Offset + basis.AxisX * cMin.x + basis.AxisY * cMin.y + basis.RayDirection * cMin.z;
                const glm::dvec3 b =
                     basis.Offset + basis.AxisX * cMax.x + basis.AxisY * cMax.y + basis.RayDirection * cMax.z;
                CardBuildData card;
                card.Center    = glm::vec3( ( a + b ) * 0.5 );
                card.Extent    = glm::vec3( glm::abs( b - a ) * 0.5 );
                card.Direction = static_cast<CardDirection>( d );
                out.Cards.push_back( card );
            }
        }
        return Common::MakeSuccess( out );
    }
} // namespace Desert::Assets
