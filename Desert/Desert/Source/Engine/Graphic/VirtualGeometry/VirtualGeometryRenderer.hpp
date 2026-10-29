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
// RasterBinning + HW/SW raster, EmitDepthTargets, ShadeBinning + material compute shading). Compute + indirect,
// no mesh shaders (decision 10-28). Contract page: Notion Wiki «VIRT-GEOM — контракт».
//
// RDG ORDER (SceneRenderer::AddFrameVirtualGeometry, after AddFrameGpuCull's instance phase 1):
//  1. instance cull       InstanceCulling (GPU-DRIVEN, unchanged owner): a kVirtualGeometry primitive's surviving
//                         instances go to ClusterCandidates (one per (instance, submesh DAG root, view)) instead
//                         of bins. Same frustum / caster / distance / two-phase HZB code, no second instance
//                         culler.
//  2. cluster cull (Main) "VirtualGeometry: Cluster Cull": persistent-thread BVH traversal; per node / cluster:
//                         frustum, previous-frame furthest HZB, LOD (Project(LodBounds, error) vs
//                         MaxPixelsPerEdge), HW/SW split (kSwRasterMaxEdgePixels). Rejected-by-HZB go to the
//                         carry.
//  3. raster (Main)       "VirtualGeometry: SW Raster" (compute, one workgroup per cluster) and
//                         "VirtualGeometry: HW Raster" (indirect draw, no depth target: 64-bit atomicMax in the
//                         fragment) both into the visibility buffer.
//  4. HZB                 HZBRenderer::AddPasses( Furthest ) over the depth step 3 wrote (EmitDepth's depth).
//  5. Post phase          instance phase 2 + cluster cull of the carry against this HZB, raster again.
//  6. emit depth          "VirtualGeometry: Emit Depth": visibility buffer -> scene depth + velocity + stencil
//                         (the non-VG binned passes then depth-test against it).
//  7. material resolve    "VirtualGeometry: Shade Binning" (pixels -> one bin per GBuffer draw slot's material
//                         pipeline) then one indirect compute dispatch per material pipeline writing the G-buffer
//                         targets of SceneRenderer::AddFrameGBuffer ("VirtualGeometry: Shade <material>"). The
//                         binned "Deferred: GBuffer" pass draws the non-VG meshes after it, depth-equal.
// SHADOW VIEWS: each CSM cascade and VSM level is a GpuCullView of steps 1-3 (no HZB phase, depth only): the
// cascade rasters into an R32_UINT image copied into its depth layer ("VirtualGeometry: Emit Shadow Depth"), the
// VSM level rasters straight into VirtualShadowMapFrame::PhysicalPool (R32_UINT atomic max) through the page
// table.
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

    // Where a raster writes. Exactly one of the three is valid.
    struct RasterTarget
    {
        RDG::BufferRef  VisibilityBuffer; // uint64[width * height] (VirtualGeometryLayout.hpp PackVisibility)
        RDG::TextureRef ShadowDepth;      // R32_UINT cascade image
        RDG::TextureRef VsmPhysicalPool;  // VirtualShadowMapFrame::PhysicalPool (+ its page table, VsmPageTable)
        RDG::BufferRef  VsmPageTable;
        uint32_t        Width  = 0;
        uint32_t        Height = 0;
    };

    struct MaterialResolveTargets
    {
        // The G-buffer AddFrameGBuffer writes (DeferredLightingRenderer.hpp inputs), as storage images.
        RDG::TextureRef GBufferA;
        RDG::TextureRef GBufferB;
        RDG::TextureRef ShadingWord;
        RDG::TextureRef Emissive;
        RDG::TextureRef Velocity;
    };

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

        // Step 3/5: SW then HW raster of @p culled into @p target.
        [[nodiscard]] Common::BoolResultStr
        AddRaster( RDG::Builder& graph, const GpuDriven::GpuSceneBuffers& scene, const VirtualMeshBuffers& meshes,
                   const ClusterCullResult& culled, const RasterTarget& target ) const;

        // Step 6: depth (+ velocity) from the visibility buffer. Returns the scene depth it wrote.
        [[nodiscard]] Common::ResultStr<RDG::TextureRef>
        AddEmitDepth( RDG::Builder& graph, const GpuDriven::GpuSceneBuffers& scene,
                      const VirtualMeshBuffers& meshes, RDG::BufferRef visibilityBuffer,
                      RDG::TextureRef sceneDepth, RDG::TextureRef velocity, uint32_t width,
                      uint32_t height ) const;

        // Step 7: shade binning + one compute dispatch per GBuffer material pipeline. @p gbufferPass: the GBuffer
        // pass's draw tables (its GpuDrawSlot::MaterialRow and pipeline per (primitive, submesh)).
        [[nodiscard]] Common::BoolResultStr
        AddMaterialResolve( RDG::Builder& graph, const GpuDriven::GpuSceneBuffers& scene,
                            const VirtualMeshBuffers& meshes, const GpuDriven::PassDrawTable& gbufferTable,
                            const GpuDriven::PassDrawBuffers& gbufferPass, RDG::BufferRef visibilityBuffer,
                            const ClusterCullResult& mainCulled, const ClusterCullResult& postCulled,
                            const MaterialResolveTargets& targets, uint32_t width, uint32_t height ) const;

        // Shadow cascade: the R32_UINT image a cascade's AddRaster wrote -> the cascade's depth layer.
        [[nodiscard]] Common::BoolResultStr AddEmitShadowDepth( RDG::Builder& graph, RDG::TextureRef shadowDepth,
                                                                RDG::TextureRef cascadeDepth,
                                                                uint32_t        cascadeLayer ) const;

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
    };
    static_assert( sizeof( ClusterCutStatus ) == 32, "std430 twin: ClusterCut.shader ClusterCutStatus" );

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
