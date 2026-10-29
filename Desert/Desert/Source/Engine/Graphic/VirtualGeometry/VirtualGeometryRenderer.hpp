#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/GpuDriven/DrawCommandCache.hpp>
#include <Engine/Graphic/GpuDriven/GpuScene.hpp>
#include <Engine/Graphic/GpuDriven/InstanceCulling.hpp>
#include <Engine/Graphic/RDG/RDGResources.hpp>
#include <Engine/Graphic/VirtualGeometry/VirtualMeshTable.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace Desert::Graphic::RDG
{
    class Builder;
}

// VIRT-GEOM contract: THE VIRTUAL-GEOMETRY FRAME (UE Nanite::FRenderer: InstanceHierarchyCull/NodeAndClusterCull,
// RasterBinning, HW raster). Compute + indirect, no mesh shaders (decision 10-28). Contract page: Notion Wiki
// «VIRT-GEOM — контракт».
//
// DECISION 10-29 (step 11): the clusters are HARDWARE-rastered THROUGH THE MATERIAL CELLS - a `.VirtualGeometry`
// permutation of each opaque Static `.Binned` cell, drawn per material group into the same depth + G-buffer /
// forward targets as the bins. UE rasters into a 64-bit visibility buffer and shades with ShadeBinning + one compute
// material pass per material; here every material pixel shader is reused unchanged (no compute twin per material),
// no int64 atomics are required, and HZB / velocity / stencil come from the normal raster. The visibility buffer
// stays the route of the SW raster of sub-pixel triangles (not built).
//
// RDG ORDER (SceneRenderer::AddFrameVirtualGeometry, after AddFrameDrawBins' instance phase 1):
//  1. instance cull       InstanceCulling (GPU-DRIVEN, unchanged owner): a VG primitive's instance that survives the
//                         frustum / caster / distance test in a view with a cut view appends one ClusterCandidate per
//                         DAG row and goes into NO bin of that view (nor its occlusion carry). Other views (shadow
//                         cascades today) still bin it.
//  2. cluster cull (Main) "VirtualGeometry: Cluster Cut": persistent-thread BVH traversal; per node / cluster:
//                         frustum, previous-frame furthest HZB, LOD (Project(LodBounds, error) vs
//                         MaxPixelsPerEdge). Occluded nodes / clusters are counted and dropped (no post carry yet).
//  3. raster binning      "VirtualGeometry: Raster Binning" (AddRasterBinning): the cut's HW clusters -> one
//                         VkDrawIndirectCommand per IndirectGroup of the main view's pass + the instance id rows.
//  4. raster              the main pass's mesh list (MeshRenderer::SetFrameVirtualGeometry), after its bins: per
//                         group IndirectGroup::VirtualGeometryPipeline, RendererAPI::RenderClusterIndirectGroup.
//                         Depth, G-buffer / forward colour, velocity as any binned draw; the HZB of the frame
//                         (AddFrameDrawBinsPostOcclusion) therefore holds the clusters.
// NOT YET: the post phase (phase-1-occluded clusters re-tested against this frame's HZB, binned and drawn by the
// GBuffer post pass), the shadow views (a cut per cascade view, its raster in the cascade's static layer), the SW
// raster.
namespace Desert::Graphic::VirtualGeometry
{
    // One view the cluster cull serves: the instance cull's view (same GpuCullView, same frustum, same HZBs).
    struct ClusterCullView
    {
        GpuDriven::CullViewInput Instance{};
        // The LOD threshold for this view, pixels per edge (Scalability VirtualGeometry.MaxPixelsPerEdge x the
        // view's LOD scale, VIEW-LOD-FOV). Shadow views use their own pixel size (UE: VSM page texel).
        float    MaxPixelsPerEdge = 1.0f;
        uint32_t ViewWidth        = 0; // pixels the LOD error projects into
        uint32_t ViewHeight       = 0;
    };

    // The cluster cull's input: the VG candidates InstanceCulling appended (GpuClusterCandidate::View indexes the
    // span of ClusterCullView handed with them). The count lives on the GPU (the producer's append counter);
    // Capacity bounds the rows read, VisibleCapacity the clusters the cut can emit - the caller's exact bound,
    // the sum of ClusterCount over every DAG a candidate can name (a cut never draws more than its DAG holds).
    struct ClusterCandidates
    {
        RDG::BufferRef Rows;  // GpuClusterCandidate[Capacity]
        RDG::BufferRef Count; // uint32 at offset 0: rows written (clamped to Capacity)
        uint32_t       Capacity        = 0;
        uint32_t       VisibleCapacity = 0;
    };

    // What one phase leaves for the rasters, per dispatch of all views.
    struct ClusterCullResult
    {
        RDG::BufferRef VisibleClusters;    // GpuVisibleCluster[capacity], SW from the front, HW from the back
        RDG::BufferRef RasterArgs;         // GpuClusterRasterArgs — Access::IndirectArgs
        RDG::BufferRef Status;             // ClusterCutStatus (what the cut could not do, by DAG row)
        uint32_t       VisibleCapacity = 0; // rows of VisibleClusters (HW row i = VisibleCapacity - 1 - i)
        RDG::BufferRef OccludedCandidates; // GpuClusterCandidate[] for the post phase (Main only)
        RDG::BufferRef OccludedArgs;       // VkDispatchIndirectCommand of the post phase (Main only)
    };

    // THE CUT'S HW CLUSTERS SORTED INTO ONE MESH PASS'S MATERIAL GROUPS (AddRasterBinning): what that pass's VG
    // raster draws - per IndirectGroup g of the pass table, one VkDrawIndirectCommand at GroupArgs + g x 16
    // (vertexCount = kClusterMaxTriangles x 3, instanceCount = its clusters, firstInstance = its first Rows entry),
    // drawn with the group's `.VirtualGeometry` cell and Rows bound as the instance id stream (a_VisibleRow).
    struct RasterBins
    {
        RDG::BufferRef GroupArgs;  // VkDrawIndirectCommand[GroupCount] - Access::IndirectArgs
        RDG::BufferRef Rows;       // uint32[VisibleCapacity]: a VisibleClusters row per draw instance - vertex input
        RDG::BufferRef VisibleClusters; // the cut's (Common/GpuScene.glslh VgRasterVisibleClusters)
        uint32_t       GroupCount = 0;
    };
    inline constexpr uint32_t kRasterBinsArgsStride = 16u; // sizeof( VkDrawIndirectCommand )

    class VirtualGeometryRenderer
    {
    public:
        VirtualGeometryRenderer();
        ~VirtualGeometryRenderer();
        VirtualGeometryRenderer( const VirtualGeometryRenderer& )            = delete;
        VirtualGeometryRenderer& operator=( const VirtualGeometryRenderer& ) = delete;

        // Step 2/5: "VirtualGeometry: Cluster Cut" of @p candidates (InstanceCulling's VG output) for @p views
        // (Programs/VirtualGeometry/ClusterCut.shader). Every view is of the one mesh pass @p passTable /
        // @p passSlots describe: a candidate's submesh transform is the row that pass's DrawCommandCache placed
        // for (primitive, lod 0, the DAG's submesh), indexing @p submeshTransforms. Refused: no view, views of two
        // passes, a missing table, the PostOcclusion phase (its HZB carry is not built yet), views with two
        // previous-frame HZBs. Node and cluster boxes are tested against the views' Instance.PrevHzb (the instance
        // cull's test, Common/GpuCullTests.glslh); @p neutralHzb is bound when no view has one (nothing occluded).
        [[nodiscard]] Common::ResultStr<ClusterCullResult>
        AddClusterCull( RDG::Builder& graph, GpuDriven::CullPhase phase, const GpuDriven::GpuSceneBuffers& scene,
                        const VirtualMeshBuffers& meshes, const GpuDriven::PassDrawTable& passTable,
                        const GpuDriven::PassDrawBuffers& passSlots, RDG::BufferRef submeshTransforms,
                        const ClusterCandidates& candidates, std::span<const ClusterCullView> views,
                        RDG::TextureRef neutralHzb ) const;

        // Step 11: "VirtualGeometry: Raster Binning" (Programs/VirtualGeometry/RasterBinning.shader) of @p culled's
        // HW clusters into the material groups of the mesh pass @p passTable / @p passSlots describe (a cluster's
        // group: its DAG submesh's lod-0 slot -> bin -> IndirectGroup). Refused: a pass with no group, missing
        // tables. A cluster whose submesh does not draw in the pass is binned nowhere.
        [[nodiscard]] Common::ResultStr<RasterBins>
        AddRasterBinning( RDG::Builder& graph, const GpuDriven::GpuSceneBuffers& scene,
                          const VirtualMeshBuffers& meshes, const GpuDriven::PassDrawTable& passTable,
                          const GpuDriven::PassDrawBuffers& passSlots, const ClusterCullResult& culled ) const;

        struct Programs;

    private:
        std::unique_ptr<Programs> m_Programs;
    };

    // ClusterCut.shader's Status block: each failure counted with the lowest DAG row it hit (kInvalidIndex =
    // none), so the host names the mesh.
    struct ClusterCutStatus
    {
        uint32_t StackOverflow      = 0; // hierarchy nodes a full traversal stack could not take
        uint32_t StackOverflowDag   = 0xFFFFFFFFu;
        uint32_t MissingSlot        = 0; // candidates whose primitive has no lod-0 draw slot for the DAG's submesh
        uint32_t MissingSlotDag     = 0xFFFFFFFFu;
        uint32_t VisibleOverflow    = 0; // clusters past ClusterCandidates::VisibleCapacity
        uint32_t VisibleOverflowDag = 0xFFFFFFFFu;
        uint32_t OccludedNodes      = 0; // node children the previous-frame HZB hid (not a failure)
        uint32_t OccludedClusters   = 0; // clusters the previous-frame HZB hid (not a failure)
        // VG instances InstanceCull still put into a bin of a view the cut serves (an invariant: 0; the raster
        // draws them), copied from the candidates' count block.
        uint32_t BinnedVgInstances = 0;
        uint32_t Pad               = 0;
    };
    static_assert( sizeof( ClusterCutStatus ) == 40, "std430 twin: ClusterCut.shader ClusterCutStatus" );

    struct ClusterCutCount
    {
        uint32_t ClustersVisible = 0; // RasterArgs.HwDraw[1]
        ClusterCutStatus Status{};
    };

    // HOW MANY CLUSTERS THE CUT DREW (DesertCtl `state views.virtualGeometry`), the CullPhaseCounts pattern: the
    // frame's RasterArgs + Status reach the host through a frame-in-flight readback slot, counted when the slot
    // comes round again. A Status failure is logged once per message with the mesh the DAG row belongs to.
    class ClusterCutCounts
    {
    public:
        // Counts the slot of @p frameIndex, then hands it this frame's @p culled.
        void AddFrame( RDG::Builder& graph, uint64_t frameIndex, const ClusterCullResult& culled );
        // A frame with no cut: nothing is handed, and the counts read so far are forgotten.
        void                                 Forget();
        [[nodiscard]] const ClusterCutCount& LastCount() const
        {
            return m_Last;
        }

    private:
        struct Slot
        {
            RDG::ExternalBuffer RasterArgs;
            RDG::ExternalBuffer Status;
        };
        void Count( const Slot& slot );

        std::vector<Slot> m_Slots;
        ClusterCutCount   m_Last;
        std::string       m_LastError; // logged once per message
    };
} // namespace Desert::Graphic::VirtualGeometry
