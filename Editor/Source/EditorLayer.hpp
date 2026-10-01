#pragma once

#include <Common/Core/Subsystems/SubsystemCollection.hpp>
#include <Editor/Core/PanelMaximize.hpp>

#include <Engine/Core/WorldStreamer.hpp>
#include <Engine/Desert.hpp>
#include <Engine/Assets/AsyncAssetLoader.hpp>
#include <ImGui/imgui.h>
#include "Editor/ImGuiIntegration/ImGuiLayer.hpp"
#include "Editor/Widgets/UIHelper/ImGuiUI.hpp"
#include "Editor/Panels/IPanel.hpp"
#include "Editor/Core/CommandPalette.hpp"
#include "Editor/Core/Commands/CommandRegistry.hpp"
#include "Editor/Core/Selection/EntityCommands.hpp"
#include "Editor/Panels/FileExplorer/AssetCommands.hpp"
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
#include "Editor/LevelEditor/ControlService.hpp"
#include "Editor/LevelEditor/AssetCompiling.hpp"
#include "Editor/LevelEditor/EditorStartup.hpp"
#include "Editor/LevelEditor/ProfilerWindow.hpp"
#include "Editor/Widgets/WindowChrome.hpp"
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
        void DrawMenuBar();

        // Opens/closes panels whose context appeared or vanished (see IPanel::IsContextual).
        void UpdateContextualPanels();

        // The palette's and the control channel's list, built from m_Commands (see CommandRegistry.hpp).
        [[nodiscard]] std::vector<PaletteCommand> BuildPaletteCommands();
        // The palette groups of modules not cut out yet (panels, documents, add shape, the palette's own
        // door, open, scenes/views, save/play/window): registered in palette order between the subject-owned
        // providers; the later EDL cuts move each to its module (DockLayout, DocumentHost, MainMenu,
        // SceneFiles...).
        void AppendPanelCommands( std::vector<PaletteCommand>& commands );
        void AppendMaximizeCommands( std::vector<PaletteCommand>& commands );
        void AppendAddShapeCommands( std::vector<PaletteCommand>& commands );
        void AppendPaletteDoorCommand( std::vector<PaletteCommand>& commands );
        void AppendSceneCommands( std::vector<PaletteCommand>& commands );
        void AppendSceneTailCommands( std::vector<PaletteCommand>& commands );
        void AppendWindowCommands( std::vector<PaletteCommand>& commands );

        // Ctrl+P "go to anything": draws the overlay over the dictionary above. No-op unless open.
        void DrawCommandPalette();
        // The palette asked for BY NAME, from its own dictionary — the only way an unattended run can put
        // it on screen, since a keystroke is not available here. Deferred rather than opened in the
        // closure: Draw() closes the palette on the line after it runs an entry, so opening it from inside
        // itself would work over the socket and do nothing under a person's hand.
        bool m_OpenPaletteRequested = false;

        // After an unclean exit, offers to reopen the newest autosave. No-op unless one was found.
        void DrawRecoveryPopup();

        // Modal for naming + saving the current docking layout (opened from View -> Layouts).
        void DrawLayoutSavePopup();

        // Builds a ready-to-Play demo: a WASD character (Jolt CharacterVirtual) with a 3rd-person child
        // camera, a ground floor, a sun light, and obstacles. (Remove the call in OnAttach for a blank scene.)
        // Builds a walkable greybox house (walls + doorway + roof, static colliders) parented under one root.

        // ===== Popups =====
        void DrawPopups();
        void DrawProjectPopup();
        void FollowImGuiWithEvents();

        // The one navigation `run Browse <folder>` and a field's "Show in browser" share.
        Common::BoolResultStr ShowFolderInBrowser( const std::string& folder );
        // Leaving the editor from its own frame's x or File > Exit: every dirty document asks first, and the
        // editor closes once the last one is answered. Cancel on any of them keeps the editor open. The
        // control channel's `quit` does not come here — an unattended run has nobody to answer.
        void RequestEditorExit();

        // Runs one render frame for a scene (outline aid + Begin/RegistryRender/OnUpdate/End). Called for
        // every open document each frame so all viewports stay live.
        Common::BoolResultStr UpdateSceneFrame( Desert::Core::Scene& scene, Render::RenderRegistry* registry,
                                                const Common::Timestep& ts );

        // Startup content is DATA, not code — these build entities into m_Workspace.ActiveScene() so the result
        // can be serialized to a .desce ONCE and loaded like any scene afterwards.

    private:
        Engine::Application* m_Application;

        // The window frame the OS no longer draws, because the editor asked for a window without one
        // (Sandbox.hpp: ApplicationInfo::Decorated). Held as an optional rather than a value because it
        // binds a reference to the Application's window, which does not exist at construction time — and
        // it stays EMPTY when the window is decorated, which is what keeps "the editor draws the frame"
        // and "the OS draws the frame" one code path with one condition instead of two builds.
        std::optional<UI::WindowChrome> m_WindowChrome;
        // The last title pushed to the window is NOT stored here: Window::GetTitle owns it, and
        // SyncWindowTitle compares against that. See Window.hpp.
        void SyncWindowTitle();

        std::shared_ptr<Assets::AssetManager> m_AssetManager;
        // The library the boot's "Indexing animation clips" stage fills (Assets::IndexAnimationClips).
        std::unique_ptr<Animation::AnimationLibrary> m_AnimationLibrary;
        std::unique_ptr<ImportManager>               m_ImportManager;
        // The mesh cook after the reveal and the .demat/.shader live reload (UE: FAssetCompilingManager). See
        // Editor/LevelEditor/AssetCompiling.hpp.
        AssetCompiling m_AssetCompiling{ m_AssetManager, m_AnimationLibrary, m_ImportManager };

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
        // The window (tool panel or document) to bring to the front of its dock next frame; ONE slot for both.
        std::string  m_FocusPanel;
        DocumentHost m_Documents{ m_Workspace, m_AssetManager, m_FocusPanel,
                                  [this]( const std::string& folder ) { return ShowFolderInBrowser( folder ); } };

        std::shared_ptr<ImGui::ImGuiLayer> m_ImGuiLayer;
        // THE TOOLS. A container that cannot hold a document — see Editor/Core/PanelRegistry.hpp. That is
        // what makes "the View menu lists exactly the tools" true by construction rather than by a predicate
        // the menu, the command palette and --open-panel would each have had to remember.
        PanelRegistry m_Panels;
        std::optional<Common::SubsystemCollection<EditorLayer>> m_Subsystems;

        // `m_ContextualShown` STOOD HERE — a set of raw panel pointers, inserted and erased in five
        // places and QUERIED IN NONE. Its own comment claimed it was what stopped a panel the user opened
        // by hand from being auto-closed; that is actually done by `IPanel::Pinned()`, which every branch
        // of UpdateContextualPanels already tests. So the set was write-only state, and one that went
        // dangling wholesale at `m_Panels.Clear()`. Removed with its five writes (A8-2), which is the same
        // decision this task took on `CloudNoiseService::GetGeneration` and `InstancesDirty`.
        //
        // The panel to bring to the front of its dock this frame IS read, and stays: it lives above
        // m_Documents, which shares this one slot for the document windows.

        // "Maximize panel" / "Restore panel": the one panel lifted out of its dock, and the node it came from.
        PanelMaximize m_PanelMaximize;

        CommandPalette m_CommandPalette;
        // Every palette provider, in palette order; registered in OnAttach.
        CommandRegistry m_Commands;

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
               .RequestExit = [this] { RequestEditorExit(); },
               .SaveLayoutAs =
                    [this]
               {
                   m_LayoutNameBuf[0]    = '\0';
                   m_ShowSaveLayoutPopup = true;
               },
               .ResetLayout = [this] { m_ResetDefaultLayout = true; } } };
        // The strip below the menu bar and the title bar's project / level sections (UE: SLevelEditorToolBar).
        // See Editor/LevelEditor/LevelToolbar.hpp.
        LevelToolbar m_Toolbar{ m_Workspace, m_SceneFiles, m_Play, m_Preferences, m_Profiler.Shown() };
        // The bottom strip (UE: SStatusBar); the bottom drawer's chevron stays with the dock layout and arrives
        // as an action. See Editor/LevelEditor/StatusBar.hpp.
        StatusBar m_StatusBar{ m_Workspace, m_SceneFiles, m_Documents, m_AssetCompiling.CookQueue(),
                               [this] { DrawBottomDrawerToggle(); } };
        // Headless capture: `--shot`, `--play`, `--camera`/`--look` (UE: the automation screenshot director). See
        // Editor/LevelEditor/ShotDirector.hpp.
        ShotDirector m_Shots{ m_Workspace, m_SceneFiles, m_Play, m_Capture };
        // The control channel (UE: Remote Control), after every module it reads. See
        // Editor/LevelEditor/ControlService.hpp.
        ControlService m_Control{ m_Workspace, m_SceneFiles, m_Play,    m_Documents,
                                  m_Capture,   m_Panels,     m_Commands };

        // Crash recovery: set at startup when the previous session crashed and an autosave was found.
        bool                  m_ShowRecoveryPrompt = false;
        std::filesystem::path m_RecoveryAutosave;

        // Saveable layouts: pending "reset to default docking" and the save-layout modal state.
        bool m_ResetDefaultLayout  = false;
        bool m_ShowSaveLayoutPopup = false;

        // Bottom drawer (Assets / Logs / Shader Code). Collapsing SHRINKS the dock node to its tab bar
        // instead of closing the panels: a closed panel has to be rediscovered from a menu, a collapsed
        // one is still right there. m_BottomHeight remembers the expanded size across toggles.
        ImGuiID                                 m_BottomDockId    = 0;
        bool                                    m_BottomCollapsed = false;
        float                                   m_BottomHeight    = 0.0f;
        void                                    DrawBottomDrawerToggle();
        char                                    m_LayoutNameBuf[64] = {};

        // Set by the first OnUIRender that draws the editor rather than a loading frame.
        bool m_RealFrameDrawn = false;
        // The staged boot, the splash and its hand-over, the content settle (UE: FEditorLoadingScreen), after every
        // module it reads; built in the constructor, which receives the splash. See Editor/LevelEditor/EditorStartup.hpp.
        EditorStartup m_Startup;

        // The palette providers that hold state or several slots (EDL-2b). Declared after every slot they point
        // at; the census is taken once per build (m_Commands.OnBuildBegin) and read by Assets, Foliage and Open.
        AssetFileCensus m_PaletteAssetFiles;
        std::unique_ptr<EntityCommands> m_EntityCommands;
        std::unique_ptr<AssetCommands>  m_AssetCommands;
    };
} // namespace Desert::Editor