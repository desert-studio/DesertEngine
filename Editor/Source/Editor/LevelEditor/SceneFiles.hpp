#pragma once

// SCENE FILES (UE: FEditorFileUtils — New / Open / Save of the level; UEditorEngine::Map_Load).
//
// Which file the open scene is, whether it has unsaved changes, the deferred load and New Scene (serviced
// between frames — they tear down render resources), THE ONE save path (Ctrl+S, File -> Save, the palette,
// "Save and Open"), the recent list, the Scenes menu and the Open Scene / unsaved-changes dialogs
// (SceneFileDialogs.cpp). A member of EditorLayer BY VALUE, declared after SceneWorkspace and ViewportCapture.

#include <Engine/Desert.hpp>
#include <Common/Content/TextAssetHeader.hpp>
#include "Editor/Core/CommandPalette.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Desert::Editor
{
    class SceneWorkspace;
    class ViewportCapture;

    class SceneFiles
    {
    public:
        SceneFiles( SceneWorkspace& workspace, const std::shared_ptr<Assets::AssetManager>& assets,
                    ViewportCapture& capture );

        // Replaces the world WITHOUT asking about unsaved edits — only for callers that are not a user
        // choosing a scene, or that come after the ask (Desert/Tests/Editor/AssetOpenRoute holds the list).
        void RequestLoad( const Common::Filepath& path );
        void RequestNew()
        {
            m_NewSceneRequested = true;
        }
        // Serviced by DrawDialogs, as File -> Save always was.
        void RequestSave()
        {
            m_SaveSceneRequested = true;
        }
        [[nodiscard]] bool HasPendingLoad() const
        {
            return m_SceneLoadRequested.has_value();
        }
        [[nodiscard]] bool HasPendingNew() const
        {
            return m_NewSceneRequested;
        }
        [[nodiscard]] bool HasPendingRequests() const
        {
            return HasPendingLoad() || m_NewSceneRequested || m_SaveSceneRequested;
        }
        // Drains Core::SceneOpenRequest through the unsaved-changes gate.
        void ConsumeOpenRequest();
        // The deferred load (with the Init fallback for a refused file), then New Scene. True when a load ran.
        [[nodiscard]] bool ServiceRequests();

        // Serializes the active scene to @p path (startup content). False when the bytes did not land.
        [[nodiscard]] bool SaveSceneTo( const std::string& path );
        // THE ONE place the open scene is saved from; true when the scene on disk is now current — a caller
        // about to destroy the in-memory scene MUST branch on it (Editor/Core/SceneSaveRules.hpp).
        [[nodiscard]] bool SaveOpenScene();

        [[nodiscard]] bool HasUnsavedChanges() const;
        // WHICH FILE THE OPEN SCENE IS; empty for a scene never on disk. The editor's, not the scene's: Play ->
        // Stop rebuilds the scene from a snapshot and would lose an identity kept inside it.
        [[nodiscard]] const Common::Filepath& OpenScenePath() const
        {
            return m_OpenScenePath;
        }
        // A scene built and written by the editor itself is an open file like any other from then on.
        void AdoptOpenPath( const Common::Filepath& path )
        {
            m_OpenScenePath = path;
        }

        // Every .desce under the project's scenes root, recursively, sorted by Label.
        static std::vector<Common::Filepath> CollectAvailableScenes();
        // How a scene is NAMED in every picker: its path relative to the scenes root.
        static std::string Label( const Common::Filepath& path );

        void AppendOpenSceneCommands( std::vector<PaletteCommand>& commands );
        void AppendSaveSceneCommand( std::vector<PaletteCommand>& commands );

        void DrawScenesMenu();
        void DrawOpenSceneMenuItem();
        void DrawDialogs();

    private:
        void LoadSceneInternal( const Common::Filepath& requested );
        void NewSceneInternal();
        // Drops the scene's text header when `destination` is not the file it was opened as (a copy is a new
        // asset with a new GUID); returns the header it had, for a failed save to put back.
        std::optional<Common::Content::TextAssetHeaderSerialized>
        ForgetAssetIdentityUnlessSameFile( const std::string& destination );
        // WHERE Ctrl+S goes: the open file, or — never on disk — one named after the scene.
        [[nodiscard]] Common::Filepath SceneSaveDestination() const;

        void PrepareScenePopup();
        void DrawOpenScenePopup();
        // "Discard unsaved changes?" for a scene opened by drag-drop / double-click (see m_PendingOpenScene).
        void DrawConfirmOpenScenePopup();
        void DrawSaveScenePopup();

        SceneWorkspace&                              m_Workspace;
        const std::shared_ptr<Assets::AssetManager>& m_Assets;
        ViewportCapture&                             m_Capture;

        // The CommandHistory revision at the last save/load.
        uint64_t                        m_SavedRevision = 0;
        Common::Filepath                m_OpenScenePath;
        std::optional<Common::Filepath> m_SceneLoadRequested;
        bool                            m_NewSceneRequested  = false;
        bool                            m_SaveSceneRequested = false;
        std::vector<Common::Filepath>   m_RecentScenes;

        bool                          m_OpenScenePopup = false;
        std::vector<Common::Filepath> m_AvailableScenes;
        int                           m_SelectedSceneIndex = -1;
        // Open Scene popup: substring filter over the (recursive) scene list.
        char m_SceneFilter[128] = {};

        // A scene a panel asked to open while the current one had unsaved edits: held until the confirm
        // popup says discard/save/cancel. m_SaveAndOpenError: "Save and Open" could not write the scene —
        // the modal STAYS OPEN and shows it; cleared whenever the modal is dismissed.
        std::optional<Common::Filepath> m_PendingOpenScene;
        bool                            m_ConfirmOpenScenePopup = false;
        std::string                     m_SaveAndOpenError;
    };
} // namespace Desert::Editor
