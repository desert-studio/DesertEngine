#include "DrawCounters.hpp"

#include <cstdint>

// The whole translation unit is the instrument. Under Shipping the header's inline no-ops take over and
// nothing here has a caller; compiling it to an empty object keeps the file in the build (so it cannot
// rot unnoticed) while leaving nothing for the linker to pull in.
#if DESERT_DEV_INSTRUMENTS

#include <array>

namespace Desert::Graphic
{
    namespace
    {
        // Not atomics: every recorded draw goes through VulkanRendererAPI, and its own header records that
        // `m_CurrentCommandBuffer` non-null IS the "recording" state and BeginFrame is its only writer.
        // Recording is single-threaded by that invariant; making these atomic would suggest otherwise.
        DrawCounters g_Recording;
        DrawCounters g_Finished;
        using PassCounters = std::array<MeshPassDrawCounters, kCountedMeshPasses>;
        PassCounters g_PassRecording{};
        PassCounters g_PassFinished{};
        // The pass MeshPassScope opened; kCountedMeshPasses = none (a draw outside the mesh passes).
        uint32_t g_ActivePass = kCountedMeshPasses;
    } // namespace

    void DrawCounter::Roll()
    {
        g_Finished      = g_Recording;
        g_Recording     = DrawCounters{};
        g_PassFinished  = g_PassRecording;
        g_PassRecording = PassCounters{};
    }

    void DrawCounter::Record( uint32_t instanceCount )
    {
        ++g_Recording.Draws;
        g_Recording.Instances += instanceCount;
        if ( g_ActivePass < kCountedMeshPasses )
            ++g_PassRecording[g_ActivePass].CpuDraws;
    }

    void DrawCounter::BeginMeshPass( const uint32_t pass )
    {
        g_ActivePass = pass < kCountedMeshPasses ? pass : kCountedMeshPasses;
    }

    void DrawCounter::EndMeshPass()
    {
        g_ActivePass = kCountedMeshPasses;
    }

    void DrawCounter::RecordCpuIndirect()
    {
        if ( g_ActivePass < kCountedMeshPasses )
            ++g_PassRecording[g_ActivePass].CpuDraws;
    }

    void DrawCounter::RecordBins( const uint32_t bins, const uint32_t calls )
    {
        if ( g_ActivePass >= kCountedMeshPasses )
            return;
        g_PassRecording[g_ActivePass].BinsIssued += bins;
        g_PassRecording[g_ActivePass].IndirectCalls += calls;
    }

    MeshPassDrawCounters DrawCounter::LastFrameMeshPass( const uint32_t pass )
    {
        return pass < kCountedMeshPasses ? g_PassFinished[pass] : MeshPassDrawCounters{};
    }

    DrawCounters DrawCounter::LastFrame()
    {
        return g_Finished;
    }
} // namespace Desert::Graphic

#endif // DESERT_DEV_INSTRUMENTS
