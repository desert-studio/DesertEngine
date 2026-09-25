#pragma once

#include <Engine/Core/FrameManager.hpp>

#include <cstdint>

// WHEN A CACHED Render2D MATERIAL EXECUTOR MAY BE DESTROYED.
//
// The rule lives in a function of its own, with no engine state in it, because it is the load-bearing
// half of A8-1 and the whole point is that it can be ASSERTED rather than described. `Render2DExecutor
// Retirement` in the pointer-ownership suite walks the boundary case by case; a comment above a
// `for` loop could not have been walked at all.
//
// THE PROBLEM IT SOLVES IS A LEAK, AND SAYING SO EXACTLY MATTERS. Render2D keeps one MaterialExecutor
// per bound texture in three caches keyed by the texture's ADDRESS, and nothing ever removed an entry —
// not Init(), not a resize. Every viewport resize destroys the backdrop pyramid and every new texture
// gets a new address, so the caches grew for the life of the process, each entry holding a descriptor
// set and its share of a pool.
//
// It is NOT a use-after-free, and the register said it might be until this was written down: an entry's
// `Texture2DProperty` does hold the address of an image that may already be gone, but `ExecutorFor`
// re-points it with `SetImage` before every use and `Apply()` only ever runs on an executor that was
// just bound — so the stale pointer is overwritten before anything reads it, and an address that gets
// recycled onto a new image simply reuses that executor with the new image bound. The audit's own row
// was more alarming than the code deserved; the leak is real and the dangle is not.
//
// WHY THE WINDOW IS WHAT IT IS. Destroying an executor destroys descriptor sets that a submitted frame
// may still be reading, which is a use-after-free in the GPU rather than the CPU and shows up as
// corruption rather than a crash. An entry may therefore only be retired once no frame that could have
// recorded a draw against it can still be in flight. The caller passes that window in — it is
// `ExecutorRetireWindow()` below, the one place that says how long a recorded frame lives for this cache.

namespace Desert::Graphic::Render2D
{
    /**
     * @brief Frames that must pass before a cache entry may be retired: frames in flight x renderer slots.
     *
     * Frames in flight is the GPU bound — the oldest submitted frame that may still read an executor's sets.
     * The renderer-slot factor is margin this window carried while it was shared with the material properties'
     * dirty countdown (now versions, PropertyVersion.hpp); it is kept so retiring stays exactly as conservative
     * as it was. Frames in flight is floored at 3 before FrameManager knows it, so the window is never zero.
     */
    inline uint32_t ExecutorRetireWindow()
    {
        const uint32_t framesInFlight = Engine::FrameManager::GetInstance().GetMaxFramesInFlight();
        return ( framesInFlight > 0 ? framesInFlight : 3u ) * Engine::kMaxRendererSlots;
    }

    /**
     * @brief May a cache entry last used on @p lastUsedFrame be destroyed on @p currentFrame?
     *
     * @param window frames that must have passed since the last use. Callers pass
     *               ExecutorRetireWindow(); a smaller number is the defect this exists to stop.
     *
     * Strictly greater than @p window, not >=: an entry used `window` frames ago is exactly at the edge
     * of the in-flight range, and the edge belongs to the GPU.
     */
    inline bool MayRetireExecutor( uint64_t lastUsedFrame, uint64_t currentFrame, uint32_t window )
    {
        // A frame counter that has not advanced past the last use (the first frames of a process, or a
        // clock read twice in one frame) can retire nothing.
        if ( currentFrame <= lastUsedFrame )
            return false;
        return ( currentFrame - lastUsedFrame ) > static_cast<uint64_t>( window );
    }
} // namespace Desert::Graphic::Render2D
