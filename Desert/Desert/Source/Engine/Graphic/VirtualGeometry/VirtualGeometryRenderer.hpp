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
//                         DAG row and goes into NO bin of that view (nor its occlusion carry). Every cull view of
//                         the draw bins (main + drawn shadow cascades) has a cut view.
//  2. cluster cull (Main) "VirtualGeometry: Cluster Cut": persistent-thread BVH traversal; per node / cluster:
//                         frustum, previous-frame furthest HZB, LOD (Project(LodBounds, error) vs
//                         MaxPixelsPerEdge). Occluded node children / clusters are carried to the post phase
//                         (ClusterCullResult::OccludedCandidates).
//  3. raster binning      "VirtualGeometry: Raster Binning" (AddRasterBinning): the cut's HW clusters -> one
//                         VkDrawIndirectCommand per IndirectGroup of the main view's pass + the instance id rows.
//  4. raster              the main pass's mesh list (MeshRenderer::SetFrameVirtualGeometry), after its bins: per
//                         group IndirectGroup::VirtualGeometryPipeline, RendererAPI::RenderClusterIndirectGroup.
//                         Depth, G-buffer / forward colour, velocity as any binned draw; the HZB of the frame
//                         (AddFrameDrawBinsPostOcclusion) therefore holds the clusters.
//  5. cluster cull (PostOcclusion) "VirtualGeometry: Cluster Cut Post" (AddFrameDrawBinsPostOcclusion, after this
//                         frame's furthest HZB): the main phase's carry re-cut against that HZB through
//                         ViewProjection, raster-binned and drawn by the GBuffer post-occlusion pass
//                         (MeshRenderer::SetFrameVirtualGeometry, CullPhase::PostOcclusion).
//  6. shadow cascades     one cut + raster binning per drawn cascade view (its own dispatch: one RasterBins per
//                         view), LOD from the main camera as the bins (the shadows draw the main view's LOD), no
//                         HZB (cascades do not occlusion-cull), drawn in each cascade's static layer.
// NOT YET: the SW raster of sub-pixel triangles (visibility buffer).
namespace Desert::Graphic::VirtualGeometry
{
    // One view the cluster cull serves: the instance cull's view (same GpuCullView, same frustum, same HZBs).
    struct ClusterCullView
    {
        GpuDriven::CullViewInput Instance{};
        // The LOD threshold for this view, pixels per edge (Scalability VirtualGeometry.MaxPixelsPerEdge x the
        // view's LOD scale, VIEW-LOD-FOV). Shadow cascades use the main view's (their LOD is the main camera's;
        // UE: the VSM page texel).
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
        // Main only: the post phase's ClusterCandidates (Rows / Count), capacity VisibleCapacity. A row's Node
        // names the occluded node child or cluster (ClusterCut.shader VG_POST_CHILD / VG_POST_CLUSTER).
        RDG::BufferRef OccludedCandidates; // GpuClusterCandidate[VisibleCapacity]
        RDG::BufferRef OccludedCount;      // uint32 at offset 0, 0 at offset 4 (the candidates' count block)
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
        // passes, a missing table, views with two HZBs, a PostOcclusion phase with no view holding Instance.Hzb.
        // @p views are the cut views [@p firstView, firstView + size): GpuClusterCandidate::View is a cut view of
        // the frame, a candidate of another view is skipped (one dispatch per mesh pass's views).
        // Main: node and cluster boxes are tested against the views' Instance.PrevHzb (the instance cull's test,
        // Common/GpuCullTests.glslh), the occluded carried into the result's OccludedCandidates; @p neutralHzb is
        // bound when no view has one (nothing occluded). PostOcclusion: @p candidates is a Main result's carry
        // (same @p views, same order), tested against the views' Instance.Hzb (this frame's).
        [[nodiscard]] Common::ResultStr<ClusterCullResult>
        AddClusterCull( RDG::Builder& graph, GpuDriven::CullPhase phase, const GpuDriven::GpuSceneBuffers& scene,
                        const VirtualMeshBuffers& meshes, const GpuDriven::PassDrawTable& passTable,
                        const GpuDriven::PassDrawBuffers& passSlots, RDG::BufferRef submeshTransforms,
                        const ClusterCandidates& candidates, std::span<const ClusterCullView> views,
                        uint32_t firstView, RDG::TextureRef neutralHzb ) const;

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
        uint32_t OccludedOverflow  = 0; // occluded entries past the post carry's capacity (missing one frame)
    };
    static_assert( sizeof( ClusterCutStatus ) == 40, "std430 twin: ClusterCut.shader ClusterCutStatus" );

    struct ClusterCutCount
    {
        uint32_t ClustersVisible = 0; // the main view's main phase: RasterArgs.HwDraw[1]
        // What the rasters DREW: per cut, the sum of RasterBins::GroupArgs instanceCount over the groups the mesh
        // pass drew (a group with no `.VirtualGeometry` pipeline draws none of the clusters binned into it).
        uint32_t         ClustersDrawn       = 0; // the main view: main + post phase
        uint32_t         ShadowClustersDrawn = 0; // every shadow cascade
        ClusterCutStatus Status{};                // the main view's main phase
    };

    // Which cut of the frame a ClusterCutCounts entry is.
    enum class ClusterCutKind : uint32_t
    {
        Main,          // the main view, phase 1
        PostOcclusion, // the main view, phase 2
        ShadowCascade, // one cascade view
    };

    // HOW MANY CLUSTERS THE CUTS DREW (DesertCtl `state views.virtualGeometry`), the CullPhaseCounts pattern: each
    // cut's RasterArgs + Status (+ its raster's GroupArgs) reach the host through a frame-in-flight readback slot,
    // counted when the slot comes round again. A Status failure is logged once per message with the mesh the DAG
    // row belongs to.
    class ClusterCutCounts
    {
    public:
        // Counts the slot of @p frameIndex and empties it for this frame's cuts (Add).
        void BeginFrame( uint64_t frameIndex );
        // Hands this frame's cut @p culled of @p kind. @p bins: its raster's bins when the pass drew them, with
        // @p drawnGroups (one flag per group of bins: the pass drew that group's clusters); null = drew nothing.
        void Add( RDG::Builder& graph, ClusterCutKind kind, const ClusterCullResult& culled, const RasterBins* bins,
                  std::vector<uint8_t> drawnGroups );
        // A frame with no cut: nothing is handed, and the counts read so far are forgotten.
        void                                 Forget();
        [[nodiscard]] const ClusterCutCount& LastCount() const
        {
            return m_Last;
        }

    private:
        struct Entry
        {
            ClusterCutKind       Kind = ClusterCutKind::Main;
            RDG::ExternalBuffer  RasterArgs;
            RDG::ExternalBuffer  Status;
            RDG::ExternalBuffer  GroupArgs; // none: the cut was not drawn
            std::vector<uint8_t> DrawnGroups;
        };
        struct Slot
        {
            std::vector<Entry> Entries;
        };
        void Count( const Slot& slot );

        std::vector<Slot> m_Slots;
        Slot*             m_Current = nullptr; // this frame's slot (BeginFrame)
        ClusterCutCount   m_Last;
        std::string       m_LastError; // logged once per message
    };
} // namespace Desert::Graphic::VirtualGeometry
