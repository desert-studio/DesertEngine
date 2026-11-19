#pragma once

#include <cstdint>

namespace Desert::Core::Rules
{
    // UE's EStreamingSourceTargetBehavior: whether a streaming source's TargetGrids and TargetHLODLayers are the
    // only grids it streams (Include) or the ones it leaves alone (Exclude). With both lists empty it streams
    // every grid whichever it states (Rules::TargetsGridNamed). One definition for the rule
    // (Rules::StreamingSource) and the component that authors it (ECS::StreamingSourceData, a reflected enum
    // stored as its number), so this header stays free of both.
    enum class StreamingSourceTargetBehavior : std::uint8_t
    {
        Include = 0,
        Exclude = 1,
    };
} // namespace Desert::Core::Rules
