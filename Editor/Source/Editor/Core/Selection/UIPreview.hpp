#pragma once

#include <glm/glm.hpp>
#include <entt/entt.hpp>

#include <string>

namespace Desert::Editor::Core
{
    // Editor "UI Preview" — Play-in-editor for UI (like UMG's preview / PIE). When Enabled, the ViewportPanel
    // feeds the viewport's mouse + keyboard into the scene's UI canvas so buttons / toggles / sliders react
    // right in the editor; the EditorUIPass reads this snapshot to drive RenderCanvas2D with real input.
    // Disabled = Design mode (the mouse drives select + drag/resize/anchor handles instead).
    //
    // Written once per frame by the ViewportPanel, read next frame by the EditorUIPass (the pass renders
    // before the panel draws, so a 1-frame lag is expected and harmless for UI). Global singleton: preview
    // acts on the focused viewport's canvas.
    struct UIPreview
    {
        bool Enabled  = false; // the Design/Preview toggle
        bool HasInput = false; // viewport hovered this frame -> actually feed pointer input

        glm::vec2 MousePx     = { 0.0f, 0.0f }; // viewport-local mouse (display px, top-left origin)
        glm::vec2 DisplaySize = { 0.0f, 0.0f }; // viewport display size (px) — to scale into framebuffer space

        bool        Down      = false; // LMB held (also the previous-frame value, for the release edge)
        bool        Released  = false; // down->up edge this frame (fires button clicks)
        // RMB HELD, not an edge. The press edge a context menu opens on is derived inside the UI frame from
        // UIViewContext::PrevRightDown, exactly as the left button's is; reporting the edge here as well
        // would be a second place computing the same fact, and the two would eventually disagree about
        // which frame the click was in.
        bool        RightDown = false;
        bool        Escape    = false; // closes the innermost open context menu / modal
        float       Scroll    = 0.0f;  // wheel notches (drives ScrollView)
        bool        Tab       = false; // advance keyboard focus
        bool        Submit    = false; // Enter — activate the focused control
        bool        Backspace = false;
        std::string TypedText; // UTF-8 chars typed this frame (drives the focused InputField)

        entt::entity Focused = entt::null; // persisted keyboard focus across frames

        static UIPreview& Get()
        {
            static UIPreview s;
            return s;
        }
    };
} // namespace Desert::Editor::Core
