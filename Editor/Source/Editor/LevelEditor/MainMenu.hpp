#pragma once

// THE LEVEL EDITOR'S MAIN MENU (UE: UToolMenus "LevelEditor.MainMenu", FLevelEditorMenu).
//
// File / Edit / View / Window / Scenes / Graphics / About, drawn into the main menu bar that EditorLayer opens
// (the bar is also the window's title bar and carries the project, the level name and the stats beside the
// menus). Also the menu's own palette entries: "Menu / Open the <name> menu", "Close the open menu", and the
// Edit menu's Undo / Redo. A member of EditorLayer BY VALUE; its collaborators arrive by reference, and the
// four entries whose action belongs to the layout or to the editor's lifetime arrive as MainMenuActions.

#include "Editor/Core/CommandPalette.hpp"

#include <functional>
#include <string>
#include <vector>

namespace Desert::Editor
{
    class DocumentHost;
    class PanelRegistry;
    class PreferencesWindow;
    class SceneFiles;
    class SceneWorkspace;

    // What a menu entry does when its action is not the menu's to own (UE: the FUIAction a menu entry binds).
    struct MainMenuActions
    {
        std::function<void()> RebuildCookedAssets; // File ▸ Rebuild Cooked Assets
        std::function<void()> RequestExit;         // File ▸ Exit: the same ordered close as the title bar's x
        std::function<void()> SaveLayoutAs;        // View ▸ Layouts ▸ Save Current Layout...
        std::function<void()> ResetLayout;         // View ▸ Layouts ▸ Reset to Default Layout
    };

    class MainMenu
    {
    public:
        MainMenu( SceneWorkspace& workspace, SceneFiles& sceneFiles, DocumentHost& documents,
                  PanelRegistry& panels, PreferencesWindow& preferences, bool& showProfiler,
                  MainMenuActions actions );

        // Inside BeginMainMenuBar: re-opens the menu the channel holds open, then draws the seven menus in bar
        // order.
        void DrawMenus();

        // "Menu": open each of the bar's menus by name, and close the held one.
        void AppendMenuCommands( std::vector<PaletteCommand>& commands );
        // Edit ▸ Undo / Redo as palette entries ("Action / Undo", "Action / Redo").
        static void AppendUndoRedoCommands( std::vector<PaletteCommand>& commands );

    private:
        void DrawFileMenu();
        void DrawEditMenu();
        void DrawViewMenu();
        // Window ▸ Documents: the open documents, focused with a RADIO and closed with an x. A radio and
        // not a checkbox on purpose — a tick reads as "shown / hidden", which is the very thing a document
        // cannot be. See DocumentWell.
        void DrawWindowMenu();
        void DrawGraphicsMenu();
        void DrawAboutMenu();
        void DrawStyleSubmenu();

        SceneWorkspace&    m_Workspace;
        SceneFiles&        m_SceneFiles;
        DocumentHost&      m_Documents;
        PanelRegistry&     m_Panels;
        PreferencesWindow& m_Preferences;
        // View ▸ Profiler ticks the profiler window, which the layer draws itself.
        bool&           m_ShowProfiler;
        MainMenuActions m_Actions;

        // THE MENU HELD OPEN, by name, for as long as the channel says so. Empty = nothing held.
        //
        // This is what `--open-menu` used to be, and the difference is the whole point: a flag could hold
        // one menu open for the WHOLE RUN and could never let go, because there was no later moment at
        // which to tell it to. A menu is now opened and closed like anything else on the palette, so a
        // session can photograph the View menu and then carry on working.
        //
        // Re-issued every frame rather than opened once, for the reason it always was: a menu closes as
        // soon as focus leaves it, and a capture may land on any frame.
        std::string m_HeldOpenMenu;
    };
} // namespace Desert::Editor
