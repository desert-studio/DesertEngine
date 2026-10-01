#pragma once

#include <Common/Core/Subsystems/SubsystemCollection.hpp>
#include <Editor/Core/PanelMaximize.hpp>
#include <Editor/Import/BackgroundCook.hpp>
#include <Engine/Assets/ContentGate.hpp>

#include <Engine/Core/BootTimeline.hpp>
#include <Engine/Core/WorldStreamer.hpp>
#include <Engine/Desert.hpp>
#include <Engine/Assets/AsyncAssetLoader.hpp>
#include <Engine/Runtime/AssetHotReload.hpp>
#include <ImGui/imgui.h>
#include "Editor/ImGuiIntegration/ImGuiLayer.hpp"
#include "Editor/Widgets/UIHelper/ImGuiUI.hpp"
#include "Editor/Panels/IPanel.hpp"
#include "Editor/Core/CommandPalette.hpp"
#include "Editor/Core/Commands/CommandRegistry.hpp"
#include "Editor/Core/Selection/EntityCommands.hpp"
#include "Editor/Panels/FileExplorer/AssetCommands.hpp"
#include "Editor/Core/PlayWorldCommands.hpp"
#include "Editor/Core/Control/ControlPipeline.hpp"
#include "Editor/Core/Control/ControlProtocol.hpp"
#include "Editor/Core/Control/ControlSocket.hpp"
#include "Editor/Core/Control/ControlState.hpp"
#include "Editor/Core/SceneViewIdentity.hpp"
#include "Editor/Core/Selection/AuthoringContext.hpp"
#include "Editor/Core/Selection/SelectionTransformProperties.hpp"
#include "Editor/Core/FlightRules.hpp"
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
#include "Editor/Widgets/WindowChrome.hpp"
#include "Editor/Splash/RevealGate.hpp"
#include "Editor/Splash/SplashScreen.hpp"

#include <Engine/Assets/ItemProgress.hpp>

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
        // frame back off the swapchain. See ServiceControlChannel.
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

        // ===== Control channel (Editor/Core/Control) =====
        // Drained at the TOP of OnUpdate: accept, read one request, execute it. Everything it can run is
        // a palette entry.
        void ServiceControlChannel();
        // Executes one request and decides whether its reply leaves now or waits for the frame that proves
        // it. THE ONLY place a control request is run: there are two ways to arrive at one — read off the
        // socket, or released by the readiness gate several frames later — and one way to run it.
        void RunControlRequest( const Control::Request& request );
        // Drop an in-flight request whose CONNECTION has gone, rather than answering its successor. True
        // when it did. See the definition: a reply of 311 commands was measured reaching the wrong client.
        [[nodiscard]] bool AbandonControlRequestIfItsAskerIsGone();
        // Sampled after the deferred queues have drained and BEFORE the scene is rendered — "was anything
        // outstanding while this frame was being made". Judged later, by the gate, at OnFramePresented.
        // Also called once at the end of OnAttach: an unsampled census must not read as a settled editor.
        void SampleFrameQuiescence();
        // Runs one request against the live editor. Never throws, always answers.
        [[nodiscard]] Control::Response ExecuteControlRequest( const Control::Request& request );
        // The `set` for the channel's second subject — the editor's own view. See
        // Editor/Core/ViewportCameraProperties.hpp for why a camera pose is a property write and not a
        // palette command.
        [[nodiscard]] Control::Response SetViewportCameraProperty( const Control::Request& request );
        // The `selection` subject's entity and its transform: exactly one selected entity that has a
        // TransformComponent, or a refusal saying what is selected instead.
        [[nodiscard]] Common::ResultStr<std::pair<Common::UUID, Core::SelectionTransform>>
        SelectedTransform() const;
        // The active view IF it is the editor's fly camera; null in Play, where the scene's own
        // CameraComponent drives. NoEditorCameraReason() is the refusal that goes with the null.
        [[nodiscard]] ::Desert::Core::EditorCamera* ActiveEditorCamera() const;
        [[nodiscard]] std::string                   NoEditorCameraReason() const;
        // THE ONE PLACEMENT: `--camera`/`--look` and the control channel both land here, through the
        // editor's own view-axis-gizmo and F-focus gestures. Two copies would drift.
        static void PlaceEditorCamera( ::Desert::Core::EditorCamera& camera, const glm::vec3& position,
                                       const glm::vec3& forward );
        // Everything ControlState needs, read off this layer in one pass.
        [[nodiscard]] Control::EditorSnapshot TakeEditorSnapshot() const;
        // CAPTURING THE COMPOSITED FRAME, in two halves, because a swapchain image may only be touched
        // between its acquire and its present.
        //
        // Recorded at the end of OnUIRender, while the frame is still being built and the image is
        // legitimately ours; collected in OnFramePresented, once the present that carried the copy has
        // gone out. Doing it all after the present produced a correct picture and a Vulkan spec violation
        // that only the validation layer mentioned — see RecordWindowCaptureIfDue.
        void RecordWindowCaptureIfDue();

        // After an unclean exit, offers to reopen the newest autosave. No-op unless one was found.
        void DrawRecoveryPopup();

        // Modal for naming + saving the current docking layout (opened from View -> Layouts).
        void DrawLayoutSavePopup();

        // Builds a ready-to-Play demo: a WASD character (Jolt CharacterVirtual) with a 3rd-person child
        // camera, a ground floor, a sun light, and obstacles. (Remove the call in OnAttach for a blank scene.)
        void BuildCharacterDemoScene();
        // Builds a walkable greybox house (walls + doorway + roof, static colliders) parented under one root.
        void BuildHouse( const glm::vec3& origin );

        /// @p rightMargin is how much of the bar's right-hand end is already spoken for — the window
        /// buttons — so the stats right-align against them instead of underneath them.
        void DrawEngineStats( float rightMargin );
        void DrawProfilerWindow();
        /// The profiler's CPU+GPU table as log lines — the panel's button and --gpu-profile share it.
        void DumpProfilerToLog();
        /// --flight: times the previous frame's row and appends this frame's (Editor/Core/FlightRules.hpp).
        /// @p counted is whether the capture counts this frame; an uncounted one is a Settling row.
        void RecordFlightFrame( bool counted );
        /// --flight, on the last frame: writes the CSV and logs the summary. False when either failed.
        [[nodiscard]] bool FinishFlight();

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
        void BuildStarterScene();    // fresh Hub project's DefaultScene: sun/ground/cube/light/camera
        void BuildCornellShowcase(); // sandbox demo: baked into CornellDemo.desce on first launch

        // Force re-cook of Cooked/ from sources, re-register cooked assets, refresh the asset panel.
        void RebuildCookedAssets();

    private:
        bool m_ShowProfiler = true; // View ▸ Profiler toggles the profiler window

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
        // The startup mesh cook, run after the reveal (AL1-11); see Editor/Import/BackgroundCook.hpp.
        std::unique_ptr<BackgroundCookQueue>         m_BackgroundCook;
        std::chrono::steady_clock::time_point        m_BackgroundCookStart;
        std::size_t                                  m_BackgroundCookChanged  = 0;
        std::size_t                                  m_BackgroundCookFailed   = 0;
        bool                                         m_BackgroundCookReported = false;
        Runtime::AssetHotReload                      m_AssetHotReload; // .demat/.shader live reload

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
        // File / Edit / View / Window / Scenes / Graphics / About and the menu's palette entries (UE:
        // FLevelEditorMenu). The layout's two entries and the editor's exit stay with their owners and arrive as
        // actions.
        MainMenu m_MainMenu{ m_Workspace,
                             m_SceneFiles,
                             m_Documents,
                             m_Panels,
                             m_Preferences,
                             m_ShowProfiler,
                             { .RebuildCookedAssets = [this] { RebuildCookedAssets(); },
                               .RequestExit         = [this] { RequestEditorExit(); },
                               .SaveLayoutAs =
                                    [this]
                               {
                                   m_LayoutNameBuf[0]    = '\0';
                                   m_ShowSaveLayoutPopup = true;
                               },
                               .ResetLayout = [this] { m_ResetDefaultLayout = true; } } };
        // The strip below the menu bar and the title bar's project / level sections (UE: SLevelEditorToolBar).
        // See Editor/LevelEditor/LevelToolbar.hpp.
        LevelToolbar m_Toolbar{ m_Workspace, m_SceneFiles, m_Play, m_Preferences, m_ShowProfiler };
        // The bottom strip (UE: SStatusBar); the bottom drawer's chevron stays with the dock layout and arrives
        // as an action. See Editor/LevelEditor/StatusBar.hpp.
        StatusBar m_StatusBar{ m_Workspace, m_SceneFiles, m_Documents, m_BackgroundCook,
                               [this] { DrawBottomDrawerToggle(); } };

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

        // Staged startup loading: the heavy boot work (mesh cooking, asset preload) runs one stage per
        // frame from OnUpdate, each announced on the splash, with the main window still hidden.
        struct StartupStage
        {
            std::string           Label;
            std::function<void()> Run;
            // What one item of this stage costs on the splash's bar, in measured seconds per item, and how
            // many items there are — asked when the plan is made, before any stage runs. Empty = one item.
            double                       SecondsPerItem = 0.01;
            std::function<std::size_t()> CountItems;
            // Instead of CountItems, for a stage whose items do not cost alike: each item's own cost, in
            // the order the stage works through them.
            std::function<std::vector<double>()> ItemCosts;
            std::size_t                          ProgressStage = 0; // its id in m_Progress
        };
        std::vector<StartupStage> m_StartupStages;
        size_t                    m_StartupNext = 0;

        // ===== The splash's progress, and the moment the editor is shown =====
        //
        // THE BAR IS WEIGHED IN WORK (Splash/SplashProgress.hpp): the engine shader compile (OnAttach — not a
        // stage, because the render systems resolve their shaders in their constructors), every entry of
        // m_StartupStages and the settle wait after them, each weighted by its item count times a measured
        // cost per item. The plan is made once the cooked registry is read, which is what counts the items.
        void MakeSplashPlan();
        void BeginSplashStage( std::size_t stage, std::optional<std::size_t> items = std::nullopt );
        void PushSplash();
        // The item line of the stage running now, for an engine call that works through a list.
        Assets::ItemProgress                  SplashItems();
        Splash::ProgressModel                 m_Progress;
        std::chrono::steady_clock::time_point m_ProgressEpoch = std::chrono::steady_clock::now();
        std::size_t                           m_ShaderStage   = 0;
        std::size_t                           m_SettleStage   = 0;
        // Scene loads already finished when the settle began: the settle counts only the rest.
        std::size_t m_SettleBase = 0;
        // Every condition the splash hand-over depends on, read off this layer for Splash::MayReveal.
        Splash::RevealState CurrentRevealState() const;
        // Called at every presented frame; the first one presented after the start is over shows the
        // hidden main window and closes the splash. Until then the splash is the only window.
        void RevealWhenReady();
        // THUMB2: before the hand-over, upload the opening folder's cached thumbnails as workers finish
        // them, and hold the hand-over until they are all up (no time bound, THM1n).
        void UploadSplashThumbnails();
        bool m_ThumbnailsHoldReveal = false;
        // THUMB3: the open scene's materials — their cached pictures decoded, the missing ones captured on the
        // splash (Splash::SceneThumbnailCaptureAllowed) within Splash::kSceneCaptureBudgetMs.
        void        WarmSplashScene();
        bool        m_SplashWarmStarted = false;
        std::size_t m_SplashWarmTotal   = 0; // captures queued when the warm-up started
        std::size_t m_SplashWarmShown   = 0; // what the splash line last said was left
        bool m_SplashPicturesReasked    = false; // the captures landed and their PNGs were asked for (THM1n-13)
        // When every other reveal condition first held: the start of the thumbnails' budget.
        std::optional<std::chrono::steady_clock::time_point> m_RevealOtherwiseReadySince;
        void StartBackgroundCook();
        void DrainBackgroundCook();
        void ReloadRecookedMesh( const std::filesystem::path& source );
        // KEPT after it is closed, until the layer goes: Close() only starts the crossfade, and the
        // object's destructor is what waits for its window and thread — at teardown, not on the frame
        // the editor has just appeared on.
        std::unique_ptr<Splash::SplashScreen> m_Splash;
        bool                                  m_Revealed = false;
        // The pending count the splash last showed during the settle, so the label is pushed on change only.
        size_t m_SplashOutstandingShown = SIZE_MAX;
        // The splash's close was acted on (Application::Close asked once, not every frame until it lands).
        bool m_QuitFromSplash = false;
        // Set by the first OnUIRender that draws the editor rather than a loading frame.
        bool m_RealFrameDrawn = false;
        // WHERE THE ELAPSED TOTAL LIVES NOW. It used to be a `long long` accumulated here with the
        // accumulation rule written in this comment; the rule (sum of the stages, NOT wall clock between
        // the first and the last, because a stage runs one per frame) now lives in `Core::BootTimeline`
        // alongside the line format, so that the shipping runtime's boot numbers and this one mean the
        // same thing. Nothing about the per-frame scheduler above moved.
        ::Desert::Core::BootTimeline m_Boot{ "Editor" };
        bool                         StartupLoading() const
        {
            return m_StartupNext < m_StartupStages.size();
        }

        // ===== Demand-driven content: the wait that replaced the eager preload =====
        //
        // WHY THERE IS A SECOND KIND OF "STILL LOADING". The cloud kinds are no longer read at boot; they
        // are read when the scene that wants them says so, on `JobSystem` workers, and `AssetRef` is what
        // makes "not here yet" a state a consumer can branch on. That removes the boot cost (measured:
        // 1312.7 ms of a 5707.0 ms boot for the noise volumes alone) but it introduces a question the
        // eager model never had to answer: what does the editor SHOW while the read is in flight?
        //
        // The answer is not "the scene without its clouds". A sky that appears several frames after the
        // rest of the world is exactly the hitch GAP_ANALYSIS §3.1 warns the lazy model moves into the
        // frame -- it is not a stall, but it is a visible change, and shipping it would be trading a
        // measurable boot cost for an unmeasurable visual one. So the splash that was already up for the
        // staged boot stays up until the content the scene asked for has settled, and the cost
        // stays in the loading screen where it was.
        //
        // THE RULE ITSELF IS NOT HERE ANY MORE. It was three fields and two methods in this class, and
        // the shipping runtime held a hand-copied half of it -- a marker that logged the same condition
        // and had no state, so the frames it described were presented anyway. One implementation, both
        // hosts, and the two conditions can be tested without a device: Engine/Assets/ContentGate.hpp.
        //
        // `Ready` at construction is correct FOR THIS HOST only: the editor opens on an empty scene
        // behind its own staged-boot overlay, and the first scene load calls BeginWorld. The runtime
        // constructs its gate `Loading`.
        Assets::ContentGate m_Content{ Assets::ContentState::Ready };

        bool ContentSettling() const
        {
            return m_Content.Loading();
        }

        /// A scene has just loaded; whatever it asks for has not been asked for yet. Starts the wait.
        void BeginContentSettle();
        /// One tick of the wait: decides whether the frame just rendered closed the chain.
        void UpdateContentSettling();

        // Screenshot mode counters (see Editor/Core/ShotOptions.hpp).
        int  m_ShotFrame        = 0;
        bool m_ShotCameraPlaced = false;
        // Set when any PNG of this capture could not be written; becomes the process exit status.
        bool m_ShotFailed = false;
        // --flight: one row per frame of Play, written as the CSV when the capture ends.
        Flight::FlightLog m_FlightLog;

        // ===== Control channel =====
        // Present only when `--control-socket` named one; silent otherwise. See
        // Editor/Core/Control/ControlChannelOptions.hpp for why an editor does not listen by default.
        Control::ControlSocket m_ControlSocket;

        // The gate that makes "command -> frame -> snapshot" a property rather than a coincidence. Armed
        // when a request executes; discharged by the first PRESENTED frame that was rendered with nothing
        // outstanding. Editor/Core/Control/ControlPipeline.hpp has the argument.
        Control::FrameGate m_ControlGate;

        // The request whose reply the gate is holding, and the reply itself. Held together because they
        // are one thing: a reply parked without its request could not say what it was answering, and a
        // request parked without its reply would have to be re-run to produce one.
        std::optional<Control::Request>  m_ControlInFlight;
        std::optional<Control::Response> m_ControlPendingReply;

        // WHICH CONNECTION asked for it. Not "was somebody connected": the editor notices a disconnect and
        // accepts the next client in the SAME service call, so a request parked across that gap would have
        // its reply written to a stranger. Measured — a 311-command answer delivered to the wrong client,
        // with an id that matched because both had sent 1.
        uint64_t m_ControlInFlightClient = 0;

        // The outstanding work sampled while THIS frame was being built. Not read at the moment the gate
        // judges it: by then the answer has moved on, and the question is about the picture.
        Control::EditorQuiescence m_FrameQuiescence;

        // The palette providers that hold state or several slots (EDL-2b). Declared after every slot they point
        // at; the census is taken once per build (m_Commands.OnBuildBegin) and read by Assets, Foliage and Open.
        AssetFileCensus m_PaletteAssetFiles;
        std::unique_ptr<EntityCommands> m_EntityCommands;
        std::unique_ptr<AssetCommands>  m_AssetCommands;

        // Frames since the layer attached. The gate's clock — deliberately this layer's own count and not
        // the renderer's frame-in-flight index, which wraps at three and could not order anything.
        uint64_t m_FrameIndex = 0;

        // A `quit` the channel asked for. Honoured after its reply has actually gone out, so the last
        // answer is not lost to the exit — a client that never hears "ok" cannot tell a clean shutdown
        // from a crash.
        std::optional<int32_t> m_ControlQuitCode;
    };
} // namespace Desert::Editor