#pragma once

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>

// VIRT-GEOM contract: the std430 rows of the virtual-geometry GPU tables and the visibility-buffer word. Their
// GLSL twin is Editor/Resources/Shaders/Common/VirtualGeometry.glslh (written in step 2 next to
// Common/GpuScene.glslh); every offset below is pinned by a static_assert and spelled in a comment of the twin.
//
// UE: Nanite's FPackedCluster / FPackedHierarchyNode (NaniteDataDecode.ush), FVisibleCluster
// (NaniteDataDecode.ush, NaniteCulling.ush) and the 64-bit visibility buffer (NaniteRasterizer.usf). Contract
// page: Notion Wiki «VIRT-GEOM — контракт».
//
// OWNERS. Clusters / Hierarchy / Dags are VirtualMeshTable's (VirtualMeshTable.hpp), one row range per resident
// mesh, written once at mesh upload. The candidate queue, the visible-cluster list, the raster args and the
// visibility buffer are per-frame RDG transients of VirtualGeometryRenderer (VirtualGeometryRenderer.hpp).
// Vertices, vertex refs and packed triangles are NOT here: they are the mesh's GeometryPool spans, read through
// the device addresses a GpuClusterDag row carries.
namespace Desert::Graphic::VirtualGeometry
{
    inline constexpr uint32_t kInvalidIndex = 0xFFFFFFFFu;

    // ONE ROW PER SUBMESH DAG of a resident virtual-geometry mesh (Geometry::ClusterDag of that submesh).
    struct GpuClusterDag
    {
        // Device addresses inside the mesh's GeometryPool spans (GeometryPool::Describe; stable while resident).
        uint64_t VertexAddress =
             0; // 0  the submesh's first Desert::Vertex (mesh BaseVertex + Submesh VertexOffset)
        uint64_t VertexRefAddress = 0; // 8  ClusterDag::VertexRefs, uint32 each (the mesh's cluster index span)
        uint64_t TriangleAddress  = 0; // 16 ClusterDag::Triangles, one packed uint32 per triangle
        uint64_t StreamAddress    = 0; // 24 the submesh's first MeshVertexStreams (0 = default streams)
        uint32_t RootNode         = 0; // 32 its root in Hierarchy[]
        uint32_t FirstCluster     = 0; // 36 its clusters: Clusters[FirstCluster, +ClusterCount)
        uint32_t ClusterCount     = 0; // 40
        uint32_t Submesh          = 0; // 44 the submesh index: GBuffer draw slot (lod 0, submesh) -> MaterialRow
        // NO SUBMESH TRANSFORM HERE. This row is the mesh's (one VirtualMeshTable for the process), the
        // DrawCommandCache that places a primitive is a view's (one per MeshRenderer), so a cache row written here
        // would be the last view's. The cut reads it where the cache put it when it placed the primitive: the
        // GpuDrawSlot of (lod 0, Submesh) of the view's main pass -> SubmeshTransforms[slot.SubmeshTransform]
        // (Common/VirtualGeometry.glslh VgSubmeshToWorld), the row the binned draw of the same submesh reads.
        uint32_t Pad0         = 0; // 48
        uint32_t VertexStride = 0; // 52 bytes between two vertices at VertexAddress (sizeof( Desert::Vertex ))
        uint32_t Pad1         = 0; // 56
        uint32_t Pad2         = 0; // 60
    };
    static_assert( sizeof( GpuClusterDag ) == 64, "std430 twin in Common/VirtualGeometry.glslh" );
    static_assert( offsetof( GpuClusterDag, RootNode ) == 32, "Common/VirtualGeometry.glslh" );
    static_assert( offsetof( GpuClusterDag, VertexStride ) == 52, "Common/VirtualGeometry.glslh" );

    // ONE ROW PER RESIDENT VIRTUAL-GEOMETRY MESH: its submesh DAGs. GpuScenePrimitive::VirtualMesh names it.
    struct GpuVirtualMesh
    {
        uint32_t FirstDag = 0; // 0 Dags[FirstDag, +DagCount): one per submesh, in submesh order
        uint32_t DagCount = 0; // 4
        uint32_t Pad0     = 0; // 8
        uint32_t Pad1     = 0; // 12
    };
    static_assert( sizeof( GpuVirtualMesh ) == 16, "std430 twin in Common/VirtualGeometry.glslh" );

    // ONE ROW PER CLUSTER (Geometry::ClusterRecord packed for the cull and the rasters).
    struct GpuCluster
    {
        glm::vec4 BoxCenter{ 0.0f };  // 0  xyz cm submesh space, w = LodError (cm)
        glm::vec4 BoxExtent{ 0.0f };  // 16 xyz half size, w = MaxEdgeLength (cm): the HW/SW split
        glm::vec4 LodBounds{ 0.0f };  // 32 xyz centre, w radius: the generating group's sphere
        uint32_t  FirstVertexRef = 0; // 48 words from GpuClusterDag::VertexRefAddress
        uint32_t  FirstTriangle  = 0; // 52 words from GpuClusterDag::TriangleAddress
        uint32_t  Counts         = 0; // 56 bits 0-7 VertexCount, 8-15 TriangleCount (<= 128 each), 16-23 Level
        uint32_t  Dag            = 0; // 60 its row in Dags[]
    };
    static_assert( sizeof( GpuCluster ) == 64, "std430 twin in Common/VirtualGeometry.glslh" );
    static_assert( offsetof( GpuCluster, FirstVertexRef ) == 48, "Common/VirtualGeometry.glslh" );
    // GpuCluster::Counts (Common/VirtualGeometry.glslh VgClusterVertexCount / TriangleCount / Level).
    inline constexpr uint32_t        kClusterCountBits = 8u;
    inline constexpr uint32_t        kClusterMaxLevel  = ( 1u << kClusterCountBits ) - 1u;
    [[nodiscard]] constexpr uint32_t PackClusterCounts( const uint32_t vertexCount, const uint32_t triangleCount,
                                                        const uint32_t level )
    {
        return vertexCount | ( triangleCount << kClusterCountBits ) | ( level << ( 2u * kClusterCountBits ) );
    }

    // ONE BVH NODE (Geometry::ClusterHierarchyNode packed). A leaf child is one group: LodBounds / the w of
    // BoxExtent are the group's LodBounds / ParentLodError, Child = kHierarchyLeaf | its first cluster (row in
    // Clusters[]), ClusterCount its member count.
    inline constexpr uint32_t kHierarchyLeaf = 1u << 31;
    struct GpuClusterHierarchyNode
    {
        glm::vec4 BoxCenter[4]{}; // 0   w = MinLodError below the child
        glm::vec4 BoxExtent[4]{}; // 64  w = MaxParentLodError below the child
        glm::vec4 LodBounds[4]{}; // 128
        uint32_t  Child[4]{ kInvalidIndex, kInvalidIndex, kInvalidIndex, kInvalidIndex }; // 192 node row | leaf
        uint32_t  ClusterCount[4]{}; // 208 0 for an interior child
    };
    static_assert( sizeof( GpuClusterHierarchyNode ) == 224, "std430 twin in Common/VirtualGeometry.glslh" );
    static_assert( offsetof( GpuClusterHierarchyNode, Child ) == 192, "Common/VirtualGeometry.glslh" );

    // THE CULL'S WORK QUEUE (UE FCandidateNode / FCandidateCluster): persistent-thread traversal pops one entry,
    // tests it, pushes its surviving children. Node = a Hierarchy row, or kHierarchyLeaf | cluster row.
    struct GpuClusterCandidate
    {
        uint32_t Instance = 0; // 0 its row in GpuScene Instances[]
        uint32_t Dag      = 0; // 4 its row in Dags[]
        uint32_t Node     = 0; // 8
        uint32_t View     = 0; // 12 the GpuCullView it is culled for
    };
    static_assert( sizeof( GpuClusterCandidate ) == 16, "std430 twin in Common/VirtualGeometry.glslh" );

    // ONE ROW PER CLUSTER TO RASTERISE (UE FVisibleCluster). VisibleClusters[] holds SW clusters from the front
    // and HW clusters from the back of one buffer (UE layout), so one capacity serves both.
    struct GpuVisibleCluster
    {
        uint32_t Instance = 0; // 0
        uint32_t Cluster  = 0; // 4 its row in Clusters[]
        uint32_t View     = 0; // 8
        uint32_t Flags    = 0; // 12 VisibleClusterFlags
    };
    static_assert( sizeof( GpuVisibleCluster ) == 16, "std430 twin in Common/VirtualGeometry.glslh" );

    namespace VisibleClusterFlags
    {
        inline constexpr uint32_t kSoftware      = 1u << 0; // drawn by the compute raster
        inline constexpr uint32_t kPostOcclusion = 1u << 1; // found by phase 2
    } // namespace VisibleClusterFlags

    // The rasters' indirect arguments, written by the cluster cull. HwDraw is a VkDrawIndirectCommand: vertexCount
    // = kClusterMaxTriangles * 3, instanceCount = HW clusters; the vertex shader reads triangle gl_VertexIndex / 3
    // of VisibleClusters[capacity - 1 - gl_InstanceIndex] and emits a NaN position past TriangleCount (UE's
    // non-mesh-shader HW path). SwDispatch is a VkDispatchIndirectCommand: one workgroup per SW cluster.
    struct GpuClusterRasterArgs
    {
        uint32_t SwDispatch[3]{ 0u, 1u, 1u }; // 0
        uint32_t SwCount = 0;                 // 12
        uint32_t HwDraw[4]{ 0u, 0u, 0u, 0u }; // 16 vertexCount, instanceCount, firstVertex, firstInstance
    };
    static_assert( sizeof( GpuClusterRasterArgs ) == 32, "std430 twin in Common/VirtualGeometry.glslh" );

    // THE VISIBILITY BUFFER WORD: one uint64 per pixel in a storage BUFFER (shaderBufferInt64Atomics, Vulkan 1.2;
    // a storage image of R64_UINT needs VK_EXT_shader_image_atomic_int64, which MoltenVK does not expose).
    // High 32 bits = the reversed-Z depth's float bits (nearer = larger, so atomicMax keeps the nearest; the
    // HZB's convention, HZBRenderer.hpp), low 32 = (visible-cluster index << kVisTriangleBits) | triangle.
    // 0 = nothing drawn (the far plane).
    inline constexpr uint32_t kVisTriangleBits    = 7u;  // kClusterMaxTriangles = 128
    inline constexpr uint32_t kVisClusterBits     = 25u; // 33.5 M visible clusters per view
    inline constexpr uint32_t kMaxVisibleClusters = 1u << kVisClusterBits;
    // THE BUDGET OF ONE CLUSTER CUT DISPATCH (UE r.Nanite.MaxVisibleClusters, 4 M): the visible list of a cut is
    // sized min(the caller's exact bound, this) - VirtualGeometryRenderer::AddClusterCut. The VSM cuts all 17
    // clipmap levels in one dispatch, so its exact bound is 17 x one view's; past the budget a cluster is not
    // emitted and is counted (ClusterCutStatus::VisibleOverflow -> views.virtualGeometry.overflow), never written
    // out of range.
    inline constexpr uint32_t kMaxVisibleClustersPerCut = 1u << 22;
    static_assert( kMaxVisibleClustersPerCut <= kMaxVisibleClusters, "the budget is inside the visibility word" );
    [[nodiscard]] constexpr uint64_t PackVisibility( const uint32_t depthBits, const uint32_t visibleCluster,
                                                     const uint32_t triangle )
    {
        return ( static_cast<uint64_t>( depthBits ) << 32 ) |
               static_cast<uint64_t>( ( visibleCluster << kVisTriangleBits ) | triangle );
    }
    static_assert( kVisTriangleBits + kVisClusterBits == 32u, "payload is the low 32 bits" );

    // The HW/SW split (UE r.Nanite.MinPixelsPerEdgeHW = 32): a cluster whose MaxEdgeLength projects below this
    // many pixels goes to the compute raster.
    inline constexpr float kSwRasterMaxEdgePixels = 32.0f;
} // namespace Desert::Graphic::VirtualGeometry
