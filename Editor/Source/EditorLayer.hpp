#pragma once

#include <Common/Core/Subsystems/SubsystemCollection.hpp>

#include <Engine/Core/WorldStreamer.hpp>
#include <Engine/Desert.hpp>
#include <Engine/Assets/AsyncAssetLoader.hpp>
#include <ImGui/imgui.h>
#include "Editor/Widgets/UIHelper/ImGuiUI.hpp"
#include "Editor/Panels/IPanel.hpp"
#include "Editor/Core/PlayWorldCommands.hpp"
#include "Editor/Core/SceneViewIdentity.hpp"
#include "Editor/Core/Selection/AuthoringContext.hpp"
#include "Editor/Core/PanelRegistry.hpp"
#include "Editor/RenderSystems/RenderRigistry.hpp"
#include "Editor/LevelEditor/PlaySession.hpp"
#include "Editor/LevelEditor/SceneWorkspace.hpp"
#include "Editor/LevelEditor/SceneFiles.hpp"
#include "Editor/LevelEditor/ViewportCapture.hpp"
#include "Editor/LevelEditor/DocumentHost.hpp"
#include "Editor/LevelEditor/MainMenu.hpp"
#include "Editor/LevelEditor/PreferencesWindow.hpp"
#include "Editor/LevelEditor/LevelToolbar.hpp"
#include "Editor/LevelEditor/StatusBar.hpp"
#include "Editor/LevelEditor/ShotDirector.hpp"
#include "Editor/LevelEditor/SessionRecovery.hpp"
#include "Editor/LevelEditor/LevelEditorCommands.hpp"
#include "Editor/LevelEditor/ControlService.hpp"
#include "Editor/LevelEditor/AssetCompiling.hpp"
#include "Editor/LevelEditor/EditorStartup.hpp"
#include "Editor/LevelEditor/ProfilerWindow.hpp"
#include "Editor/LevelEditor/DockLayout.hpp"
#include "Editor/LevelEditor/EditorImGuiHost.hpp"
#include "Editor/Panels/FileExplorer/AssetThumbnailPool.hpp"
#include "Editor/Splash/SplashScreen.hpp"

#include <chrono>
#include <optional>

#include <Common/Content/TextAssetHeader.hpp>

#include <filesystem>
#include <unordered_map>
#include <unordered_set>

namespace Desert::Editor
{
    class ImportManager;
    class FileExplorerPanel;
    class WorldPartitionPanel;
    class ViewportPanel;

    class EditorLayer;

    void CreateSubsystems( Common::SubsystemCollection<EditorLayer>& collection );

    class EditorLayer : public Common::Layer
    {
    public:
        template <typename T>
        [[nodiscard]] T* GetSubsystem() const
        {
            return m_Subsystems ? m_Subsystems->template Get<T>() : nullptr;
        }

        // @p splash is the start-up splash CreateApplication put up before the renderer existed; this
        // layer reports its steps to it and takes it down on the first real frame (RevealWhenReady).
        EditorLayer( Engine::Application* application, const std::string& layerName,
                     std::unique_ptr<Splash::SplashScreen> splash );
        ~EditorLayer();

        [[nodiscard]] Common::BoolResultStr OnAttach() override;
        [[nodiscard]] Common::BoolResultStr OnDetach() override;
        [[nodiscard]] Common::BoolResultStr OnUpdate( const Common::Timestep& ts ) override;
        [[nodiscard]] Common::BoolResultStr OnUIRender() override;

        // The frame is out. This is where the control channel keeps its promise: a reply leaves only
        // after a frame that already reflects the command it answers, and a `shot.window` reads that very
        // frame back off the swapchain. See Editor/LevelEditor/ControlService.hpp.
        void OnFramePresented() override;

    private:
        // ===== Popups =====

        // The one navigation `run Browse <folder>` and a field's "Show in browser" share.
        Common::BoolResultStr ShowFolderInBrowser( const std::string& folder );
        Common::BoolResultStr SyncBrowserToAsset( const std::string& file );
        // Leaving the editor from its own frame's x or File > Exit: every dirty document asks first, and the
        // editor closes once the last one is answered. Cancel on any of them keeps the editor open. The
        // control channel's `quit` does not come here — an unattended run has nobody to answer.
        void RequestEditorExit();

        // This frame's capture verdict (ShotDirector::AdmitFrame) from the layer's own state; false outside a
        // headless capture.
        bool AdmitShotFrame();

        // Runs one render frame for a scene (outline aid + Begin/RegistryRender/OnUpdate/End). Called for
        // every open document each frame so all viewports stay live.

        // Startup content is DATA, not code — these build entities into m_Workspace.ActiveScene() so the result
        // can be serialized to a .desce ONCE and loaded like any scene afterwards.

    private:
        Engine::Application* m_Application;

        // The window frame, the close gate, the ImGui context and its backend (UE: FSlateApplication). BEFORE
        // m_LevelCommands, which binds its window chrome slot. See Editor/LevelEditor/EditorImGuiHost.hpp.
        EditorImGuiHost m_ImGuiHost;
        // The last title pushed to the window is NOT stored here: Window::GetTitle owns it, and
        // SyncWindowTitle compares against that. See Window.hpp.

        std::shared_ptr<Assets::AssetManager> m_AssetManager;
        // The library the boot's "Indexing animation clips" stage fills (Assets::IndexAnimationClips).
        std::unique_ptr<Animation::AnimationLibrary> m_AnimationLibrary;
        std::unique_ptr<ImportManager>               m_ImportManager;
        // The mesh cook after the reveal and the .demat/.shader live reload (UE: FAssetCompilingManager). See
        // Editor/LevelEditor/AssetCompiling.hpp.
        AssetCompiling m_AssetCompiling{ m_AssetManager, m_AnimationLibrary, m_ImportManager };
        // THE EDITOR'S THUMBNAIL POOL (UE: FAssetThumbnailPool belongs to the editor; the Content Browser only
        // draws from it — AssetThumbnail.cpp). Built in OnAttach before the panels and released in OnDetach
        // right after m_Panels.Clear(); declared BEFORE m_Panels, so even ~EditorLayer destroys the panel that
        // draws from it first. EditorStartup drives it directly for the splash's warm-up and upload passes.
        std::unique_ptr<AssetThumbnailPool> m_ThumbnailPool;

        FileExplorerPanel* m_FileExplorerPanel = nullptr; // non-owning (lives in m_Panels)
        // Non-owning (lives in m_Panels). Kept because the command palette offers the panel's Convert
        // action, and a palette entry has to reach the object that owns the action.
        WorldPartitionPanel* m_WorldPartitionPanel = nullptr;

        // The open worlds (primary scene, extra documents and viewports) and the Play session on the active one.
        // By value and BEFORE m_Panels, so they outlive the panels that point into them; their collaborators
        // arrive by reference (see Editor/LevelEditor/SceneWorkspace.hpp, PlaySession.hpp).
        SceneWorkspace m_Workspace{ m_Panels, m_AssetManager, m_AnimationLibrary };
        PlaySession    m_Play{ m_Workspace, m_AssetManager };
        // Pictures out of the viewport and the window, and the project tile (UE: FScreenshotRequest).
        ViewportCapture m_Capture{ m_Workspace };
        // New / Open / Save of the level file, its dialogs and the Scenes menu (UE: FEditorFileUtils).
        SceneFiles m_SceneFiles{ m_Workspace, m_AssetManager, m_Capture };

        // Opening, focus and closing of the asset documents, the unsaved-close question, the well and its
        // tabs, the refusal past the view budget (UE: UAssetEditorSubsystem + the document half of
        // FGlobalTabmanager). See Editor/LevelEditor/DocumentHost.hpp.
        // The dockspace, its layouts and the tool windows (UE: FTabManager / LevelEditorLayout). BEFORE
        // m_Documents: it owns the one focus slot (tool panel or document) the document host holds by reference.
        // See Editor/LevelEditor/DockLayout.hpp.
        DockLayout   m_Dock{ m_Panels, m_Documents, m_Workspace, m_SceneFiles };
        DocumentHost m_Documents{ m_Workspace, m_AssetManager, m_Dock.FocusSlot(),
                                  [this]( const std::string& folder ) { return ShowFolderInBrowser( folder ); } };

        // THE TOOLS. A container that cannot hold a document — see Editor/Core/PanelRegistry.hpp. That is
        // what makes "the View menu lists exactly the tools" true by construction rather than by a predicate
        // the menu, the command palette and --open-panel would each have had to remember.
        PanelRegistry                                           m_Panels;
        std::optional<Common::SubsystemCollection<EditorLayer>> m_Subsystems;

        // `m_ContextualShown` STOOD HERE — a set of raw panel pointers, inserted and erased in five
        // places and QUERIED IN NONE. Its own comment claimed it was what stopped a panel the user opened
        // by hand from being auto-closed; that is actually done by `IPanel::Pinned()`, which every branch
        // of UpdateContextualPanels already tests. So the set was write-only state, and one that went
        // dangling wholesale at `m_Panels.Clear()`. Removed with its five writes (A8-2), which is the same
        // decision this task took on `CloudNoiseService::GetGeneration` and `InstancesDirty`.
        // The focus slot this note once described lives in DockLayout (m_Dock.FocusSlot()).

        // Edit ▸ Preferences... and the toolbar's gear (UE: SSettingsEditor). See
        // Editor/LevelEditor/PreferencesWindow.hpp.
        PreferencesWindow m_Preferences{ m_Workspace };
        // The profiler window, the menu bar's engine stats and the --flight rows (UE: SProfilerWindow). Before the
        // menu and the toolbar, which toggle it. See Editor/LevelEditor/ProfilerWindow.hpp.
        ProfilerWindow m_Profiler{ m_Application, m_Workspace, m_Play, m_Shots };
        // File / Edit / View / Window / Scenes / Graphics / About and the menu's palette entries (UE:
        // FLevelEditorMenu). The layout's two entries and the editor's exit stay with their owners and arrive as
        // actions.
        MainMenu m_MainMenu{
             m_Workspace,
             m_SceneFiles,
             m_Documents,
             m_Panels,
             m_Preferences,
             m_Profiler.Shown(),
             { .RebuildCookedAssets = [this]
               { m_AssetCompiling.RebuildCookedAssets( m_Workspace.ActiveScene().get(), m_FileExplorerPanel ); },
               .RequestExit  = [this] { RequestEditorExit(); },
               .SaveLayoutAs = [this] { m_Dock.RequestSaveLayoutAs(); },
               .ResetLayout  = [this] { m_Dock.RequestResetLayout(); } } };
        // The strip below the menu bar and the title bar's project / level sections (UE: SLevelEditorToolBar).
        // See Editor/LevelEditor/LevelToolbar.hpp.
        LevelToolbar m_Toolbar{ m_Workspace, m_SceneFiles, m_Play, m_Preferences, m_Profiler.Shown() };
        // The bottom strip (UE: SStatusBar); the bottom drawer's chevron stays with the dock layout and arrives
        // as an action. See Editor/LevelEditor/StatusBar.hpp.
        StatusBar m_StatusBar{ m_Workspace, m_SceneFiles, m_Documents, m_AssetCompiling.CookQueue(),
                               [this] { m_Dock.DrawBottomDrawerToggle(); } };
        // Headless capture: `--shot`, `--play`, `--camera`/`--look` (UE: the automation screenshot director). See
        // Editor/LevelEditor/ShotDirector.hpp.
        ShotDirector m_Shots{ m_Workspace, m_SceneFiles, m_Play, m_Capture };
        // Autosave, the crash lock and its recovery pop-up, the device-lost save (UE: FPackageAutoSaver). See
        // Editor/LevelEditor/SessionRecovery.hpp.
        SessionRecovery m_Recovery{ m_Workspace, m_SceneFiles, m_Play, m_AssetManager };
        // The palette's dictionary, the Ctrl+P overlay and the level's global shortcuts (UE:
        // FLevelEditorCommands), after every module whose commands it lists. See
        // Editor/LevelEditor/LevelEditorCommands.hpp.
        LevelEditorCommands m_LevelCommands{
             { .Workspace      = m_Workspace,
               .Files          = m_SceneFiles,
               .Play           = m_Play,
               .Documents      = m_Documents,
               .Dock           = m_Dock,
               .Menu           = m_MainMenu,
               .Compiling      = m_AssetCompiling,
               .Panels         = m_Panels,
               .AssetsSlot     = m_AssetManager,
               .FileExplorer   = m_FileExplorerPanel,
               .WorldPartition = m_WorldPartitionPanel,
               .App            = m_Application,
               .Chrome         = m_ImGuiHost.Chrome(),
               .ShowFolder     = [this]( const std::string& folder ) { return ShowFolderInBrowser( folder ); } } };
        // The control channel (UE: Remote Control), after every module it reads. See
        // Editor/LevelEditor/ControlService.hpp.
        ControlService m_Control{
             m_Workspace, m_SceneFiles, m_Play, m_Documents, m_Capture, m_Panels, m_LevelCommands.Registry() };

        // QualityBoot::Start's answer, taken in the constructor (before the workspace's first renderer) and
        // returned by OnAttach.
        Common::BoolResultStr m_QualityStart = Common::MakeSuccess( true );
        // Set by the first OnUIRender that draws the editor rather than a loading frame.
        bool m_RealFrameDrawn = false;
        // The staged boot, the splash and its hand-over, the content settle (UE: FEditorLoadingScreen), after
        // every module it reads; built in the constructor, which receives the splash. See
        // Editor/LevelEditor/EditorStartup.hpp.
        EditorStartup m_Startup;
    };
} // namespace Desert::Editor