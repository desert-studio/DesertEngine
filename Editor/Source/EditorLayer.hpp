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
#include "Editor/Core/SubjectEditorRegistry.hpp"
#include "Editor/Core/DocumentWell.hpp"
#include "Editor/Core/DocumentPlacement.hpp"
#include "Editor/Core/FlightRules.hpp"
#include "Editor/Core/PanelRegistry.hpp"
#include "Editor/RenderSystems/RenderRigistry.hpp"
#include "Editor/LevelEditor/PlaySession.hpp"
#include "Editor/LevelEditor/SceneWorkspace.hpp"
#include "Editor/LevelEditor/SceneFiles.hpp"
#include "Editor/LevelEditor/ViewportCapture.hpp"
#include "Editor/Widgets/ToolbarLayout.hpp"
#include "Editor/Widgets/WindowChrome.hpp"
#include "Editor/Splash/RevealGate.hpp"
#include "Editor/Splash/SplashScreen.hpp"
#include "Editor/Core/UnsavedClose.hpp"

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

        // ===== Menus =====
        void DrawFileMenu();
        void DrawEditMenu();
        void DrawViewMenu();
        // Window ▸ Documents: the open documents, focused with a RADIO and closed with an x. A radio and
        // not a checkbox on purpose — a tick reads as "shown / hidden", which is the very thing a document
        // cannot be. See DocumentWell.
        void DrawWindowMenu();
        void DrawGraphicsMenu();
        void DrawAboutMenu();

        // ===== Menu sections =====
        void DrawStyleSubmenu();
        void DrawOpenSceneMenuItem();
        void DrawPreferencesWindow(); // Edit -> Preferences... (persisted to ~/.desertengine/editor.json)

        // ===== Top Bar Sections =====
        void DrawProjectSection();
        void DrawSceneRenameSection();

        // UE5-style toolbar strip below the menu bar. Left: save + undo/redo, editor modes, transform
        // tools, the two snap steps. Centre: playback. Right: package, profiler, preferences. Drawn inside
        // the dockspace host window so it takes a fixed height above the docked panels.
        //
        // Every control here writes to state that already has one owner elsewhere (CommandHistory,
        // ViewportMode, GizmoState, Scene::GetState) — the bar reports and commands, it never stores.
        void DrawToolbar();
        // One toolbar button. `active` is the armed/on state: tinted fill plus a 2px underline.
        bool ToolbarButton( const char* icon, const char* label, bool active = false,
                            const char* tooltip = nullptr, bool enabled = true );
        void ToolbarSeparator();
        // A snap step: the button reports the current step and opens the list that changes it, with the
        // shared snapping toggle at the top. `rotation` picks the angle step over the grid step.
        void DrawSnapControl( bool rotation );

        // Bottom status bar: scene state (Edit/Play), scene name, current selection, and FPS/frame time.
        void DrawStatusBar();
        // Triangles drawn by the scene's meshes, summed over entities. Cached — see m_TriangleCache.
        uint64_t SceneTriangleCount();
        // The status bar's console line (UE's "Enter Console Command"); handed to the Lua console to run.
        char m_StatusCmd[256] = {};

        // Triangle census for the status bar. Walking every entity's submeshes each frame is cheap on a
        // 24-entity scene and is not on a large one, so the answer is cached and recomputed on the two
        // things that can change it: an edit (the revision moves) and an entity appearing or vanishing.
        // A mesh finishing an ASYNC load bumps neither, so the cache also has a frame budget — a count
        // that is three seconds stale is a status bar; a count that is permanently wrong is a lie.
        uint64_t m_TriangleCache      = 0;
        uint64_t m_TriangleCacheRev   = static_cast<uint64_t>( -1 );
        size_t   m_TriangleCacheCount = static_cast<size_t>( -1 );
        int      m_TriangleCacheAge   = 0;
        // Opens/closes panels whose context appeared or vanished (see IPanel::IsContextual).
        void UpdateContextualPanels();

        // The palette's and the control channel's list, built from m_Commands (see CommandRegistry.hpp).
        [[nodiscard]] std::vector<PaletteCommand> BuildPaletteCommands();
        // The palette groups of modules not cut out yet (panels, documents, menu, add shape, the palette's own
        // door, open, scenes/views, save/play/undo/window): registered in palette order between the subject-owned
        // providers; the later EDL cuts move each to its module (DockLayout, DocumentHost, MainMenu, SceneFiles...).
        void AppendPanelCommands( std::vector<PaletteCommand>& commands );
        void AppendMaximizeCommands( std::vector<PaletteCommand>& commands );
        void AppendDocumentCommands( std::vector<PaletteCommand>& commands );
        void AppendMenuCommands( std::vector<PaletteCommand>& commands );
        void AppendAddShapeCommands( std::vector<PaletteCommand>& commands );
        void AppendPaletteDoorCommand( std::vector<PaletteCommand>& commands );
        void AppendOpenCommands( std::vector<PaletteCommand>& commands );
        void AppendSceneCommands( std::vector<PaletteCommand>& commands );
        void AppendSceneTailCommands( std::vector<PaletteCommand>& commands );
        void AppendWindowCommands( std::vector<PaletteCommand>& commands );

        // Runs one action a document published (ISubjectDocument::Actions), addressed by subject + label.
        // Named rather than a lambda in the list above — see the definition for both reasons.
        [[nodiscard]] Common::BoolResultStr RunDocumentAction( const SubjectId&   subject,
                                                               const std::string& label );

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
        // Destroys every document the user asked to close, behind ONE device-idle wait. This is what returns
        // the document's Scene, SceneRenderer and renderer slot.
        //
        // The request comes from m_DocumentsToClose, filled by the x on the window, the x in the Documents
        // menu or Close All — never from a visibility flag. That is the point of the split: a tool's
        // visibility is a setting the user keeps, and while documents shared the panel list they shared that
        // bool too, so unticking one in the View menu DESTROYED it and re-ticking could not bring it back.
        void ServiceDocumentCloses();
        // Asks for a document to be closed. Queued, never immediate: closing destroys GPU resources, which
        // is not legal from inside the ImGui pass that is drawing them.
        // @p reason is why, in the user's words, and it is REQUIRED. A document now closes for three
        // different causes — the user dismissed it, Close All, or its subject stopped existing — and a log
        // line that could not tell them apart would make "my window vanished" unanswerable.
        void RequestDocumentClose( const SubjectId& subject, std::string reason );
        // A close THE PERSON asked for. A dirty document is not queued: it gets the Save / Don't Save / Cancel
        // question (Editor/Core/UnsavedClose.hpp) and closes only on the answer. Every user gesture that
        // closes a document goes through here; RequestDocumentClose stays for closes the editor makes on its
        // own (the subject is gone, view memory is short), which have nobody to ask.
        void AskDocumentClose( const SubjectId& subject, std::string reason );
        // The answer to a pending close question for @p subject. Returns whether the document is now queued
        // for closing; false for Cancel, for a Save that wrote nothing, and when no question was pending.
        bool AnswerCloseQuestion( const SubjectId& subject, UnsavedCloseChoice choice );
        // Leaving the editor from its own frame's x or File > Exit: every dirty document asks first, and the
        // editor closes once the last one is answered. Cancel on any of them keeps the editor open. The
        // control channel's `quit` does not come here — an unattended run has nobody to answer.
        void RequestEditorExit();
        void DrawCloseQuestionPopup();
        // Every open document, queued for closing. One implementation behind Window ▸ Close All Documents
        // and behind the palette entry of the same name — the menu item used to carry the loop itself, and
        // a second copy of it in the palette would be two answers to "what does Close All close".
        void RequestCloseAllDocuments();

        // ── A DOCUMENT CLOSES WITH ITS SUBJECT ────────────────────────────────────────────────────────
        //
        // Queues a close for every open document whose IsSubjectAlive() has gone false — the entity was
        // deleted, the component removed, the asset dropped from the manager, the scene closed. The owner
        // ruled out closing on focus loss (a layout that moves itself reads as a lost panel) and ruled IN
        // closing with the subject, because the alternative is a window editing nothing.
        //
        // A SWEEP AND NOT A SUBSCRIPTION: there is no single event that covers all four ways a subject can
        // die, and a subscription to one of them would make the other three look handled.
        void CloseDocumentsWhoseSubjectIsGone();

        // Hands the renderer slot back for every document whose window has been undrawn for
        // kFramesHiddenBeforeSlotRelease frames. Called from ServiceDocumentCloses so it shares that
        // function's device-idle wait — see ISubjectDocument::ReleaseView.
        void ReleaseSlotsOfHiddenDocuments();

        // The label a component-subject document is named with: the entity's tag plus what the window is
        // about ("Hero · Anim Graph"). Resolved ONCE, when the document is built — a document must not
        // need a scene to know its own name, and renaming the entity must not open a second window.
        [[nodiscard]] std::string SubjectEntityName( const SubjectId& subject, const char* what ) const;

        // The name to put in a REFUSAL dialog for a subject no document was built for: the asset's file
        // stem, or the entity's tag. "this subject" when neither resolves — the refusal happens before any
        // editor is consulted, so this is all that is knowable about it.
        [[nodiscard]] std::string RefusedSubjectName( const SubjectId& subject ) const;

        // Does the entity @p owner in the ACTIVE scene carry component T? The presence test every
        // component-subject registration is built from (SubjectEditorRegistry::Registration::Exists) —
        // written once, templated, because five copies of the selection-to-entity-to-component dance is
        // how one of them comes to be missing the null check.
        template <typename ComponentT>
        [[nodiscard]] bool EntityHasComponent( const Common::UUID& owner ) const
        {
            if ( !m_Workspace.ActiveScene() || owner.IsNull() )
                return false;
            const auto entOpt = m_Workspace.ActiveScene()->FindEntityByID( owner );
            return entOpt && entOpt->get().HasComponent<ComponentT>();
        }
        // Brings @p subject's window to the front and makes it the most recently used document.
        void FocusDocument( const SubjectId& subject );
        // Ctrl+Tab: move to the next document in most-recently-used order. See DocumentWell::NextMostRecent.
        void CycleDocuments();

        // ===== The document well (layout option B.1) =====
        // The "Documents" window: the tab the documents dock beside, the index of what is open, and — when
        // nothing is open — the empty state that says what the area is for plus the list of recently closed
        // documents. It closes with its x like a tool (its neighbours take the area), comes back from
        // Window > Documents or by itself when a document is opened, and its open/closed state is a line of
        // the editor layout (DocumentWell::LayoutLine), so it survives a restart.
        // Closing the window closes no document: they stay open and the well shows the same tabs on return.
        // The window title one document is drawn with — its type's icon (from the registration), its
        // subject's name, and the identity DocumentTitle baked into GetName(). A member rather than a free
        // function because the icon comes from m_SubjectEditors.
        [[nodiscard]] std::string DocumentDisplayTitle( const ISubjectDocument& document ) const;

        void DrawDocumentWell();
        // Every open document, drawn into the well's dock node. Separate from the tool loop because the two
        // have separate owners and separate close semantics — a tool passes &GetVisibility() to Begin, a
        // document passes a frame-local bool whose false is a CLOSE REQUEST, not a hidden window.
        void DrawDocuments();
        // UE's major tabs: "Scene" plus one tab per open document that OpensAsMajorTab(); the one in front
        // owns the whole dock area and the level's panels are not drawn.
        void               DrawMajorTabStrip();
        [[nodiscard]] bool MajorTabActive() const
        {
            return !m_ActiveMajorTab.IsNull();
        }
        // The refusal, on screen. A seventh renderer consumer is refused; before this the refusal was a
        // line in the log and the click simply looked dead. The census text already existed — it had
        // nowhere to be shown.
        void DrawOpenRefusedPopup();

        // Who is holding a view right now, by name. Shown when an open is refused — "out of memory"
        // without the list leaves the user with nothing to close. The main viewport and every extra scene
        // view hold one for as long as they exist; the Details preview and each asset document are
        // demand-driven and may be open while holding nothing.
        struct ViewConsumer
        {
            std::string Name;
            bool        HoldsView = false;
            // Whether this consumer will ever build a view. A CPU-only asset document (the four cloud
            // editors) holds none and is not waiting for one, and the census has to say so — "will allocate
            // when it draws" would name it as something to close to free memory it was never going to take.
            // See ISubjectDocument::ClaimsView.
            bool ClaimsView = true;
            // What a document that claims a view but has not built it yet will allocate on its first frame
            // (ISubjectDocument::ViewForecastBytes). Zero for everything else: a view that exists is counted
            // by what it HOLDS (SceneRenderer::LiveHoldings), not by a forecast.
            uint64_t ForecastBytes = 0;
            // Set for a consumer the user can close FROM THE REFUSAL ITSELF: an open document. The main
            // viewport and the Details preview carry no handle — neither is a window a person closes to make
            // room. Last in the struct so the shorter aggregate initialisations below keep meaning what they say.
            std::optional<SubjectId> Document = {};
        };
        [[nodiscard]] std::vector<ViewConsumer> ViewCensus() const;
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
        SceneFiles      m_SceneFiles{ m_Workspace, m_AssetManager, m_Capture };

        // AssetTypeID -> the editor that opens it. Holds factories only; the documents it builds are owned by
        // m_OpenDocuments below.
        SubjectEditorRegistry m_SubjectEditors;

        // THE OPEN DOCUMENTS, owned separately from the tools. See Editor/Core/OpenDocuments.hpp for the
        // whole argument; the short version is that a tool's visibility is a setting and a document's
        // existence is not, so one bool cannot serve both — and while they shared m_Panels it had to.
        //
        // DECLARED BEFORE THE VIEWS THAT READ IT, and the order is load-bearing rather than tidy: every view
        // below is constructed with a reference to this member.
        OpenDocuments m_OpenDocuments;
        // ONE VIEW OF THEM — the tabbed well, its Ctrl+Tab ring and its recently-closed list. The Clouds
        // window is a second view of the same container and neither knows the other exists.
        DocumentWell m_DocumentWell{ m_OpenDocuments };
        // Close requests, drained between frames by ServiceDocumentCloses. Filled by the x on a document
        // window, the x in Window ▸ Documents, Close All, and the refusal dialog's own Close buttons.
        struct PendingDocumentClose
        {
            SubjectId   Subject;
            std::string Reason;
        };
        std::vector<PendingDocumentClose> m_DocumentsToClose;
        // Close questions waiting for an answer, the front one shown as a modal. Same shape as the queue
        // above because an answer that closes moves the entry there unchanged.
        std::vector<PendingDocumentClose> m_CloseQuestions;
        // RequestEditorExit raised the questions: close the editor once the last one is answered.
        bool m_ExitAfterCloseQuestions = false;

        // How long a document must go UNDRAWN BY EVERY VIEW before it gives its renderer slot back. A
        // document behind another one's tab is open and invisible, and it was holding one of the six
        // renderer slots for as long as the user left it there — see ISubjectDocument::ReleaseView.
        //
        // COUNTED RATHER THAN ACTED ON AT ONCE. Dragging a dock tab, collapsing a node and switching layouts
        // all hide a window for a frame or two, and tearing a Scene and a SceneRenderer down and building
        // them back for that would turn a flick of the mouse into a hitch. The threshold is the smallest
        // number of frames that is unambiguously "the user left it there" rather than "the layout moved".
        //
        // THE COUNT ITSELF LIVES ON m_OpenDocuments, not here, and the move is the point: with the Clouds
        // window there are two views that can draw a document, so "nobody drew it" is a fact about all of
        // them and cannot be maintained by either one. See OpenDocuments::NoteDrawn / EndFrame.
        static constexpr uint32_t kFramesHiddenBeforeSlotRelease = 30;
        // Which document window has the keyboard focus, as of the last frame. Drives the radio in
        // Window ▸ Documents and is where Ctrl+Tab starts from.
        SubjectId m_FocusedDocument;
        // Whether a document window has the keyboard NOW. m_FocusedDocument outlives the focus on purpose, so
        // Ctrl+S needs this separately to decide between the document's asset and the scene (SaveShortcut.hpp).
        bool m_DocumentHasFocus = false;
        // Ctrl+Tab holds the ring still. Landing on a document by cycling must NOT reorder the ring, or the
        // second press would come straight back to where the first started; the order is committed once Ctrl
        // is released, which is the behaviour every alt-tab ring has.
        bool m_CyclingDocuments = false;
        // What the layout file last said about the well's window; a difference marks imgui.ini dirty.
        bool m_DocumentWellOpenInLayout = true;
        void RegisterDocumentWellLayoutHandler();

        // WHERE EACH DOCUMENT KIND WAS LAST PUT (Editor/Core/DocumentPlacement.hpp), keyed by DocumentKindKey —
        // domain and facet, stable across runs. Persisted in imgui.ini as [DocumentPlacement][Kinds], so a
        // named layout carries it too. The next opening of that kind goes back there.
        std::unordered_map<std::string, Editor::DocumentPlacement::Remembered> m_RememberedPlacement;
        // Documents whose opening has been placed. The placement is applied ONCE, on the first frame the
        // window exists; afterwards the window is the person's and is only observed.
        std::unordered_set<SubjectId> m_PlacedDocuments;
        SubjectId                     m_ActiveMajorTab; // null = the level ("Scene") is in front
        std::unordered_set<SubjectId> m_SeenMajorTabs;  // a tab not seen before comes to the front once
        glm::vec2                     m_MajorTabOrigin{ 0.0f };
        glm::vec2                     m_MajorTabSize{ 0.0f };
        void                          RegisterDocumentPlacementHandler();

        // A refused open, waiting to be shown (see DrawOpenRefusedPopup). Holds the census by value: the
        // documents it names may be closed while the dialog is up, and a row pointing at a destroyed panel
        // is the dangling reference this split exists to avoid.
        struct OpenRefusal
        {
            std::string                 AssetName;
            std::string                 TypeName;
            Engine::ViewBudget::Verdict Verdict;          // RequestBytes = the document's forecast + PendingBytes
            Engine::ViewBudget::Reading Reading;          // the ceiling, its source and the usage it was judged on
            uint64_t                    PendingBytes = 0; // spoken for by open documents that have not drawn yet
            std::vector<Engine::ViewBudget::HeldView> Views; // SceneRenderer::LiveHoldings at the refusal
            std::vector<ViewConsumer>                 Census;
        };
        std::optional<OpenRefusal> m_OpenRefusal;
        bool                       m_OpenRefusalPending = false; // raise the modal on the next ImGui frame

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
        // The panel to bring to the front of its dock this frame IS read, and stays.
        std::string m_FocusPanel;

        // "Maximize panel" / "Restore panel": the one panel lifted out of its dock, and the node it came from.
        PanelMaximize m_PanelMaximize;

        CommandPalette m_CommandPalette;
        // Every palette provider, in palette order; registered in OnAttach.
        CommandRegistry m_Commands;

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
        char                                    m_LayoutNameBuf[64]  = {};

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