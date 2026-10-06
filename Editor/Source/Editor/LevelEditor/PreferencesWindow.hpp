#pragma once

// EDITOR PREFERENCES WINDOW (UE: SSettingsEditor over UEditorPerProjectUserSettings).
//
// The window Edit ▸ Preferences... and the toolbar's gear open: the editor camera speed, gizmo snap, autosave and
// the selection outline, each bound to the live EditorPreferences store and written on release of its control
// (see the note above Draw in PreferencesWindow.cpp). A member of EditorLayer BY VALUE; the main menu and the
// toolbar reach it by reference.

namespace Desert::Editor
{
    class SceneWorkspace;

    class PreferencesWindow
    {
    public:
        // The workspace is where the camera speed is pushed: the active scene's editor camera follows the slider.
        explicit PreferencesWindow( SceneWorkspace& workspace ) : m_Workspace( workspace )
        {
        }

        // Draws the window while it is open; no-op otherwise.
        void Draw();

        void Open()
        {
            m_Open = true;
        }
        void Toggle()
        {
            m_Open = !m_Open;
        }
        [[nodiscard]] bool IsOpen() const
        {
            return m_Open;
        }

    private:
        SceneWorkspace& m_Workspace;
        bool            m_Open = false;
    };
} // namespace Desert::Editor
