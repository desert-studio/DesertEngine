#pragma once

// THE WINDOW BUTTONS' LOOK, IN ONE PLACE, WITH NO RENDERER BEHIND IT. Two windows of this editor draw their
// own minimize and close buttons: the main window (WindowChrome, through ImGui) and the start-up splash
// (native layers, because ImGui cannot draw while the main thread is busy loading — see SplashScreen.hpp).
// The two draw with different machinery, so what they share is what makes them the SAME buttons: the
// square's width, the hover and pressed colours, the glyphs and the font the glyphs come from. Kept here,
// a colour changed for one window is changed for both; kept in each drawer, the first change would part
// them.

#include <filesystem>

namespace Desert::Editor::UI
{
    struct ButtonColour
    {
        float R = 0.0f;
        float G = 0.0f;
        float B = 0.0f;
        float A = 0.0f;
    };

    // Every window button is one square of the bar's height and this wide, so the buttons read as one group
    // and the hit target is the whole square rather than the glyph.
    inline constexpr float kWindowButtonWidth = 46.0f;

    // Hover and pressed: a faint white wash for the ordinary buttons; red for close, because a button that
    // ends the session should not look like the one that toggles a gizmo. At rest every button is clear.
    inline constexpr ButtonColour kWindowButtonHovered = { 1.0f, 1.0f, 1.0f, 0.12f };
    inline constexpr ButtonColour kWindowButtonPressed = { 1.0f, 1.0f, 1.0f, 0.20f };
    inline constexpr ButtonColour kCloseButtonHovered  = { 0.77f, 0.16f, 0.16f, 1.0f };
    inline constexpr ButtonColour kCloseButtonPressed  = { 0.62f, 0.12f, 0.12f, 1.0f };

    // The glyphs, as UTF-8, from the Material Design Icons font (the editor's icon font; these two equal
    // ICON_MDI_WINDOW_MINIMIZE / ICON_MDI_WINDOW_CLOSE, which WindowChrome draws through the ImGui atlas).
    inline constexpr const char* kMinimizeGlyph = "\xf3\xb0\x96\xb0"; // U+F05B0
    inline constexpr const char* kCloseGlyph    = "\xf3\xb0\x96\xad"; // U+F05AD

    // The icon font's file, relative to the editor's working directory. The editor's ImGui atlas merges it
    // (EditorResources::Initialize); the splash loads it natively.
    inline const std::filesystem::path kIconFontFile = "Resources/Fonts/materialdesignicons-webfont.ttf";
} // namespace Desert::Editor::UI
