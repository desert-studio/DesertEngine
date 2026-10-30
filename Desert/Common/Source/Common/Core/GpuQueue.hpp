#pragma once

#include <cstdint>

namespace Common::Profiling
{
    // The hardware queue a GPU profiler scope is recorded on (the profiler's view of RDG::Pipe). Two queues
    // run concurrently, so their timestamps are two timelines: the graphics one is tiled against the frame
    // bracket, the async compute one against its own segments, and never one against the other (UE: the
    // GPU profiler keeps one timeline per queue, and "stat gpu" prints Unaccounted per queue).
    // Its own header so the backend-free timestamp layout (Engine/Graphic/GpuTimestampLayout.hpp) can name it
    // without the profiler and Optick.
    enum class GpuQueue : uint8_t
    {
        Graphics,
        AsyncCompute,
    };
} // namespace Common::Profiling
