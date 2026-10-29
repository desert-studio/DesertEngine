#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Core/Frustum.hpp>
#include <Engine/Graphic/GpuDriven/DrawCommandCache.hpp>
#include <Engine/Graphic/GpuDriven/GpuScene.hpp>
#include <Engine/Graphic/GpuDriven/GpuSceneLayout.hpp>
#include <Engine/Graphic/RDG/RDGResources.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace Desert::Graphic::RDG
{
    class Builder;
}

// GPU-DRIVEN phase CULL — InstanceCulling: WHICH INSTANCES EACH (VIEW x PASS) DRAWS, DECIDED ON THE GPU
// (UE FInstanceCullingContext + the two-pass occlusion of InstanceCulling/BuildInstanceDrawCommands and
// HZB occlusion, Renderer/Private/InstanceCulling/*.cpp, Renderer/Private/HZB.cpp).
//
// Programs/GpuDriven/InstanceCull.shader (reading Common/GpuScene.glslh), per instance per view:
//  1. shadow-caster filter, ISM distance fade (KeepsInstanceAtDistance), frustum test of the instance's world box
//     against the caller's Core::Frustum planes (GpuCullView::FrustumPlanes) — replaces the CPU IsVisibleInView
//     per draw (VisibilityCulling.hpp:23-31; MeshRendererForward.cpp, MeshRendererShadow.cpp) and
//     CollectIsmInstances (InstanceCullDistance.hpp:97);
//  2. a VSM level (CullViewFlags::kVsmPageMask) adds the "VSM: Cull Casters" drawn-page test: the virtual shadow
//     map's caster cull (VirtualShadowMap.hpp AddCasterCulling) is a CullView flavour of this pass, not a culler;
//  3. LOD on the GPU: the GLSL twin of Geometry::SelectLODFromBounds after MeshRenderer::ComputeLOD's order;
//  4. TWO-PHASE FURTHEST-HZB occlusion: phase 1 against the PREVIOUS frame's HZB through PrevViewProjection,
//     the rejected go to OcclusionCarry; phase 2 re-tests them against this frame's HZB. A view without an HZB
//     (CSM, VSM) has no phase 2;
//  5. each survivor is counted into the bins of its (primitive, pass, LOD) slots and written to VisibleInstances.
//
// One dispatch per MESH PASS over all instances x that pass's views: a pass's bins are its own buffers
// (DrawCommandCache::PassDrawBuffers), so one dispatch binds one pass table. At most one HZB and one VSM dirty
// mask per dispatch (the main view's; every VSM level shares the frame's mask): refused otherwise.
namespace Desert::Graphic::GpuDriven
{
    enum class CullPhase : uint32_t
    {
        // Against the previous frame's HZB (or frustum only when the view has none).
        Main,
        // The instances Main rejected, against this frame's HZB built from Main's depth.
        PostOcclusion,
    };

    // One view x pass the cull serves this frame. View's FrustumPlanes / ViewIndex are filled from Frustum and the
    // view's position in the list. PrevHzb / Hzb: the view's FURTHEST-depth HZB of the previous / this frame
    // (invalid = no occlusion test). VsmDirtyMask: VirtualShadowMapFrame::DirtyMask for a kVsmPageMask view.
    struct CullViewInput
    {
        GpuCullView     View{};
        Core::Frustum   Frustum{};
        RDG::TextureRef PrevHzb{};
        RDG::TextureRef Hzb{};
        RDG::BufferRef  VsmDirtyMask{};
    };

    // What a pass draws with after one phase. The views of one mesh pass share the phase's buffers: this view's
    // records are IndirectDraws[FirstIndirectDraw, +bins) and their firstInstance already points into its range of
    // VisibleInstances. A bin the cull left empty keeps its record with InstanceCount 0 (decision 10-28: one fixed
    // table, no compaction, no GPU draw count).
    struct CulledDraws
    {
        RDG::BufferRef IndirectDraws;    // GpuIndirectDraw[views x Bins] — Access::IndirectArgs
        RDG::BufferRef VisibleInstances; // GpuVisibleInstance[views x VisibleCapacity]
        // uint[views x VisibleCapacity]: VisibleInstances[i].Instance at the same index - the binned draws'
        // per-instance vertex stream (4 B per visible instance, read at gl_InstanceIndex).
        RDG::BufferRef VisibleInstanceIds;
        uint32_t       FirstIndirectDraw = 0;
        // One past the last VisibleInstances row this view's records name ((view + 1) x VisibleCapacity): the rows
        // the binned vertex stage reaches through a_VisibleRow = gl_InstanceIndex.
        uint32_t VisibleRowEnd = 0;
    };

    // Phase 1's left-overs that phase 2 consumes, and the per (view, instance) Visibility words both write.
    struct OcclusionCarry
    {
        RDG::BufferRef OccludedInstances; // GpuOccludedInstance[views x instances]
        RDG::BufferRef OccludedCount;     // uint count, uint capacity, 2 x pad
        RDG::BufferRef DispatchArgs;      // VkDispatchIndirectCommand of phase 2 (+ pad)
        RDG::BufferRef Visibility;        // uint[views x InstanceCount] — CullVisibility words
        uint32_t       ViewCount     = 0;
        uint32_t       InstanceCount = 0;
    };

    // THE VIRTUAL-GEOMETRY CANDIDATES phase 1 appends (VIRT-GEOM contract step 1; UE InstanceHierarchyCull
    // writing Nanite's candidate nodes): a surviving instance of a primitive with DAG rows, in a view that has a
    // cluster-cut view, appends one GpuClusterCandidate (VirtualGeometryLayout.hpp) per DAG row of its primitive.
    // The test is the same frustum / caster / distance test as the bins'; the previous-frame HZB does not hold a
    // VG instance back (the cluster cut owns VG occlusion: per-cluster HZB + its own carry), so an occluded VG
    // instance still appends. It goes into no bin of that view: the VG raster draws its clusters.
    // Invalid PrimitiveDags = no VG this frame (nothing bound past dummies, nothing appended).
    // Bytes of one candidate row (VirtualGeometry::GpuClusterCandidate, static_asserted in
    // VirtualGeometryRenderer.cpp; the shader's VgClusterCandidate).
    inline constexpr uint32_t kVirtualGeometryCandidateBytes = 16u;

    struct VirtualGeometryCandidateInput
    {
        RDG::BufferRef PrimitiveDags; // uvec2[PrimitiveRows]: (first DagEntries row, DAG count); count 0 = not VG
        RDG::BufferRef DagEntries;    // uvec2[]: (Dags[] row, its RootNode)
        // uint[views.size()] indexed by the AddMainPhase view index: the GpuClusterCandidate::View it appends as
        // (the cluster cut's view row), DESERT_GPU_INVALID_INDEX = the view appends nothing.
        RDG::BufferRef CutViewOf;
        uint32_t       PrimitiveRows = 0;
        // Candidate rows PER CUT VIEW: the caller's exact bound of one view's append (instances x DAGs); every cut
        // view owns its own region of that many rows (UE: per-view candidate ranges, so a view's cut dispatch
        // covers only its own candidates).
        uint32_t Capacity = 0;
        uint32_t CutViews = 0; // regions: every CutViewOf value is below it
    };

    // The append's output, compacted PER CUT VIEW: cut view v's rows are Rows[v x Capacity, v x Capacity +
    // Counts[v]). Count block (uint32): [0] = VG instances that reached a bin of a view with a cut view (an
    // invariant: 0), [1 + v] = rows cut view v appended (may pass Capacity: the reader clamps).
    struct VirtualGeometryCandidateOutput
    {
        RDG::BufferRef Rows;  // GpuClusterCandidate[CutViews x Capacity]
        RDG::BufferRef Count; // uint32[1 + CutViews]
        uint32_t       Capacity = 0; // rows per cut view
        uint32_t       CutViews = 0;
    };

    struct MainPhaseResult
    {
        std::vector<CulledDraws>       Draws; // one per CullViewInput, same order
        OcclusionCarry                 Carry;
        VirtualGeometryCandidateOutput VirtualGeometry; // invalid refs when the input was
    };

    class InstanceCulling
    {
    public:
        InstanceCulling();
        ~InstanceCulling();
        InstanceCulling( const InstanceCulling& )            = delete;
        InstanceCulling& operator=( const InstanceCulling& ) = delete;

        // Phase 1 for every view in @p views: per mesh pass, one dispatch over the scene's instances x its views.
        // @p passes: indexed by MeshPass (kMeshPassCount entries; a pass with no bins has invalid refs).
        // Refused for an empty @p views (the caller adds no draw then), a missing pipeline, or a pass whose views
        // name more than one HZB / dirty mask. @p neutralHzb: a 1-texel texture any graph pass wrote (SceneRenderer
        // System.Black), bound for a pass none of whose views has an HZB (the HZB flag is then off in the push).
        // @p virtualGeometry: the VG candidate append (invalid PrimitiveDags = none); refused when its tables are
        // half given or its Capacity is 0.
        [[nodiscard]] Common::ResultStr<MainPhaseResult>
        AddMainPhase( RDG::Builder& graph, const GpuSceneBuffers& scene, std::span<const PassDrawTable> tables,
                      std::span<const PassDrawBuffers> passes, std::span<const CullViewInput> views,
                      RDG::TextureRef neutralHzb, const VirtualGeometryCandidateInput& virtualGeometry ) const;

        // Phase 2: re-tests @p carry against each view's (this frame's) Hzb; one indirect dispatch per mesh pass
        // sized by the occluded count. Views with no Hzb get an empty CulledDraws (their draws are all in phase
        // 1).
        [[nodiscard]] Common::ResultStr<std::vector<CulledDraws>>
        AddPostOcclusionPhase( RDG::Builder& graph, const GpuSceneBuffers& scene,
                               std::span<const PassDrawTable> tables, std::span<const PassDrawBuffers> passes,
                               std::span<const CullViewInput> views, const OcclusionCarry& carry ) const;

        struct Program;

    private:
        std::unique_ptr<Program> m_Program;
    };
} // namespace Desert::Graphic::GpuDriven
