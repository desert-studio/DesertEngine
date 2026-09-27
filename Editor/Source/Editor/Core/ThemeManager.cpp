#define IMGUI_DEFINE_MATH_OPERATORS

#include "ThemeManager.hpp"

#include <ImGui/imgui.h>
#include "imgui_internal.h"

namespace Desert::Editor
{
    static ImVec4 s_SelectedColor( 0.3f, 0.6f, 0.9f, 1.0f );
    static ImVec4 s_IconColor( 0.78f, 0.78f, 0.78f, 1.0f );

    // Shorthand: integer RGBA, all four channels 0..255 → normalized ImVec4.
    //
    // ALPHA IS AN INTEGER TOO, and that is the point. It used to be a float passed through untouched while
    // every caller wrote it as a byte (140, 89, 178...), so each of those colours reached ImGui with an alpha
    // of 140.0 — clamped to fully opaque. The modal dim was the visible casualty: a solid (20,20,20) sheet
    // over the whole editor, so a refusal dialog floated over what looked like an empty window. One unit for
    // all four channels leaves no second convention to mix up. Asserted by the ThemePalette suite.
    static constexpr ImVec4 C( int r, int g, int b, int a = 255 )
    {
        return { static_cast<float>( r ) / 255.0f, static_cast<float>( g ) / 255.0f,
                 static_cast<float>( b ) / 255.0f, static_cast<float>( a ) / 255.0f };
    }

    void ThemeManager::SetDarkTheme()
    {
        auto&    style  = ImGui::GetStyle();
        ImVec4*  colors = style.Colors;

        // ── Palette ─────────────────────────────────────────────────────────
        // Sampled straight out of a UE5 screenshot (the Static Mesh Editor), not eyeballed: UE's editor
        // chrome is a very short ramp — panel #242424, section bar #2F2F2F, every input sunk to near-black
        // #0F0F0F, and ONE saturated blue for selection. Guessing "a bit lighter here" is exactly what made
        // the old theme read as a different program in every panel.
        const ImVec4 Bg        = C( 36, 36, 36 );   // #242424  main window / child / row bg
        const ImVec4 BgInput   = C( 15, 15, 15 );   // #0F0F0F  input / frame bg (sunk, not raised)
        const ImVec4 BgPopup   = C( 30, 30, 30 );   // #1E1E1E  popup bg
        const ImVec4 TabActive = C( 36, 36, 36 );   // #242424  focused tab = the panel it opens
        const ImVec4 TabInact  = C( 21, 21, 21 );   // #151515  unfocused tab
        const ImVec4 TitleBar  = C( 21, 21, 21 );   // #151515  title-bar / menu-bar
        const ImVec4 Border    = C( 26, 26, 26 );   // #1A1A1A  borders / row rules / separators
        const ImVec4 Btn       = C( 51, 51, 51 );   // #333333  button normal
        const ImVec4 BtnHov    = C( 64, 64, 64 );   // #404040  button hovered
        const ImVec4 BtnAct    = C( 41, 41, 41 );   // #292929  button active (pressed)
        // Selection is NOT the accent. UE highlights a selected row with a muted blue-grey (#40576F,
        // sampled off a selected outliner entry) and keeps the saturated blue for controls that report a
        // VALUE — checkmarks, slider grabs, drag-drop. A selection painted in the accent shouts over the
        // content it is supposed to be pointing at.
        const ImVec4 Select    = C( 64, 87, 111 );  // #40576F  selected row
        const ImVec4 SelectHov = C( 78, 104, 132 ); // #4E6884  hovered selection
        const ImVec4 SelectAct = C( 53, 72, 92 );   // #35485C  active selection
        const ImVec4 Accent    = C( 0, 112, 224 );  // #0070E0  checkmark / slider / accent

        s_SelectedColor = Accent;
        s_IconColor     = C( 180, 180, 180 );

        // ── Text ─────────────────────────────────────────────────────────────
        colors[ImGuiCol_Text]         = C( 200, 200, 200 ); // #C8C8C8
        colors[ImGuiCol_TextDisabled] = C( 128, 128, 128 ); // #808080

        // ── Windows ──────────────────────────────────────────────────────────
        colors[ImGuiCol_WindowBg]  = Bg;
        colors[ImGuiCol_ChildBg]   = Bg;
        colors[ImGuiCol_PopupBg]   = BgPopup;

        // ── Borders ──────────────────────────────────────────────────────────
        colors[ImGuiCol_Border]       = Border;
        colors[ImGuiCol_BorderShadow] = ImVec4( 0, 0, 0, 0 );

        // ── Frames (input fields, combos) ────────────────────────────────────
        colors[ImGuiCol_FrameBg]        = BgInput;
        colors[ImGuiCol_FrameBgHovered] = C( 26, 26, 26 );
        colors[ImGuiCol_FrameBgActive]  = C( 10, 10, 10 );

        // ── Title-bars ────────────────────────────────────────────────────────
        colors[ImGuiCol_TitleBg]          = TitleBar;
        colors[ImGuiCol_TitleBgActive]    = TitleBar;
        colors[ImGuiCol_TitleBgCollapsed] = TitleBar;
        colors[ImGuiCol_MenuBarBg]        = TitleBar;

        // ── Scrollbars ────────────────────────────────────────────────────────
        colors[ImGuiCol_ScrollbarBg]          = C( 21, 21, 21, 0 );
        colors[ImGuiCol_ScrollbarGrab]        = C( 80, 80, 80 );
        colors[ImGuiCol_ScrollbarGrabHovered] = C( 100, 100, 100 );
        colors[ImGuiCol_ScrollbarGrabActive]  = C( 120, 120, 120 );

        // ── Checkmarks / sliders ─────────────────────────────────────────────
        colors[ImGuiCol_CheckMark]        = Accent;
        colors[ImGuiCol_SliderGrab]       = Accent;
        colors[ImGuiCol_SliderGrabActive] = C( 110, 175, 240 );

        // ── Buttons ───────────────────────────────────────────────────────────
        colors[ImGuiCol_Button]        = Btn;
        colors[ImGuiCol_ButtonHovered] = BtnHov;
        colors[ImGuiCol_ButtonActive]  = BtnAct;

        // ── Headers (TreeNode selected, Selectable selected) ──────────────────
        colors[ImGuiCol_Header]        = Select;
        colors[ImGuiCol_HeaderHovered] = SelectHov;
        colors[ImGuiCol_HeaderActive]  = SelectAct;

        // ── Separators ────────────────────────────────────────────────────────
        colors[ImGuiCol_Separator]        = Border;
        colors[ImGuiCol_SeparatorHovered] = C( 85, 85, 85 );
        colors[ImGuiCol_SeparatorActive]  = C( 110, 110, 110 );

        // ── Resize grips ──────────────────────────────────────────────────────
        colors[ImGuiCol_ResizeGrip]        = C( 0, 112, 224, 25 );
        colors[ImGuiCol_ResizeGripHovered] = C( 0, 112, 224, 170 );
        colors[ImGuiCol_ResizeGripActive]  = C( 0, 112, 224, 242 );

        // ── Tabs ──────────────────────────────────────────────────────────────
        colors[ImGuiCol_Tab]                = TabInact;
        colors[ImGuiCol_TabHovered]         = TabActive;
        colors[ImGuiCol_TabActive]          = TabActive;
        colors[ImGuiCol_TabUnfocused]       = TabInact;
        colors[ImGuiCol_TabUnfocusedActive] = C( 37, 37, 37 );

        // ── Docking ───────────────────────────────────────────────────────────
        colors[ImGuiCol_DockingEmptyBg] = C( 21, 21, 21 );
        colors[ImGuiCol_DockingPreview] = C( 0, 112, 224, 178 );

        // ── Tables ────────────────────────────────────────────────────────────
        colors[ImGuiCol_TableHeaderBg]     = C( 47, 47, 47 );
        colors[ImGuiCol_TableBorderStrong] = Border;
        colors[ImGuiCol_TableBorderLight]  = C( 30, 30, 30 );
        colors[ImGuiCol_TableRowBg]        = ImVec4( 0, 0, 0, 0 );
        colors[ImGuiCol_TableRowBgAlt]     = C( 42, 42, 42 );

        // ── Misc ──────────────────────────────────────────────────────────────
        colors[ImGuiCol_PlotLines]             = C( 156, 156, 156 );
        colors[ImGuiCol_PlotLinesHovered]      = Accent;
        colors[ImGuiCol_PlotHistogram]         = C( 230, 179,   0 );
        colors[ImGuiCol_PlotHistogramHovered]  = C( 255, 153,   0 );
        colors[ImGuiCol_TextSelectedBg]        = C( 0, 112, 224, 89 );
        colors[ImGuiCol_DragDropTarget]        = Accent;
        colors[ImGuiCol_NavHighlight]          = Accent;
        colors[ImGuiCol_NavWindowingHighlight] = C( 255, 255, 255, 178 );
        colors[ImGuiCol_NavWindowingDimBg]     = C( 204, 204, 204,  51 );
        colors[ImGuiCol_ModalWindowDimBg]      = C(  20,  20,  20, 140 );

        // ── Style / sizes ─────────────────────────────────────────────────────
        // UE's Details panel is a stack of equal-height bands separated by a 1px rule — no gaps. Hence the
        // tighter vertical item spacing and the roomier frame padding: the row's height comes from the
        // control, not from the space around it.
        style.WindowPadding     = ImVec2( 8.0f, 6.0f );
        style.FramePadding      = ImVec2( 6.0f, 4.0f );
        style.CellPadding       = ImVec2( 4.0f, 2.0f );
        style.ItemSpacing       = ImVec2( 8.0f, 3.0f );
        style.ItemInnerSpacing  = ImVec2(  4.0f,  4.0f );
        style.IndentSpacing     = 18.0f;
        style.ScrollbarSize     = 10.0f;
        style.GrabMinSize       =  8.0f;

        style.WindowBorderSize  = 1.0f;
        style.ChildBorderSize   = 1.0f;
        style.PopupBorderSize   = 1.0f;
        style.FrameBorderSize   = 0.0f;
        style.TabBorderSize     = 0.0f;

        style.WindowRounding    = 0.0f;
        style.ChildRounding     = 0.0f;
        style.FrameRounding     = 2.0f;
        style.PopupRounding     = 2.0f;
        style.ScrollbarRounding = 0.0f;
        style.GrabRounding      = 2.0f;
        style.TabRounding       = 2.0f;

        style.WindowTitleAlign    = ImVec2( 0.0f, 0.5f );
        style.ColorButtonPosition = ImGuiDir_Left;
    }

    void ThemeManager::SetBlackTheme()
    {
        auto&    style  = ImGui::GetStyle();
        ImVec4*  colors = style.Colors;

        ImGui::StyleColorsDark();

        colors[ImGuiCol_Text]                  = ImVec4( 1.00f, 1.00f, 1.00f, 1.00f );
        colors[ImGuiCol_TextDisabled]          = ImVec4( 0.50f, 0.50f, 0.50f, 1.00f );
        colors[ImGuiCol_WindowBg]              = ImVec4( 0.10f, 0.10f, 0.10f, 1.00f );
        colors[ImGuiCol_ChildBg]               = ImVec4( 0.00f, 0.00f, 0.00f, 0.00f );
        colors[ImGuiCol_PopupBg]               = ImVec4( 0.19f, 0.19f, 0.19f, 0.92f );
        colors[ImGuiCol_Border]                = ImVec4( 0.19f, 0.19f, 0.19f, 0.29f );
        colors[ImGuiCol_BorderShadow]          = ImVec4( 0.00f, 0.00f, 0.00f, 0.24f );
        colors[ImGuiCol_FrameBg]               = ImVec4( 0.05f, 0.05f, 0.05f, 0.54f );
        colors[ImGuiCol_FrameBgHovered]        = ImVec4( 0.19f, 0.19f, 0.19f, 0.54f );
        colors[ImGuiCol_FrameBgActive]         = ImVec4( 0.20f, 0.22f, 0.23f, 1.00f );
        colors[ImGuiCol_TitleBg]               = ImVec4( 0.00f, 0.00f, 0.00f, 1.00f );
        colors[ImGuiCol_TitleBgActive]         = ImVec4( 0.06f, 0.06f, 0.06f, 1.00f );
        colors[ImGuiCol_TitleBgCollapsed]      = ImVec4( 0.00f, 0.00f, 0.00f, 1.00f );
        colors[ImGuiCol_MenuBarBg]             = ImVec4( 0.14f, 0.14f, 0.14f, 1.00f );
        colors[ImGuiCol_ScrollbarBg]           = ImVec4( 0.05f, 0.05f, 0.05f, 0.54f );
        colors[ImGuiCol_ScrollbarGrab]         = ImVec4( 0.34f, 0.34f, 0.34f, 0.54f );
        colors[ImGuiCol_ScrollbarGrabHovered]  = ImVec4( 0.40f, 0.40f, 0.40f, 0.54f );
        colors[ImGuiCol_ScrollbarGrabActive]   = ImVec4( 0.56f, 0.56f, 0.56f, 0.54f );
        colors[ImGuiCol_CheckMark]             = ImVec4( 0.33f, 0.67f, 0.86f, 1.00f );
        colors[ImGuiCol_SliderGrab]            = ImVec4( 0.34f, 0.34f, 0.34f, 0.54f );
        colors[ImGuiCol_SliderGrabActive]      = ImVec4( 0.56f, 0.56f, 0.56f, 0.54f );
        colors[ImGuiCol_Button]                = ImVec4( 0.05f, 0.05f, 0.05f, 0.54f );
        colors[ImGuiCol_ButtonHovered]         = ImVec4( 0.19f, 0.19f, 0.19f, 0.54f );
        colors[ImGuiCol_ButtonActive]          = ImVec4( 0.20f, 0.22f, 0.23f, 1.00f );
        colors[ImGuiCol_Header]                = ImVec4( 0.00f, 0.00f, 0.00f, 0.52f );
        colors[ImGuiCol_HeaderHovered]         = ImVec4( 0.00f, 0.00f, 0.00f, 0.36f );
        colors[ImGuiCol_HeaderActive]          = ImVec4( 0.20f, 0.22f, 0.23f, 0.33f );
        colors[ImGuiCol_Separator]             = ImVec4( 0.28f, 0.28f, 0.28f, 0.29f );
        colors[ImGuiCol_SeparatorHovered]      = ImVec4( 0.44f, 0.44f, 0.44f, 0.29f );
        colors[ImGuiCol_SeparatorActive]       = ImVec4( 0.40f, 0.44f, 0.47f, 1.00f );
        colors[ImGuiCol_ResizeGrip]            = ImVec4( 0.28f, 0.28f, 0.28f, 0.29f );
        colors[ImGuiCol_ResizeGripHovered]     = ImVec4( 0.44f, 0.44f, 0.44f, 0.29f );
        colors[ImGuiCol_ResizeGripActive]      = ImVec4( 0.40f, 0.44f, 0.47f, 1.00f );
        colors[ImGuiCol_Tab]                   = ImVec4( 0.00f, 0.00f, 0.00f, 0.52f );
        colors[ImGuiCol_TabHovered]            = ImVec4( 0.14f, 0.14f, 0.14f, 1.00f );
        colors[ImGuiCol_TabActive]             = ImVec4( 0.20f, 0.20f, 0.20f, 0.36f );
        colors[ImGuiCol_TabUnfocused]          = ImVec4( 0.00f, 0.00f, 0.00f, 0.52f );
        colors[ImGuiCol_TabUnfocusedActive]    = ImVec4( 0.14f, 0.14f, 0.14f, 1.00f );
        colors[ImGuiCol_DockingPreview]        = ImVec4( 0.33f, 0.67f, 0.86f, 1.00f );
        colors[ImGuiCol_DockingEmptyBg]        = ImVec4( 1.00f, 0.00f, 0.00f, 1.00f );
        colors[ImGuiCol_PlotLines]             = ImVec4( 1.00f, 0.00f, 0.00f, 1.00f );
        colors[ImGuiCol_PlotLinesHovered]      = ImVec4( 1.00f, 0.00f, 0.00f, 1.00f );
        colors[ImGuiCol_PlotHistogram]         = ImVec4( 1.00f, 0.00f, 0.00f, 1.00f );
        colors[ImGuiCol_PlotHistogramHovered]  = ImVec4( 1.00f, 0.00f, 0.00f, 1.00f );
        colors[ImGuiCol_TableHeaderBg]         = ImVec4( 0.00f, 0.00f, 0.00f, 0.52f );
        colors[ImGuiCol_TableBorderStrong]     = ImVec4( 0.00f, 0.00f, 0.00f, 0.52f );
        colors[ImGuiCol_TableBorderLight]      = ImVec4( 0.28f, 0.28f, 0.28f, 0.29f );
        colors[ImGuiCol_TextSelectedBg]        = ImVec4( 0.20f, 0.22f, 0.23f, 1.00f );
        colors[ImGuiCol_DragDropTarget]        = ImVec4( 0.33f, 0.67f, 0.86f, 1.00f );
        colors[ImGuiCol_NavHighlight]          = ImVec4( 1.00f, 0.00f, 0.00f, 1.00f );
        colors[ImGuiCol_NavWindowingHighlight] = ImVec4( 1.00f, 0.00f, 0.00f, 0.70f );
        colors[ImGuiCol_NavWindowingDimBg]     = ImVec4( 0.80f, 0.80f, 0.80f, 0.35f );
        colors[ImGuiCol_ModalWindowDimBg]      = ImVec4( 0.80f, 0.80f, 0.80f, 0.35f );
    }

    ImVec4 ThemeManager::GetSelectedColor()
    {
        return s_SelectedColor;
    }

    ImVec4 ThemeManager::GetIconColor()
    {
        return s_IconColor;
    }

    ImVec4 ThemeManager::GetSectionHeaderColor()
    {
        return C( 47, 47, 47 ); // #2F2F2F — UE's category bar, one step above the panel
    }

    ImVec4 ThemeManager::GetAxisColor( int axis )
    {
        // Sampled from the UE reference's transform fields; the viewport gizmo uses the same three.
        switch ( axis )
        {
            case 0:
                return C( 203, 38, 0 ); // X
            case 1:
                return C( 103, 169, 0 ); // Y
            case 2:
                return C( 44, 126, 237 ); // Z
            default:
                return C( 150, 150, 150 );
        }
    }

    ImVec4 ThemeManager::GetHighlightColor()
    {
        return C( 255, 170, 60 ); // amber: armed tool / live plane / mode is on
    }

    ImVec4 ThemeManager::GetWarningColor()
    {
        return C( 242, 191, 89 );
    }

    ImVec4 ThemeManager::GetErrorColor()
    {
        return C( 255, 115, 102 );
    }

    ImVec4 ThemeManager::GetSuccessColor()
    {
        return C( 92, 214, 122 );
    }

} // namespace Desert::Editor
