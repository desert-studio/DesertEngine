#define IMGUI_DEFINE_MATH_OPERATORS

#include <Editor/Panels/Scalability/AntiAliasingPaletteCommands.hpp>
#include <Engine/Graphic/RenderConfig.hpp>
#include <Engine/World/Landscape/LandscapeData.hpp>
#include <Engine/Core/Glfw.hpp>
#include <Engine/Core/PlayerStart.hpp>
#include <Editor/Core/SaveShortcut.hpp>
#include <Editor/Core/ContentCreateCommands.hpp>
#include <Editor/Core/DetailsNavigation.hpp>
#include <Engine/Graphic/Environment/EnvironmentBake.hpp>
#include <Engine/Assets/BootContent.hpp>
#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/Serialization/MeshBinary.hpp>
#include <Common/Core/AssetHandle.hpp>
#include <Common/Content/AssetRedirector.hpp>
#include <Common/Content/ContentChunks.hpp>
#include <Engine/Assets/TextureSourceAsset.hpp>
#include <Engine/Assets/MeshDerivedData.hpp>
#include <Editor/Import/MeshDeriver.hpp>

#include <Common/Core/AssetPathIndex.hpp>
#include <Common/Utilities/ContentScanLedger.hpp>
#include <Engine/Graphic/ResourceLedger.hpp>
#include <Engine/Assets/SyncLoadLedger.hpp>
#include "EditorLayer.hpp"

#include <Engine/Runtime/ResourceRegistry.hpp>

#include <Engine/Assets/TextureSourceAsset.hpp>

#include <functional>
#include <set>

#include <Editor/Widgets/ThumbnailCache.hpp>
#include <Editor/Widgets/ThumbnailService.hpp>
#include <Engine/Core/SceneAssetRoots.hpp>
#include <Common/Core/Core.hpp>
#include <Common/Core/CrashHandler.hpp>
#include <Common/Core/Profiler.hpp>
#include <Editor/Import/ImportOptionsDialog.hpp>
#include <Engine/Runtime/Services/Mesh/MeshService.hpp>
#include <Engine/Runtime/Services/AssetServiceRegistration.hpp>
#include <Common/Core/JobSystem.hpp>

// 1. Engine Core
#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Entity.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/Localization/LocalizationService.hpp>
#include <Engine/ECS/EntityLock.hpp>
#include <Engine/Geometry/PrimitiveType.hpp>
#include <Common/Core/Units.hpp>
#include <Engine/Geometry/DynamicMesh.hpp>
#include <Engine/Geometry/ProceduralCharacterFactory.hpp>
#include <Engine/Animation/Animator.hpp>
#include <Engine/Animation/Rig/ControlHierarchy.hpp>
#include <Engine/Animation/Rig/ControlRigStage.hpp>
#include <Engine/Assets/AssetEviction.hpp>
#include <Engine/Assets/Mesh/AnimationAsset.hpp>
#include <Engine/Scripting/ScriptEngine.hpp>
#include <Engine/Core/WorldStreamer.hpp>
#include <Engine/Core/Serialize/SceneFormat.hpp>
#include "Editor/Core/CommandLine.hpp"
#include "Editor/Core/Control/ControlChannelOptions.hpp"
#include "Editor/Core/PanelRequests.hpp"
#include "Editor/Core/SceneOpenRequest.hpp"
#include "Editor/Core/SceneSaveRules.hpp"
#include "Editor/Core/ShotOptions.hpp"
#include "Editor/Core/MaterialAssetUtils.hpp"
#include <Engine/Assets/Prefab/PrefabAsset.hpp>
#include <Common/Utilities/FileSystem.hpp>

// 2. Editor Base & Infrastructure
#include "Editor/Core/GizmoState.hpp"
#include "Editor/Core/CommandHistory.hpp"
#include "Editor/Core/Commands/LandscapeLayerCommands.hpp"
#include "Editor/Core/Commands/SceneCommands.hpp"
#include "Editor/Core/EditorPreferences.hpp"
#include "Editor/Packaging/GamePackager.hpp"
#include "Editor/Core/ProjectContext.hpp"

#include <Engine/Core/Input.hpp>
#include <Common/Core/KeyCodes.hpp>
#include <Common/Core/Version.hpp>
#include <Common/Settings/MachineSettings.hpp>
#include <Engine/Graphic/QualityBoot.hpp>
#include "Editor/Core/ImGuiUtilities.hpp"
#include <ImGui/imgui_internal.h>

#include <array>
#include <format>
#include "Editor/Import/ImportManager.hpp"
#include "Editor/Splash/SplashControls.hpp"
#include "Editor/Splash/SplashImage.hpp"
#include "Editor/Widgets/WindowButtonStyle.hpp"
#include "Editor/Builtin/BuiltinMeshRegistry.hpp"

// 3. Editor Panels
#include "Editor/Panels/SceneHierarchy/SceneHierarchyPanel.hpp"
#include <Editor/Core/SkeletonAssignPalette.hpp>
#include <Editor/Panels/AnimationEditor/SkeletonReferenceSlots.hpp>
#include <Engine/Assets/Mesh/SkinnedMeshAsset.hpp>
#include <Engine/Assets/Serialization/AnimationClipWrite.hpp>
#include "Editor/Panels/FileExplorer/FileExplorerPanel.hpp"
#include "Editor/Panels/ViewportPanel/ViewportPanel.hpp"

#include <Engine/Core/Serialize/WorldPartitionConversion.hpp>
#include "Editor/Panels/Landscape/LandscapeCommands.hpp"
#include "Editor/LevelEditor/ViewportCommands.hpp"
#include "Editor/Panels/Clouds/CloudCommands.hpp"
#include "Editor/Panels/Localization/LanguageCommands.hpp"
#include "Editor/Panels/Build/BuildCommands.hpp"
#include "Editor/Panels/UI/UICommands.hpp"
#include "Editor/Core/DebugCommands.hpp"
#include "Editor/Panels/Modeling/ModelingCommands.hpp"
#include "Editor/Panels/Foliage/FoliageCommands.hpp"
#include "Editor/Core/Rigging/HumanoidCommands.hpp"
#include "Editor/Core/Selection/AuthoringContext.hpp"
#include "Editor/Core/ToastManager.hpp"
#include "Editor/Core/OpenableAssets.hpp"
#include "Editor/Core/ControlNudgeRequest.hpp"
#include "Editor/Core/Commands/PoseEditTransaction.hpp"
#include <Engine/Animation/Rig/ControlManipulator.hpp>
#include "Editor/Core/SubjectOpenRequest.hpp"

// 4. Misc
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <Engine/Core/SceneRenderCollectors.hpp>
#include <Engine/ECS/System/MeshECSSystem.hpp>
#include <Engine/ECS/System/TextECSSystem.hpp>
#include <Engine/ECS/System/SkyboxECSSystem.hpp>
#include <Engine/ECS/System/HeightFogECSSystem.hpp>
#include <Engine/ECS/System/VolumetricCloudECSSystem.hpp>
#include <Engine/ECS/System/TimeOfDayECSSystem.hpp>
#include <Engine/Graphic/Materials/DataDrivenMaterial.hpp>
#include <Editor/Core/DocumentPlacement.hpp>
#include <Editor/Core/Rigging/RigBuilder.hpp>
#include <Editor/Core/Selection/ModelingState.hpp>
#include <Editor/Core/Selection/SelectionManager.hpp>
#include <Engine/ECS/System/PointLightSystem.hpp>
#include <Engine/ECS/System/SpotLightSystem.hpp>
#include <Engine/ECS/System/AnimationECSSystem.hpp>
#include <Engine/ECS/System/AttachmentSystem.hpp>
#include <Engine/ECS/System/PhysicsECSSystem.hpp>
#include <Engine/ECS/System/LevelSequenceSystem.hpp>
#include <Engine/ECS/System/LocomotionSystem.hpp>
#include <Engine/ECS/System/ScriptSystem.hpp>
#include <Engine/ECS/System/AudioECSSystem.hpp>

#include <algorithm> // std::sort / std::transform (scene list)
#include <span>      // the View menu's groups, declared as data rather than as control flow
#include <cctype>    // std::tolower (scene filter)
#include <chrono>    // per-stage startup timing (see the staged boot in OnUpdate)
#include "Editor/LevelEditor/WindowTitles.hpp"
#include "Editor/LevelEditor/AssetEditorRegistrations.hpp"
#include "Editor/Core/AssetOpen.hpp"
#include <Engine/Animation/AnimationLibrary.hpp>

namespace Desert::Editor
{

    // A tool panel that only makes sense for a particular selection or mode opens itself when that
    // context appears and steps aside when it goes away — so the tab strip carries what the current work
    // needs instead of every panel at once. Opening one BY HAND pins it (explicit intent wins) until the
    // user closes it again; see IPanel::IsContextual.
    // Cognitive complexity 27 against a threshold of 19, PRE-EXISTING and reported for any edit inside
    // this constructor (Г26 added the autosave-migration call below). Named as debt, not fixed here.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity)
    EditorLayer::EditorLayer( Engine::Application* application, const std::string& layerName,
                              std::unique_ptr<Splash::SplashScreen> splash )
         : Common::Layer( layerName ), m_Application( application ),
           m_Startup( application, m_AssetManager, m_ImportManager, m_AnimationLibrary, m_Workspace, m_SceneFiles,
                      m_AssetCompiling, m_RealFrameDrawn, std::move( splash ) )

    {
        m_AssetManager = std::make_shared<Assets::AssetManager>();

        m_ImportManager = std::make_unique<ImportManager>();
        // WHAT THIS MACHINE CAN AFFORD — a different file from editor.json and deliberately so (К3).
        // editor.json is one person's copy of the EDITOR and the packaged game never opens it, while every
        // value in machine.json is read by SceneRenderer, which the packaged game runs; the schema is one
        // and the PLACE is the parameter, so a shipped build reads the same fields out of the player's own
        // directory (Runtime/Source/Main.cpp).
        //
        // FIRST, before any SceneRenderer is CONSTRUCTED, and for two independent reasons. MSAA is baked
        // into the pipelines at SceneRenderer::Init, so a later load would apply one start behind; and a
        // renderer initialises its own copy of these values from this store, so one built before the load
        // would hold the schema defaults and push two of them into global sampler state.
        // The device exists here (the layer attaches after it), so the quality starts in one step.
        m_QualityStart = Graphic::QualityBoot::Start( std::filesystem::path( ProjectContext::ConfigDirectory() ) /
                                                      "machine.json" );

        // Filled by the "Indexing animation clips" stage above, from the registry's clip rows.
        m_AnimationLibrary = std::make_unique<Animation::AnimationLibrary>( m_AssetManager.get() );
        Desert::Runtime::ResourceRegistry::BindOnDemandAssets( m_AssetManager );
        m_Workspace.CreatePrimaryScene();
        // Documents follow the active scene too. The Cloud Layout document READS the focused scene's cloud
        // layer for its preview numbers — the scene is an input, never a second subject — and it stopped
        // following it the moment documents left the panel list, which is exactly the "a middle link drops a
        // property" shape this codebase has paid for seven times.
        m_Workspace.OnActiveSceneChanged(
             [this]( const std::shared_ptr<Desert::Core::Scene>& scene )
             {
                 for ( auto& document : m_Documents.Documents() )
                     document->SetScene( scene );
             } );

        // The scene/asset-manager the undoable structural commands operate on (the scene OBJECT is reused
        // across loads — Clear() + deserialize — so this stays valid; the history itself is cleared on
        // load/Play/Stop instead).
        Commands::SetContext( m_Workspace.ActiveScene().get(), m_AssetManager.get() );

        LOG_INFO( "[Editor] Desert Engine {} ({} branch)", Common::Version::Full(), Common::Version::Branch() );

        // User prefs (snap steps, camera speed, autosave) from ~/.desertengine/editor.json. Snap values
        // apply immediately; the camera speed is applied on the first frame (the camera exists by then).
        EditorPreferences::Load();

        // THE FIRST LEVEL (EditorStartup::ChooseInitialLevel; UE: UEditorEngine::InitEditor's EditorStartupMap):
        // the capture's own scene, the project's DefaultScene, or the Basic level template as an untitled scene.
        m_Startup.ChooseInitialLevel( m_Shots );

        BuiltinMeshRegistry::Init( nullptr );

        // Crash recovery: the pop-up for the previous session's autosave after an unclean exit, then this
        // session's lock (SessionRecovery::OfferAndArm); a clean shutdown (OnDetach) removes it.
        m_Recovery.OfferAndArm( m_Dock );
    }

    EditorLayer::~EditorLayer() = default;

    [[nodiscard]] Common::BoolResultStr EditorLayer::OnAttach()
    {
        if ( !m_QualityStart )
        {
            return m_QualityStart;
        }
        if ( Common::EventTree* events = Events() )
        {
            m_Panels.JoinEvents( *events, EventNode() );
            m_Subsystems.emplace( *this, *events, EventNode() );
        }
        // THE CONTROL CHANNEL, IF ONE WAS ASKED FOR. Before anything else, so a client that started this
        // editor can connect and watch the boot rather than guessing how long to wait for the socket.
        //
        // A REFUSAL ENDS THE RUN. Carrying on unheard would be the worst of both: the client waits for a
        // socket that will never appear, and the editor it was meant to drive sits there being driven by
        // nobody. `--control-socket` is only ever passed by something that intends to connect.
        if ( const auto& channel = Control::ControlChannelOptions::Get(); channel.Requested() )
        {
            if ( const auto listening = m_Control.Listen( channel.SocketPath ); !listening )
                return Common::MakeFormattedError( "control channel: {}", listening.GetError() );
        }

        // THE WINDOW, THE CLOSE GATE AND THE IMGUI CONTEXT (EditorImGuiHost; UE: FSlateApplication::Create).
        if ( auto hosted = m_ImGuiHost.Attach( *m_Application, [this]() { RequestEditorExit(); } ); !hosted )
            return hosted;

        // THE DOCKING LAYOUT FILE, OFF THE PROJECT (DockLayout::BindLayoutFile).
        if ( const auto bound = DockLayout::BindLayoutFile(); !bound.IsSuccess() )
            return Common::MakeError( bound.GetError() );

        // Before the first frame, which is when ImGui reads the layout file.
        m_Documents.RegisterDocumentWellLayoutHandler();
        m_Documents.RegisterDocumentPlacementHandler();

        // THE COOKED ASSET REGISTRY, THE ENGINE SHADERS AND THE SCENE SYSTEMS (EditorStartup::BootContent; UE:
        // FLevelEditorModule::StartupModule). A refusal ends the run.
        if ( auto booted = m_Startup.BootContent(); !booted )
            return booted;

        // THE ANIMATION LIBRARY IS NOT FILLED HERE, and its absence is the fix rather than an omission: a fill
        // loop in OnAttach once ran before the clips were known and reported only the procedural ones, and the
        // runtime had none at all. Both hosts call `Assets::IndexAnimationClips` (BootContentCensus).

        // NOT INITIALISED WHEN A SCENE LOAD IS ALREADY QUEUED, and that condition is why the line moved
        // rather than why it is conditional. The constructor above has already called LoadScene() for
        // `--scene` or for the project's default scene, so by the time OnAttach gets here the empty "New
        // Scene" this would build a renderer for is a scene NOBODY WILL EVER SEE: OnUpdate returns early
        // for the whole of the staged startup load and draws no scene frame, and the first thing it does
        // when that finishes is LoadSceneInternal, whose own Init() ran second. Measured at ~1.4 s of every
        // Debug start (Г8) — pipelines and framebuffers built, waited on and destroyed.
        //
        // WHAT THIS SAVES CHANGED WITH Г11, AND THE LINE IS STILL RIGHT. Init() no longer rebuilds the
        // renderer on a second call, so the first one would no longer be THROWN AWAY — it would simply
        // happen earlier. What it would still be is a renderer built against an empty scene, before the
        // staged startup load has cooked and preloaded anything, on a frame nobody sees; deferring it to
        // the load that a person is actually waiting for is what keeps the two costs from being paid one
        // after the other in the same second.
        //
        // It is NOT a "run Init once" flag: re-running Init() is legal, and is what binds a newly loaded
        // scene to the renderer (SceneRenderer::Init — the renderer half is once, the scene half is every
        // time). Only this first, pre-empted one is skipped, and the deferred-load site in OnUpdate is what
        // guarantees the scene ends up initialised even if the load refuses the file.
        //
        // Propagated rather than reported: OnAttach owns a channel and Application::PushLayer now reads
        // it, and an editor whose main scene never initialised has no viewport to show anything in.
        if ( !m_SceneFiles.HasPendingLoad() && !m_SceneFiles.HasPendingNew() )
        {
            if ( const auto inited = m_Workspace.ActiveScene()->Init(); !inited.IsSuccess() )
                return Common::MakeFormattedError( "main scene failed to initialise: {}", inited.GetError() );
        }

        // EVERY TOOL ENTERS THROUGH PanelRegistry::Add / Adopt, and that is the whole of the guarantee that
        // the View menu lists tools only: the registry REFUSES an ISubjectDocument at compile time, so a
        // document cannot be here to be listed. See Editor/Core/PanelRegistry.hpp.
        // THE PALETTE'S PROVIDERS, in palette order: the order IS the palette's order of groups and the control
        // channel's list. See Editor/LevelEditor/LevelEditorCommands.hpp.
        m_LevelCommands.RegisterProviders();
        // THE TOOLS, in View-menu order (DockLayout.hpp, EditorPanels.cpp — UE: RegisterTabSpawner).
        {
            m_ThumbnailPool = std::make_unique<AssetThumbnailPool>( m_AssetManager.get() );
            m_Startup.AttachThumbnailPool( m_ThumbnailPool.get() );
            const EditorPanelHandles handles =
                 RegisterEditorPanels( m_Panels, m_Workspace, m_Play, m_Documents, m_AssetManager,
                                       m_AnimationLibrary, *m_ThumbnailPool );
            m_FileExplorerPanel              = handles.FileExplorer;
            m_WorldPartitionPanel            = handles.WorldPartition;
        }

        // ── WHICH EDITOR OPENS WHICH KIND OF SUBJECT, AND HOW A PATH BECOMES ONE OF THEM ──────────────────
        //
        // The registrations live beside the asset editors, in Editor/LevelEditor/AssetEditorRegistrations.cpp
        // (UE: each asset editor registers its AssetTypeActions with the AssetTools registry). The registry they
        // fill is DocumentHost's; the order is the file's and is unchanged.
        RegisterAssetEditors( m_Documents, m_Workspace, m_AssetManager, m_AnimationLibrary );

        // NOTHING OPENS A PANEL AT BOOT ANY MORE, and the absence is the point.
        //
        // `--open-panel <name>` stood here: a flag that put a tool on screen because macOS refuses this
        // machine synthetic input, so there was no other way to photograph one. It could only ever act
        // ONCE, at startup, which is all a flag can do — and every task that needed a different window
        // added another flag beside it.
        //
        // The control channel replaces the whole family. "Panel" / "Open Details" is a command palette
        // entry, so it is reachable by a person with Ctrl+P and by a client at any moment in the session,
        // as many times as it likes. See LevelEditorCommands::BuildPaletteCommands and Editor/Core/Control.

        // Only when the scene above really was initialised. Every editor pass builds its pipeline
        // against `scene->GetTargetFramebuffer()`, which does not exist until SceneRenderer::Init has
        // run — and when a scene load is already queued that Init is deliberately skipped (see the
        // comment beside it). The load recreates this registry after its own Init, which is what the
        // three other call sites of this line are for.
        if ( m_Workspace.ActiveScene()->IsInitialized() )
            m_Workspace.RebuildRenderRegistry();

        // THE FIRST SAMPLE IS TAKEN BEFORE THE FIRST FRAME, and without it the readiness gate is wrong in
        // the one case it exists for. ServiceControlChannel runs at the TOP of OnUpdate and judges the
        // sample from the frame before; on frame zero there is no frame before, so a default-constructed
        // EditorQuiescence would answer Settled() — every flag false — and the very first request of a
        // session, which is the one a client sends while the editor is still cooking, would be answered
        // from an editor that has read nothing. An unsampled census must not read as a settled editor.
        m_Control.SampleFrameQuiescence( m_Startup.StartupLoading() || m_Startup.ContentSettling() );

        return BOOLSUCCESS;
    }

    [[nodiscard]] Common::BoolResultStr EditorLayer::OnUpdate( const Common::Timestep& ts )
    {
        DESERT_PROFILE_SCOPE( "Layer::OnUpdate" );

        // THE CONTROL CHANNEL GOES FIRST, ahead of the startup-loading return below and not after it.
        //
        // A client connects while the editor is still cooking assets — that is the normal case, since it
        // launched the process — and a channel that only started answering once loading finished would
        // look, from the outside, exactly like an editor that had hung. It answers `state` throughout, so
        // the client can watch the boot; anything that changes the picture is held by the quiescence gate
        // until the staged load is done, which is what PendingWork::StartupLoading is for.
        // THE NUDGE CLOCK, AND IT RUNS BEFORE THE CHANNEL DELIBERATELY. A request queued by a command
        // this frame must not be aged by the same frame's tick, or the drop that protects the reply gate
        // would fire a frame early and a slow viewport would lose a nudge it was about to perform.
        Core::ControlNudgeRequests::Tick();

        m_Control.ServiceChannel();

        // THE EDITOR'S FRAME BOUNDARY FOR Core::OpenLevel (UEngine::TickWorldTravel for the PIE world context).
        // In Play the queued level replaces the PLAYED world and Stop still returns to the authored one
        // (PlayWorldTravel); outside Play there is no game world and the request is refused with its target.
        if ( const auto travelled = m_Play.ServiceTravel(); !travelled )
        {
            LOG_ERROR( "[Editor] {}", travelled.GetError() );
            Editor::ToastManager::Push( travelled.GetError(), Editor::ToastLevel::Warning, 5.0f );
        }

        // A New Landscape run that finished on the JobSystem is applied here, on the main thread and ahead of
        // this frame's scene update, as one undo step. A cancel is the user's own act, so it is told, not flagged.
        if ( auto created = Commands::FinishCreateLandscape() )
        {
            if ( created->IsSuccess() )
                Editor::ToastManager::Push( "New Landscape created", Editor::ToastLevel::Info, 3.0f );
            else if ( created->GetError() == World::Landscape::kLandscapeGenerateCancelled )
                Editor::ToastManager::Push( "New Landscape cancelled", Editor::ToastLevel::Info, 3.0f );
            else
                Editor::ToastManager::Push( created->GetError(), Editor::ToastLevel::Error, 6.0f );
        }

        // Staged startup loading (EditorStartup::RunStartupFrame): ONE heavy stage per frame, and while it runs
        // the scene is NOT rendered — the frame is ImGui-only and the window it goes to is still hidden.
        if ( m_Startup.RunStartupFrame() )
        {
            m_Control.SampleFrameQuiescence( m_Startup.StartupLoading() || m_Startup.ContentSettling() );
            return BOOLSUCCESS;
        }

        // A scene handed over by a panel (dropped on the viewport, double-clicked in the asset browser).
        // It goes through the SAME deferred load as the menu — but a drag is easy to do by accident, so
        // unsaved work is not thrown away silently: the confirm popup decides, and only then do we queue.
        m_SceneFiles.ConsumeOpenRequest();

        // ONE PUMP PER TICK, AND IT IS THE FIRST THING THIS LAYER DOES AFTER THE BOOT.
        //
        // `AsyncAssetLoader` never calls a delegate from inside `Request()` -- not even for an asset that
        // is already resident -- so a host that forgets this line gets a loader that reads files and
        // never tells anybody. It is first rather than last because a completion is what UPLOADS a cloud
        // volume, and doing that before the frame's passes resolve their inputs is what lets the volume
        // be used by the same frame it arrived in rather than by the next one.
        Assets::AsyncAssetLoader::Get().Pump();
        // The environment cache's readbacks land here and go to a worker for the encode (AL1-3).
        Graphic::EnvironmentCacheWriter::Get().Pump();
        m_Startup.UpdateContentSettling();
        m_AssetCompiling.DrainBackgroundCook();

        // Scene loads wait until the startup stages finished (a scene expects cooked/preloaded assets).
        // A load that ran starts the content settle before the refused-load fallback and New Scene.
        if ( !m_Startup.StartupLoading() )
        {
            if ( m_SceneFiles.ServiceLoadRequest() )
            {
                m_Startup.BeginContentSettle();
                m_SceneFiles.InitializeIfLoadRefused();
            }
            m_SceneFiles.ServiceNewRequest();
        }

        // Opening an extra scene view, a second angle or the four-up grid allocates a SceneRenderer + Init()
        // (WaitDeviceIdle + framebuffer creation) — serviced here, between frames, like scene load/stop above.
        if ( !m_Startup.StartupLoading() )
            m_Workspace.ServiceRequests();

        // ...and closing one destroys the same resources, so it is deferred to the same place. It must also
        // run BEFORE the OnPreUpdate loop below and before UpdateSceneFrame: a document whose window the user
        // dismissed last frame would otherwise get one more full scene render, and — until this existed at
        // all — every subsequent frame for the rest of the session, holding one of the six renderer slots.
        // A playing document ends Play first (UE closes a level after EndPlayMap), then the document goes.
        for ( const uint64_t id : m_Workspace.DismissedSceneViews() )
        {
            m_Play.EndIfBoundTo( id );
            m_Workspace.CloseSceneView( id );
        }
        m_Workspace.CloseDismissedSceneViewports();

        // A DOCUMENT WHOSE SUBJECT HAS GONE IS QUEUED FOR CLOSING BEFORE THE QUEUE IS SERVICED, so the
        // window disappears on the same frame the entity or the asset did rather than one later. It runs
        // after CloseDismissedSceneViews above deliberately: closing a scene view is one of the ways a
        // subject dies, and a document over an entity in that scene must see the scene gone, not still
        // going. See CloseDocumentsWhoseSubjectIsGone.
        m_Documents.CloseDocumentsWhoseSubjectIsGone();

        // Documents follow the scene views exactly, and for the same reason: closing one destroys a Scene
        // and a SceneRenderer, neither of which is legal from inside the ImGui pass that noticed the click.
        // Closes run BEFORE opens so a slot handed back this frame is available to whatever the user is
        // opening in it. The hidden-document slot release rides in the same function, behind the same
        // device-idle wait, for the same reason.
        m_Documents.ServiceDocumentCloses();
        m_Documents.ServiceSubjectOpenRequests();

        // Stop is deferred here (between frames) so it never destroys/recreates render resources while a
        // command buffer that references them is in flight — see PlaySession::RequestStop.
        m_Play.ServiceRequests();

        // The autosave timer (SessionRecovery::Tick).
        m_Recovery.Tick( ts.GetSeconds() );

        // Apply any deferred panel state (e.g. viewport resize) before scene rendering.
        // Panels defer GPU-side resize from OnUIRender to here so descriptor set pools are
        // never destroyed while their DS are bound to the recording command buffer.
        for ( auto& panel : m_Panels )
            panel->OnPreUpdate();
        // The documents get the same call, from their own owner. Two loops rather than one is the visible
        // cost of the split, and it is the cost that buys "the View menu cannot list a document": every
        // place that used to iterate one container now names which of the two it means.
        for ( auto& document : m_Documents.Documents() )
            document->OnPreUpdate();

        // IS THIS FRAME A FRAME OF THE CAPTURE? Decided ONCE, here — after every deferred load and resize of
        // this frame has been applied, before anything ticks — and read by both halves of a headless run: the
        // world (Play starts on the first recorded frame, and under `--play` steps only on recorded frames)
        // and the writer below. Frame N of the sequence is tick N of game time (ShotRecordGate.hpp).
        const bool shotRecorded = AdmitShotFrame();

        // The timestep this frame is driven by. Identical to the wall-clock one the application measured,
        // EXCEPT under a `--play` capture, where it is the fixed step from ShotOptions on a recorded frame
        // and zero on any other.
        //
        // Substituted for the whole layer update rather than only for the scene: a capture is reproducible
        // only if nothing in it integrates a number that came from a clock, and "the scene is deterministic
        // but the thing above it is not" is the kind of split that holds until the day something above it
        // starts feeding the scene. Outside `--play` this is `ts` itself, so no existing frame moves.
        const Common::Timestep frameTs( ShotOptions::Get().FrameSeconds( ts.GetSeconds(), shotRecorded ) );

        // THE WORLDS INSIDE UI RENDER-TEXTURE ELEMENTS, advanced HERE and nowhere else (Ю16). Each one is
        // a whole scene render, and it has to be recorded before ANY pass of this frame opens: the canvas
        // walk that samples the result runs inside the UI external pass, and Vulkan has no nested render
        // pass. Same constraint, same position in the frame, as PreviewViewport::Update — which says so in
        // its own header after the editor was bitten by the descriptor-pool version of it.
        //
        // Every open document, not only the focused one, for the same reason UpdateSceneFrame below runs
        // for every one: a secondary viewport showing a canvas is a live view, and a render-texture
        // element in it that stopped being advanced would show a frozen world with nothing in the log.
        m_Workspace.TickRenderTextures( frameTs );

        // The one thumbnail pump, gated by the reveal (EditorStartup::TickThumbnails).
        m_Startup.TickThumbnails();

        m_Dock.UpdateContextualPanels();

        // Asset hot-reload: pick up edited .demat/.shader files (runs BEFORE scene rendering so
        // a shader-triggered pipeline invalidation never touches an in-recording frame).
        m_AssetCompiling.TickHotReload( frameTs, m_Workspace.ActiveScene().get() );

        // Destroy invalidated runtime materials (shader switched in the editor / hot reload) at
        // the only safe point: before any command recording, behind a device-idle wait. Doing it
        // where Invalidate() is called (mid-UI, mid-recording) kills descriptor pools the current
        // command buffer references -> device lost.
        if ( auto* materialService = Runtime::ResourceRegistry::GetMaterialService() )
            materialService->CollectGarbage();

        // Advance + upload any playing UI videos at this safe point (behind the device-idle wait, before
        // command recording) so authored videos animate live in the viewport; SetData flushes its own
        // transfer, and the UI walk later just samples the freshly-updated frame texture.
        if ( auto* videoService = Runtime::ResourceRegistry::GetVideoService() )
            videoService->UpdateAll();

        // Screenshot mode (ShotDirector): `--play` starts the world, then the camera is placed for the frame about
        // to be rendered.
        m_Shots.BeginPlayIfDue( shotRecorded );
        m_Shots.PlaceCamera( m_Startup.StartupLoading() );

        // WAS ANYTHING STILL OUTSTANDING WHEN THIS FRAME WAS MADE? Sampled HERE, and the position is the
        // whole of its meaning: after every deferred queue above has drained — scene loads, document
        // closes, asset opens, leaving Play — and before a single pixel of this frame is rendered.
        //
        // Sampled rather than asked for later, because by the time the frame has been presented the
        // answer has moved on, and the question the control channel needs answered is about the picture:
        // "did this frame have everything the command asked for in it, or was some of it still queued?"
        // Editor/Core/Control/ControlPipeline.hpp is where that question is judged.
        m_Control.SampleFrameQuiescence( m_Startup.StartupLoading() || m_Startup.ContentSettling() );

        // Multi-scene editing: drive EVERY open document each frame so all viewports render live. The active
        // one is m_Workspace.ActiveScene() (rebound on viewport focus); RigBuilder below acts on it only. The
        // outline aid + Begin/RegistryRender/OnUpdate/End are folded into UpdateSceneFrame (see below), applied
        // per scene so a secondary viewport is a full, independent render — not a static snapshot.
        // (SceneWorkspace::TickWorlds; UE: UEditorEngine::Tick's loop over the WorldContexts.)
        if ( auto ticked = m_Workspace.TickWorlds( frameTs, m_Play, m_Shots.RecordingThisFrame() ); !ticked )
            return ticked;

        // Runs a queued "Convert to Skinned" (rig builder) here, outside ImGui component iteration — the swap
        // removes the StaticMeshComponent the Details panel is drawing, so it must not happen mid-render.
        if ( m_Workspace.ActiveScene() && m_AssetManager )
            RigBuilder::ProcessPending( *m_Workspace.ActiveScene(), *m_AssetManager );

        // Screenshot mode, END OF FRAME (ShotDirector::EndFrame): the --flight sample, and on the capture's last
        // frame the profiler dump, the --flight CSV and the capture's status to close with.
        if ( const auto finished = m_Shots.EndFrame( shotRecorded, m_Startup.StartupLoading(),
                                                     m_Startup.ContentSettling(), m_Profiler ) )
            m_Application->Close( *finished );

        return BOOLSUCCESS;
    }

    // The frame is out: the start-up's reveal, then the control channel keeps its promise (ControlService) — a
    // `quit` whose reply has gone out comes back as the status to close with.
    bool EditorLayer::AdmitShotFrame()
    {
        ShotFrameConditions frame;
        frame.SceneLoadPending = m_SceneFiles.HasPendingLoad();
        frame.StartupLoading   = m_Startup.StartupLoading();
        frame.SplashOnScreen   = m_Startup.SplashOnScreen();
        frame.ContentSettling  = m_Startup.ContentSettling();
        // The picture the writer reads back (ViewportCapture), at the size this frame renders at: the panels'
        // deferred resizes were applied just before this call.
        if ( m_Workspace.ActiveScene() )
            if ( const auto img = m_Workspace.ActiveScene()->GetFinalImage() )
            {
                frame.ViewportWidth  = img->GetWidth();
                frame.ViewportHeight = img->GetHeight();
            }
        return m_Shots.AdmitFrame( frame );
    }

    void EditorLayer::OnFramePresented()
    {
        m_Startup.RevealWhenReady();
        if ( const auto quit = m_Control.OnFramePresented() )
            m_Application->Close( *quit );
    }

    Common::BoolResultStr EditorLayer::ShowFolderInBrowser( const std::string& folder )
    {
        if ( m_FileExplorerPanel == nullptr )
            return Common::MakeFormattedError<bool>(
                 "the Assets browser does not exist in this session; '{}' cannot be shown", folder );
        Core::PanelRequests::Open( "Assets" );
        if ( !m_FileExplorerPanel->NavigateToPath( folder ) )
            return Common::MakeFormattedError<bool>( "'{}' is not a folder the Assets browser can reach", folder );
        return PaletteCommandDone();
    }

    void EditorLayer::RequestEditorExit()
    {
        m_Documents.AskCloseAll( [this] { m_Application->Close( 0 ); } );
    }

    Common::BoolResultStr EditorLayer::OnUIRender()
    {
        // The ImGui frame and ImGuizmo's (EditorImGuiHost::BeginFrame).
        m_ImGuiHost.BeginFrame();

        SyncEditorWindowTitle( *m_Application, m_Workspace.ActiveScene().get() );

        // LOADING FRAMES DRAW NOTHING. They go to a window that is still hidden — the splash is what a
        // person sees until the start is over — and there is no dockspace or panel to draw yet (the
        // viewport panel would touch the not-yet-rendered scene image). The fullscreen ImGui overlay that
        // stood here was replaced by the splash, and deleted with the same change.
        if ( m_Startup.StartupLoading() || m_Startup.ContentSettling() )
        {
            m_ImGuiHost.EndFrame();
            return BOOLSUCCESS;
        }
        // NOT YET REAL WHILE A SCENE LOAD IS STILL QUEUED. The frame right after the last stage draws the
        // editor over an empty scene — the queued load runs in the NEXT OnUpdate and only then starts the
        // settle wait — and revealing on it showed the window 3 s before the content had settled
        // (measured on the first run of this change: "on screen" logged before "[Content] settled").
        if ( !m_SceneFiles.HasPendingLoad() && !m_SceneFiles.HasPendingNew() )
            m_RealFrameDrawn = true;

        // The level's global shortcuts (Ctrl+Z/Y/D/C/V/N/R/S, Ctrl+P, Ctrl+Tab), at frame start, before any panel
        // iterates the scene. See Editor/LevelEditor/LevelEditorCommands.hpp.
        m_LevelCommands.HandleShortcuts( ::ImGui::GetIO() );

        // The menu bar, which is the window's title bar (MainMenu::DrawBar).
        m_MainMenu.DrawBar( m_Toolbar, m_Profiler, m_ImGuiHost.Chrome() ? &*m_ImGuiHost.Chrome() : nullptr );

        m_Dock.BeginHost();

        // Toolbar strip FIRST so it reserves its height at the top; the DockSpace below then fills the
        // remaining area (drawing it after a full-height DockSpace(0,0) would push the bar off-screen).
        m_Toolbar.Draw();

        m_Dock.DrawDockSpace();

        m_Dock.DrawPanels();
        m_Dock.RouteEvents( Events(), EventNode() );

        // The well BEFORE the documents: it reads back the dock node id the documents are about to be
        // docked into, and a document opened this frame would otherwise float once and settle next frame.
        if ( !m_Documents.MajorTabActive() )
            m_Documents.DrawDocumentWell();
        m_Documents.DrawDocuments();

        // EVERY VIEW HAS NOW HAD ITS TURN — the tool panels above (the Clouds window is one of them) and
        // the document well's own strip. Only here can "nobody drew this document" be answered, which is
        // why the run of undrawn frames is closed at this point and not inside either draw loop.
        m_Documents.Documents().EndFrame();

        if ( !m_Documents.MajorTabActive() )
            m_Profiler.DrawProfilerWindow();

        m_StatusBar.Draw();

        m_LevelCommands.DrawPalette();
        m_Dock.DrawRecoveryPopup();
        m_Dock.DrawLayoutSavePopup();
        m_Documents.DrawOpenRefusedPopup();
        ImportOptions::DrawWindow(); // a dropped file never imported asks for its options first (THM1l)
        m_Documents.DrawCloseQuestionPopup();

        // Transient bottom-right notifications (save/import/validation). Drawn last so they float on top.
        Editor::ToastManager::Get().Draw();

        DockLayout::EndHost();

        // The edges the OS frame used to give us. LAST, and outside the dockspace host: these are eight
        // 6px windows of their own, and submitting them here is what puts them above the panels that reach
        // the screen edge. A no-op while the window is maximized, and absent entirely when the OS draws
        // the frame.
        m_ImGuiHost.DrawResizeBorders();

        m_ImGuiHost.EndFrame();

        // AFTER the interface has been recorded into the swapchain pass and BEFORE the frame is submitted:
        // the only window in which the presented image is legally ours to copy out of. A no-op unless a
        // `shot.window` is waiting on exactly this frame. See RecordWindowCaptureIfDue.
        m_Control.RecordWindowCaptureIfDue();

        return BOOLSUCCESS;
    }

    Common::BoolResultStr EditorLayer::OnDetach()
    {
        m_Subsystems.reset();
        m_ImGuiHost.UninstallCloseGate();
        // The socket goes first, and its file with it (ControlService::Close).
        m_Control.Close();
        if ( const auto kept = m_Dock.KeepLayoutAcrossQuit(); !kept.IsSuccess() )
            LOG_ERROR( "[Layout] the layout from before the maximize was not saved: {}", kept.GetError() );

        // The one emergency save after a device loss, then the lock drop of a clean shutdown (SessionRecovery).
        m_Recovery.SaveOnDeviceLost();
        SessionRecovery::Disarm();

        // The launcher's tile picture, refreshed on the way out — HERE, while the device and the
        // scene's final image still exist. Everything below this point is teardown; a few lines
        // further down there is a WaitDeviceIdle and the release of exactly the GPU objects this
        // readback needs.
        //
        // Not on an UNATTENDED run: those open scratch projects in worktrees, and the picture would
        // be of a scene nobody chose, written into a project nobody will open. Same rule, and the
        // same reason, as staying out of the recent-projects registry — and the same correction:
        // this asked `shot.Active()` and therefore missed every control-channel session, which is
        // the unattended path that no longer needs a capture flag at all. See
        // Editor/Core/CommandLine.hpp::IsUnattendedSession.
        //
        // A failure is logged and nothing else. Refusing to shut down because a picture could not
        // be written would be the tail wagging the dog.
        if ( !Editor::IsUnattendedSession( Editor::ShotOptions::Get(),
                                           Control::ControlChannelOptions::Get().Requested() ) )
            if ( const auto thumbnail = m_Capture.WriteProjectThumbnail(); !thumbnail.IsSuccess() )
                LOG_WARN( "[Project] the tile thumbnail was not written on exit: {}", thumbnail.GetError() );

        // The app loop exits right after the last PresentFinalImage, so the GPU is still chewing on that
        // frame's command buffer. Panels own GPU objects — offscreen SceneRenderers (Details preview, asset
        // thumbnails, node-graph preview), framebuffers, descriptor pools — and destroying those while that
        // buffer is in flight is what produced the "can't be called on VkPipeline/VkDescriptorPool ... that
        // is currently in use by VkCommandBuffer" validation errors on quit. Idle first, then tear down.
        Graphic::Renderer::GetInstance().WaitDeviceIdle();

        // The thumbnail service is NOT one of the panels, and the sentence above is why that matters: it
        // names "asset thumbnails" among the GPU objects this teardown covers, but m_Panels.clear() cannot
        // reach a function-static that the panels merely talk to. Left to itself the service is destroyed at
        // __cxa_finalize, long after the device is gone, and ~AssetThumbnailRenderer's WaitDeviceIdle() then
        // dereferences a null renderer API — exit 139, measured. Released here, deterministically, while
        // there is still a device to wait on. See ThumbnailService::Shutdown().
        //
        // Before the panels rather than after: a panel's own teardown must never be able to queue one last
        // preview into a service that has already let its renderer go.
        ThumbnailService::Get().Shutdown();
        // The same reason for the environment cache: its readbacks own staging buffers and command buffers.
        Graphic::EnvironmentCacheWriter::Get().Drain();

        // The SECOND half of the same problem, and the half the sentence above still does not cover: the
        // component widgets keep their thumbnail caches in function-statics (StaticMeshComponent.cpp,
        // SkinnedMeshComponentWidget.cpp), so those GPU images belong to no panel and no service. Cleared
        // here for the same reason and at the same moment. See ThumbnailCache::ReleaseAll().
        ThumbnailCache::ReleaseAll();

        // Documents BEFORE tools, and both before the ImGui layer: a document owns a PreviewViewport whose
        // UIHelper holds descriptor sets, and the device has already been idled above. Explicit rather than
        // left to ~EditorLayer, which runs after the layer stack has moved on.
        (void)m_Documents.Documents().ReleaseAll();
        m_Panels.Clear();
        // EVERY ALIAS OF A PANEL DIES WITH THE PANEL, and this line is the half of that CloseSceneView
        // already had and OnDetach did not. `m_Panels.Clear()` destroys every panel while
        // m_FileExplorerPanel and every SceneDocument::Viewport still name one; nothing between here and
        // the end of OnDetach reads them today, so this was latent rather than live — and "nothing reads
        // it today" is the weakest guarantee in this audit, because it is about the code that exists
        // rather than about the code. A8-2.
        m_FileExplorerPanel = nullptr;
        // The pool the browser drew from goes right after the browser, at the moment it went before (C1).
        m_Startup.AttachThumbnailPool( nullptr );
        m_ThumbnailPool.reset();
        m_WorldPartitionPanel = nullptr;
        // Reported and not returned even though OnDetach has a channel: everything below this line still
        // has to run, and an early return would leave the extra documents and their render slots alive.
        m_ImGuiHost.Detach();

        // A scene names its views' renderers by raw pointer and owns none of them, so every renderer dies
        // after the scene that names it (FIX6) — the order is SceneWorkspace::Teardown's.
        m_Workspace.Teardown();

        return BOOLSUCCESS;
    }

} // namespace Desert::Editor
