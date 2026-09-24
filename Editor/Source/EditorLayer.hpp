#pragma once

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
#include "Editor/Core/Control/ControlPipeline.hpp"
#include "Editor/Core/Control/ControlProtocol.hpp"
#include "Editor/Core/Control/ControlSocket.hpp"
#include "Editor/Core/Control/ControlState.hpp"
#include "Editor/Core/SceneViewIdentity.hpp"
#include "Editor/Core/Selection/AuthoringContext.hpp"
#include "Editor/Core/SubjectEditorRegistry.hpp"
#include "Editor/Core/DocumentWell.hpp"
#include "Editor/Core/FlightRules.hpp"
#include "Editor/Core/PanelRegistry.hpp"
#include "Editor/RenderSystems/RenderRigistry.hpp"
#include "Editor/Widgets/WindowChrome.hpp"
#include "Editor/Splash/SplashScreen.hpp"

#include <chrono>
#include <optional>

#include <Common/Content/TextAssetHeader.hpp>

#include <filesystem>
#include <unordered_map>

namespace Desert::Editor
{
    class ImportManager;
    class FileExplorerPanel;
    class ViewportPanel;

    class EditorLayer : public Common::Layer
    {
    public:
        // @p splash is the start-up splash CreateApplication put up before the renderer existed; this
        // layer reports its steps to it and takes it down on the first real frame (RevealWhenReady).
        EditorLayer( const Engine::Application* application, const std::string& layerName,
                     std::unique_ptr<Splash::SplashScreen> splash );
        ~EditorLayer();

        [[nodiscard]] Common::BoolResultStr OnAttach() override;
        [[nodiscard]] Common::BoolResultStr OnDetach() override;
        [[nodiscard]] Common::BoolResultStr OnUpdate( const Common::Timestep& ts ) override;
        [[nodiscard]] Common::BoolResultStr OnUIRender() override;
        void                                OnEvent( Common::Event& event ) override;

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
        void DrawScenesMenu();
        void DrawGraphicsMenu();
        void DrawAboutMenu();

        // ===== Menu sections =====
        void DrawStyleSubmenu();
        void DrawOpenSceneMenuItem();
        void DrawPreferencesWindow(); // Edit -> Preferences... (persisted to ~/.desertengine/editor.json)

        // ===== Top Bar Sections =====
        void DrawProjectSection();
        void DrawSceneRenameSection();
        void DrawPlayButton( const ImVec2& size = ImVec2( 0.0f, 0.0f ) );
        void DrawPauseButton( const ImVec2& size = ImVec2( 0.0f, 0.0f ) );

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

        // THE DICTIONARY, AND THE ONLY ONE. Every command this editor can be asked to perform without a
        // mouse: the tool panels, the open documents, the entities in the open scene, the menu bar, the
        // openable assets, the focused document's preview viewpoints, and the plain actions.
        //
        // Extracted from DrawCommandPalette so that the CONTROL CHANNEL runs these same entries and calls
        // these same closures. That is the whole design of the channel in one function: everything a
        // person can reach with Ctrl+P, an agent can reach by naming a group and a label, by construction
        // rather than by anybody maintaining a second list. See Editor/Core/Control/ControlDispatch.hpp.
        //
        // Built on demand — when the palette opens, or when a request arrives — never per frame. THAT
        // SENTENCE USED TO BE FALSE: DrawCommandPalette rebuilt it on every frame the overlay was up, and
        // the dictionary walks the scene's entities, the levels on disk and every openable file under the
        // content root. See DrawCommandPalette for what makes rebuilding on OPEN correct rather than a
        // snapshot going stale.
        [[nodiscard]] std::vector<PaletteCommand> BuildPaletteCommands();

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
        // Collect what was recorded and write it as a PNG. Distinct from WriteViewportPng, which reads the
        // scene's own image and holds no interface at all; neither substitutes for the other. False on any
        // failure, with the reason in @p outError.
        [[nodiscard]] bool WriteWindowPng( const std::string& path, std::string& outError );

        // After an unclean exit, offers to reopen the newest autosave. No-op unless one was found.
        void DrawRecoveryPopup();

        // Modal for naming + saving the current docking layout (opened from View -> Layouts).
        void DrawLayoutSavePopup();

        // Play mode: snapshot the scene on Play, restore it on Stop (so play-time changes don't persist).
        void OnScenePlay();
        void OnSceneStop();
        void OnScenePauseToggle();

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
        void DrawOpenScenePopup();
        // "Discard unsaved changes?" for a scene opened by drag-drop / double-click (see m_PendingOpenScene).
        void DrawConfirmOpenScenePopup();
        void DrawSaveScenePopup();
        void DrawNewScenePopup();
        void DrawReloadScenePopup();
        void DrawProjectPopup();

        void PrepareScenePopup();
        // Every .desce under the project's scenes root, recursively, sorted by the label the UI shows.
        // Two readers: the Open Scene popup (through m_AvailableScenes) and the command palette, which
        // does NOT cache it — the palette is rebuilt only while it is open or when the control channel
        // asks, which is exactly when a fresh answer is wanted.
        static std::vector<Common::Filepath> CollectAvailableScenes();
        void LoadScene( const Common::Filepath& path );
        void LoadSceneInternal( const Common::Filepath& path );

        void NewSceneInternal(); // clears the current scene to a fresh empty one (File -> New Scene / Ctrl+N)

        // ===== Multi-scene editing (independent SceneRenderers) =====
        // Adds the standard ECS systems to a scene (shared by the main scene and any extra scene views).
        void BuildSceneSystems( Desert::Core::Scene& scene );
        // Opens a new, empty scene alongside the main one — its own SceneRenderer + RenderRegistry + a live
        // dockable viewport. Work on a UI/main-menu scene next to the game scene without switching.
        void AddSceneView();
        // Destroys the document named @p id: its viewport panel, render registry, scene and renderer, in that
        // order and behind a device-idle wait. Called from OnUpdate (between frames) when the user closes a
        // scene-view window; a no-op for an id that is already gone. This is what gives the renderer slot
        // back — see Engine/Core/RendererSlotPool.hpp.
        void CloseSceneView( uint64_t id );
        // Closes every scene view whose window the user dismissed since the last frame. One pass at the top
        // of OnUpdate, because a close destroys GPU resources and removes a panel from m_Panels — neither is
        // legal from inside the ImGui pass that is iterating it.
        void CloseDismissedSceneViews();

        // ===== Several ANGLES on ONE scene (what "New Scene View" was not) =====
        // Opens another viewport onto the ACTIVE document: its own SceneRenderer and its own camera, the
        // same world. Not a second Scene — that is AddSceneView above, and it is a second DOCUMENT: an
        // independent copy that drifts from this one the moment either is edited. The engine side is
        // Scene::AddView (Engine/Core/SceneViewList.hpp); the ECS is still walked once per frame no
        // matter how many of these are open.
        void AddSceneViewport();

        // ── THE FOUR-UP GRID (UE's pattern, spelled in our docking) ───────────────────────────────
        //
        // What UE's four-viewport layout buys is "see the same object from fixed orthogonal directions
        // at once". UE spells it as a dedicated splitter widget with a per-pane type menu, because its
        // viewport area is not a general docking host. OURS IS — every viewport is an ordinary dockable
        // window — so a bespoke splitter here would be a second, weaker layout system: its panes could
        // not be tabbed, floated, resized against the Outliner, or saved as a named layout, all of
        // which the docking already gives. So this opens the three extra views, aims the four cameras
        // at Perspective / Top / Front / Right, and asks DockBuilder for the quarters.
        //
        // Runs from OnUpdate (between frames) because opening a view leases a renderer slot and builds
        // GPU resources. REFUSES with the reason printed when the slot budget cannot carry four.
        void BuildViewportGrid();
        // The window titles waiting for the quarters, in grid order: top-left, top-right, bottom-left,
        // bottom-right. Filled by BuildViewportGrid and drained by the dockspace pass on the NEXT frame
        // — DockBuilder must run inside the ImGui frame, and BuildViewportGrid runs outside one.
        std::vector<std::string> m_PendingViewportGrid;
        // Destroys viewport @p id: its panel, then the renderer whose destructor hands the slot back.
        // The scene is NOT touched beyond dropping the view — the other viewports of that world go on.
        void CloseSceneViewport( uint64_t id );
        void CloseDismissedSceneViewports();

        // ===== Asset documents (one window per asset, opened from the browser) =====
        // Drains Core::SubjectOpenRequests and, per request, focuses the document already open on that subject
        // or builds a new one through m_AssetEditors. Runs from OnUpdate (between frames) because it adds to
        // m_OpenDocuments, and REFUSES past the six renderer slots with the census printed by name — a seventh
        // consumer would otherwise be handed slot 0 to share, which fails silently and days later.
        void ServiceSubjectOpenRequests();
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
        // function's device-idle wait — see ISubjectDocument::ReleaseRendererSlot.
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
            if ( !m_MainScene || owner.IsNull() )
                return false;
            const auto entOpt = m_MainScene->FindEntityByID( owner );
            return entOpt && entOpt->get().HasComponent<ComponentT>();
        }
        // Brings @p subject's window to the front and makes it the most recently used document.
        void FocusDocument( const SubjectId& subject );
        // Ctrl+Tab: move to the next document in most-recently-used order. See DocumentWell::NextMostRecent.
        void CycleDocuments();

        // ===== The document well (layout option B.1) =====
        // The permanent "Documents" window: the tab the documents dock beside, the index of what is open,
        // and — when nothing is open — the empty state that says what the area is for plus the list of
        // recently closed documents. It does not collapse when it empties: a layout that moves on its own is
        // what users report as "the editor lost my panel".
        // The window title one document is drawn with — its type's icon (from the registration), its
        // subject's name, and the identity DocumentTitle baked into GetName(). A member rather than a free
        // function because the icon comes from m_SubjectEditors.
        [[nodiscard]] std::string DocumentDisplayTitle( const ISubjectDocument& document ) const;

        void DrawDocumentWell();
        // Every open document, drawn into the well's dock node. Separate from the tool loop because the two
        // have separate owners and separate close semantics — a tool passes &GetVisibility() to Begin, a
        // document passes a frame-local bool whose false is a CLOSE REQUEST, not a hidden window.
        void DrawDocuments();
        // The refusal, on screen. A seventh renderer consumer is refused; before this the refusal was a
        // line in the log and the click simply looked dead. The census text already existed — it had
        // nowhere to be shown.
        void DrawOpenRefusedPopup();

        // Who is holding a renderer slot right now, by name. Printed when an open is refused — "no free
        // slot" without the list leaves the user with nothing to close. The main viewport and every extra
        // scene view hold one for as long as they exist; the Details preview and each asset document are
        // demand-driven and may be open while holding nothing.
        struct RendererSlotConsumer
        {
            std::string Name;
            bool        HoldsSlot = false;
            // Whether this consumer will ever want a slot. A CPU-only asset document (the four cloud
            // editors) holds none and is not waiting for one, and the census has to say so — "no slot right
            // now, but will claim one when it draws" would name it as something to close to free a slot it
            // was never going to take. See ISubjectDocument::ClaimsRendererSlot.
            bool ClaimsSlot = true;
            // Set for a consumer the user can close FROM THE REFUSAL ITSELF: an open document. A census that
            // names five things and offers no way to act on any of them is a longer version of "no free
            // slot". The main viewport and the Details preview carry no handle — neither is a window a
            // person closes to make room. Last in the struct so the two- and three-field aggregate
            // initialisations below keep meaning what they say.
            std::optional<SubjectId> Document = {};
        };
        [[nodiscard]] std::vector<RendererSlotConsumer> RendererSlotCensus() const;
        // Rebinds the editor to a focused document: m_MainScene (and thus every play/save/gizmo call site)
        // points at it, Commands + the scene-bound panels follow. kPrimarySceneViewId = the primary/main
        // scene. An id whose document has been closed rebinds nothing and says so — see SceneViewIdentity.hpp
        // for why the viewports name their document instead of numbering it.
        void SetActiveScene( uint64_t id );
        // Runs one render frame for a scene (outline aid + Begin/RegistryRender/OnUpdate/End). Called for
        // every open document each frame so all viewports stay live.
        Common::BoolResultStr UpdateSceneFrame( Desert::Core::Scene& scene, Render::RenderRegistry* registry,
                                                const Common::Timestep& ts );

        // Startup content is DATA, not code — these build entities into m_MainScene so the result
        // can be serialized to a .desce ONCE and loaded like any scene afterwards.
        void BuildStarterScene();    // fresh Hub project's DefaultScene: sun/ground/cube/light/camera
        void BuildCornellShowcase(); // sandbox demo: baked into CornellDemo.desce on first launch
        // Serializes m_MainScene to @p path. False when the bytes did not land, with the reason logged;
        // the file that was there (if any) is unchanged. Both callers generate startup content, so a
        // false here means the project's own default scene is not on disk.
        [[nodiscard]] bool SaveSceneTo( const std::string& path );
        // Drops the scene's text header when `destination` is not the file it was opened as (a copy is a
        // new asset with a new GUID); returns the header it had, for a failed save to put back.
        std::optional<Common::Content::TextAssetHeaderSerialized>
        ForgetAssetIdentityUnlessSameFile( const std::string& destination );

        // WHERE Ctrl+S goes: the file the scene was opened from, or — for a scene that has never been on
        // disk — one named after it, which is the only thing there is to name it after. That fallback is
        // the ONLY surviving name-to-path derivation in the editor and it is reachable only when there is
        // no path; it used to run on EVERY save, in the engine, and silently sent a save meant for an
        // open file into a second file beside it.
        [[nodiscard]] Common::Filepath SceneSaveDestination() const;

        // THE ONE place the open scene is saved from. Every entry point (Ctrl+S, File -> Save, the
        // command palette, the "Save and Open" button) goes through it, so the policy — clear the
        // unsaved-changes mark and announce success ONLY when the bytes landed — is written once and
        // decided by a pure, tested rule (Editor/Core/SceneSaveRules.hpp). Returns whether the scene on
        // disk is now current; a caller about to destroy the in-memory scene MUST branch on it.
        [[nodiscard]] bool SaveOpenScene();

        // Force re-cook of Cooked/ from sources, re-register cooked assets, refresh the asset panel.
        void RebuildCookedAssets();

        // Read the resolved viewport back off the GPU and write it to @p path as a PNG, creating the
        // parent directory if it is missing. The single implementation behind the `--shot` still, every
        // frame of a `--shot-sequence`, and the F9 dump. False on any failure, always with the reason
        // logged and the numbers in it.
        bool WriteViewportPng( const std::string& path );

        // Writes `<project>/.thumbnail.png` — the picture the LAUNCHER puts on this project's tile.
        //
        // 512x288, centre-cropped to 16:9 from whatever the viewport happens to be. The aspect is
        // not a preference: the launcher's grid is built out of 16:9 tiles, so a square or
        // arbitrary-aspect file would either letterbox (which reads as a broken image) or crop
        // differently on every project. Fixing it here means the launcher never has to guess.
        //
        // Called after a scene save and again on a clean exit, so the tile shows what the project
        // last looked like rather than what it looked like the day it was created. Failure is
        // returned, not swallowed — but the callers treat it as non-fatal: a project with no
        // thumbnail is a state the launcher already draws, and losing a picture must never fail a
        // save or hold up a shutdown.
        [[nodiscard]] Common::BoolResultStr WriteProjectThumbnail();

    private:
        // The resolved viewport as RGBA8, plus its size. One readback for every consumer: a capture
        // that differed from a dump in flip, format or the device-idle wait that makes the readback
        // legal would be a defect nobody could see in either picture alone.
        [[nodiscard]] Common::BoolResultStr ReadViewportRGBA8( std::vector<uint8_t>& outPixels, uint32_t& outWidth,
                                                               uint32_t& outHeight );

        enum class EditorState
        {
            Paused = 0,
            Play,
        };

        // INITIALISED, and it was not. The constructor's init list never named it and the declaration
        // carried no initialiser, so a freshly booted editor read whichever byte its allocation landed
        // on. Measured 2026-09-22 from the control channel: two launches of the SAME binary minutes
        // apart reported `"playing": false` and `"playing": true`, with the scene sitting in Edit both
        // times — `OnScenePlay` guards on the SCENE's state, so the two can disagree and only this one
        // was garbage. It is read by the state snapshot the control channel publishes and by
        // CloseSceneView, which discards a play snapshot on the strength of it.
        EditorState m_EditorState = EditorState::Paused;
        std::string m_PlaySnapshot;        // serialized scene captured on Play, restored on Stop
        // A partitioned world in Play keeps only the camera's neighbourhood in the ECS (WorldStreamer.hpp);
        // null in Edit and for a world without a WorldPartition block. Ended before Stop restores the snapshot.
        std::unique_ptr<Desert::Core::WorldStreamer> m_WorldStreamer;
        double                                       m_WorldStreamClock = 0.0; // seconds of Play, for retries
        bool        m_ShowProfiler = true; // View ▸ Profiler toggles the profiler window

    private:
        const Engine::Application* m_Application;

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
        // BEFORE the preloader, which holds a non-owning reference to it and must therefore not outlive
        // it: members are destroyed in reverse declaration order.
        std::unique_ptr<Animation::AnimationLibrary> m_AnimationLibrary;
        std::unique_ptr<Assets::AssetPreloader>      m_AssetPreloader;
        std::unique_ptr<ImportManager>               m_ImportManager;
        Runtime::AssetHotReload                      m_AssetHotReload; // .demat/.shader live reload

        FileExplorerPanel* m_FileExplorerPanel = nullptr; // non-owning (lives in m_Panels)

        // m_MainScene is the ACTIVE document — rebound to the focused viewport's scene so the 100+ existing
        // call sites (play/save/gizmo/autosave) operate on it without change. m_PrimaryScene keeps a handle
        // to the original (index -1) so we can rebind back to it.
        std::shared_ptr<Desert::Core::Scene> m_MainScene;
        std::shared_ptr<Desert::Core::Scene> m_PrimaryScene;

        std::unique_ptr<Render::RenderRegistry> m_RenderRegistry;

        // Extra scenes opened alongside the main one (Scenes -> New Scene View). Each owns its own renderer,
        // editor render-registry and a live ViewportPanel (non-owning ptr; the panel lives in m_Panels).
        struct SceneDocument
        {
            // The document's name for as long as it exists, and the ONLY thing a viewport's activation
            // callback captures. Not its position: see Editor/Core/SceneViewIdentity.hpp for why an index
            // silently activates the wrong document the moment a view in front of it is closed.
            uint64_t                                Id = kPrimarySceneViewId;
            std::string                             Name;
            std::shared_ptr<Desert::Core::Scene>    Scene;
            std::unique_ptr<Graphic::SceneRenderer> Renderer;
            std::unique_ptr<Render::RenderRegistry> Registry;
            ViewportPanel*                          Viewport = nullptr;
        };
        std::vector<std::unique_ptr<SceneDocument>> m_ExtraScenes;

        // A SECOND ANGLE, not a second document: no Scene of its own and no RenderRegistry of its own —
        // the scene replays its external passes onto every view's renderer, so the grid, the collider
        // wireframes and the 2D UI overlay arrive here without a second copy of the editor's pass set.
        struct SceneViewport
        {
            uint64_t    Id = kPrimarySceneViewId;
            std::string Name;
            // WEAK. The document this looks at can be closed while this viewport is open; a shared_ptr
            // here would keep a dead scene's registry alive and the viewport would keep rendering it.
            std::weak_ptr<Desert::Core::Scene>      Scene;
            std::unique_ptr<Graphic::SceneRenderer> Renderer;
            ViewportPanel*                          Viewport = nullptr;
        };
        std::vector<std::unique_ptr<SceneViewport>> m_ExtraViewports;

        // ONE id source for documents AND viewports. They share the ImGui window-id space and the
        // authoring-context owner space, so two surfaces holding the same number would dock into one
        // window and fight over the selected bone.
        SceneViewIdSource                           m_SceneViewIds;
        uint64_t m_ActiveSceneId = kPrimarySceneViewId; // which document the editor is bound to

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

        // How long a document must go UNDRAWN BY EVERY VIEW before it gives its renderer slot back. A
        // document behind another one's tab is open and invisible, and it was holding one of the six
        // renderer slots for as long as the user left it there — see ISubjectDocument::ReleaseRendererSlot.
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
        // Ctrl+Tab holds the ring still. Landing on a document by cycling must NOT reorder the ring, or the
        // second press would come straight back to where the first started; the order is committed once Ctrl
        // is released, which is the behaviour every alt-tab ring has.
        bool m_CyclingDocuments = false;
        // The dock node the documents live in (layout option B.1): the centre column is split, the level
        // keeps the left node, documents get the right one. Read back from the well window's own dock id
        // every frame rather than remembered from the one frame the layout was built — a value captured at
        // build time is 0 for the whole of every later session.
        ImGuiID m_DocumentDockId = 0;

        // A refused open, waiting to be shown (see DrawOpenRefusedPopup). Holds the census by value: the
        // documents it names may be closed while the dialog is up, and a row pointing at a destroyed panel
        // is the dangling reference this split exists to avoid.
        struct OpenRefusal
        {
            std::string                       AssetName;
            std::string                       TypeName;
            uint32_t                          Live    = 0;
            uint32_t                          Pending = 0;
            std::vector<RendererSlotConsumer> Census;
        };
        std::optional<OpenRefusal> m_OpenRefusal;
        bool                       m_OpenRefusalPending = false; // raise the modal on the next ImGui frame

        std::shared_ptr<ImGui::ImGuiLayer> m_ImGuiLayer;
        // THE TOOLS. A container that cannot hold a document — see Editor/Core/PanelRegistry.hpp. That is
        // what makes "the View menu lists exactly the tools" true by construction rather than by a predicate
        // the menu, the command palette and --open-panel would each have had to remember.
        PanelRegistry m_Panels;

        // `m_ContextualShown` STOOD HERE — a set of raw panel pointers, inserted and erased in five
        // places and QUERIED IN NONE. Its own comment claimed it was what stopped a panel the user opened
        // by hand from being auto-closed; that is actually done by `IPanel::Pinned()`, which every branch
        // of UpdateContextualPanels already tests. So the set was write-only state, and one that went
        // dangling wholesale at `m_Panels.Clear()`. Removed with its five writes (A8-2), which is the same
        // decision this task took on `CloudNoiseService::GetGeneration` and `InstancesDirty`.
        //
        // The panel to bring to the front of its dock this frame IS read, and stays.
        std::string m_FocusPanel;

        CommandPalette m_CommandPalette;

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
        ImGuiID m_BottomDockId    = 0;
        bool    m_BottomCollapsed = false;
        float   m_BottomHeight    = 0.0f;
        void    DrawBottomDrawerToggle();
        char                                    m_LayoutNameBuf[64] = {};
        std::unique_ptr<Graphic::SceneRenderer> m_SceneRenderer;
        bool                                    m_OpenScenePopup        = false;
        bool                                    m_SaveSceneRequested    = false;
        // Set when "Save and Open" could not write the scene: the modal STAYS OPEN and shows this, so
        // the choice the user is making ("throw this scene away") is made knowing the save did not
        // happen. Cleared whenever the modal is dismissed.
        std::string                             m_SaveAndOpenError;
        bool                                    m_NewSceneRequested     = false;
        bool                                    m_AddSceneViewRequested = false; // Scenes -> New Scene View
        // Deferred for the same reason as the flag above: opening a viewport leases a renderer slot and
        // builds GPU resources, neither of which may happen inside the ImGui pass.
        bool                                    m_AddSceneViewportRequested = false;
        // Scene -> Four-Up Viewports. Deferred like the two above, and for the same reason.
        bool                                    m_ViewportGridRequested = false;

        // Staged startup loading: the heavy boot work (mesh cooking, asset preload) runs one stage per
        // frame from OnUpdate, each announced on the splash, with the main window still hidden.
        struct StartupStage
        {
            std::string           Label;
            std::function<void()> Run;
        };
        std::vector<StartupStage> m_StartupStages;
        size_t                    m_StartupNext = 0;

        // ===== The splash's steps, and the moment the editor is shown =====
        //
        // THE STEPS THE SPLASH COUNTS are the shader preload (OnAttach — not a stage, because the render
        // systems resolve their shaders in their constructors and it has to finish before they exist),
        // every entry of m_StartupStages, and the settle wait after them. One count, derived here, so the
        // "N / M" a person reads cannot drift from the list that is actually run.
        static constexpr size_t kSplashShaderStep = 0;
        size_t                  SplashStepCount() const
        {
            return m_StartupStages.size() + 2;
        }
        size_t SplashSettleStep() const
        {
            return m_StartupStages.size() + 1;
        }
        void ReportSplashStep( const std::string& label, size_t step );
        // Called at every presented frame; the first one presented after the start is over shows the
        // hidden main window and closes the splash. Until then the splash is the only window.
        void RevealWhenReady();
        // KEPT after it is closed, until the layer goes: Close() only starts the crossfade, and the
        // object's destructor is what waits for its window and thread — at teardown, not on the frame
        // the editor has just appeared on.
        std::unique_ptr<Splash::SplashScreen> m_Splash;
        bool                                  m_Revealed = false;
        // Set by the first OnUIRender that draws the editor rather than a loading frame.
        bool m_RealFrameDrawn = false;
        // WHERE THE ELAPSED TOTAL LIVES NOW. It used to be a `long long` accumulated here with the
        // accumulation rule written in this comment; the rule (sum of the stages, NOT wall clock between
        // the first and the last, because a stage runs one per frame) now lives in `Core::BootTimeline`
        // alongside the line format, so that the shipping runtime's boot numbers and this one mean the
        // same thing. Nothing about the per-frame scheduler above moved.
        ::Desert::Core::BootTimeline m_Boot{ "Editor" };
        bool                      StartupLoading() const
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

        // ── THE PALETTE AS AN AUTHORING OWNER ─────────────────────────────────────────────────────
        //
        // The command palette can put the editor into Control mode and pick a control, so it is a WRITER
        // of the authoring context and therefore has to hold it like every other writer -- Kind::Panel,
        // its own durable copy, and refusals that name it. Without this the two entries would have to
        // reach into a viewport's context, which is the process-wide-statics shape AuthoringContext.hpp
        // was written to end.
        Core::AuthoringContext     m_PaletteAuthoring;
        const Core::AuthoringOwner m_PaletteAuthoringOwner = Core::AuthoringOwner::ForPanel( "Command Palette" );

        // Frames since the layer attached. The gate's clock — deliberately this layer's own count and not
        // the renderer's frame-in-flight index, which wraps at three and could not order anything.
        uint64_t m_FrameIndex = 0;

        // A capture that could not even be RECORDED — the surface refuses TRANSFER_SRC, the swapchain is
        // gone. Carried from the record half to the collect half so the refusal names the real reason
        // rather than "nothing was captured", which would be the symptom and not the cause.
        std::string m_ControlCaptureError;

        // A `quit` the channel asked for. Honoured after its reply has actually gone out, so the last
        // answer is not lost to the exit — a client that never hears "ok" cannot tell a clean shutdown
        // from a crash.
        std::optional<int32_t> m_ControlQuitCode;

        // WHICH FILE THE OPEN SCENE IS. Set by a load that succeeded, adopted by a save that landed,
        // and cleared by File -> New Scene, which produces a scene that is not any file yet.
        //
        // It is the editor's, not the scene's, and that is the point: Play -> Stop clears the scene and
        // rebuilds it from a snapshot, so an identity kept inside Scene would either be destroyed by that
        // (and the next Ctrl+S would go somewhere else) or have to be saved and restored around it by
        // hand, which is the link that gets dropped. The document is open in the editor; the editor knows
        // which one.
        //
        // Empty means "this scene has never been on disk", and SceneSaveDestination is the ONE place that
        // turns that into a path. Everything else that used to do it is gone: SceneSerializer derived the
        // destination from the scene's NAME on every save, which is why an open U52_LockProbe.desce was
        // never written and a U52_Lock_Probe.desce appeared beside it under a green "Saved" toast.
        Common::Filepath m_OpenScenePath;

        std::optional<Common::Filepath> m_SceneLoadRequested;
        // Stop tears down + recreates GPU render resources (framebuffers / render graph). It must run
        // BETWEEN frames (like a scene load), never inline in the ImGui Stop-button handler — otherwise the
        // next frame begins a render pass against a just-destroyed framebuffer (driver access violation in
        // vkCmdBeginRenderPass). Deferred to the top of OnUpdate.
        bool                          m_PendingSceneStop = false;
        std::vector<Common::Filepath> m_AvailableScenes;
        std::vector<Common::Filepath> m_RecentScenes;
        int                           m_SelectedSceneIndex = -1;
        // Open Scene popup: substring filter over the (recursive) scene list — with subfolders the list is
        // long enough that scrolling for a name is worse than typing it.
        char m_SceneFilter[128] = {};

        // A scene a panel asked to open (dropped on the viewport, double-clicked in the browser) while the
        // current one had unsaved edits: held until the confirm popup says discard/save/cancel.
        std::optional<Common::Filepath> m_PendingOpenScene;
        bool                            m_ConfirmOpenScenePopup = false;
    };
} // namespace Desert::Editor