#pragma once

#include <imgui.h>

namespace Desert::Graphic
{
    struct RenderMethods;
    struct TemporalViewStatus;
}

namespace Desert::Editor
{
    // THE VIEW'S RENDER METHODS, in its bottom-left corner: what this viewport's frame renders for GI, reflections
    // and shadows (Graphic::ResolveRenderMethods), one row each. A row lowered below the view's request (by the
    // Scalability ceiling, the device or an unimplemented method) is drawn amber with its cause — the same text
    // the log got once (Graphic::FormatRenderMethods), so the overlay and the log cannot disagree.
    // Under them, the temporal resolve of the view's last frame (Graphic::FormatTemporalViewStatus): method,
    // quality, split and jitter phase, amber on a frame whose history was reset, with the reason.
    void DrawRenderMethodsOverlay( const Graphic::RenderMethods&      methods,
                                   const Graphic::TemporalViewStatus& temporal, const ImVec2& viewportMin,
                                   const ImVec2& viewportMax );
} // namespace Desert::Editor
