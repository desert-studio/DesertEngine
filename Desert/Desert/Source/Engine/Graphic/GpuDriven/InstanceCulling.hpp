#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/GpuDriven/DrawCommandCache.hpp>
#include <Engine/Graphic/GpuDriven/GpuScene.hpp>
#include <Engine/Graphic/GpuDriven/GpuSceneLayout.hpp>
#include <Engine/Graphic/RDG/RDGResources.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace Desert::Graphic::RDG
{
    class Builder;
}

// GPU-DRIVEN contract (C0) — InstanceCulling: WHICH INSTANCES EACH (VIEW x PASS) DRAWS, DECIDED ON THE GPU
// (UE FInstanceCullingContext + the two-pass occlusion of InstanceCulling/BuildInstanceDrawCommands and
// HZB occlusion, Renderer/Private/InstanceCulling/*.cpp, Renderer/Private/HZB.cpp).
//
// Responsibility, per instance per view, in one compute pass ("GPUDriven: Cull Phase 1/2",
// Programs/GpuDriven/InstanceCull.shader, reading Common/GpuScene.glslh):
//  1. frustum test of LocalBounds x InstanceToWorld (or the pose box) against GpuCullView::ViewProjection —
//     replaces the CPU IsVisibleInView per draw (VisibilityCulling.hpp:23-31; MeshRendererForward.cpp:156,658,
//     680,887,1019,1156; MeshRendererShadow.cpp:336,353,768,973) and the ISM CPU pass CollectIsmInstances
//     (InstanceCullDistance.hpp:97), distance cull included;
//  2. TWO-PHASE HZB occlusion for EVERY mesh: phase 1 tests against the PREVIOUS frame's HZB through
//     PrevViewProjection; survivors are drawn, the rejected go to OccludedInstances; the pass builds this
//     frame's HZB from that depth (SceneRenderer::AddFrameHZB, SceneRendererFrameDeferred.cpp:1133-1162, moved
//     between the phases); phase 2 re-tests the rejected against it and draws the newly visible. A view without
//     an HZB (CSM cascade today) runs phase 1 with frustum only and no phase 2;
//  3. LOD on the GPU: the GLSL twin of Geometry::SelectLODFromBounds (Engine/Geometry/LODSelection.hpp:177 and
//     BoundsScreenRadiusSquared :68) — replaces MeshRenderer::ComputeLOD (MeshRenderer.cpp:193-209);
//  4. for each submesh slot of (primitive, pass, lod): atomically reserve a place in the slot's bin, write
//     GpuVisibleInstance there and count it into the bin's GpuIndirectDraw::InstanceCount.
// Then "GPUDriven: Compact" writes the pass's IndirectDraws (IndirectDrawSubmission.hpp decides the shape).
//
// The virtual shadow map's caster cull (Systems/Scene/Shadows/VirtualShadowMap.hpp:95-235, AddCasterCulling) is
// the same job with a page-mask test instead of a frustum: it becomes a CullView flavour of this pass (its
// VsmCullInstanceGpu / VsmCullDrawGpu records are GpuSceneInstance / GpuDrawBin), not a second culler.
// Grass (GpuInstances.hpp) and SDF near-field keep their own generators; their output lands in bins too.
namespace Desert::Graphic::GpuDriven
{
    enum class CullPhase : uint32_t
    {
        // Against the previous frame's HZB (or frustum only when the view has none).
        Main,
        // The instances Main rejected, against this frame's HZB built from Main's depth.
        PostOcclusion,
    };

    // One view x pass the cull serves this frame. Hzb / PrevHzb are graph textures of the view's HZB chain
    // (SceneRenderer::AddFrameHZB); invalid = no occlusion test for this view.
    struct CullViewInput
    {
        GpuCullView     View{};
        RDG::TextureRef PrevHzb{};
        RDG::TextureRef Hzb{};
    };

    // What a pass draws with after one phase: the args table (+ the per-group counts on the count path) and the
    // visible list gl_InstanceIndex indexes. Each phase has its own; phase 2's draws LOAD phase 1's targets.
    struct CulledDraws
    {
        RDG::BufferRef IndirectDraws;    // GpuIndirectDraw[Bins] — Access::IndirectArgs
        RDG::BufferRef GroupDrawCounts;  // uint[Groups] — Access::IndirectArgs; invalid on the fixed path
        RDG::BufferRef VisibleInstances; // GpuVisibleInstance[VisibleCapacity]
    };

    // Phase 1's left-overs that phase 2 consumes.
    struct OcclusionCarry
    {
        RDG::BufferRef OccludedInstances; // GpuOccludedInstance[]
        RDG::BufferRef OccludedCount;     // uint + VkDispatchIndirectCommand of phase 2
    };

    struct MainPhaseResult
    {
        std::vector<CulledDraws> Draws; // one per CullViewInput, same order
        OcclusionCarry           Carry;
    };

    class InstanceCulling
    {
    public:
        // Phase 1 for every view in @p views: one dispatch over the scene's instances x views. Refused for an
        // empty @p views (the caller adds no draw then).
        [[nodiscard]] Common::ResultStr<MainPhaseResult>
        AddMainPhase( RDG::Builder& graph, const GpuSceneBuffers& scene, std::span<const PassDrawBuffers> passes,
                      std::span<const CullViewInput> views ) const;

        // Phase 2: re-tests @p carry against each view's (this frame's) Hzb; one indirect dispatch sized by the
        // occluded count. Views with no Hzb get an empty CulledDraws (their draws are all in phase 1).
        [[nodiscard]] Common::ResultStr<std::vector<CulledDraws>>
        AddPostOcclusionPhase( RDG::Builder& graph, const GpuSceneBuffers& scene,
                               std::span<const PassDrawBuffers> passes, std::span<const CullViewInput> views,
                               const OcclusionCarry& carry ) const;
    };
} // namespace Desert::Graphic::GpuDriven
