#include "DrawCounters.hpp"

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
