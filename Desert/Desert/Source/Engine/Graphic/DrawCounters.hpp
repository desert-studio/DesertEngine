#pragma once

#include <Common/Core/DevInstruments.hpp>

#include <cstdint>

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
#endif
    };
} // namespace Desert::Graphic
