#pragma once

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
    class DrawCounter
    {
    public:
        /// Called by BeginFrame: the frame being closed becomes the readable one.
        static void Roll();

        /// Called ONLY by the two funnels in VulkanRendererAPI.
        static void Record( uint32_t instanceCount );

        /// The finished frame. Zero before the first Roll, which is honest: nothing has finished yet.
        [[nodiscard]] static DrawCounters LastFrame();
    };
} // namespace Desert::Graphic
