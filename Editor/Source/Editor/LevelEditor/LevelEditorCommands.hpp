#pragma once

// THE LEVEL EDITOR'S COMMANDS (UE: FLevelEditorCommands + FLevelEditorActionCallbacks,
// Editor/LevelEditor/Private/LevelEditorActions.cpp). One home for:
//   - the command dictionary: every palette provider, registered in palette order (CommandRegistry.hpp). That
//     order IS the palette's order of groups and the control channel's vocabulary;
//   - the Ctrl+P "go to anything" overlay drawn over that dictionary (CommandPalette.hpp);
//   - the level's global keyboard shortcuts: Ctrl+Z/Y/D/C/V/N/R/S and Ctrl+P, and the documents' Ctrl+Tab.
//
// A member of EditorLayer BY VALUE, declared after every module whose commands it lists and before the control
// service, which reads the dictionary (Registry()). Every slot arrives by reference and is read when an entry
// RUNS, so re-pointing the main scene or the asset manager needs no re-registration.

#include "Editor/Core/CommandPalette.hpp"
#include "Editor/Core/Commands/CommandRegistry.hpp"
#include "Editor/Core/Selection/EntityCommands.hpp"
#include "Editor/Panels/FileExplorer/AssetCommands.hpp"
#include <Common/Core/ResultStr.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct ImGuiIO;

namespace Desert::Engine
{
    class Application;
}

namespace Desert::Assets
{
    class AssetManager;
}

namespace Desert::Editor
{
    class AssetCompiling;
    class DockLayout;
    class DocumentHost;
    class FileExplorerPanel;
    class MainMenu;
    class PanelRegistry;
    class PlaySession;
    class SceneFiles;
    class SceneWorkspace;
    class WorldPartitionPanel;

    namespace UI
    {
        class WindowChrome;
    }

    class LevelEditorCommands
    {
    public:
        // The modules whose commands the palette lists. The two panel pointers are the layer's own slots, filled
        // after the tools are registered (AssetCommands reads them when an entry runs). @p Chrome is
        // engaged when the editor draws its own window frame — the "Window" group then offers the frame's
        // actions. @p ShowFolder is the one "Show in browser" navigation (EditorLayer::ShowFolderInBrowser).
        struct Modules
        {
            SceneWorkspace&                              Workspace;
            SceneFiles&                                  Files;
            PlaySession&                                 Play;
            DocumentHost&                                Documents;
            DockLayout&                                  Dock;
            MainMenu&                                    Menu;
            AssetCompiling&                              Compiling;
            const PanelRegistry&                         Panels;
            const std::shared_ptr<Assets::AssetManager>& AssetsSlot;
            FileExplorerPanel* const&                    FileExplorer;
            WorldPartitionPanel* const&                  WorldPartition;
            Engine::Application*                         App;
            const std::optional<UI::WindowChrome>&       Chrome;
            AssetCommands::ShowFolder                    ShowFolder;
        };

        explicit LevelEditorCommands( Modules modules );

        // Registers every palette provider, in palette order. Once, in OnAttach.
        void RegisterProviders();

        // The dictionary the control channel lists and runs (ControlService).
        [[nodiscard]] CommandRegistry& Registry()
        {
            return m_Commands;
        }

        // The palette's and the control channel's list, built from the dictionary.
        [[nodiscard]] std::vector<PaletteCommand> BuildPaletteCommands();

        // The level's global shortcuts, at frame start, before any panel iterates the scene: the edit ones
        // (edit mode only, never while a text field owns the keyboard), Ctrl+P and the documents' Ctrl+Tab.
        void HandleShortcuts( const ImGuiIO& io );

        // Ctrl+P "go to anything": draws the overlay over the dictionary. No-op unless open.
        void DrawPalette();

    private:
        // The palette groups no other module owns (add shape, the palette's own door, scenes/views).
        void        AppendAddShapeCommands( std::vector<PaletteCommand>& commands );
        void        AppendPaletteDoorCommand( std::vector<PaletteCommand>& commands );
        static void AppendSceneCommands( std::vector<PaletteCommand>& commands );

        Modules m_Modules;

        CommandPalette m_Palette;
        // Every palette provider, in palette order; registered in RegisterProviders.
        CommandRegistry m_Commands;
        // The palette asked for BY NAME, from its own dictionary — the only way an unattended run can put it on
        // screen, since a keystroke is not available here. Deferred rather than opened in the closure: Draw()
        // closes the palette on the line after it runs an entry, so opening it from inside itself would work
        // over the socket and do nothing under a person's hand.
        bool m_OpenPaletteRequested = false;

        // The providers that hold state or several slots. The census is taken once per build
        // (m_Commands.OnBuildBegin) and read by Assets, Foliage and Open.
        AssetFileCensus m_AssetFiles;
        EntityCommands  m_EntityCommands;
        AssetCommands   m_AssetCommands;
    };
} // namespace Desert::Editor
