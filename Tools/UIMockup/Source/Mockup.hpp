#pragma once

#include <imgui.h>
#include <imgui_internal.h> // ImRect

#include <span>
#include <string_view>

struct ImFont;

namespace Desert::UIMockup
{
    // The editor's font set, loaded with the editor's sizes (EditorResources::Initialize): regular text
    // with the Material Design icons merged in, bold, and the larger bold used for titles.
    struct Fonts
    {
        ImFont* regular   = nullptr;
        ImFont* bold      = nullptr;
        ImFont* extraBold = nullptr;
    };

    // One mockup: a name for --mockup, a line for --list, and the function that draws the whole frame.
    // The function owns the full display (ImGui::GetMainViewport()); it is called once per frame.
    struct Mockup
    {
        std::string_view name;
        std::string_view summary;
        void ( *draw )( const Fonts& );
    };

    // Every mockup the tool knows, in --list order (Mockups.cpp).
    std::span<const Mockup> AllMockups();
    const Mockup*           FindMockup( std::string_view name );

    // ── Shared drawing helpers (MockupWidgets.cpp) ──────────────────────────────────────────────────
    // Only what two or more mockups draw; anything one mockup needs stays in that mockup's file.

    // UE's category bar: a flat ThemeManager::GetSectionHeaderColor() strip across the content width with
    // a bold label, optionally followed by right-aligned dim text.
    void SectionHeader( const Fonts& fonts, const char* label, const char* trailing = nullptr );

    // A dark, gridded rectangle standing in for a 3D viewport, with its caption centred. Mockups only -
    // a real panel never draws this. Returns the rectangle so the caller can overlay on it.
    ImRect ViewportPlaceholder( const char* caption, ImVec2 size );

    // A small square toggle-looking button; @p on paints it in the highlight amber (an armed mode/tool).
    bool ToolButton( const char* label, bool on, ImVec2 size = ImVec2( 0, 0 ) );

    // Colour from 0..255 integers, the way ThemeManager spells its palette.
    constexpr ImU32 Rgb( int r, int g, int b, int a = 255 )
    {
        return IM_COL32( r, g, b, a );
    }
} // namespace Desert::UIMockup
