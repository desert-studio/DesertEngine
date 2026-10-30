#pragma once

#include <Engine/UI/Ecs/UICanvasRendererEcs.hpp>

#include <glm/glm.hpp>
#include <entt/entt.hpp>

#include <string>
#include <vector>

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
        bool                                  RightDown = false;
        bool MiddleDown = false; // MMB held; its edge is derived in the frame, as the right one's is
        // The double-click interval the host's toolkit uses (ImGui's MouseDoubleClickTime), so a double click
        // in the preview is the same gesture as one anywhere else in the editor (UIInput::DoubleClickSeconds).
        float DoubleClickSeconds = 0.5f;
        float       Scroll    = 0.0f;  // wheel notches (drives ScrollView)
        std::vector<::Desert::UI::UIKeyEvent> Keys; // key presses this frame, in order (UIInput::Keys)
        ::Desert::UI::UIKeyMods Mods = ::Desert::UI::UIKeyMods::None; // modifiers held (UIInput::Mods)
        std::string TypedText; // UTF-8 chars typed this frame (drives the focused InputField)

        entt::entity Focused = entt::null; // persisted keyboard focus across frames

        // Draw the retained widget gallery (UI/Retained/WidgetGallery.hpp) over the level in this view, with
        // the preview's input -- "UI: Toggle the widget gallery". Feeds input as Preview does.
        bool WidgetGallery = false;
        // WRITTEN BY THE PASS, read by the panel: the pointer shape the UI frame asked for
        // (UIViewContext::Cursor), shown by the ViewportPanel while the viewport is hovered.
        ::Desert::UI::UICursorShape Cursor = ::Desert::UI::UICursorShape::Default;

        static UIPreview& Get()
        {
            static UIPreview s;
            return s;
        }
    };
} // namespace Desert::Editor::Core
