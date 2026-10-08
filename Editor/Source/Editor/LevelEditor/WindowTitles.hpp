#pragma once

#include <format>
#include <string>
#include <string_view>

namespace Desert::Engine
{
    class Application;
} // namespace Desert::Engine

namespace Desert::Core
{
    class Scene;
} // namespace Desert::Core

namespace Desert::Editor
{
    // THE OS WINDOW'S NAME: "Desert Engine — <project> — <level>". With the system frame gone it is painted
    // nowhere, but the Dock, Mission Control, the taskbar and every window switcher read it.
    inline constexpr std::string_view kEditorWindowTitleFormat = "Desert Engine — {} — {}";

    // Pushes the title when it differs from Window::GetTitle (the window owns the string; one
    // glfwSetWindowTitle per change, not sixty a second). Nothing without a window or a scene.
    void SyncEditorWindowTitle( Engine::Application& application, ::Desert::Core::Scene* scene );

    // THE TITLE OF A WINDOW WITH A GLYPH: "<icon>  <label>###<id>". One shape, one home: a panel and a document
    // are both drawn with it, and the two call sites used to glue the same five pieces by hand.
    inline constexpr std::string_view kIconWindowTitleFormat = "{}  {}###{}";

    inline std::string IconWindowTitle( std::string_view icon, std::string_view label, std::string_view id )
    {
        return std::format( kIconWindowTitleFormat, icon, label, id );
    }

    // The name a person reads for a panel: the ImGui "##id" suffix dropped (the palette's Panel labels, the
    // channel's `input` panel address).
    [[nodiscard]] inline std::string PanelShownName( const std::string& name )
    {
        std::string shown = name;
        if ( const auto hash = shown.find( "##" ); hash != std::string::npos )
            shown.erase( hash );
        return shown;
    }

    // Icon shown before a panel's tab/title + its View-menu entry (see WindowTitles.cpp).
    [[nodiscard]] const char* PanelIcon( const std::string& name );
    // "<icon>  <label>###<stable id>" for a tool panel; the ImGui window id stays panel->GetName().
    [[nodiscard]] std::string PanelDisplayTitle( const std::string& name );
} // namespace Desert::Editor
