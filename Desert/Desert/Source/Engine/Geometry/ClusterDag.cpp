#include "ClusterDag.hpp"

#include "MeshSimplifier.hpp"

#include <meshoptimizer.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string>

// The DAG build is the meshoptimizer 1.3 clusterlod scheme (demo/clusterlod.h, MIT, zeux): clusterize ->
// partition -> lock group borders -> simplify each group -> re-clusterize, with the bounds and error
// monotonicity rules of that file. UE NaniteBuilder (FClusterDAG::Reduce) is the same algorithm over METIS; what
// differs from both is written where it differs.
namespace Desert::Geometry
{
    namespace
    {
        // meshopt_SimplifyVertex_* flags use bits 0..2 of a lock byte. Bit 7 is lockBoundary's scratch "seen by a
        // prior group" mark (as in clusterlod.h); bit 6 is a lock that outlives its level (see kStuckLock).
        constexpr unsigned char kSeenByPriorGroup = 1u << 7;
        // The vertices of a group that could not halve. clusterlod.h clears every lock at each level, so a
        // group that simplified NEXT TO a stuck one sees the shared edge as an open border at the next level and
        // may slide along it - a crack against the stuck group's clusters, which stay rendered as roots. Kept
        // locked for every later level, that border is the same edge chain on both sides.
        constexpr unsigned char kStuckLock = 1u << 6;

        // Target clusters per partition: meshopt_partitionClusters may overshoot by a third, so 24 keeps every
        // group at or below kClusterGroupMaxClusters.
        constexpr std::size_t kPartitionTarget = kClusterGroupMaxClusters * 3u / 4u;
        static_assert( kPartitionTarget + kPartitionTarget / 3u <= kClusterGroupMaxClusters );

        struct Bounds
        {
            ClusterSphere Sphere{};
            float         Error = 0.0f;
        };

        // A cluster while the DAG is built: its submesh-relative triangle list and the bounds it is selected
        // by (the generating group's merged sphere + error; level 0: its own sphere, error 0).
        struct BuiltCluster
        {
            std::vector<uint32_t> Indices;
            uint32_t              VertexCount = 0;
            Bounds                Lod{};
            uint32_t              Level = 0;
        };

        struct BuiltGroup
        {
            std::vector<uint32_t> Members; // BuiltCluster ids
            Bounds                Lod{};   // merged member spheres; Error = ParentLodError
            uint32_t              Level = 0;
        };

        struct Mesh
        {
            const float* Positions   = nullptr;
            uint32_t     VertexCount = 0;
            const float* Attributes  = nullptr;
        };

        Bounds MergeBounds( const std::vector<BuiltCluster>& clusters, const std::vector<uint32_t>& members )
        {
            std::vector<float> centers( members.size() * 3 ), radii( members.size() );
            float              error = 0.0f;
            for ( std::size_t i = 0; i < members.size(); ++i )
            {
                const Bounds& b = clusters[members[i]].Lod;
                std::copy_n( b.Sphere.Center, 3, &centers[i * 3] );
                radii[i] = b.Sphere.Radius;
                error    = std::max( error, b.Error );
            }
            const meshopt_Bounds merged = meshopt_computeSphereBounds(
                 centers.data(), members.size(), sizeof( float ) * 3, radii.data(), sizeof( float ) );
            Bounds out;
            std::copy_n( merged.center, 3, out.Sphere.Center );
            out.Sphere.Radius = merged.radius;
            out.Error         = error;
            return out;
        }

        // Splits @p indices into clusters of kClusterMaxTriangles / kClusterMaxVertices
        // (meshopt_buildMeshletsSpatial, the contract's level-0 splitter, used for every level so a level's
        // clusters are cut the same way).
        void Clusterize( const Mesh& mesh, const std::vector<uint32_t>& indices, uint32_t level,
                         std::vector<BuiltCluster>& clusters, std::vector<uint32_t>& outIds )
        {
            constexpr std::size_t kMinTriangles = kClusterMaxTriangles / 3u; // clusterlod.h default
            const std::size_t     maxMeshlets =
                 meshopt_buildMeshletsBound( indices.size(), kClusterMaxVertices, kMinTriangles );
            std::vector<meshopt_Meshlet> meshlets( maxMeshlets );
            std::vector<uint32_t>        meshletVertices( indices.size() );
            std::vector<unsigned char>   meshletTriangles( indices.size() );
            const std::size_t            count = meshopt_buildMeshletsSpatial(
                 meshlets.data(), meshletVertices.data(), meshletTriangles.data(), indices.data(), indices.size(),
                 mesh.Positions, mesh.VertexCount, sizeof( float ) * 3, kClusterMaxVertices, kMinTriangles,
                 kClusterMaxTriangles, 0.5f );
            for ( std::size_t m = 0; m < count; ++m )
            {
                const meshopt_Meshlet& meshlet = meshlets[m];
                meshopt_optimizeMeshletLevel( &meshletVertices[meshlet.vertex_offset], meshlet.vertex_count,
                                              &meshletTriangles[meshlet.triangle_offset], meshlet.triangle_count,
                                              1 );
                BuiltCluster cluster;
                cluster.VertexCount = meshlet.vertex_count;
                cluster.Level       = level;
                cluster.Indices.resize( std::size_t( meshlet.triangle_count ) * 3 );
                for ( std::size_t j = 0; j < cluster.Indices.size(); ++j )
                    cluster.Indices[j] =
                         meshletVertices[meshlet.vertex_offset + meshletTriangles[meshlet.triangle_offset + j]];
                outIds.push_back( static_cast<uint32_t>( clusters.size() ) );
                clusters.push_back( std::move( cluster ) );
            }
        }

        // meshopt_partitionClusters over position-welded indices: groups of adjacent / near clusters. Returns the
        // groups as lists of cluster ids.
        std::vector<std::vector<uint32_t>> Partition( const Mesh& mesh, const std::vector<BuiltCluster>& clusters,
                                                      const std::vector<uint32_t>& ids,
                                                      const std::vector<uint32_t>& remap )
        {
            if ( ids.size() <= kPartitionTarget )
                return { ids };
            std::vector<uint32_t> counts( ids.size() ), welded;
            for ( std::size_t i = 0; i < ids.size(); ++i )
            {
                const auto& indices = clusters[ids[i]].Indices;
                counts[i]           = static_cast<uint32_t>( indices.size() );
                for ( uint32_t v : indices )
                    welded.push_back( remap[v] );
            }
            std::vector<uint32_t> part( ids.size() );
            const std::size_t     partCount = meshopt_partitionClusters(
                 part.data(), welded.data(), welded.size(), counts.data(), counts.size(), mesh.Positions,
                 mesh.VertexCount, sizeof( float ) * 3, kPartitionTarget );
            std::vector<std::vector<uint32_t>> groups( partCount );
            for ( std::size_t i = 0; i < ids.size(); ++i )
                groups[part[i]].push_back( ids[i] );
            return groups;
        }

        // clusterlod.h lockBoundary: a (position-welded) vertex used by two groups of the level is locked, so both
        // groups keep the shared edge chain whatever level each side renders at. kStuckLock survives.
        void LockGroupBorders( std::vector<unsigned char>& locks, const std::vector<BuiltCluster>& clusters,
                               const std::vector<std::vector<uint32_t>>& groups,
                               const std::vector<uint32_t>&              remap )
        {
            for ( unsigned char& l : locks )
                l &= static_cast<unsigned char>( ~( meshopt_SimplifyVertex_Lock | kSeenByPriorGroup ) );
            for ( const auto& group : groups )
            {
                for ( uint32_t id : group )
                    for ( uint32_t v : clusters[id].Indices )
                    {
                        unsigned char& l = locks[remap[v]];
                        if ( l & kSeenByPriorGroup )
                            l |= meshopt_SimplifyVertex_Lock;
                    }
                for ( uint32_t id : group )
                    for ( uint32_t v : clusters[id].Indices )
                        locks[remap[v]] |= kSeenByPriorGroup;
            }
            for ( std::size_t i = 0; i < locks.size(); ++i )
            {
                const unsigned char welded = locks[remap[i]];
                const bool          locked = ( welded & ( meshopt_SimplifyVertex_Lock | kStuckLock ) ) != 0;
                locks[i] = static_cast<unsigned char>( ( locks[i] & ~meshopt_SimplifyVertex_Lock ) |
                                                       ( locked ? meshopt_SimplifyVertex_Lock : 0 ) );
            }
        }

        // The group's attribute-aware reduction (MeshSimplifier's metric and weights), absolute error in cm.
        // Sparse: the index list is a subset of the submesh. Permissive: collapses cross attribute seams as the
        // discrete chain does. ErrorClamped: the reported error is bounded by the deviation from the ORIGINAL
        // surface, not accumulated level over level (clusterlod.h default). No sloppy fallback: a group that
        // cannot halve becomes a root (ClusterDag.hpp step 4) rather than being reduced by a lower-quality
        // reducer.
        std::vector<uint32_t> SimplifyGroup( const Mesh& mesh, const std::vector<uint32_t>& merged,
                                             const std::vector<unsigned char>& locks, float reduction,
                                             float& error )
        {
            const std::size_t     target = std::size_t( double( merged.size() / 3 ) * reduction ) * 3;
            std::vector<uint32_t> out( merged.size() );
            constexpr unsigned    kOptions = meshopt_SimplifySparse | meshopt_SimplifyErrorAbsolute |
                                          meshopt_SimplifyErrorClamped | meshopt_SimplifyPermissive;
            const std::size_t attributeCount = mesh.Attributes ? kSimplifyAttributeCount : 0;
            // Named, not a ternary argument: MSVC hung on a ternary passed by value (memory msvc-hang-ternary).
            const float* weights = nullptr;
            if ( mesh.Attributes )
                weights = kSimplifyAttributeWeights;
            error = 0.0f;
            out.resize( meshopt_simplifyWithAttributes(
                 out.data(), merged.data(), merged.size(), mesh.Positions, mesh.VertexCount, sizeof( float ) * 3,
                 mesh.Attributes, sizeof( float ) * attributeCount, weights, attributeCount, locks.data(), target,
                 std::numeric_limits<float>::max(), kOptions, &error ) );
            return out;
        }

        void ClusterBox( const Mesh& mesh, const std::vector<uint32_t>& indices, float* boxMin, float* boxMax,
                         float& maxEdge )
        {
            for ( int a = 0; a < 3; ++a )
            {
                boxMin[a] = std::numeric_limits<float>::max();
                boxMax[a] = -std::numeric_limits<float>::max();
            }
            maxEdge = 0.0f;
            for ( std::size_t t = 0; t + 2 < indices.size(); t += 3 )
                for ( int k = 0; k < 3; ++k )
                {
                    const float* p = mesh.Positions + std::size_t( indices[t + k] ) * 3;
                    const float* q = mesh.Positions + std::size_t( indices[t + ( k + 1 ) % 3] ) * 3;
                    float        e = 0.0f;
                    for ( int a = 0; a < 3; ++a )
                    {
                        boxMin[a] = std::min( boxMin[a], p[a] );
                        boxMax[a] = std::max( boxMax[a], p[a] );
                        e += ( p[a] - q[a] ) * ( p[a] - q[a] );
                    }
                    maxEdge = std::max( maxEdge, std::sqrt( e ) );
                }
        }

        // ---- BVH over groups (UE FClusterDAG / BuildHierarchyTopDown, 4-wide) ----

        struct HierarchyItem
        {
            uint32_t      Child = kClusterHierarchyNone;
            float         BoxMin[3]{};
            float         BoxMax[3]{};
            ClusterSphere Sphere{};
            float         MinLodError       = 0.0f;
            float         MaxParentLodError = 0.0f;
        };

        HierarchyItem MergeItems( const HierarchyItem* items, std::size_t count, uint32_t child )
        {
            HierarchyItem      out;
            std::vector<float> centers( count * 3 ), radii( count );
            out.Child             = child;
            out.MinLodError       = std::numeric_limits<float>::max();
            out.MaxParentLodError = 0.0f;
            for ( int a = 0; a < 3; ++a )
            {
                out.BoxMin[a] = std::numeric_limits<float>::max();
                out.BoxMax[a] = -std::numeric_limits<float>::max();
            }
            for ( std::size_t i = 0; i < count; ++i )
            {
                const HierarchyItem& it = items[i];
                for ( int a = 0; a < 3; ++a )
                {
                    out.BoxMin[a]      = std::min( out.BoxMin[a], it.BoxMin[a] );
                    out.BoxMax[a]      = std::max( out.BoxMax[a], it.BoxMax[a] );
                    centers[i * 3 + a] = it.Sphere.Center[a];
                }
                radii[i]              = it.Sphere.Radius;
                out.MinLodError       = std::min( out.MinLodError, it.MinLodError );
                out.MaxParentLodError = std::max( out.MaxParentLodError, it.MaxParentLodError );
            }
            const meshopt_Bounds merged = meshopt_computeSphereBounds( centers.data(), count, sizeof( float ) * 3,
                                                                       radii.data(), sizeof( float ) );
            std::copy_n( merged.center, 3, out.Sphere.Center );
            out.Sphere.Radius = merged.radius;
            return out;
        }

        // Fills node @p index with @p items, splitting by the median of the box centres along the widest axis into
        // kClusterHierarchyFanout children while more than that remain.
        void BuildNode( std::vector<ClusterHierarchyNode>& nodes, uint32_t index,
                        std::vector<HierarchyItem> items )
        {
            if ( items.size() > kClusterHierarchyFanout )
            {
                float lo[3], hi[3];
                for ( int a = 0; a < 3; ++a )
                {
                    lo[a] = std::numeric_limits<float>::max();
                    hi[a] = -std::numeric_limits<float>::max();
                }
                for ( const HierarchyItem& it : items )
                    for ( int a = 0; a < 3; ++a )
                    {
                        const float c = 0.5f * ( it.BoxMin[a] + it.BoxMax[a] );
                        lo[a]         = std::min( lo[a], c );
                        hi[a]         = std::max( hi[a], c );
                    }
                int axis = 0;
                for ( int a = 1; a < 3; ++a )
                    if ( hi[a] - lo[a] > hi[axis] - lo[axis] )
                        axis = a;
                std::stable_sort( items.begin(), items.end(),
                                  [axis]( const HierarchyItem& x, const HierarchyItem& y )
                                  { return x.BoxMin[axis] + x.BoxMax[axis] < y.BoxMin[axis] + y.BoxMax[axis]; } );
                std::vector<HierarchyItem> children;
                const std::size_t          n = items.size();
                for ( std::size_t k = 0; k < kClusterHierarchyFanout; ++k )
                {
                    const std::size_t first = n * k / kClusterHierarchyFanout,
                                      last  = n * ( k + 1 ) / kClusterHierarchyFanout;
                    if ( last - first == 1 )
                    {
                        children.push_back( items[first] );
                        continue;
                    }
                    const auto child = static_cast<uint32_t>( nodes.size() );
                    nodes.emplace_back();
                    BuildNode( nodes, child,
                               std::vector<HierarchyItem>( items.begin() + first, items.begin() + last ) );
                    children.push_back( MergeItems( items.data() + first, last - first, child ) );
                }
                items = std::move( children );
            }
            ClusterHierarchyNode node;
            for ( std::size_t c = 0; c < items.size(); ++c )
            {
                const HierarchyItem& it = items[c];
                std::copy_n( it.BoxMin, 3, node.BoxMin[c] );
                std::copy_n( it.BoxMax, 3, node.BoxMax[c] );
                node.LodBounds[c]         = it.Sphere;
                node.MinLodError[c]       = it.MinLodError;
                node.MaxParentLodError[c] = it.MaxParentLodError;
                node.Child[c]             = it.Child;
            }
            nodes[index] = node;
        }

        // UE BuildHierarchy: one subtree per DAG level (groups of one level are spatially disjoint, mixing levels
        // makes boxes overlap), then a root over the level subtrees. Node 0 is the root.
        std::vector<ClusterHierarchyNode> BuildHierarchy( const ClusterDag& dag )
        {
            std::vector<ClusterHierarchyNode>       nodes( 1 );
            std::vector<std::vector<HierarchyItem>> perLevel( dag.LevelCount );
            for ( std::size_t g = 0; g < dag.Groups.size(); ++g )
            {
                const ClusterGroupRecord& group = dag.Groups[g];
                HierarchyItem             it;
                it.Child = kClusterHierarchyLeaf | static_cast<uint32_t>( g );
                std::copy_n( group.BoxMin, 3, it.BoxMin );
                std::copy_n( group.BoxMax, 3, it.BoxMax );
                it.Sphere            = group.LodBounds;
                it.MaxParentLodError = group.ParentLodError;
                it.MinLodError       = std::numeric_limits<float>::max();
                for ( uint32_t c = 0; c < group.ClusterCount; ++c )
                    it.MinLodError = std::min( it.MinLodError, dag.Clusters[group.FirstCluster + c].LodError );
                perLevel[group.Level].push_back( it );
            }
            std::vector<HierarchyItem> levelRoots;
            for ( auto& items : perLevel )
            {
                if ( items.empty() )
                    continue;
                if ( items.size() == 1 )
                {
                    levelRoots.push_back( items[0] );
                    continue;
                }
                const auto node = static_cast<uint32_t>( nodes.size() );
                nodes.emplace_back();
                const HierarchyItem merged = MergeItems( items.data(), items.size(), node );
                BuildNode( nodes, node, std::move( items ) );
                levelRoots.push_back( merged );
            }
            BuildNode( nodes, 0, std::move( levelRoots ) );
            return nodes;
        }
    } // namespace

    Common::ResultStr<ClusterDag> BuildClusterDag( std::span<const float> positions, uint32_t vertexCount,
                                                   std::span<const uint32_t> triangles, const float* attributes,
                                                   const ClusterDagSettings& settings )
    {
        if ( triangles.empty() || triangles.size() % 3 != 0 )
            return Common::MakeFormattedError<ClusterDag>( "cluster DAG: {} indices is not a triangle list",
                                                           triangles.size() );
        if ( positions.size() < std::size_t( vertexCount ) * 3 )
            return Common::MakeFormattedError<ClusterDag>( "cluster DAG: {} position floats for {} vertices",
                                                           positions.size(), vertexCount );
        for ( std::size_t i = 0; i < triangles.size(); ++i )
            if ( triangles[i] >= vertexCount )
                return Common::MakeFormattedError<ClusterDag>( "cluster DAG: index {} = {} past {} vertices", i,
                                                               triangles[i], vertexCount );
        if ( !( settings.GroupReduction > 0.0f && settings.GroupReduction < 1.0f ) ||
             !( settings.MinReductionGain > settings.GroupReduction && settings.MinReductionGain < 1.0f ) )
            return Common::MakeFormattedError<ClusterDag>(
                 "cluster DAG: GroupReduction {} / MinReductionGain {} outside 0 < reduction < gain < 1",
                 settings.GroupReduction, settings.MinReductionGain );

        const Mesh mesh{ positions.data(), vertexCount, attributes };

        std::vector<uint32_t> remap( vertexCount );
        meshopt_generatePositionRemap( remap.data(), mesh.Positions, vertexCount, sizeof( float ) * 3 );
        std::vector<unsigned char> locks( vertexCount, 0 );

        std::vector<BuiltCluster> clusters;
        std::vector<BuiltGroup>   groups;
        std::vector<uint32_t>     active;
        Clusterize( mesh, std::vector<uint32_t>( triangles.begin(), triangles.end() ), 0, clusters, active );
        for ( uint32_t id : active )
        {
            const meshopt_Bounds b =
                 meshopt_computeClusterBounds( clusters[id].Indices.data(), clusters[id].Indices.size(),
                                               mesh.Positions, vertexCount, sizeof( float ) * 3 );
            std::copy_n( b.center, 3, clusters[id].Lod.Sphere.Center );
            clusters[id].Lod.Sphere.Radius = b.radius;
            clusters[id].Lod.Error         = 0.0f;
        }

        uint32_t level = 0;
        while ( active.size() > 1 )
        {
            const std::vector<std::vector<uint32_t>> parts = Partition( mesh, clusters, active, remap );
            LockGroupBorders( locks, clusters, parts, remap );

            std::vector<uint32_t> next;
            std::size_t           stuck    = 0;
            float                 maxError = 0.0f;
            for ( const std::vector<uint32_t>& part : parts )
            {
                BuiltGroup group;
                group.Members = part;
                group.Level   = level;
                group.Lod     = MergeBounds( clusters, part ); // Error = the largest member error

                std::vector<uint32_t> merged;
                for ( uint32_t id : part )
                    merged.insert( merged.end(), clusters[id].Indices.begin(), clusters[id].Indices.end() );

                float                       error = 0.0f;
                const std::vector<uint32_t> simplified =
                     SimplifyGroup( mesh, merged, locks, settings.GroupReduction, error );
                if ( simplified.empty() ||
                     double( simplified.size() ) > double( merged.size() ) * settings.MinReductionGain )
                {
                    // Cannot halve: the group is a root; its border stays locked for every later level.
                    group.Lod.Error = kClusterRootError;
                    for ( uint32_t v : merged )
                        locks[remap[v]] |= kStuckLock;
                    groups.push_back( std::move( group ) );
                    ++stuck;
                    continue;
                }

                // Monotone and STRICT: the parent error exceeds every member's, so a cut at an error equal to a
                // member's (cut 0 = LOD0 above all) selects the member, never its zero-cost simplification.
                const float memberMax = group.Lod.Error;
                float       parent    = std::max( memberMax, error );
                if ( !( parent > memberMax ) )
                    parent = std::nextafter( memberMax, std::numeric_limits<float>::max() );
                group.Lod.Error = parent;
                maxError        = std::max( maxError, parent );

                std::vector<uint32_t> made;
                Clusterize( mesh, simplified, level + 1, clusters, made );
                for ( uint32_t id : made )
                    clusters[id].Lod = group.Lod; // the generating group's sphere + error
                next.insert( next.end(), made.begin(), made.end() );
                groups.push_back( std::move( group ) );
            }
            LOG_INFO(
                 "[ClusterDag] level {}: {} clusters in {} groups ({} could not halve), parent error <= {:.4f} cm",
                 level, active.size(), parts.size(), stuck, maxError );
            active = std::move( next );
            ++level;
        }
        if ( active.size() == 1 )
        {
            BuiltGroup root;
            root.Members   = active;
            root.Level     = level;
            root.Lod       = clusters[active[0]].Lod;
            root.Lod.Error = kClusterRootError;
            groups.push_back( std::move( root ) );
            LOG_INFO( "[ClusterDag] level {}: 1 cluster (root), {} triangles", level,
                      clusters[active[0]].Indices.size() / 3 );
        }

        // Assemble the records: clusters stored grouped, local triangles over cluster-local vertex refs.
        ClusterDag dag;
        dag.Groups.reserve( groups.size() );
        dag.Clusters.reserve( clusters.size() );
        std::array<uint32_t, 256>                           localVertices{};
        std::array<unsigned char, kClusterMaxTriangles * 3> localTriangles{};
        for ( std::size_t g = 0; g < groups.size(); ++g )
        {
            const BuiltGroup&  built = groups[g];
            ClusterGroupRecord group;
            group.FirstCluster   = static_cast<uint32_t>( dag.Clusters.size() );
            group.ClusterCount   = static_cast<uint32_t>( built.Members.size() );
            group.LodBounds      = built.Lod.Sphere;
            group.ParentLodError = built.Lod.Error;
            group.Level          = built.Level;
            for ( int a = 0; a < 3; ++a )
            {
                group.BoxMin[a] = std::numeric_limits<float>::max();
                group.BoxMax[a] = -std::numeric_limits<float>::max();
            }
            dag.LevelCount = std::max( dag.LevelCount, built.Level + 1 );
            for ( uint32_t id : built.Members )
            {
                const BuiltCluster& c             = clusters[id];
                const std::size_t   triangleCount = c.Indices.size() / 3;
                if ( triangleCount == 0 || triangleCount > kClusterMaxTriangles ||
                     c.VertexCount > kClusterMaxVertices )
                    return Common::MakeFormattedError<ClusterDag>(
                         "cluster DAG: level {} cluster of {} triangles / {} vertices past the caps {} / {}",
                         c.Level, triangleCount, c.VertexCount, kClusterMaxTriangles, kClusterMaxVertices );
                const std::size_t vertexRefs = meshopt_extractMeshletIndices(
                     localVertices.data(), localTriangles.data(), c.Indices.data(), c.Indices.size() );
                if ( vertexRefs > kClusterMaxVertices )
                    return Common::MakeFormattedError<ClusterDag>(
                         "cluster DAG: level {} cluster references {} vertices", c.Level, vertexRefs );
                ClusterRecord record;
                record.FirstVertexRef = static_cast<uint32_t>( dag.VertexRefs.size() );
                record.VertexCount    = static_cast<uint32_t>( vertexRefs );
                record.FirstTriangle  = static_cast<uint32_t>( dag.Triangles.size() );
                record.TriangleCount  = static_cast<uint32_t>( triangleCount );
                dag.VertexRefs.insert( dag.VertexRefs.end(), localVertices.begin(),
                                       localVertices.begin() + vertexRefs );
                for ( std::size_t t = 0; t < triangleCount; ++t )
                    dag.Triangles.push_back( uint32_t( localTriangles[t * 3] ) |
                                             ( uint32_t( localTriangles[t * 3 + 1] ) << 8 ) |
                                             ( uint32_t( localTriangles[t * 3 + 2] ) << 16 ) );
                ClusterBox( mesh, c.Indices, record.BoxMin, record.BoxMax, record.MaxEdgeLength );
                record.LodBounds = c.Lod.Sphere;
                record.LodError  = c.Lod.Error;
                record.Group     = static_cast<uint32_t>( g );
                record.Level     = c.Level;
                for ( int a = 0; a < 3; ++a )
                {
                    group.BoxMin[a] = std::min( group.BoxMin[a], record.BoxMin[a] );
                    group.BoxMax[a] = std::max( group.BoxMax[a], record.BoxMax[a] );
                }
                dag.Clusters.push_back( record );
            }
            dag.Groups.push_back( group );
        }
        dag.Hierarchy = BuildHierarchy( dag );
        LOG_INFO( "[ClusterDag] {} triangles -> {} clusters, {} groups, {} levels, {} hierarchy nodes",
                  triangles.size() / 3, dag.Clusters.size(), dag.Groups.size(), dag.LevelCount,
                  dag.Hierarchy.size() );
        return Common::MakeSuccess( std::move( dag ) );
    }

    std::vector<uint32_t> CutClusterDag( const ClusterDag& dag, float relativeError, float boundsRadius )
    {
        const float           threshold = relativeError * boundsRadius;
        std::vector<uint32_t> out;
        for ( const ClusterRecord& c : dag.Clusters )
        {
            if ( !( c.LodError <= threshold && threshold < dag.Groups[c.Group].ParentLodError ) )
                continue;
            for ( uint32_t t = 0; t < c.TriangleCount; ++t )
            {
                const uint32_t word = dag.Triangles[c.FirstTriangle + t];
                for ( int k = 0; k < 3; ++k )
                    out.push_back( dag.VertexRefs[c.FirstVertexRef + ( ( word >> ( 8 * k ) ) & 0xFFu )] );
            }
        }
        return out;
    }
} // namespace Desert::Geometry
