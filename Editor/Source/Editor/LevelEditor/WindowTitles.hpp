#pragma once

#include <format>
#include <string>
#include <string_view>

namespace Desert::Editor
{
    // THE TITLE OF A WINDOW WITH A GLYPH: "<icon>  <label>###<id>". One shape, one home: a panel and a document
    // are both drawn with it, and the two call sites used to glue the same five pieces by hand.
    inline constexpr std::string_view kIconWindowTitleFormat = "{}  {}###{}";

    inline std::string IconWindowTitle( std::string_view icon, std::string_view label, std::string_view id )
    {
        return std::format( kIconWindowTitleFormat, icon, label, id );
    }

    // Icon shown before a panel's tab/title + its View-menu entry (see WindowTitles.cpp).
    [[nodiscard]] const char* PanelIcon( const std::string& name );
    // "<icon>  <label>###<stable id>" for a tool panel; the ImGui window id stays panel->GetName().
    [[nodiscard]] std::string PanelDisplayTitle( const std::string& name );
} // namespace Desert::Editor
