#pragma once

#include <cstdint>

namespace Desert::UI
{
    // The pointer shape the UI asks its host for (Slate's EMouseCursor). The UI DECIDES it — in EndUIFrame,
    // from the control the pointer is over or captured by (UIViewContext::Cursor) — and the host only shows
    // it, so a text field shows the I-beam in every host rather than in the hosts that remembered to ask.
    enum class UICursorShape : uint8_t
    {
        Default,
        TextBeam,
        Hand,
        ResizeLeftRight,
        ResizeUpDown,
        ResizeAll,
        NotAllowed
    };
} // namespace Desert::UI
