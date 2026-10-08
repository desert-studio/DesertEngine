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
#include "Editor/Core/EditorResources.hpp"
#include "Editor/Core/ThemeManager.hpp"
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
#include <ImGuizmo.h>
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
    namespace
    {
        // MESHES COOKED BEFORE THEIR HEADER STATED A BOX (MeshBinaryHeader.hpp): the gather reads headers
        // only and cannot learn their box, so the editor — which links the mesh reader — reads each body
        // ONCE and hands the box to the registry, whose local cache keeps it from then on. Said in one line
        // naming them, because a re-cook is what makes the read unnecessary.
        void NoteBoundsOfMeshesCookedWithoutThem( const std::vector<std::string>& keys )
        {
            if ( keys.empty() )
                return;
            std::string named;
            for ( const std::string& key : keys )
            {
                named += named.empty() ? key : ", " + key;
                const std::filesystem::path file = Common::AssetHandle::PathForStableKey( key );
                // The render-form bytes through the DDC, not the file at the key: an imported mesh has no
                // `.stmesh` of its own since AF4h (its row comes from the import record, FIX8), and the
                // DDC answers for it and for an authored `.stmesh` alike.
                const auto bytes = Assets::LoadMeshPlatformData( file );
                if ( !bytes )
                {
                    LOG_ERROR( "[ContentRegistry] '{}' could not be read for its box: {}", key, bytes.GetError() );
                    continue;
                }
                const auto mesh = Assets::Serialization::ReadMeshAssetData( bytes.GetValue(), file.string() );
                if ( !mesh )
                {
                    LOG_ERROR( "[ContentRegistry] '{}' could not be decoded for its box: {}", key,
                               mesh.GetError() );
                    continue;
                }
                Assets::ContentRegistry::NoteBounds( file,
                                                     Assets::Serialization::MeshDataBounds( mesh.GetValue() ) );
            }
            LOG_WARN( "[ContentRegistry] {} mesh(es) state no box in their header, so their bodies were read once "
                      "(the local cache keeps the boxes); re-cook them to drop the read: {}",
                      keys.size(), named );
        }
    } // namespace

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

        // Launched with --project (Project Hub): adopt the project's name and queue its default scene
        // (loaded through the normal deferred path on the first frame, when the renderer is ready).
        // Startup content is DATA: a template's DefaultScene ships in its Payload (Templates/Starter), and the
        // launcher refuses a template that names a scene it does not carry. A DefaultScene that is not on disk
        // is therefore an error naming the path, never a scene built in code. With no scene to open, the
        // editor opens the Basic level template as an untitled scene (UE: EditorStartupMap / TemplateMapInfos).
        // Screenshot mode names its own scene; it is the whole point of the flag.
        if ( ShotDirector::NamesScene() )
        {
            if ( const auto refused = m_Shots.QueueScene() )
                m_Application->Close( *refused );
        }
        else if ( ProjectContext::HasProject() )
        {
            m_Workspace.ActiveScene()->SetSceneName( ProjectContext::Current().Name );
            if ( const auto scenePath = ProjectContext::DefaultScenePath(); !scenePath.empty() )
            {
                if ( std::filesystem::exists( scenePath ) )
                    m_SceneFiles.RequestLoad( scenePath );
                else
                {
                    LOG_ERROR( "[Editor] The project's DefaultScene '{}' does not exist — opening an untitled "
                               "scene instead. Restore the file or point DefaultScene in the .deproj at a scene "
                               "that is there.",
                               scenePath );
                    Editor::ToastManager::Push( "The project's default scene is missing (see the log)",
                                                Editor::ToastLevel::Error );
                }
            }
        }
        // Nothing to open: the Basic level template, as an untitled scene (SceneFiles::NewSceneInternal).
        if ( !m_SceneFiles.HasPendingLoad() )
            m_SceneFiles.RequestNew();

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

        // THE WINDOW FRAME, IF THIS EDITOR OWNS IT. Asked of the window rather than assumed from the
        // ApplicationInfo that requested it: a fullscreen-over-the-taskbar window is frameless whatever was
        // asked for, and the window is the one that knows what actually happened (Window::IsDecorated).
        if ( const auto& window = m_Application->GetWindow(); window && !window->IsDecorated() )
        {
            m_WindowChrome.emplace(
                 *window,
                 // The same ordered close the control channel's `quit` takes: Run() leaves its loop, every
                 // layer is detached, the device goes idle. Two ways to end a session would drift.
                 [this]() { RequestEditorExit(); } );
        }

        // THE OS FRAME'S CLOSE ASKS WHAT File -> Exit ASKS. The application no longer stops on the event
        // itself: RequestEditorExit either closes at once (nothing dirty) or raises the Save / Don't Save /
        // Cancel questions and closes after the last one; Cancel leaves the editor running, so the
        // platform's should-close flag is cleared here rather than left set behind a live window.
        m_Application->GetCloseGate().Install(
             [this]()
             {
                 RequestEditorExit();
                 if ( const auto& window = m_Application->GetWindow() )
                 {
                     // GLFW takes back the handle Window hands out as const void*.
                     // NOLINTNEXTLINE(bugprone-casting-through-void,cppcoreguidelines-pro-type-const-cast)
                     auto* native = static_cast<GLFWwindow*>( const_cast<void*>( window->GetNativeWindow() ) );
                     glfwSetWindowShouldClose( native, GLFW_FALSE );
                 }
                 return false;
             } );

        // 1. Create ImGui Context first
        ::ImGui::CreateContext();

        // 2. Initialize Editor Resources (Adds fonts to the atlas)
        Editor::EditorResources::Initialize( UI::IconFontFile().string() );

        // 3. Initialize Engine ImGui Layer (Initializes backend and uploads fonts)
        m_ImGuiLayer = ImGui::ImGuiLayer::Create();
        if ( const auto attached = m_ImGuiLayer->OnAttach(); !attached.IsSuccess() )
            return Common::MakeFormattedError( "ImGui layer failed to attach: {}", attached.GetError() );

        ImGuiIO& io = ::ImGui::GetIO();
        (void)io;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard; // Enable Keyboard Controls
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;  // Enable Gamepad Controls
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;     // Enable Docking
        io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;   // Enable Multi-Viewport / Platform Windows

        // THE DOCKING LAYOUT FILE, OFF THE PROJECT (DockLayout::BindLayoutFile).
        if ( const auto bound = DockLayout::BindLayoutFile(); !bound.IsSuccess() )
            return Common::MakeError( bound.GetError() );

        // Before the first frame, which is when ImGui reads the layout file.
        m_Documents.RegisterDocumentWellLayoutHandler();
        m_Documents.RegisterDocumentPlacementHandler();

        // Setup ImGui style
        ThemeManager::SetDarkTheme();

        // When viewports are enabled we tweak WindowRounding/WindowBg so platform windows can look identical to
        // regular ones
        ImGuiStyle& style = ::ImGui::GetStyle();
        if ( io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable )
        {
            style.WindowRounding              = 0.0f;
            style.Colors[ImGuiCol_WindowBg].w = 1.0f;
        }

        // THE COOKED ASSET REGISTRY, BEFORE ANY CONTENT IS ASKED FOR, including the engine shaders a few
        // lines down, the earliest content this host creates. Every kind resolves its references through
        // the registry rows, so anything asked before the file was read would come back empty; and
        // the boot's cook stages call `ContentRegistry::NoteFile` as they write, which would be writing
        // into rows that `Load` was about to replace.
        //
        // A REFUSAL ENDS THE RUN, on the terms §1.4 sets: an editor that starts with a registry it
        // could not parse is an editor showing an empty Content Browser over a project full of files,
        // and "looks almost right" is the failure mode that costs the most to find.
        std::vector<std::string> unboxedMeshes;
        const auto               registry = Assets::ContentRegistry::Gather( nullptr, &unboxedMeshes );
        if ( !registry )
            return Common::MakeFormattedError( "the cooked asset registry: {}", registry.GetError() );
        LOG_INFO( "[ContentRegistry] {} row(s), {} handle(s) bound before anything was loaded",
                  Assets::ContentRegistry::Get().Count(), registry.GetValue() );
        NoteBoundsOfMeshesCookedWithoutThem( unboxedMeshes );

        // The committed registry file is gone (AF9): nothing reads it, and a developer tree may still hold
        // the last copy, untracked. It is harmless — said once so nobody mistakes it for the live registry.
        if ( const std::filesystem::path stale = Common::Constants::Path::CurrentProjectRoot().ProjectDir /
                                                 Common::Constants::Path::COOKED_DIR_NAME / "AssetRegistry.dreg";
             Common::Utils::FileSystem::Exists( stale ) )
            LOG_WARN( "[ContentRegistry] '{}' is a stale file from before the registry was gathered at start; "
                      "nothing reads it and it can be deleted",
                      stale.string() );

        // Shaders must exist BEFORE the render systems below are constructed (their default materials
        // resolve shaders in the ctor). Meshes/skyboxes are staged instead. The longest single wait of the
        // start, and one call: the splash says what it is before it begins, and cannot say more during it.
        m_Startup.BeginShaderStage();
        // The splash's close button, pressed during this one long call, stops it between programs.
        if ( const auto shaders = Assets::CompileEngineShaders( m_AssetManager, m_Startup.SplashItems(),
                                                                [this]() { return m_Startup.CloseRequested(); } );
             !shaders )
            return Common::MakeFormattedError( "the engine shaders: {}", shaders.GetError() );
        // The imported materials choose among these shaders' Import blocks; every cook below comes after.
        LOG_INFO( "[Import] {} import template(s) published from the loaded shaders",
                  ImportManager::PublishImportTemplates( *m_AssetManager ) );

        m_Workspace.BuildSceneSystems( *m_Workspace.ActiveScene() );

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
        // THE PALETTE'S PROVIDERS, in palette order (CommandRegistry.hpp). Each subject-owned provider reads the
        // main-scene / asset-manager SLOT when an entry runs, so re-pointing the slot needs no re-registration.
        // The order of these calls IS the palette's order of groups (and the control channel's list).
        {
            const auto camera = [this] { return m_Workspace.ActiveEditorCamera(); };
            m_EntityCommands  = std::make_unique<EntityCommands>( m_Workspace.ActiveScene(),
                                                                  m_Documents.SubjectEditors(), camera );
            m_AssetCommands   = std::make_unique<AssetCommands>(
                 m_FileExplorerPanel, m_WorldPartitionPanel, m_Workspace.ActiveScene(), m_AssetManager,
                 m_PaletteAssetFiles, camera,
                 [this]( const std::string& folder ) { return ShowFolderInBrowser( folder ); } );
            using Out = std::vector<PaletteCommand>;
            m_Commands.OnBuildBegin( [this] { m_PaletteAssetFiles.Take(); } );
            m_Commands.Register( "Panels", [this]( Out& out ) { m_Dock.AppendPanelCommands( out ); } );
            m_Commands.Register( "Debug (crash)", []( Out& out ) { AppendCrashCommand( out ); } );
            m_Commands.Register( "Assets (selection)",
                                 [this]( Out& out ) { m_AssetCommands->AppendSelectionCommands( out ); } );
            m_Commands.Register( "Panels (maximize)",
                                 [this]( Out& out ) { m_Dock.AppendMaximizeCommands( out ); } );
            // Details: scroll to a field / open an asset picker, as the last Details frame drew them (CTL2).
            m_Commands.Register( "Details",
                                 []( Out& out )
                                 {
                                     for ( PaletteCommand& command :
                                           DetailsPaletteCommands( GetDetailsNavigation() ) )
                                         out.push_back( std::move( command ) );
                                 } );
            // Anti-aliasing method (AA1): the Scalability panel's choice, reachable from the control channel.
            m_Commands.Register( "Anti-aliasing",
                                 []( Out& out )
                                 {
                                     for ( PaletteCommand& command : AntiAliasingPaletteCommands(
                                                Common::Scalability::QualityState::Catalog() ) )
                                     {
                                         out.push_back( std::move( command ) );
                                     }
                                 } );
            m_Commands.Register( "Clouds", []( Out& out ) { AppendCloudCommands( out ); } );
            m_Commands.Register( "Language", []( Out& out ) { AppendLanguageCommands( out ); } );
            m_Commands.Register( "Documents", [this]( Out& out ) { m_Documents.AppendDocumentCommands( out ); } );
            m_Commands.Register( "Entity", [this]( Out& out ) { m_EntityCommands->Append( out ); } );
            m_Commands.Register( "Modeling (Mesh To Collision)", [this]( Out& out )
                                 { AppendMeshToCollisionCommands( out, m_Workspace.ActiveScene() ); } );
            m_Commands.Register( "Entity (collapse)",
                                 [this]( Out& out ) { m_EntityCommands->AppendCollapse( out ); } );
            m_Commands.Register( "Menu", [this]( Out& out ) { m_MainMenu.AppendMenuCommands( out ); } );
            m_Commands.Register( "Level viewport", []( Out& out ) { AppendViewportCommands( out ); } );
            m_Commands.Register( "Modeling (Select Elements)",
                                 []( Out& out ) { AppendSelectElementsCommand( out ); } );
            m_Commands.Register( "Landscape", [this]( Out& out )
                                 { AppendLandscapeCommands( out, m_Workspace.ActiveScene() ); } );
            m_Commands.Register( "Modeling (Create Shape)", []( Out& out ) { AppendCreateShapeCommands( out ); } );
            m_Commands.Register( "Humanoid", [this]( Out& out )
                                 { AppendHumanoidCommands( out, m_Workspace.ActiveScene() ); } );
            m_Commands.Register( "Scene (add shape)", [this]( Out& out ) { AppendAddShapeCommands( out ); } );
            m_Commands.Register( "Modeling", [this]( Out& out )
                                 { AppendModelingCommands( out, m_Workspace.ActiveScene() ); } );
            m_Commands.Register( "UI",
                                 [this]( Out& out ) { AppendUICommands( out, m_Workspace.ActiveScene() ); } );
            m_Commands.Register( "Palette", [this]( Out& out ) { AppendPaletteDoorCommand( out ); } );
            m_Commands.Register( "Assets (import)",
                                 [this]( Out& out ) { m_AssetCommands->AppendImportCommands( out ); } );
            m_Commands.Register( "Foliage",
                                 [this]( Out& out ) {
                                     AppendFoliageCommands( out, m_Workspace.ActiveScene(), m_AssetManager,
                                                            m_PaletteAssetFiles.Files() );
                                 } );
            m_Commands.Register( "Open", [this]( Out& out )
                                 { m_Documents.AppendOpenCommands( out, m_PaletteAssetFiles.Files() ); } );
            m_Commands.Register( "Assets (folders)",
                                 [this]( Out& out ) { m_AssetCommands->AppendFolderCommands( out ); } );
            m_Commands.Register( "Scene", []( Out& out ) { AppendSceneCommands( out ); } );
            m_Commands.Register( "Scene (new views)",
                                 [this]( Out& out ) { m_Workspace.AppendNewViewCommands( out ); } );
            m_Commands.Register( "Debug (GPU allocations)",
                                 []( Out& out ) { AppendGpuAllocationCommand( out ); } );
            m_Commands.Register( "Scene (view layout)",
                                 [this]( Out& out ) { m_Workspace.AppendViewLayoutCommands( out ); } );
            m_Commands.Register( "Scene (actions)", [this]( Out& out ) { AppendSceneTailCommands( out ); } );
            m_Commands.Register( "AssetCompiling",
                                 [this]( Out& out ) { m_AssetCompiling.AppendActionCommands( out ); } );
            // SAVE SCENE ANSWERS WHETHER IT SAVED. `(void)SaveOpenScene()` stood here against a
            // `[[nodiscard]] bool` — the attribute was on the declaration and the cast silenced it — so a
            // scene that could not be written came back over the channel as a success. This is the same
            // family as the toast that once said "Saved 'X'" for a file that had not been written
            // (FileSystem.hpp's note on the write primitive that is gone).
            m_Commands.Register( "SceneFiles (save)",
                                 [this]( Out& out ) { m_SceneFiles.AppendSaveSceneCommand( out ); } );
            m_Commands.Register( "Play", [this]( Out& out ) { m_Play.AppendPlayCommands( out ); } );
            m_Commands.Register( "Edit (Undo, Redo)",
                                 []( Out& out ) { MainMenu::AppendUndoRedoCommands( out ); } );
            m_Commands.Register( "Documents (close all)",
                                 [this]( Out& out ) { m_Documents.AppendCloseAllCommand( out ); } );
            m_Commands.Register(
                 "Window", [this]( Out& out )
                 { DockLayout::AppendWindowCommands( out, m_WindowChrome ? m_Application : nullptr ); } );
            m_Commands.Register( "Build", []( Out& out ) { AppendBuildCommands( out ); } );
        }
        // THE TOOLS, in View-menu order (DockLayout.hpp, EditorPanels.cpp — UE: RegisterTabSpawner).
        {
            const EditorPanelHandles handles = RegisterEditorPanels( m_Panels, m_Workspace, m_Play, m_Documents,
                                                                     m_AssetManager, m_AnimationLibrary );
            m_FileExplorerPanel              = handles.FileExplorer;
            m_WorldPartitionPanel            = handles.WorldPartition;
            m_Startup.AttachFileExplorer( m_FileExplorerPanel );
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
        // as many times as it likes. See BuildPaletteCommands and Editor/Core/Control.

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

        // First-frame prefs application (needs a live camera) + autosave timer (SessionRecovery::Tick).
        {
            static bool s_CameraSpeedApplied = false;
            if ( !s_CameraSpeedApplied )
            {
                if ( auto cam = m_Workspace.ActiveScene()->GetMainCamera().lock() )
                    if ( auto* editorCam = dynamic_cast<::Desert::Core::EditorCamera*>( cam.get() ) )
                    {
                        editorCam->SetMovementSpeed( EditorPreferences::Get().CameraSpeed );
                        s_CameraSpeedApplied = true;
                    }
            }

            m_Recovery.Tick( ts.GetSeconds() );
        }

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
        if ( m_AssetManager )
        {
            if ( m_Workspace.PrimaryRegistry() != nullptr )
            {
                m_Workspace.PrimaryRegistry()->TickRenderTextures( *m_AssetManager, frameTs );
            }
            for ( const auto& doc : m_Workspace.Documents() )
            {
                if ( doc->Registry )
                {
                    doc->Registry->TickRenderTextures( *m_AssetManager, frameTs );
                }
            }
        }

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
        // one is m_Workspace.ActiveScene() (rebound on viewport focus); RigBuilder / F9 below act on it only. The
        // outline aid + Begin/RegistryRender/OnUpdate/End are folded into UpdateSceneFrame (see below), applied
        // per scene so a secondary viewport is a full, independent render — not a static snapshot.
        if ( auto r = UpdateSceneFrame( *m_Workspace.PrimaryScene(), m_Workspace.PrimaryRegistry(), frameTs ); !r )
            return Common::MakeError( r.GetError() );
        for ( const auto& doc : m_Workspace.Documents() )
            if ( auto r = UpdateSceneFrame( *doc->Scene, doc->Registry.get(), frameTs ); !r )
                return Common::MakeError( r.GetError() );

        // Runs a queued "Convert to Skinned" (rig builder) here, outside ImGui component iteration — the swap
        // removes the StaticMeshComponent the Details panel is drawing, so it must not happen mid-render.
        if ( m_Workspace.ActiveScene() && m_AssetManager )
            RigBuilder::ProcessPending( *m_Workspace.ActiveScene(), *m_AssetManager );

        if ( const auto& shot = ShotOptions::Get();
             shot.FlightRoute && m_Workspace.ActiveScene() && !m_SceneFiles.HasPendingLoad() &&
             !m_Startup.StartupLoading() &&
             m_Workspace.ActiveScene()->GetState() == ::Desert::Core::Scene::SceneState::Play )
            m_Profiler.RecordFlightFrame( !m_Startup.ContentSettling() );

        // Screenshot mode, SECOND HALF (ShotDirector::CountRenderedFrame). On the capture's last frame the layer
        // adds its own records — the profiler dump, the --flight CSV — and closes with the capture's status.
        if ( m_Shots.CountRenderedFrame( shotRecorded ) )
        {
            const auto& shot = ShotOptions::Get();
            if ( shot.GpuProfile )
                ProfilerWindow::DumpProfilerToLog();
            if ( shot.FlightRoute && !m_Profiler.FinishFlight() )
                m_Shots.MarkFailed();
            m_Application->Close( m_Shots.Finish() );
        }

        // DEBUG: press F9 to dump the final rendered viewport image to F:/DesertEngine/frame_dump.png. Useful
        // because external GDI/PrintWindow capture returns white for the Vulkan surface — this reads the actual
        // rendered frame back from the GPU. Edge-detected so one press = one dump.
        {
            static bool s_f9Prev = false;
            const bool  f9       = Input::Keyboard::IsKeyPressed( Common::KeyCode::F9 );
            if ( f9 && !s_f9Prev )
            {
                if ( !m_Capture.WriteViewportPng( "F:/DesertEngine/frame_dump.png" ) )
                    LOG_ERROR( "[Dump] final frame could not be written" );
            }
            s_f9Prev = f9;
        }

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

    Common::BoolResultStr EditorLayer::UpdateSceneFrame( Desert::Core::Scene&    scene,
                                                         Render::RenderRegistry* registry,
                                                         const Common::Timestep& ts )
    {
        // Editor-only VIEW state (from EditorPreferences, not scene data) pushed per scene before it
        // records this frame: the selection outline, and — since К2 — the debug/show flags that used to be
        // serialized into the level. Both must land BEFORE BeginScene, which is where the renderer hands
        // them on to its systems.
        //
        // Only scenes that reach this function are pushed to, and that is the point: the asset-thumbnail,
        // inspector-preview and photogrammetry renderers own their own SceneRenderer, are never fed here,
        // and therefore keep DebugViewState's all-off defaults. They used to have to remember to switch the
        // grid off by hand on a scene they owned.
        //
        // ONCE PER VIEW, not once per scene. These reach the renderer's systems from BeginScene, and a
        // scene has a LIST of renderers now — pushing only to view 0 left every second viewport with
        // DebugViewState's all-off defaults, i.e. no grid, no collider wireframes, and the machine
        // quality schema defaults instead of this machine's.
        for ( size_t viewIndex = 0; viewIndex < scene.GetViewCount(); ++viewIndex )
        {
            auto* sr = scene.GetViewRenderer( viewIndex );
            if ( sr == nullptr )
                continue;
            const auto& prefs = EditorPreferences::Get();
            sr->SetOutlineSettings( prefs.OutlineColor, prefs.OutlineWidth, prefs.OutlineSmoothness,
                                    prefs.EnableOutline );
            // THE USER'S ANSWER, MINUS WHAT THIS SCENE'S VIEWPORTS ARE HIDING RIGHT NOW. `prefs.DebugView`
            // is what the user chose and what editor.json holds; a viewport MODE (2D UI editing hides the
            // ground grid) suppresses a flag in the COPY that reaches the renderer and never in the store.
            // Before К10 the mode wrote the store directly and every unrelated EditorPreferences::Save()
            // could make the suppression permanent — see Editor/Core/ViewportModes.hpp.
            sr->SetDebugView( ViewportPanel::EffectiveDebugView( prefs.DebugView, scene ) );
            // AND WHAT THIS MACHINE CAN AFFORD, on the same terms and for the same reason: post AA, mesh
            // LOD, the sampler's filter and anisotropy, the cloud tier. It was scene data until К3, so a
            // weak machine could not turn the picture down without editing a file that goes to everybody.
            // The offscreen preview renderers are not fed here either — the inspector preview pushes its
            // own copy with a cheaper cloud tier, and the other two keep the schema defaults.
            sr->SetQuality( Common::Scalability::QualityState::Resolved() );
        }

        // THE WORLD'S CLOCK, set up for this frame before the scene ticks it (Core::WorldTime).
        //
        // A HEADLESS CAPTURE holds the preview clock at zero on every frame it does not RECORD (this frame's
        // ShotRecordGate verdict: a load pending, the splash up, content settling, the viewport size not yet
        // held), because how many such frames there are depends on the machine, and a world that moved during them
        // (the cloud wind accumulates) would make two captures of one scene differ. Without `--play` the counted
        // frames then step by a FIXED step from zero; under `--play` the preview never runs — Play resets the
        // clock and `ts` is already the fixed step (ShotOptions::FrameSeconds), so the clock just follows it.
        // Outside a capture the measured step drives it, and the viewport's Realtime toggle decides whether
        // preview time moves while editing.
        if ( const auto& shot = ShotOptions::Get(); shot.Active() )
        {
            const bool counting = m_Shots.RecordingThisFrame();
            scene.GetWorldTime().SetFixedStep( shot.PlayActive() ? std::nullopt
                                                                 : std::optional( ShotOptions::PlayStepSeconds ) );
            if ( !counting && scene.GetState() == ::Desert::Core::Scene::SceneState::Edit )
            {
                scene.GetWorldTime().Reset();
            }
            scene.SetPreviewRealtime( counting && !shot.PlayActive() );
        }
        else
        {
            scene.GetWorldTime().SetFixedStep( std::nullopt );
            scene.SetPreviewRealtime( EditorPreferences::Get().ViewportRealtime );
        }

        // BEFORE the scene's frame, not between its phases: the scene opens and closes each view's
        // renderer itself now (Scene::OnUpdate), and nothing may sit between a renderer's open and its
        // close. Today this records nothing into the graph anyway — the editor's injected passes execute
        // inside the renderer's own update — so moving it costs the frame nothing.
        if ( registry != nullptr )
        {
            registry->BeginFrame( ts );
            registry->Render();
        }

        {
            DESERT_PROFILE_SCOPE( "Scene::OnUpdate" );
            // Play's time stops while streaming waits for the cell under the camera (WP12, decision O2); the
            // streamer's Tick above goes on, so the loader keeps reading and the wait ends by itself.
            const bool streamingWaits = m_Play.TickStreaming( scene, ts );
            if ( auto frame = scene.OnUpdate( streamingWaits ? Common::Timestep( 0.0f ) : ts ); !frame )
                return Common::MakeError( frame.GetError() );
        }

        return BOOLSUCCESS;
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
        m_ImGuiLayer->Begin();

        // ImGuizmo is a single global per-frame state — begin it ONCE here, before any panel issues a
        // Manipulate(). The viewport's object gizmo relies on this.
        ImGuizmo::BeginFrame();

        SyncWindowTitle();

        // LOADING FRAMES DRAW NOTHING. They go to a window that is still hidden — the splash is what a
        // person sees until the start is over — and there is no dockspace or panel to draw yet (the
        // viewport panel would touch the not-yet-rendered scene image). The fullscreen ImGui overlay that
        // stood here was replaced by the splash, and deleted with the same change.
        if ( m_Startup.StartupLoading() || m_Startup.ContentSettling() )
        {
            m_ImGuiLayer->End();
            return BOOLSUCCESS;
        }
        // NOT YET REAL WHILE A SCENE LOAD IS STILL QUEUED. The frame right after the last stage draws the
        // editor over an empty scene — the queued load runs in the NEXT OnUpdate and only then starts the
        // settle wait — and revealing on it showed the window 3 s before the content had settled
        // (measured on the first run of this change: "on screen" logged before "[Content] settled").
        if ( !m_SceneFiles.HasPendingLoad() && !m_SceneFiles.HasPendingNew() )
            m_RealFrameDrawn = true;

        // ---- Global editing shortcuts ----
        // Edit mode only (Play discards its changes on Stop anyway) and never while a text field owns the
        // keyboard. Runs at frame start, before any panel iterates the scene.
        {
            const ImGuiIO& io   = ::ImGui::GetIO();
            const bool editMode = m_Workspace.ActiveScene()->GetState() == ::Desert::Core::Scene::SceneState::Edit;
            if ( editMode && !io.WantTextInput && io.KeyCtrl )
            {
                if ( ::ImGui::IsKeyPressed( ImGuiKey_Z, false ) )
                {
                    if ( io.KeyShift )
                        CommandHistory::Get().Redo();
                    else
                        CommandHistory::Get().Undo();
                }
                if ( ::ImGui::IsKeyPressed( ImGuiKey_Y, false ) )
                    CommandHistory::Get().Redo();

                if ( ::ImGui::IsKeyPressed( ImGuiKey_D, false ) )
                {
                    if ( Core::SelectionManager::Count() > 0 )
                        if ( auto dups = Commands::DuplicateEntities( Core::SelectionManager::GetSelection() );
                             !dups.empty() )
                            Core::SelectionManager::SetSelection( std::move( dups ) );
                }

                if ( ::ImGui::IsKeyPressed( ImGuiKey_C, false ) && Core::SelectionManager::Count() > 0 )
                    Commands::CopySelectionToClipboard( Core::SelectionManager::GetSelection() );
                if ( ::ImGui::IsKeyPressed( ImGuiKey_V, false ) )
                    if ( auto pasted = Commands::PasteClipboard(); !pasted.empty() )
                        Core::SelectionManager::SetSelection( std::move( pasted ) );

                if ( ::ImGui::IsKeyPressed( ImGuiKey_N, false ) )
                    m_SceneFiles.RequestNew(); // Ctrl+N -> fresh empty scene (deferred, see OnUpdate)
                // Ctrl+R -> the open scene again from its file; an untitled scene has none (the menu greys it).
                if ( ::ImGui::IsKeyPressed( ImGuiKey_R, false ) )
                    (void)m_SceneFiles.RequestReload();

                if ( ::ImGui::IsKeyPressed( ImGuiKey_S, false ) )
                {
                    // Deliberately discarded HERE and only here: Ctrl+S destroys nothing, so there is
                    // no next step to gate. SaveOpenScene has already put the star back on and told the
                    // user why if the write failed. A focused document saves its own asset instead, and
                    // reports its own failure in its window.
                    ISubjectDocument* document = m_Documents.Documents().Find( m_Documents.FocusedDocument() );
                    switch ( ResolveSaveShortcut( m_Documents.DocumentHasFocus(), document ) )
                    {
                        case SaveShortcutTarget::Scene:
                            (void)m_SceneFiles.SaveOpenScene();
                            break;
                        case SaveShortcutTarget::FocusedDocument:
                            (void)document->SaveDocument();
                            break;
                        case SaveShortcutTarget::Nothing:
                            break;
                    }
                }
            }

            // Command palette (Ctrl+P) — works in both edit and play modes, and even over a text field
            // so it stays reachable; the palette grabs the keyboard once open.
            if ( io.KeyCtrl && !io.KeyShift && ::ImGui::IsKeyPressed( ImGuiKey_P, false ) )
                m_CommandPalette.Open();

            // CTRL+TAB THROUGH THE DOCUMENTS, most recently used first. This is what makes ten open
            // documents bearable: past about six the tab you want is off the end of the strip, and the
            // keyboard is the only route to it that does not involve reading a list first.
            //
            // Outside the edit-mode guard on purpose — switching document is not an edit — but not over a
            // text field, where Tab belongs to the field.
            //
            // ImGui BINDS Ctrl+Tab ITSELF (NavUpdateWindowing, enabled by NavEnableKeyboard) and it runs in
            // NewFrame, before this layer draws — so both would fire on one press: ImGui's window-ring
            // overlay AND this. The overlay is cancelled here rather than the key being fought for, and
            // ONLY when there was a document to switch to: with no documents open, Ctrl+Tab keeps ImGui's
            // ordinary window ring, which is a reasonable thing for it to do and not ours to remove.
            m_Documents.UpdateCycleShortcut( io );
        }

        // Menu Bar
        ::ImGui::PushStyleVar( ImGuiStyleVar_WindowBorderSize, 0.0f );
        DrawMenuBar();
        ::ImGui::PopStyleVar();

        m_Dock.BeginHost();

        // Toolbar strip FIRST so it reserves its height at the top; the DockSpace below then fills the
        // remaining area (drawing it after a full-height DockSpace(0,0) would push the bar off-screen).
        m_Toolbar.Draw();

        m_Dock.DrawDockSpace();

        m_Dock.DrawPanels();
        FollowImGuiWithEvents();

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

        DrawCommandPalette();
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
        if ( m_WindowChrome )
            m_WindowChrome->DrawResizeBorders();

        m_ImGuiLayer->End();

        // AFTER the interface has been recorded into the swapchain pass and BEFORE the frame is submitted:
        // the only window in which the presented image is legally ours to copy out of. A no-op unless a
        // `shot.window` is waiting on exactly this frame. See RecordWindowCaptureIfDue.
        m_Control.RecordWindowCaptureIfDue();

        return BOOLSUCCESS;
    }

    std::vector<PaletteCommand> EditorLayer::BuildPaletteCommands()
    {
        std::vector<PaletteCommand> commands;
        commands.reserve( m_Panels.Size() + m_Documents.Documents().Count() + 32 );
        m_Commands.Build( commands );
        return commands;
    }

    void EditorLayer::AppendAddShapeCommands( std::vector<PaletteCommand>& commands )
    {
        // ADD SHAPE: the outliner's Add > Shapes, one entry per authorable primitive, through the same spawn.
        for ( const Geometry::PrimitiveType type : Geometry::kAuthorablePrimitives )
        {
            commands.push_back(
                 { "Scene", std::string( "Add shape: " ) + Geometry::PrimitiveTypeName( type ), [this, type]
                   {
                       if ( !m_Workspace.ActiveScene() )
                           return PaletteCommandOutcome( false, "no scene is open" );
                       Editor::SceneHierarchyPanel::SpawnPrimitive( *m_Workspace.ActiveScene(), type );
                       return PaletteCommandDone();
                   } } );
        }
    }

    void EditorLayer::AppendPaletteDoorCommand( std::vector<PaletteCommand>& commands )
    {
        // THE PALETTE'S OWN DOOR. Ctrl+P is the only other way to it and a keystroke is not available to
        // this machine, so the command palette was the single window in this editor that no unattended run
        // could put on screen — and therefore the one whose appearance no change to it could ever be
        // checked against. Г14's rule reaches its own instrument: a capability reachable only by hand does
        // not exist for the channel. Found by needing it, exactly as the snap steps and the entity delete
        // were: A6-1 changed WHEN this list is built and could not photograph the result.
        commands.push_back( { "View", "Open the command palette", [this]
                              {
                                  m_OpenPaletteRequested = true;
                                  return PaletteCommandDone();
                              } } );
    }

    void EditorLayer::AppendSceneCommands( std::vector<PaletteCommand>& commands )
    {
        // THE LEVELS, which every other kind of document could already be opened by name from here and a
        // level could not — the one thing an editor exists to open was the one thing the palette had no
        // entry for, and therefore the one thing the control channel could not ask for either (the
        // channel's vocabulary IS this list). A separate group from "Open" above because these are not
        // documents: opening one REPLACES the world rather than adding a tab.
        //
        // Routed through SceneOpenRequest, not through LoadScene, on purpose: that is the path that runs
        // the unsaved-changes gate, and a palette entry is at least as easy to hit by accident as the
        // drag-and-drop it was written for.
        SceneFiles::AppendOpenSceneCommands( commands );

        // The Level Viewport commands the F / Esc keys run, on the viewport the user works in.
        for ( const Editor::ViewportCommand command : Editor::kViewportCommandOrder )
            commands.push_back( { std::string( Editor::CommandInfo( command ).Context ),
                                  std::string( Editor::CommandInfo( command ).Label ),
                                  std::bind_front( &Editor::ViewportPanel::RequestCommand, command ) } );
    }

    void EditorLayer::AppendSceneTailCommands( std::vector<PaletteCommand>& commands )
    {
        // NAMED VIEWPOINTS for the focused document's preview — the replacement for `--preview-orbit
        // yaw,pitch`, whose continuous angle pair a palette entry has nowhere to carry. See
        // Editor/Core/PreviewViewpoints.hpp for why names are MORE reproducible than numbers, not less.
        //
        // Offered for the FOCUSED document only, because that is the one a person means by "the preview"
        // and because seven entries per open document would bury everything else in the list.
        if ( ISubjectDocument* focused = m_Documents.Documents().Find( m_Documents.FocusedDocument() );
             focused != nullptr && focused->HasPreview() )
        {
            for ( const PreviewViewpoint& viewpoint : kPreviewViewpoints )
            {
                const PreviewViewpoint* aim = &viewpoint;
                commands.push_back( { "Preview", std::string( viewpoint.Name ), [this, aim]
                                      {
                                          // Re-resolved rather than captured: the focus can move, and the
                                          // document can be destroyed, between this list being built and
                                          // the entry being run.
                                          ISubjectDocument* target =
                                               m_Documents.Documents().Find( m_Documents.FocusedDocument() );
                                          if ( target == nullptr || !target->HasPreview() )
                                          {
                                              // REFUSES INSTEAD OF SLIPPING PAST. That re-resolution is
                                              // exactly a case that can come back empty, and the `if`
                                              // used to swallow it: the command answered success having
                                              // aimed nothing at anything.
                                              return Common::MakeError<bool>(
                                                   "the document this viewpoint was offered for no longer "
                                                   "has a preview; the focus moved between the list being "
                                                   "built and this command running." );
                                          }
                                          target->SetPreviewViewpoint( *aim );
                                          return PaletteCommandDone();
                                      } } );
            }
        }

        // THE THREE STATES OF THE FOCUSED DOCUMENT, as ordinary commands.
        //
        // Apply, Discard and Save are ACTIONS with names — they belong in the palette by the same rule
        // that put "Save Scene" there, and putting them here rather than inventing channel operations for
        // them is what keeps the channel's vocabulary the palette's vocabulary. The artist gets them on
        // the keyboard as a side effect, which is the argument for the palette in the first place.
        //
        // APPLY AND DISCARD ARE OFFERED ONLY WHILE THERE IS SOMETHING TO APPLY. The palette lists what is
        // available THIS INSTANT, exactly as the toolbar disables the two buttons in the same state; an
        // entry that ran and did nothing would be a silent no-op reported as a success, and a client
        // would read it as "the scene now has my edit".
        if ( ISubjectDocument* focused = m_Documents.Documents().Find( m_Documents.FocusedDocument() ) )
        {
            const SubjectId subject = m_Documents.FocusedDocument();

            if ( focused->GetEditModel() == ISubjectDocument::EditModel::Staged && focused->HasUnappliedEdits() )
            {
                // Re-resolved inside, not captured: the focus can move and the document can be destroyed
                // between this list being built and the entry being run — the same rule the Preview
                // viewpoints above follow, for the same reason.
                // THE THREE `(void)` CASTS THAT USED TO BE HERE ARE A6-2 POINT 1 IN ONE PLACE. Each of
                // these operations already returns "did anything actually move" — ISubjectDocument says
                // so at length, and says WHY: "a caller must not report a save that did not happen". The
                // palette then threw the answer away, so over the channel an Apply that published nothing
                // and a Save that wrote no file both came back `{"ok":true}`.
                //
                // The reason cannot be richer than this, and that is a limit worth naming rather than
                // dressing up: those three virtuals return a bare `bool` and carry no message, so what
                // the editor honestly knows is THAT the document declined. Turning the interface into
                // BoolResultStr would touch every document type and belongs to whoever owns them.
                commands.push_back( { "Document", "Apply this document's edits to the scene", [this, subject]
                                      {
                                          ISubjectDocument* target = m_Documents.Documents().Find( subject );
                                          if ( target == nullptr )
                                              return Common::MakeError<bool>(
                                                   "the document that had these edits is no longer open." );
                                          return PaletteCommandOutcome(
                                               target->ApplyEdits(),
                                               "the document published nothing: it had no outstanding edit "
                                               "by the time the command ran, so the scene is unchanged." );
                                      } } );
                commands.push_back( { "Document", "Discard this document's unapplied edits", [this, subject]
                                      {
                                          ISubjectDocument* target = m_Documents.Documents().Find( subject );
                                          if ( target == nullptr )
                                              return Common::MakeError<bool>(
                                                   "the document that had these edits is no longer open." );
                                          return PaletteCommandOutcome(
                                               target->DiscardEdits(),
                                               "the document discarded nothing: it had no outstanding edit "
                                               "by the time the command ran." );
                                      } } );
            }

            commands.push_back( { "Document", "Save this document", [this, subject]
                                  {
                                      ISubjectDocument* target = m_Documents.Documents().Find( subject );
                                      if ( target == nullptr )
                                          return Common::MakeError<bool>( "the document to save is no longer "
                                                                          "open." );
                                      return PaletteCommandOutcome(
                                           target->SaveDocument(),
                                           "the document was NOT written. Its own log line says why; this "
                                           "command only knows that no file was produced." );
                                  } } );
        }
    }

    void EditorLayer::DrawCommandPalette()
    {
        // THE OVERLAY ITSELF, ASKED FOR BY NAME. Ctrl+P is the only other way in, and a keystroke is not
        // available to this machine — so the command palette was the one window in this editor that no
        // unattended run could photograph, which made every change to it unverifiable. Г14's rule applied
        // to the palette's own door: a capability reachable only by hand does not exist for the channel.
        //
        // A DEFERRED FLAG rather than calling Open() in the closure, and the reason is the one asymmetry
        // that would otherwise make this a knob that does nothing. CommandPalette::Draw runs the chosen
        // entry and then sets m_Open = false on the very next line, so an entry that opened the palette
        // from inside the palette would be closed again before the frame ended — working over the socket
        // and doing nothing under a person's hand. Consumed below, in this same frame, so the channel's
        // ordering guarantee still holds: the frame that answers the command is the frame that shows it.
        if ( m_OpenPaletteRequested )
        {
            m_OpenPaletteRequested = false;
            m_CommandPalette.Open();
        }

        // BUILT ON THE FRAME IT OPENS, AND NOT ON EVERY FRAME IT IS OPEN.
        //
        // This used to call BuildPaletteCommands() unconditionally, sixty times a second for as long as
        // the overlay was up — while EditorLayer.hpp said, one line above the declaration, "Built on
        // demand — when the palette opens, or when a request arrives — never per frame." The comment was
        // the design; the code was not doing it, and nothing said so.
        //
        // It became load-bearing with A6-1: the `Open` group is now enumerated from the project's FILES
        // rather than from the asset manager's cache, so a per-frame rebuild is a recursive walk of the
        // content tree sixty times a second while somebody types a query. (The scene list beside it,
        // CollectAvailableScenes, has always walked a directory tree here too, so the rebuild was already
        // doing disk work per frame — the file half simply made it bigger and more obvious.)
        //
        // Rebuilding on OPEN is not a snapshot going stale, and that is why this is the fix rather than a
        // cache: the palette takes the keyboard while it is up, so nothing can open a document, load a
        // scene or delete an entity between the build and the choice. Running an entry closes it, and the
        // next Ctrl+P builds again.
        if ( m_CommandPalette.TakeJustOpened() )
            m_CommandPalette.SetCommands( BuildPaletteCommands() );

        if ( !m_CommandPalette.IsOpen() )
            return;

        // AND THE PERSON WHO CLICKED HEARS IT TOO. The palette hands back what the chosen entry answered
        // (A6-2 point 1); before that, a command picked from Ctrl+P that failed simply closed the overlay
        // and left the editor looking as though it had obeyed. The toast is raised HERE and not inside
        // CommandPalette so that class keeps one UI dependency instead of two — the layer that owns the
        // toasts owns how a refusal is shown.
        if ( const auto chosen = m_CommandPalette.Draw(); !chosen )
            Editor::ToastManager::Push( chosen.GetError(), Editor::ToastLevel::Error );
    }

    void EditorLayer::DrawMenuBar()
    {
        namespace ImGui = ::ImGui;

        if ( !ImGui::BeginMainMenuBar() )
            return;

        m_MainMenu.DrawMenus();

        LevelToolbar::DrawProjectSection();
        m_Toolbar.DrawSceneRenameSection();
        // Play/Pause/Stop now live in the toolbar strip (LevelToolbar::Draw), not the menu bar.
        //
        // THIS BAR IS THE WINDOW'S TITLE BAR NOW. It already carried the project, the level, the menus and
        // the stats while the system frame sat above it drawing a second one; the editor asks for a window
        // without a frame (Sandbox.hpp), so the three window commands and the bar's own gestures come here.
        // Both are conditional on the window actually being frameless — with a system frame they would be a
        // second set of buttons for the same three actions.
        const float chromeWidth = m_WindowChrome ? UI::WindowChrome::WindowButtonsWidth() : 0.0f;
        m_Profiler.DrawEngineStats( chromeWidth );
        if ( m_WindowChrome )
        {
            m_WindowChrome->DrawWindowButtons();
            // LAST inside the bar, after every item: "over the bar and over nothing on it" is only a
            // question with an answer once everything on it has been submitted.
            m_WindowChrome->HandleTitleBarGestures();
        }

        ImGui::EndMainMenuBar();

        DrawPopups();
    }

    // THE ONLY PLACE THE OS STILL SHOWS THIS WINDOW'S NAME. With the system frame gone the title is no
    // longer painted anywhere on screen, but the Dock, Mission Control, the taskbar and every window
    // switcher still read it — and the window's own name was "Desert Engine — <project>" for the whole
    // session, so those lists could not tell two editors on two levels apart.
    //
    // Compared against Window::GetTitle rather than against a copy of what was last pushed here: the window
    // owns that string, and a second copy in this file would be the same one-fact-two-owners shape as a
    // remembered "is it maximized". The comparison is what keeps this to one glfwSetWindowTitle per change
    // rather than sixty a second.
    void EditorLayer::SyncWindowTitle()
    {
        const auto& window = m_Application->GetWindow();
        if ( !window || !m_Workspace.ActiveScene() )
            return;

        const std::string title = std::format( "Desert Engine — {} — {}", Editor::ProjectContext::Current().Name,
                                               m_Workspace.ActiveScene()->GetSceneName() );
        if ( window->GetTitle() != title )
            window->SetTitle( title );
    }

    void EditorLayer::DrawPopups()
    {
        m_SceneFiles.DrawDialogs();
        m_Preferences.Draw();
    }

    void EditorLayer::FollowImGuiWithEvents()
    {
        Common::EventTree* events = Events();
        if ( events == nullptr )
            return;
        Common::EventNodeId focus   = EventNode();
        Common::EventNodeId pointer = EventNode();
        for ( const auto& panel : m_Panels )
        {
            if ( panel->HoldsKeyboardFocus() )
                focus = panel->EventNode();
            if ( panel->IsUnderPointer() )
                pointer = panel->EventNode();
        }
        events->SetFocus( focus );
        events->SetHovered( pointer );
    }

    Common::BoolResultStr EditorLayer::OnDetach()
    {
        m_Subsystems.reset();
        m_Application->GetCloseGate().Uninstall();
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
        m_Startup.AttachFileExplorer( nullptr );
        m_WorldPartitionPanel = nullptr;
        // Reported and not returned even though OnDetach has a channel: everything below this line still
        // has to run, and an early return would leave the extra documents and their render slots alive.
        if ( const auto detached = m_ImGuiLayer->OnDetach(); !detached.IsSuccess() )
            LOG_ERROR( "[EditorLayer] ImGui layer failed to detach: {}", detached.GetError() );
        m_ImGuiLayer.reset();

        // A scene names its views' renderers by raw pointer and owns none of them, so every renderer dies
        // after the scene that names it (FIX6) — the order is SceneWorkspace::Teardown's.
        m_Workspace.Teardown();

        return BOOLSUCCESS;
    }

} // namespace Desert::Editor
