#include "DrawCounters.hpp"

// The whole translation unit is the instrument. Under Shipping the header's inline no-ops take over and
// nothing here has a caller; compiling it to an empty object keeps the file in the build (so it cannot
// rot unnoticed) while leaving nothing for the linker to pull in.
#if DESERT_DEV_INSTRUMENTS

namespace Desert::Graphic
{
    namespace
    {
        // Not atomics: every recorded draw goes through VulkanRendererAPI, and its own header records that
        // `m_CurrentCommandBuffer` non-null IS the "recording" state and BeginFrame is its only writer.
        // Recording is single-threaded by that invariant; making these atomic would suggest otherwise.
        DrawCounters g_Recording;
        DrawCounters g_Finished;
    } // namespace

    void DrawCounter::Roll()
    {
        g_Finished  = g_Recording;
        g_Recording = DrawCounters{};
    }

    void DrawCounter::Record( uint32_t instanceCount )
    {
        ++g_Recording.Draws;
        g_Recording.Instances += instanceCount;
    }

    DrawCounters DrawCounter::LastFrame()
    {
        return g_Finished;
    }
} // namespace Desert::Graphic

#endif // DESERT_DEV_INSTRUMENTS
