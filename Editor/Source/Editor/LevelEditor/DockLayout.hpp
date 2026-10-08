#pragma once

// THE LEVEL EDITOR'S DOCK LAYOUT (UE: FTabManager + FLayout, LevelEditorLayout): the full-screen host window
// the dockspace lives in, the default layout and the named ones (View ▸ Layouts), the layout file under the
// project, the tool panels' windows (title, focus, maximize/restore), the contextual panels that open and
// close themselves, the bottom drawer's chevron and the crash-recovery prompt drawn over the layout.
//
// The panels themselves are created by RegisterEditorPanels (EditorPanels.cpp — UE: RegisterTabSpawner), and
// stay owned by EditorLayer's PanelRegistry; this module draws them and decides where they sit.

#include <Common/Core/ResultStr.hpp>
#include <Common/Core/Events/EventTree.hpp>
#include <Editor/Core/CommandPalette.hpp>
#include <Editor/Core/PanelMaximize.hpp>
#include <ImGui/imgui.h>
#include <ImGui/imgui_internal.h> // ImGuiDockNodeFlags_NoWindowMenuButton / NoCloseButton

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace Desert::Assets
{
    class AssetManager;
} // namespace Desert::Assets

namespace Desert::Animation
{
    class AnimationLibrary;
} // namespace Desert::Animation

namespace Desert::Engine
{
    class Application;
} // namespace Desert::Engine

namespace Desert::Editor
{
    class PanelRegistry;
    class DocumentHost;
    class SceneWorkspace;
    class SceneFiles;
    class PlaySession;
    class FileExplorerPanel;
    class WorldPartitionPanel;

    class DockLayout
    {
    public:
        // References only: DocumentHost and PanelRegistry are declared AFTER this module in EditorLayer (the
        // document host holds FocusSlot() by reference, so this module must exist first); none is touched
        // before EditorLayer::OnAttach.
        DockLayout( PanelRegistry& panels, DocumentHost& documents, SceneWorkspace& workspace,
                    SceneFiles& sceneFiles );
        DockLayout( const DockLayout& )            = delete;
        DockLayout& operator=( const DockLayout& ) = delete;

        // The window (tool panel or document) to bring to the front of its dock next frame; ONE slot for both,
        // which is why DocumentHost holds it by reference.
        [[nodiscard]] std::string& FocusSlot()
        {
            return m_FocusPanel;
        }

        // io.IniFilename = <Project>/Saved/Config/EditorLayout.ini. Before the first frame.
        [[nodiscard]] static Common::BoolResultStr BindLayoutFile();

        // On quit with a panel maximized: the layout file gets the arrangement from before the maximize, and
        // ImGui's own save at shutdown is switched off so the lifted-out panel is not written as a floating
        // window.
        [[nodiscard]] Common::BoolResultStr KeepLayoutAcrossQuit();

        // After an unclean exit: the autosave to offer (empty = nothing to offer).
        void OfferRecovery( std::filesystem::path autosave );

        // View ▸ Layouts.
        void RequestSaveLayoutAs();
        void RequestResetLayout()
        {
            m_ResetDefaultLayout = true;
        }

        // Opens/closes panels whose context appeared or vanished (see IPanel::IsContextual).
        void UpdateContextualPanels();

        // One frame, in this order, around EditorLayer's toolbar and documents: BeginHost → (toolbar) →
        // DrawDockSpace → DrawPanels → … → popups → EndHost.
        void        BeginHost();
        void        DrawDockSpace();
        void        DrawPanels();
        // AFTER DrawPanels: the event tree's focus and hover follow the panel ImGui gave them to this frame;
        // @p fallback (the layer's own node) when no panel holds them. No-op without a tree.
        void RouteEvents( Common::EventTree* events, Common::EventNodeId fallback );
        static void EndHost();

        // After an unclean exit, offers to reopen the newest autosave. No-op unless one was found.
        void DrawRecoveryPopup();
        // Modal for naming + saving the current docking layout (opened from View -> Layouts).
        void DrawLayoutSavePopup();
        // The status bar's chevron that collapses/restores the bottom drawer.
        void DrawBottomDrawerToggle();

        // Palette providers: "Panel / Open <tool>", "Panel / Maximize|Restore panel", the frameless window's
        // own Maximize/Restore/Minimize (@p frameless is null when the OS draws the frame).
        void        AppendPanelCommands( std::vector<PaletteCommand>& commands );
        void        AppendMaximizeCommands( std::vector<PaletteCommand>& commands );
        static void AppendWindowCommands( std::vector<PaletteCommand>& commands, Engine::Application* frameless );

    private:
        PanelRegistry&  m_Panels;
        DocumentHost&   m_Documents;
        SceneWorkspace& m_Workspace;
        SceneFiles&     m_SceneFiles;

        std::string m_FocusPanel;
        // "Maximize panel" / "Restore panel": the one panel lifted out of its dock, and the layout it came from.
        PanelMaximize m_PanelMaximize;

        bool                  m_ShowRecoveryPrompt = false;
        std::filesystem::path m_RecoveryAutosave;

        bool m_ResetDefaultLayout  = false;
        bool m_ShowSaveLayoutPopup = false;
        char m_LayoutNameBuf[64]   = {};

        // The host window (were function statics in EditorLayer::OnUIRender).
        bool               m_HostOpen   = true;
        bool               m_Fullscreen = true;
        ImGuiDockNodeFlags m_DockspaceFlags =
             ImGuiDockNodeFlags_NoWindowMenuButton | ImGuiDockNodeFlags_NoCloseButton;

        // The bottom drawer (Assets / Logs / Shader Code) can be collapsed to its tab bar and restored, so a
        // panel is never more than a click away while the viewport still gets the space when it's not needed:
        // one is still right there. m_BottomHeight remembers the expanded size across toggles.
        ImGuiID m_BottomDockId    = 0;
        bool    m_BottomCollapsed = false;
        float   m_BottomHeight    = 0.0f;
    };

    // The tools EditorLayer's PanelRegistry holds, created in View-menu order (UE: RegisterTabSpawner). The two
    // returned are the non-owning handles EditorLayer keeps for the palette and the cook (they live in @p panels).
    struct EditorPanelHandles
    {
        FileExplorerPanel*   FileExplorer   = nullptr;
        WorldPartitionPanel* WorldPartition = nullptr;
    };
    [[nodiscard]] EditorPanelHandles
    RegisterEditorPanels( PanelRegistry& panels, SceneWorkspace& workspace, PlaySession& play,
                          DocumentHost& documents, std::shared_ptr<Assets::AssetManager>& assetManager,
                          const std::unique_ptr<Animation::AnimationLibrary>& animationLibrary );
} // namespace Desert::Editor
