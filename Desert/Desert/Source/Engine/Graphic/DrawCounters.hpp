#pragma once

#include <Common/Core/DevInstruments.hpp>

#include <array>
#include <cstdint>
#include <span>

namespace Desert::Graphic
{
    // WHAT THE FRAME ASKED THE GPU TO DRAW, and why this exists at all.
    //
    // The world-scale scene (Tools/WorldGen, 50 179 entities) could name its entity count, its file size,
    // its load time and its resident-set growth — and could NOT name a draw call or a frame time, because
    // the engine counted neither. `Docs/World/PROGRAMME.md` step 2 names that gap: the whole programme is
    // measured against detectors, and a draw-call count is the one number that tells "we draw everything"
    // apart from "we draw too much".
    //
    // ONE FUNNEL, AND THAT IS ASSERTED. Six submit paths in VulkanRendererAPI reach `vkCmdDraw*`, and a
    // counter incremented at six call sites is a counter that will be right at five of them. Both vk calls
    // are wrapped by `DrawIndexedCounted`/`DrawCounted`, and `Desert/Tests/Engine/DrawCounterFunnel`
    // asserts over the SOURCE TEXT that no seventh site exists — the same technique ReservedIdentifiers
    // uses, for the same reason: nothing in a frame reveals a draw that was not counted.
    struct DrawCounters
    {
        uint32_t Draws     = 0; ///< `vkCmdDraw` + `vkCmdDrawIndexed` recorded this frame.
        uint32_t Instances = 0; ///< their instance counts summed — an ISM batch is ONE draw, many instances.
    };

    // PER MESH PASS, how its draws were ISSUED (GPU-DRIVEN PASSES, DesertCtl `state views.meshPasses`): the
    // measure of the migration from the per-submesh CPU funnel to the culled bins. Indexed by
    // GpuDriven::MeshPass (DepthPrepass, GBuffer, Forward, ShadowCascade, VirtualShadow; MeshRenderer.cpp
    // asserts the count). A draw is attributed to the pass whose MeshDrawList is recording (MeshPassScope).
    inline constexpr uint32_t kCountedMeshPasses = 5;
    struct MeshPassDrawCounters
    {
        uint32_t CpuDraws = 0; ///< per-submesh draws the CPU issued: counted or GPU-counted (RenderMeshIndirect)
        uint32_t IndirectCalls =
             0;                  ///< vkCmdDrawIndexedIndirect calls of culled bin groups (RenderMeshIndirectGroup)
        uint32_t BinsIssued = 0; ///< bin records those calls named (a culled-empty bin included)
    };

    // GRASS-VIS-7: the grass instances the GPU drew (GrassFinalize's drawn counts, read back without a wait
    // FramesInFlight frames after their generation, UE's GPU-culling stats the same way), summed over every view
    // and variety a GrassRenderer::AddFrame of the frame drew. Full: the list the cascades and the TLAS draw (the
    // camera's side planes swept along the shadowed suns); Camera: the camera's phase-1 list (the full list's
    // count when the camera has no list of its own); Post: phase 2's appended (UE two-phase occlusion), 0 without
    // one. Cascades (GPU-COST-21): per cascade VIEW (slot x 4 + cascade, MeshRenderer's view index) the instances
    // its own list drew - those inside its caster box - 0 for a view that drew none.
    struct GrassDrawCounters
    {
        // MeshRenderer::kCascadeViews (static_assert in GrassRenderer.cpp; this header stays include-free).
        static constexpr uint32_t kCascadeViews = 8;

        uint64_t                            Full   = 0;
        uint64_t                            Camera = 0;
        uint64_t                            Post   = 0;
        std::array<uint64_t, kCascadeViews> Cascades{};
        uint32_t                            Varieties = 0; ///< the varieties whose counts had come back
    };

    // THE SNAPSHOT IS OF THE FRAME THAT FINISHED, never of the one being recorded. A HUD reading the live
    // counter would show a number that grows while it is being looked at, and two panels reading it in the
    // same frame would disagree — which is the defect shape this project keeps finding, an instrument
    // answering a different question than the one asked.
    // THE SHIPPING BOUNDARY IS HERE, IN THE INSTRUMENT'S OWN HEADER, AND NOT AT THE SIX CALL SITES.
    //
    // Putting it at the call sites would have been the obvious move and it is the wrong one twice over.
    // First, `Desert/Tests/Engine/DrawCounterFunnel` is a census over the SOURCE TEXT of
    // VulkanRenderer.cpp asserting that both funnels still call `Record` — an `#if` around those calls
    // reads to it as the funnel being removed, so the guard on the counter would have broken the guard
    // on the draws. Second, the funnel argument above says there must be ONE place that knows about the
    // counter; a boundary spelled at every call site is the same six-places problem in a new coat.
    //
    // Empty inline bodies rather than a removed declaration: the callers keep compiling unchanged, the
    // calls optimise to nothing, and `DrawCounters.cpp` is never referenced, so it is not pulled out of
    // the archive. The name does not reach the shipping binary at all, which is what
    // scripts/CI/ShippingSymbols.sh checks.
    class DrawCounter
    {
    public:
#if DESERT_DEV_INSTRUMENTS
        /// Called by BeginFrame: the frame being closed becomes the readable one.
        static void Roll();

        /// Called ONLY by the two funnels in VulkanRendererAPI.
        static void Record( uint32_t instanceCount );

        /// The finished frame. Zero before the first Roll, which is honest: nothing has finished yet.
        [[nodiscard]] static DrawCounters LastFrame();

        /// The mesh pass the following draws belong to, until EndMeshPass (MeshPassScope; not nested).
        static void BeginMeshPass( uint32_t pass );
        static void EndMeshPass();
        /// A per-submesh draw whose counts a GPU buffer holds (RenderMeshIndirect): a CPU draw of the active pass.
        static void RecordCpuIndirect();
        /// One culled bin group issued (RenderMeshIndirectGroup): @p calls indirect calls naming @p bins records.
        static void RecordBins( uint32_t bins, uint32_t calls );
        /// The finished frame's counters of @p pass (every view's draws of it summed).
        [[nodiscard]] static MeshPassDrawCounters LastFrameMeshPass( uint32_t pass );
        /// One grass variety's read-back drawn counts (GrassRenderer::AddFrame; GrassDrawCounters).
        static void RecordGrass( uint32_t full, uint32_t camera, uint32_t post,
                                 std::span<const uint32_t> cascades );
        /// The finished frame's grass counts.
        [[nodiscard]] static GrassDrawCounters LastFrameGrass();
#else
        static void Roll()
        {
        }
        static void Record( uint32_t )
        {
        }
        [[nodiscard]] static DrawCounters LastFrame()
        {
            return {};
        }
        static void BeginMeshPass( uint32_t )
        {
        }
        static void EndMeshPass()
        {
        }
        static void RecordCpuIndirect()
        {
        }
        static void RecordBins( uint32_t, uint32_t )
        {
        }
        [[nodiscard]] static MeshPassDrawCounters LastFrameMeshPass( uint32_t )
        {
            return {};
        }
        static void RecordGrass( uint32_t, uint32_t, uint32_t, std::span<const uint32_t> )
        {
        }
        [[nodiscard]] static GrassDrawCounters LastFrameGrass()
        {
            return {};
        }
#endif
    };

    // Attributes the draws recorded in its lifetime to one mesh pass (MeshDrawList::Record).
    class MeshPassScope
    {
    public:
        explicit MeshPassScope( const uint32_t pass )
        {
            DrawCounter::BeginMeshPass( pass );
        }
        ~MeshPassScope()
        {
            DrawCounter::EndMeshPass();
        }
        MeshPassScope( const MeshPassScope& )            = delete;
        MeshPassScope& operator=( const MeshPassScope& ) = delete;
    };
} // namespace Desert::Graphic
