#pragma once

#include <cstdint>
#include <string>

namespace Desert::Animation
{
    /// What one notify did during a step of playback: UE's Notify (instant) / NotifyBegin / NotifyEnd.
    enum class NotifyEventKind : uint8_t
    {
        Fire,
        Begin,
        End,
    };

    struct NotifyEvent
    {
        std::string     Name;
        NotifyEventKind Kind = NotifyEventKind::Fire;
        /// The pose-graph node whose clip reported it (UE: the notify's source player); -1 = the base clip (the
        /// Source stage, Play/CrossFade). With `LinkedSlot` >= 0 the node is of that linked layer's graph.
        int SourceNode = -1;
        int LinkedSlot = -1;

        bool operator==( const NotifyEvent& ) const = default;
    };
} // namespace Desert::Animation
