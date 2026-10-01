#pragma once

// THE LEVEL EDITOR'S TOOLBAR (UE: SLevelEditorToolBar / "LevelEditor.LevelEditorToolBar").
//
// The strip below the menu bar: save + undo/redo, editor modes, transform tools, the two snap steps on the
// left; the playback group (PlaySession's) on the middle; package, profiler and preferences on the right. Also
// the project and level-name sections that EditorLayer's title bar draws beside the menus.
//
// Every control here writes to state that already has one owner elsewhere (CommandHistory, ViewportMode,
// GizmoState, Scene::GetState, SceneFiles, PlaySession, PreferencesWindow) — the bar reports and commands, it
// never stores. A member of EditorLayer BY VALUE; its collaborators arrive by reference.

#include <string>

namespace Desert::Editor
{
    class PlaySession;
    class PreferencesWindow;
    class SceneFiles;
    class SceneWorkspace;

    class LevelToolbar
    {
    public:
        LevelToolbar( SceneWorkspace& workspace, SceneFiles& sceneFiles, PlaySession& play,
                      PreferencesWindow& preferences, bool& showProfiler )
             : m_Workspace( workspace ), m_SceneFiles( sceneFiles ), m_Play( play ), m_Preferences( preferences ),
               m_ShowProfiler( showProfiler )
        {
        }

        // The strip itself, drawn inside the dockspace host window so it takes a fixed height above the docked
        // panels.
        void Draw();

        // ===== Title-bar sections (inside BeginMainMenuBar, after the menus) =====
        // The project name and the build configuration badge.
        static void DrawProjectSection();
        // The level's name; a double click renames it.
        void DrawSceneRenameSection();

    private:
        // One toolbar button. `active` is the armed/on state: tinted fill plus a 2px underline.
        static bool ToolbarButton( const char* icon, const char* label, bool active = false,
                                   const char* tooltip = nullptr, bool enabled = true );
        static void ToolbarSeparator();
        // A snap step: the button reports the current step and opens the list that changes it, with the
        // shared snapping toggle at the top. `rotation` picks the angle step over the grid step.
        static void DrawSnapControl( bool rotation );

        SceneWorkspace&    m_Workspace;
        SceneFiles&        m_SceneFiles;
        PlaySession&       m_Play;
        PreferencesWindow& m_Preferences;
        // The Profiler button ticks the profiler window, which the layer draws itself.
        bool& m_ShowProfiler;

        // The level name being edited in place, and whether it is.
        bool        m_RenamingScene = false;
        std::string m_SceneNameBuffer;
    };
} // namespace Desert::Editor
