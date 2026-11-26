#pragma once

#include <Engine/Graphic/ShaderProtocols/ShaderLayout.hpp>
#include <Engine/Graphic/ShaderProtocols/ShaderProtocol.hpp>

#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

// VIRT-GEOM contract: the std430 rows of the virtual-geometry GPU tables and the visibility-buffer word. Each row
// is declared once here; the GLSL struct of the same layout is generated at build time into
// Generated/VirtualGeometry/VirtualGeometry.glslh (kVirtualGeometryShaderInclude below), which
// Common/VirtualGeometry.glslh includes.
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
#define DESERT_GPU_CLUSTER_DAG_FIELDS( X ) \
    /* Device addresses inside the mesh's GeometryPool spans (GeometryPool::Describe; stable while resident). \
       the submesh's first Desert::Vertex (mesh BaseVertex + Submesh VertexOffset) */ \
    X( uint64_t, VertexAddress, "VertexAddress", 0 ) \
    /* ClusterDag::VertexRefs, uint32 each (the mesh's cluster index span) */ \
    X( uint64_t, VertexRefAddress, "VertexRefAddress", 0 ) \
    /* ClusterDag::Triangles, one packed uint32 per triangle */ \
    X( uint64_t, TriangleAddress, "TriangleAddress", 0 ) \
    /* the submesh's first MeshVertexStreams (0 = default streams) */ \
    X( uint64_t, StreamAddress, "StreamAddress", 0 ) \
    X( uint32_t, RootNode, "RootNode", 0 ) /* its root in Hierarchy[] */ \
    X( uint32_t, FirstCluster, "FirstCluster", 0 ) /* its clusters: Clusters[FirstCluster, +ClusterCount) */ \
    X( uint32_t, ClusterCount, "ClusterCount", 0 ) \
    /* the submesh index: GBuffer draw slot (lod 0, submesh) -> MaterialRow */ \
    X( uint32_t, Submesh, "Submesh", 0 ) \
    /* NO SUBMESH TRANSFORM HERE. This row is the mesh's (one VirtualMeshTable for the process), the \
       DrawCommandCache that places a primitive is a view's (one per MeshRenderer), so a cache row written \
       here would be the last view's. The cut reads it where the cache put it when it placed the primitive: \
       the GpuDrawSlot of (lod 0, Submesh) of the view's main pass -> SubmeshTransforms[slot.SubmeshTransform] \
       (Common/VirtualGeometry.glslh VgSubmeshToWorld), the row the binned draw of the same submesh reads. */ \
    X( uint32_t, Pad0, "Pad0", 0 ) \
    /* bytes between two vertices at VertexAddress (sizeof( Desert::Vertex )) */ \
    X( uint32_t, VertexStride, "VertexStride", 0 ) \
    X( uint32_t, Pad1, "Pad1", 0 ) \
    X( uint32_t, Pad2, "Pad2", 0 )
    DESERT_SHADER_STRUCT( GpuClusterDag, "VgClusterDag", Std430, DESERT_GPU_CLUSTER_DAG_FIELDS );
#undef DESERT_GPU_CLUSTER_DAG_FIELDS

    // ONE ROW PER RESIDENT VIRTUAL-GEOMETRY MESH: its submesh DAGs. GpuScenePrimitive::VirtualMesh names it.
    struct GpuVirtualMesh
    {
        uint32_t FirstDag = 0; // 0 Dags[FirstDag, +DagCount): one per submesh, in submesh order
        uint32_t DagCount = 0; // 4
        uint32_t Pad0     = 0; // 8
        uint32_t Pad1     = 0; // 12
    };
    // No shader declares this row: plain C++, the stride of VirtualMeshTable's Meshes table.
    static_assert( sizeof( GpuVirtualMesh ) == 16, "VirtualMeshTable Meshes row stride" );

    // ONE ROW PER CLUSTER (Geometry::ClusterRecord packed for the cull and the rasters).
#define DESERT_GPU_CLUSTER_FIELDS( X ) \
    X( glm::vec4, BoxCenter, "BoxCenter", glm::vec4( 0.0f ) ) /* xyz cm submesh space, w = LodError (cm) */ \
    /* xyz half size, w = MaxEdgeLength (cm): the HW/SW split */ \
    X( glm::vec4, BoxExtent, "BoxExtent", glm::vec4( 0.0f ) ) \
    /* xyz centre, w radius: the generating group's sphere */ \
    X( glm::vec4, LodBounds, "LodBounds", glm::vec4( 0.0f ) ) \
    X( uint32_t, FirstVertexRef, "FirstVertexRef", 0 ) /* words from GpuClusterDag::VertexRefAddress */ \
    X( uint32_t, FirstTriangle, "FirstTriangle", 0 ) /* words from GpuClusterDag::TriangleAddress */ \
    X( uint32_t, Counts, "Counts", 0 ) /* bits 0-7 VertexCount, 8-15 TriangleCount (<= 128 each), 16-23 Level */ \
    X( uint32_t, Dag, "Dag", 0 ) /* its row in Dags[] */
    DESERT_SHADER_STRUCT( GpuCluster, "VgCluster", Std430, DESERT_GPU_CLUSTER_FIELDS );
#undef DESERT_GPU_CLUSTER_FIELDS
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
    using Vec4Array4 = std::array<glm::vec4, 4>;
    using UintArray4 = std::array<uint32_t, 4>;
#define DESERT_GPU_CLUSTER_HIERARCHY_NODE_FIELDS( X ) \
    X( Vec4Array4, BoxCenter, "BoxCenter", {} ) /* w = MinLodError below the child */ \
    X( Vec4Array4, BoxExtent, "BoxExtent", {} ) /* w = MaxParentLodError below the child */ \
    X( Vec4Array4, LodBounds, "LodBounds", {} ) \
    /* node row | leaf */ \
    X( UintArray4, Child, "Child", ( UintArray4{ kInvalidIndex, kInvalidIndex, kInvalidIndex, kInvalidIndex } ) ) \
    X( UintArray4, ClusterCount, "ClusterCount", {} ) /* 0 for an interior child */
    DESERT_SHADER_STRUCT( GpuClusterHierarchyNode, "VgHierarchyNode", Std430,
                          DESERT_GPU_CLUSTER_HIERARCHY_NODE_FIELDS );
#undef DESERT_GPU_CLUSTER_HIERARCHY_NODE_FIELDS

    // THE CULL'S WORK QUEUE (UE FCandidateNode / FCandidateCluster): persistent-thread traversal pops one entry,
    // tests it, pushes its surviving children. Node = a Hierarchy row, or kHierarchyLeaf | cluster row.
#define DESERT_GPU_CLUSTER_CANDIDATE_FIELDS( X ) \
    X( uint32_t, Instance, "Instance", 0 ) /* its row in GpuScene Instances[] */ \
    X( uint32_t, Dag, "Dag", 0 ) /* its row in Dags[] */ \
    X( uint32_t, Node, "Node", 0 ) \
    X( uint32_t, View, "View", 0 ) /* the GpuCullView it is culled for */
    DESERT_SHADER_STRUCT( GpuClusterCandidate, "VgClusterCandidate", Std430, DESERT_GPU_CLUSTER_CANDIDATE_FIELDS );
#undef DESERT_GPU_CLUSTER_CANDIDATE_FIELDS

    // ONE ROW PER CLUSTER TO RASTERISE (UE FVisibleCluster). VisibleClusters[] holds SW clusters from the front
    // and HW clusters from the back of one buffer (UE layout), so one capacity serves both.
#define DESERT_GPU_VISIBLE_CLUSTER_FIELDS( X ) \
    X( uint32_t, Instance, "Instance", 0 ) \
    X( uint32_t, Cluster, "Cluster", 0 ) /* its row in Clusters[] */ \
    X( uint32_t, View, "View", 0 ) \
    X( uint32_t, Flags, "Flags", 0 ) /* VisibleClusterFlags */
    DESERT_SHADER_STRUCT( GpuVisibleCluster, "VgVisibleCluster", Std430, DESERT_GPU_VISIBLE_CLUSTER_FIELDS );
#undef DESERT_GPU_VISIBLE_CLUSTER_FIELDS

    namespace VisibleClusterFlags
    {
        inline constexpr uint32_t kSoftware      = 1u << 0; // drawn by the compute raster
        inline constexpr uint32_t kPostOcclusion = 1u << 1; // found by phase 2
        // GpuDriven::kVisibleRestPose (GLSL DESERT_GPU_VISIBLE_REST_POSE): the cut's caster view draws this swaying
        // instance in its rest pose (GpuCull_CastsRestPose; ClusterCut.shader CutFrame.RestPose).
        inline constexpr uint32_t kRestPose = 1u << 31;
    } // namespace VisibleClusterFlags

    // The rasters' indirect arguments, written by the cluster cull. HwDraw is a VkDrawIndirectCommand: vertexCount
    // = kClusterMaxTriangles * 3, instanceCount = HW clusters; the vertex shader reads triangle gl_VertexIndex / 3
    // of VisibleClusters[capacity - 1 - gl_InstanceIndex] and emits a NaN position past TriangleCount (UE's
    // non-mesh-shader HW path). SwDispatch is a VkDispatchIndirectCommand: one SoftwareRaster.shader workgroup per SW
    // cluster up to kSwRasterMaxGroups (the workgroups stride over the rest); SwCount = the SW clusters,
    // VisibleClusters[0, SwCount).
    using UintArray3 = std::array<uint32_t, 3>;
    using UintArray4 = std::array<uint32_t, 4>;
#define DESERT_GPU_CLUSTER_RASTER_ARGS_FIELDS( X ) \
    X( UintArray3, SwDispatch, "SwDispatch", ( UintArray3{ 0u, 1u, 1u } ) ) \
    X( uint32_t, SwCount, "SwCount", 0 ) \
    /* vertexCount, instanceCount, firstVertex, firstInstance */ \
    X( UintArray4, HwDraw, "HwDraw", ( UintArray4{ 0u, 0u, 0u, 0u } ) ) \
    /* VkDispatchIndirectCommand of "VirtualGeometry: Raster Binning" Count / Scatter (UE: the raster binning \
       dispatches are indirect, sized by the visible clusters): one workgroup per VG_RASTER_BIN_GROUP_SIZE HW \
       clusters, counted by the cut as it emits (ClusterCut.shader EmitCluster), so an empty or small view \
       bins nothing or little. */ \
    X( UintArray3, BinDispatch, "BinDispatch", ( UintArray3{ 0u, 1u, 1u } ) ) \
    /* rows the cut took from the visible list, SW + HW: a cluster takes one before it picks a raster, so \
       SwCount + HwDraw[1] never pass the capacity and the two ends never meet (ClusterCut.shader \
       EmitCluster). */ \
    X( uint32_t, Emitted, "Emitted", 0 ) \
    /* 48 / 52 VG-SW-RASTER (UE Nanite: the post pass appends to the main pass's VisibleClusters): the first \
       SW row and the first HW counter value of THIS cut. A main cut starts both lists empty (0); the post cut \
       continues the main cut's counters in the same list (ClusterCut.shader WriteDispatchArgs sets them from \
       the copied counts), so its SW raster reads VisibleClusters[SwFirst, SwCount) and its raster binning the \
       HW counters [HwFirst, HwDraw[1]). */ \
    X( uint32_t, SwFirst, "SwFirst", 0 ) \
    X( uint32_t, HwFirst, "HwFirst", 0 ) \
    X( uint32_t, Pad0, "Pad0", 0 ) \
    X( uint32_t, Pad1, "Pad1", 0 )
    DESERT_SHADER_STRUCT( GpuClusterRasterArgs, "VgRasterArgs", Std430, DESERT_GPU_CLUSTER_RASTER_ARGS_FIELDS );
#undef DESERT_GPU_CLUSTER_RASTER_ARGS_FIELDS
    // ClusterCut.shader's Status block (Buffer(14), the program's binding): each failure counted with the lowest
    // DAG row it hit (kInvalidIndex = none), so the host names the mesh.
#define DESERT_CLUSTER_CUT_STATUS_FIELDS( X )     X( uint32_t, StackOverflow, "StackOverflow", 0 ) /* hierarchy nodes a full traversal stack could not take */     X( uint32_t, StackOverflowDag, "StackOverflowDag", kInvalidIndex )     X( uint32_t, MissingSlot, "MissingSlot", 0 ) /* candidates whose primitive has no lod-0 draw slot for the DAG's submesh */     X( uint32_t, MissingSlotDag, "MissingSlotDag", kInvalidIndex )     X( uint32_t, VisibleOverflow, "VisibleOverflow", 0 ) /* clusters past ClusterCandidates::VisibleCapacity */     X( uint32_t, VisibleOverflowDag, "VisibleOverflowDag", kInvalidIndex )     X( uint32_t, OccludedNodes, "OccludedNodes", 0 ) /* node children the previous-frame HZB hid (not a failure) */     X( uint32_t, OccludedClusters, "OccludedClusters", 0 ) /* clusters the previous-frame HZB hid (not a failure) */     X( uint32_t, BinnedVgInstances, "BinnedVgInstances", 0 ) /* VG instances InstanceCull still put into a bin of a view the cut serves (an invariant: 0; the raster draws them), copied from the candidates' count block */     X( uint32_t, OccludedOverflow, "OccludedOverflow", 0 ) /* occluded entries past the post carry's capacity (missing one frame) */
    DESERT_SHADER_STRUCT( ClusterCutStatus, "VgClusterCutStatus", Std430, DESERT_CLUSTER_CUT_STATUS_FIELDS );
#undef DESERT_CLUSTER_CUT_STATUS_FIELDS

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
    // The visible list's capacity a cut of the caller's exact bound @p visibleBound gets (the ONE rule:
    // VirtualGeometryRenderer::AddClusterCut sizes the list by it, the SW raster gate checks its rows by it).
    [[nodiscard]] constexpr uint32_t ClusterCutVisibleCapacity( const uint32_t visibleBound ) noexcept
    {
        return visibleBound < kMaxVisibleClustersPerCut ? visibleBound : kMaxVisibleClustersPerCut;
    }
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
    // SoftwareRaster.shader's indirect X bound (GLSL VG_SW_RASTER_MAX_GROUPS; maxComputeWorkGroupCount[0] is at
    // least 65535 on every Vulkan device).
    inline constexpr uint32_t kSwRasterMaxGroups = 65535u;

    inline constexpr const ShaderProtocols::ShaderStructDecl* kVirtualGeometryStructs[] = {
        &kGpuClusterDagStruct,      &kGpuClusterStruct,        &kGpuClusterHierarchyNodeStruct,
        &kGpuClusterCandidateStruct, &kGpuVisibleClusterStruct, &kGpuClusterRasterArgsStruct,
        &kClusterCutStatusStruct,
    };
    inline constexpr ShaderProtocols::ShaderConstantDecl kVirtualGeometryConstants[] = {
        { ShaderProtocols::ShaderConstantDecl::Type::Uint, "VG_HIERARCHY_LEAF", kHierarchyLeaf },
        { ShaderProtocols::ShaderConstantDecl::Type::Uint, "VG_INVALID_INDEX", kInvalidIndex },
        { ShaderProtocols::ShaderConstantDecl::Type::Uint, "VG_VISIBLE_SOFTWARE", VisibleClusterFlags::kSoftware },
        { ShaderProtocols::ShaderConstantDecl::Type::Uint, "VG_SW_RASTER_MAX_GROUPS", kSwRasterMaxGroups },
    };
    // Common/VirtualGeometry.glslh includes it; ClusterCut.shader's Status block takes DESERT_VgClusterCutStatus_MEMBERS.
    inline constexpr ShaderProtocols::ShaderIncludeDecl kVirtualGeometryShaderInclude{
        .Path         = "Generated/VirtualGeometry/VirtualGeometry.glslh",
        .SourceHeader = "Desert/Desert/Source/Engine/Graphic/VirtualGeometry/VirtualGeometryLayout.hpp",
        .Structs      = kVirtualGeometryStructs,
        .Constants    = kVirtualGeometryConstants,
    };
} // namespace Desert::Graphic::VirtualGeometry
