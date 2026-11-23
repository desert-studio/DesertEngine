#pragma once

#include <Common/Core/ResultStr.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// VIRT-GEOM contract (step 1, cook): THE CLUSTER LOD DAG OF ONE SUBMESH, built at cook time from the submesh's
// LOD0 triangles (UE Nanite::Build / FClusterDAG, Engine/Source/Developer/NaniteBuilder; the meshoptimizer 1.3
// clusterlod scheme). Contract page: Notion Wiki «VIRT-GEOM — контракт».
//
//  1. LEVEL 0: meshopt_buildMeshletsSpatial splits the submesh into clusters of at most kClusterMaxTriangles
//     triangles over at most kClusterMaxVertices vertices.
//  2. GROUPS: meshopt_partitionClusters joins kClusterGroupMinClusters..kClusterGroupMaxClusters adjacent clusters
//     of the level into a group; the group's outer border is LOCKED (meshopt_SimplifyLockBorder + the vertex lock
//     array), so whatever level a neighbour renders at, the shared border is the same edge chain: no crack.
//  3. SIMPLIFY each group to half its triangles (MeshSimplifier's attribute-aware reduction,
//  kSimplifyAttributeCount
//     attributes, the same weights the discrete LOD chain uses), re-split the result into clusters. Those clusters
//     are the next level; their LodError = the group's simplification error (max with the group members' own, so
//     the error grows monotonically up the DAG), their LodBounds = the group's merged sphere.
//  4. Repeat until a level is one cluster, or a level could not halve (the remainder become roots with
//     ParentLodError = kClusterRootError).
//
// NO NEW VERTICES, except PreserveArea's (ClusterDagSettings): meshopt simplification only removes indices; every
// level indexes the submesh's ORIGINAL vertex array, which an area-preserving build extends by the boundary
// vertices it moved (appended to the submesh's own range by the cook). A cluster stores submesh-relative vertex
// refs (VertexRefs) and local triangles (Triangles); the vertices stay where they are, in the mesh's one
// GeometryPool vertex span. One vertex copy for LOD0, every DAG level, the discrete LODs cut from the DAG and the
// RT BLAS.
//
// THE DISCRETE LOD CHAIN OF A VIRTUAL-GEOMETRY MESH is CUT FROM THIS DAG (CutClusterDag), not simplified a second
// time: Submesh::LODs / BakedLODs of such a mesh are the DAG's cuts at kClusterDagCutErrors. They serve the
// RT BLAS (cut 0 = LOD0), devices without the VirtualGeometry capability, and the passes VIRT-GEOM does not draw
// (forward / translucency never hold a VG mesh: a translucent material refuses the flag at import).
namespace Desert::Geometry
{
    // meshopt_buildMeshlets caps: vertices <= 256, triangles <= 512 (multiple of 4). 128 / 128 = UE
    // NANITE_MAX_CLUSTER_TRIANGLES; 128 vertices keep a local index in 7 bits (packed 3 x 8 bits per triangle) and
    // a triangle index in 7 bits of the visibility-buffer payload (VirtualGeometryLayout.hpp kVisTriangleBits).
    inline constexpr uint32_t kClusterMaxTriangles = 128u;
    inline constexpr uint32_t kClusterMaxVertices  = 128u;
    // UE NANITE_MIN/MAX_CLUSTERS_PER_GROUP.
    inline constexpr uint32_t kClusterGroupMinClusters = 8u;
    inline constexpr uint32_t kClusterGroupMaxClusters = 32u;
    // ParentLodError of a root group: never coarse enough to be replaced.
    inline constexpr float kClusterRootError = 3.0e38f;
    // The relative errors (of the submesh's bounds radius) the discrete LOD chain is cut at: LOD0 = 0 (level 0),
    // then the levels that replace the generated chain (LODChainSettings) of a mesh that is not virtual geometry,
    // coarsest last. FPS-GRASS-LOD: 0.12 and 0.5 continue the chain to a few-triangle silhouette for far
    // instanced foliage (GrassCull clamps far instances to the coarsest level); each level's error is stored with
    // it (SubmeshData::LodCutErrors) and caps its screen size by the Nanite ~1 px rule (ResolveLODScreenSizes), so a
    // coarse level is never drawn where its error would show. At most GrassRenderer kLawBuckets (15) levels.
    inline constexpr float kClusterDagCutErrors[] = { 0.0f, 0.002f, 0.008f, 0.03f, 0.12f, 0.5f };

    struct ClusterSphere
    {
        float Center[3]{};
        float Radius = 0.0f;
    };

    // ONE CLUSTER (UE FCluster). Wire-safe: plain floats / uint32, no glm (MeshBinary rule).
    struct ClusterRecord
    {
        uint32_t FirstVertexRef = 0; // ClusterDag::VertexRefs[FirstVertexRef, +VertexCount)
        uint32_t VertexCount    = 0; // <= kClusterMaxVertices
        uint32_t FirstTriangle  = 0; // ClusterDag::Triangles[FirstTriangle, +TriangleCount)
        uint32_t TriangleCount  = 0; // <= kClusterMaxTriangles
        // Culling: the AABB of its triangles, cm, submesh space (frustum + HZB test).
        float BoxMin[3]{};
        float BoxMax[3]{};
        // LOD: the GENERATING group's merged sphere and simplification error (cm). Level 0: its own sphere, error
        // 0.
        ClusterSphere LodBounds{};
        float         LodError = 0.0f;
        // The longest edge of its triangles, cm: the HW/SW raster split projects it (kSwRasterMaxEdgePixels).
        float    MaxEdgeLength = 0.0f;
        uint32_t Group         = 0; // the group it is a MEMBER of: ClusterDag::Groups[Group]
        uint32_t Level         = 0; // 0 = the submesh's own triangles
    };
    static_assert( sizeof( ClusterRecord ) == 72, "MeshBinary ClusterRecords element size" );

    // ONE GROUP (UE FClusterGroup): the clusters of one level simplified together. A cluster renders iff
    // Project( Group.LodBounds, Group.ParentLodError ) > threshold >= Project( Cluster.LodBounds, Cluster.LodError
    // ) (UE Nanite's ShouldVisitChild / SmallEnoughToDraw), monotonic by construction (step 3 above).
    struct ClusterGroupRecord
    {
        uint32_t FirstCluster = 0; // its member clusters: Clusters[FirstCluster, +ClusterCount) (stored grouped)
        uint32_t ClusterCount = 0;
        ClusterSphere LodBounds{};                        // encloses every member's LodBounds
        float         ParentLodError = kClusterRootError; // the error of the clusters simplified FROM this group
        float         BoxMin[3]{};
        float         BoxMax[3]{};
        uint32_t      Level = 0;
    };
    static_assert( sizeof( ClusterGroupRecord ) == 56, "MeshBinary ClusterGroupRecords element size" );

    // ONE BVH NODE over groups (UE FHierarchyNode, 4-wide here: UE NANITE_MAX_BVH_NODE_FANOUT is 4 on the
    // compute path). A leaf child references one group; an interior child a node. Built top-down by
    // spatial median over the groups' boxes, one subtree per DAG level then a root over them, as UE's
    // BuildHierarchy.
    inline constexpr uint32_t kClusterHierarchyFanout = 4u;
    inline constexpr uint32_t kClusterHierarchyLeaf   = 1u << 31; // Child: leaf bit | group index
    inline constexpr uint32_t kClusterHierarchyNone   = 0xFFFFFFFFu;
    struct ClusterHierarchyNode
    {
        float         BoxMin[kClusterHierarchyFanout][3]{};
        float         BoxMax[kClusterHierarchyFanout][3]{};
        ClusterSphere LodBounds[kClusterHierarchyFanout]{};
        float         MinLodError[kClusterHierarchyFanout]{};  // the smallest member LodError below the child
        float    MaxParentLodError[kClusterHierarchyFanout]{}; // the largest group ParentLodError below the child
        uint32_t Child[kClusterHierarchyFanout]{ kClusterHierarchyNone, kClusterHierarchyNone,
                                                 kClusterHierarchyNone, kClusterHierarchyNone };
    };
    static_assert( sizeof( ClusterHierarchyNode ) == 208, "MeshBinary ClusterHierarchy element size" );

    // VG-STREAMING: THE STREAMING PAGES (UE Nanite FPageStreamingState / ClusterPage, NaniteEncode's page
    // partition). The cook cuts every level of the DAG into pages: a page is a run of whole groups of ONE level,
    // contiguous in Groups (groups are stored level by level, and their clusters' VertexRefs / Triangles in group
    // order, so a page's words are one contiguous run of each), its refs + triangles at most kClusterPageMaxWords
    // words (128 KiB, UE NANITE_STREAMING_PAGE_GPU_SIZE). A group past the cap is a page of its own (a group holds
    // at most kClusterGroupMaxClusters x (128 + 128) words = 8192, so it always fits).
    //
    // ROOT PAGES (UE root pages, always resident): (a) every page of a TERMINAL group (ParentLodError =
    // kClusterRootError: the top cluster, or a group that could not halve) - nothing coarser covers its surface,
    // so it may never be absent; a level's terminal and non-terminal groups never share a page - and (b) every
    // page of the coarsest levels, taken from the top level down while all root words together stay within
    // kClusterRootPageMaxWords (less pop-in up close for a page or two of memory). The root set is closed
    // upwards (a root page never depends on a streamed one). The pages are cooked data (MeshBinary v10, the
    // partition's caps and kClusterPageFormatVersion in the VG DDC key); the runtime that keeps only the root
    // pages resident and streams the rest on the cut's requests is the VG-STREAMING remainder - until it lands,
    // a loaded mesh stages every page's words.
    //
    // DEPENDENCIES: a group's clusters may draw only where the clusters generated FROM it (its parents, one
    // level up: Generators) are known not to be fine enough - so a page is installed only when every page
    // holding a parent of its groups is resident, and evicted only when no resident page depends on it. With
    // that rule, the finest resident level is the cut: a cluster whose generating group is not resident is a
    // STREAMING LEAF and draws however coarse it is (UE NANITE_CLUSTER_FLAG_STREAMING_LEAF), so the surface is
    // covered once, with no hole and no crack (the locked group borders, step 2 above).
    inline constexpr uint32_t kClusterPageMaxWords     = 32768u; // 128 KiB of refs + triangles
    inline constexpr uint32_t kClusterRootPageMaxWords = 32768u; // the always-resident coarse levels
    // The page partition's own revision: a key input of the VG mesh DDC entry (SerializeMeshSettingsForKey).
    inline constexpr uint32_t kClusterPageFormatVersion = 1u;
    inline constexpr uint32_t kClusterPageRoot          = 1u << 0; // ClusterPageRecord::Flags
    static_assert( kClusterGroupMaxClusters * ( kClusterMaxVertices + kClusterMaxTriangles ) <= kClusterPageMaxWords,
                   "a group always fits one page" );

    struct ClusterPageRecord
    {
        uint32_t FirstGroup = 0; // Groups[FirstGroup, +GroupCount): one level, contiguous
        uint32_t GroupCount = 0;
        uint32_t FirstVertexRef = 0; // VertexRefs[FirstVertexRef, +VertexRefCount): its groups' clusters' refs
        uint32_t VertexRefCount = 0;
        uint32_t FirstTriangle  = 0; // Triangles[FirstTriangle, +TriangleCount)
        uint32_t TriangleCount  = 0;
        uint32_t Level          = 0;
        uint32_t Flags          = 0; // kClusterPageRoot
    };
    static_assert( sizeof( ClusterPageRecord ) == 32, "MeshBinary ClusterPages element size" );

    // THE COOKED DAG OF ONE SUBMESH. Node 0 is the root.
    struct ClusterDag
    {
        std::vector<ClusterRecord>        Clusters; // grouped: every group's members contiguous
        std::vector<ClusterGroupRecord>   Groups;
        std::vector<ClusterHierarchyNode> Hierarchy;
        std::vector<uint32_t> VertexRefs; // submesh-relative vertex indices (submesh VertexOffset base)
        // One word per triangle: three cluster-local vertex indices (bits 0-7, 8-15, 16-23), bits 24-31 zero.
        std::vector<uint32_t> Triangles;
        uint32_t              LevelCount = 0;
        // Per cluster: the group whose simplification made it (UE: a cluster's generating group), or
        // kClusterHierarchyNone for a level-0 cluster.
        std::vector<uint32_t> Generators;
        // The streaming pages, level by level (coarsest last), in Groups order. Root pages (kClusterPageRoot) are
        // NOT a suffix: a terminal group's page is root at whatever level it sits.
        std::vector<ClusterPageRecord> Pages;
    };

    // Cuts @p dag (Generators filled) into its streaming pages (Pages replaced). Refused: Generators not one per
    // cluster, groups not stored level by level, a group's words not contiguous after the previous group's.
    [[nodiscard]] Common::BoolResultStr PaginateClusterDag( ClusterDag& dag );

    // The page rules every loaded DAG is held to (DecodeMeshBinary refuses a v10 DAG that breaks them; the
    // cook's PaginateClusterDag ends with it): every group in exactly one page, in order, one
    // level per page, the words of each page the contiguous run of its groups' clusters, at most
    // kClusterPageMaxWords, every page of a terminal group root, at least one root page, the root set closed
    // upwards, every generator a group one level below its cluster.
    [[nodiscard]] Common::BoolResultStr ValidateClusterPages( const ClusterDag& dag );

    // The cook options (UE FMeshNaniteSettings). Key inputs of the mesh DDC entry (SerializeMeshSettingsForKey).
    struct ClusterDagSettings
    {
        // The target triangle ratio of each group's simplification (UE NaniteBuilder halves every group).
        float GroupReduction = 0.5f;
        // Simplification stops a level when a group keeps more than this fraction (meshopt cannot halve it).
        float MinReductionGain = 0.85f;
        // UE FMeshNaniteSettings::bPreserveArea (foliage): every simplified group gives the surface it lost back
        // by pushing its OPEN boundary (a leaf card's alpha-cut edge) outward in the surface's plane - UE
        // FMeshSimplifier::PreserveSurfaceArea - so a masked canopy keeps its coverage up the DAG instead of
        // thinning out with distance. Locked vertices (the group's border with its neighbours) never move, so the
        // crack-free rule holds. A moved vertex is a NEW vertex (ClusterDagBuild::AddedVertexSources): the
        // levels below still index the original one.
        bool PreserveArea                                  = false;
        bool operator==( const ClusterDagSettings& ) const = default;
    };

    // What BuildClusterDag makes: the DAG and the vertices the area preservation added. Dag.VertexRefs index the
    // input vertices [0, vertexCount) and then these, in order: added vertex i sits at
    // AddedPositions[3i .. 3i+2] (cm, submesh space) and carries every other attribute of input vertex
    // AddedVertexSources[i]. Both empty without ClusterDagSettings::PreserveArea or with no open boundary.
    struct ClusterDagBuild
    {
        ClusterDag            Dag;
        std::vector<uint32_t> AddedVertexSources;
        std::vector<float>    AddedPositions;
    };

    // Builds the DAG of one submesh. @p positions: the submesh's tightly packed xyz (vertexCount * 3, cm);
    // @p attributes: null or kSimplifyAttributeCount floats per vertex (MeshSimplifier.hpp); @p triangles: LOD0,
    // submesh-relative. Refused: no triangle, an index past vertexCount, a level that produced a cluster past the
    // caps. Logs one line per level (clusters, groups, max error).
    [[nodiscard]] Common::ResultStr<ClusterDagBuild>
    BuildClusterDag( std::span<const float> positions, uint32_t vertexCount, std::span<const uint32_t> triangles,
                     const float* attributes, const ClusterDagSettings& settings );

    // A discrete LOD cut through the DAG: the triangles (submesh-relative indices) of every cluster with
    // LodError <= @p relativeError * boundsRadius < its group's ParentLodError. Cut 0 is exactly LOD0.
    [[nodiscard]] std::vector<uint32_t> CutClusterDag( const ClusterDag& dag, float relativeError,
                                                       float boundsRadius );
} // namespace Desert::Geometry
