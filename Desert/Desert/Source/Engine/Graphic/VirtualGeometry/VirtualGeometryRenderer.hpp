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

    // What one phase leaves for the rasters, per dispatch of all views.
    struct ClusterCullResult
    {
        RDG::BufferRef VisibleClusters;    // GpuVisibleCluster[capacity], SW from the front, HW from the back
        RDG::BufferRef RasterArgs;         // GpuClusterRasterArgs — Access::IndirectArgs
        RDG::BufferRef OccludedCandidates; // GpuClusterCandidate[] for the post phase (Main only)
        RDG::BufferRef OccludedArgs;       // VkDispatchIndirectCommand of the post phase (Main only)
    };

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

        // The device can run it: shaderBufferInt64Atomics + fragmentStoresAndAtomics + bufferDeviceAddress
        // (the capability row "VirtualGeometry"). Without it the method is refused with the missing feature named
        // and VG meshes draw their DAG-cut discrete LODs through the bins.
        [[nodiscard]] static Common::BoolResultStr CheckDevice();

        // Step 2/5: the cluster cull of @p candidates (InstanceCulling's VG output) for @p views.
        [[nodiscard]] Common::ResultStr<ClusterCullResult>
        AddClusterCull( RDG::Builder& graph, GpuDriven::CullPhase phase, const GpuDriven::GpuSceneBuffers& scene,
                        const VirtualMeshBuffers& meshes, RDG::BufferRef candidates, RDG::BufferRef candidateArgs,
                        std::span<const ClusterCullView> views ) const;

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
} // namespace Desert::Graphic::VirtualGeometry
