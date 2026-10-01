#define IMGUI_DEFINE_MATH_OPERATORS

#include <Engine/World/Landscape/LandscapeData.hpp>
#include <Engine/Core/Glfw.hpp>
#include <Engine/Core/PlayerStart.hpp>
#include <Editor/Core/SaveShortcut.hpp>
#include <Editor/Core/ContentCreateCommands.hpp>
#include <Editor/Core/DetailsNavigation.hpp>
#include <Engine/Graphic/Environment/EnvironmentBake.hpp>
#include <Engine/Assets/ContentWork.hpp>
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
#include <Engine/Core/ShaderCompiler/ShaderSpirvCache.hpp>
#include <Engine/Graphic/MemoryReadout.hpp>
#include <Engine/Graphic/ResourceLedger.hpp>
#include <Engine/Graphic/DrawCounters.hpp>
#include <Engine/Assets/SyncLoadLedger.hpp>
#include "EditorLayer.hpp"

#include <Engine/Runtime/ResourceRegistry.hpp>

#include <Engine/Assets/TextureSourceAsset.hpp>

#include <functional>
#include <set>

#include <Editor/Widgets/ThumbnailService.hpp>
#include <Editor/Widgets/ThumbnailWarmup.hpp>
#include <Engine/Core/SceneAssetRoots.hpp>
#include <Common/Core/Core.hpp>
#include <Common/Core/CrashHandler.hpp>
#include <Common/Core/Profiler.hpp>
#include <Editor/Import/CookPaths.hpp>
#include <Editor/Import/MeshDnD.hpp>
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
#include <Engine/Core/Serialize/SceneSerializer.hpp>
#include <Engine/Core/WorldStreamer.hpp>
#include <Engine/Core/Serialize/SceneFormat.hpp>
#include <Engine/Core/Serialize/ExternalEntities.hpp>
#include "Editor/Core/CommandLine.hpp"
#include "Editor/Core/Control/ControlChannelOptions.hpp"
#include "Editor/Core/AutosavePaths.hpp"
#include "Editor/Core/CrashRecovery.hpp"

// The device-lost latch, read in OnDetach: a shutdown caused by a lost GPU must save the user's work
// before it goes, and must not report itself as a clean exit.
#include <Engine/Graphic/DeviceLost.hpp>
#include "Editor/Core/LayoutManager.hpp"
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
#include "Editor/Panels/SceneProperties/ScenePropertiesPanel.hpp"
#include "Editor/Panels/Debug/ShaderLibraryPanel.hpp"
#include "Editor/Panels/Debug/UIDebuggerPanel.hpp"
#include "Editor/Panels/FileExplorer/FileExplorerPanel.hpp"
#include "Editor/Widgets/ThumbnailEdit.hpp"
#include "Editor/Panels/ViewportPanel/ViewportPanel.hpp"
#include "Editor/Panels/ViewportPanel/Tools/ActiveToolBar.hpp"
#include "Editor/Panels/Scalability/ScalabilityPanel.hpp"
#include "Editor/Panels/WorldSettings/WorldSettingsPanel.hpp"
#include "Editor/Panels/WorldPartition/WorldPartitionPanel.hpp"

#include <Engine/Core/Serialize/WorldPartitionConversion.hpp>
#include "Editor/Panels/Landscape/LandscapePanel.hpp"
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
#include "Editor/Panels/Modeling/ModelingPanel.hpp"
#include "Editor/Panels/Logs/LogsPanel.hpp"
#include "Editor/Panels/Collections/CollectionsPanel.hpp"
#include "Editor/Panels/SceneProperties/ComponentWidgets/MaterialsPanelComponent.hpp"
#include "Editor/Panels/Photogrammetry/PhotogrammetryPanel.hpp"
#include "Editor/Panels/AssetReferences/AssetReferencesPanel.hpp"
#include "Editor/Panels/LuaConsole/LuaConsolePanel.hpp"
#include "Editor/Panels/Build/BuildSettingsPanel.hpp"
#include "Editor/Panels/Build/ContentChunksPanel.hpp"
#include "Editor/Packaging/ProjectChunkScheme.hpp"
#include "Editor/Panels/History/HistoryPanel.hpp"
#include "Editor/Panels/Localization/LocalizationPanel.hpp"
#include "Editor/Panels/Validation/SceneValidationPanel.hpp"
#include "Editor/Panels/Clouds/CloudsPanel.hpp"
#include "Editor/Panels/Animation/ControlRigPanel.hpp"
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
        // The recovery saves (autosave, device-lost) go through the one scene writer and only need to know
        // whether it landed; what it counted is the editor save's business.
        Common::BoolResultStr
        WrittenOrError( const Common::ResultStr<Desert::Core::ExternalEntities::WriteOutcome>& r )
        {
            if ( !r )
                return Common::MakeError( r.GetError() );
            return BOOLSUCCESS;
        }

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
    void EditorLayer::UpdateContextualPanels()
    {
        for ( auto& panel : m_Panels )
        {
            // An EXPLICIT request always wins and applies to every panel, contextual or not: a button in
            // Details ("Anim Layers") asked for this panel BY NAME. It pins it, exactly like ticking it in
            // the View menu — the user asked, so nothing auto-closes it.
            //
            // BY NAME IS ALL A TOOL CAN BE ASKED FOR, and that is why the Anim Graph, the Particle Editor,
            // the UI Editor and the Sequencer no longer come through here: "show the one Sequencer window"
            // was the most their Details buttons could say, and the window then had to guess which rig it
            // was about from the selection. They ask for a SUBJECT now
            // (Core::SubjectOpenRequests::Request), which is a different wire because it carries what to
            // edit — see Editor/Core/SubjectOpenRequest.hpp.
            switch ( Core::PanelRequests::Consume( panel->GetName() ) )
            {
                case Core::PanelRequests::Action::Open:
                    panel->GetVisibility() = true;
                    panel->Pinned()        = true;
                    m_FocusPanel           = panel->GetName();
                    break;

                // A drawer button is a switch, not a summons: pressing it again puts the panel away.
                case Core::PanelRequests::Action::Toggle:
                    panel->GetVisibility() = !panel->GetVisibility();
                    panel->Pinned()        = panel->GetVisibility();
                    if ( panel->GetVisibility() )
                        m_FocusPanel = panel->GetName();
                    break;

                case Core::PanelRequests::Action::None:
                    break;
            }

            if ( !panel->IsContextual() )
                continue;

            const bool relevant = panel->IsRelevant();
            bool&      visible  = panel->GetVisibility();

            // Pinning is set ONLY where the user actually asks for the panel (View menu / command
            // palette). Inferring it from "visible but not relevant" also fired on the very first frame
            // for a panel that merely starts visible, pinning it open forever.
            if ( relevant && !visible && !panel->Pinned() )
            {
                visible      = true;
                m_FocusPanel = panel->GetName(); // bring it forward in whatever dock it lives
            }
            else if ( !relevant && visible && !panel->Pinned() )
            {
                visible = false;
            }
            else if ( !visible )
            {
                panel->Pinned() = false; // closed by hand -> stop pinning it open
            }
        }
    }

    // Cognitive complexity 27 against a threshold of 19, PRE-EXISTING and reported for any edit inside
    // this constructor (Г26 added the autosave-migration call below). Named as debt, not fixed here.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity)
    namespace
    {
        // The splash's cost per item of each weighed stage (EditorLayer::MakeSplashPlan), read off the
        // "[Startup] ... item(s) in" lines of a Debug start on the M-series development machine.
        constexpr double kSecondsPerShader = 0.048; // 78 programs in 3.73 s
        // A texture whose cook is fresh costs its freshness check: 11 in 0.04 s.
        constexpr double kSecondsPerTextureCheck = 0.0036;
        constexpr double kSecondsPerClipRow      = 0.0002; // indexing a registry row reads nothing
        // The settle costs its first frames whether or not a read is outstanding: 0.75 s with none. A read
        // outstanding at its start adds one shader's worth; no measured start has had one.
        constexpr double kSecondsSceneSettle  = 0.75;
        constexpr double kSecondsPerSceneRead = kSecondsPerShader;

    } // namespace

    EditorLayer::EditorLayer( Engine::Application* application, const std::string& layerName,
                              std::unique_ptr<Splash::SplashScreen> splash )
         : Common::Layer( layerName ), m_Application( application ), m_Splash( std::move( splash ) )

    {
        m_AssetManager = std::make_shared<Assets::AssetManager>();

        m_ImportManager = std::make_unique<ImportManager>();
        // Cook only what's missing/stale (skips the expensive Assimp re-parse on every launch). Collections
        // hold packs (a character + its animation FBXs), so they're imported too — their skinned assets land
        // beside each source, where the content registry gathers them (see CookPaths::SkinnedAsset).
        //
        // STAGED: this used to run inline here and froze the window for seconds before the first frame.
        // The stages now execute one-per-frame from OnUpdate, each announced on the splash.
        // NOTE: shaders are NOT staged — they load synchronously in OnAttach, because the render systems
        // (MeshECSSystem's default PBR materials) resolve their shaders in their constructors.
        //
        // THE MESH AND COLLECTION COOKS ARE NO LONGER STAGES (AL1-11, owner decision V2): they run on the
        // JobSystem after the reveal (StartBackgroundCook) and the registry lists whatever cook is on the disk.
        // AND THE LOOSE TEXTURES, WHICH NOTHING COOKED. A texture under `Assets/Textures/` reached its
        // cooked form only as a mesh's dependency or through a drag-and-drop, so the one cooked texture
        // this repository then committed had no producer in any automatic path -- and a stale one (a container
        // version moved, a PNG re-exported) stayed stale until somebody dragged the file back in. The
        // freshness question costs one CRC-32C pass per source at 8.17 GB/s and answers "nothing
        // changed" without decoding anything; see TextureImporter.cpp for why it is bytes and not
        // mtimes.
        //
        // AND THE TEXTURES THAT LIVE BESIDE A MESH. `Assets/Meshes/*.png` cook to
        // their `.detex` assets beside the mesh, and they were written only when the MESH was re-imported --
        // which the boot scan skips whenever the `.stmesh` is newer than its source. So a container version
        // bump left four of them stranded at version 1 and every launch printed four load failures that no
        // automatic path could clear. Both directories are `LooseTextureRoots()`, the list the packager
        // cooks too. (This stage used to walk `Assets/Meshes/` twice; the second walk found everything
        // fresh.)
        m_StartupStages.push_back( { "Importing textures...",
                                     [this]
                                     {
                                         // The editor derives texture platform data on a DDC miss; a packaged game
                                         // has no builder.
                                         Assets::SetTexturePlatformDataBuilder(
                                              &TextureImporter::BuildPlatformData );
                                         Assets::SetMeshPlatformDataBuilder( &Editor::BuildMeshPlatformData );
                                         (void)m_ImportManager->ImportLooseTextures( SplashItems() );
                                     },
                                     kSecondsPerTextureCheck, [] { return LooseTextureSources().size(); }, nullptr,
                                     0 } );
        // THE ONLY CONTENT STAGES LEFT (AL1-9): nothing here creates an asset of any kind. Textures, materials,
        // meshes, skyboxes and the cloud kinds are created from their content-registry rows when something
        // names them, and the scene settle below waits for the ones the scene names.
        m_StartupStages.push_back(
             { "Indexing animation clips...",
               [this] { Assets::IndexAnimationClips( *m_AssetManager, *m_AnimationLibrary ); }, kSecondsPerClipRow,
               [] { return Assets::ContentRegistry::Rows( Common::Content::ContentKind::Animation ).size(); },
               nullptr, 0 } );
        // Order-free, and early among the optional stages on purpose: a missing translation shows up on
        // the very first frame drawn, and its log line is far easier to read before the rest of the
        // content's lines arrive.
        m_StartupStages.push_back( { "Requesting string tables...",
                                     [this] { Assets::RequestStringTables( m_AssetManager ); }, kSecondsPerClipRow,
                                     nullptr, nullptr, 0 } );

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
        Common::Settings::MachineSettings::Load( std::filesystem::path( ProjectContext::ConfigDirectory() ) /
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

        // Crash recovery: if the previous session left its lock behind (unclean exit) and an autosave
        // exists, arm a prompt to reopen it. Then (re)arm the lock for THIS session; a clean shutdown
        // (OnDetach) removes it.
        if ( CrashRecovery::WasUncleanExit() )
        {
            // Only a copy at this build's scene schema is offered; autosaves are never migrated, so one
            // from another generation is named here and left as it is (CrashRecovery::ChooseAutosave).
            const Autosave::RecoveryChoice choice = CrashRecovery::ChooseAutosave();
            for ( const Autosave::NotOfferedCopy& copy : choice.NotOffered )
            {
                LOG_WARN( "[Recovery] not offered: '{}' states scene schema v{} / world units v{}; this build "
                          "opens v{} / v{} only. Autosaves are not migrated -- the file is left as it is.",
                          copy.Path.string(), copy.Stated.Scene, copy.Stated.Unit, Desert::Core::kSceneVersion,
                          Desert::Core::kUnitVersion );
            }
            m_RecoveryAutosave   = choice.Offered;
            m_ShowRecoveryPrompt = !m_RecoveryAutosave.empty();
        }
        if ( !CrashRecovery::ArmSession() )
            Editor::ToastManager::Push( "Crash recovery is OFF for this session — the lock file could "
                                        "not be written (see the log)",
                                        Editor::ToastLevel::Error );

        // LoadScene( "Resources/Assets/Scene/HouseDemo.desce" );
    }

    EditorLayer::~EditorLayer() = default;

    [[nodiscard]] Common::BoolResultStr EditorLayer::OnAttach()
    {
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

        // THE DOCKING LAYOUT FILE, OFF THE PROJECT (UE: <Project>/Saved/Config/EditorLayout.ini) — never ImGui's
        // default "imgui.ini", which fopen resolves against the working directory: an editor started from /tmp
        // read and wrote /tmp/imgui.ini and came up with a different layout than one started from Editor/.
        // Static storage: io.IniFilename is a borrowed C string ImGui reads at the first frame and on every save.
        {
            static std::string          s_LayoutIni;
            const std::filesystem::path configDir = Common::Constants::Path::ProjectDir() / "Saved" / "Config";
            std::error_code             dirError;
            std::filesystem::create_directories( configDir, dirError );
            if ( dirError )
                return Common::MakeFormattedError( "the editor layout folder '{}' could not be created: {}",
                                                   configDir.string(), dirError.message() );
            s_LayoutIni    = ( configDir / "EditorLayout.ini" ).string();
            io.IniFilename = s_LayoutIni.c_str();
        }

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
        MakeSplashPlan();
        BeginSplashStage( m_ShaderStage );
        // The splash's close button, pressed during this one long call, stops it between programs.
        if ( const auto shaders = Assets::CompileEngineShaders(
                  m_AssetManager, SplashItems(), [this]() { return m_Splash && m_Splash->CloseRequested(); } );
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
            m_AssetCommands = std::make_unique<AssetCommands>(
                 m_FileExplorerPanel, m_WorldPartitionPanel, m_Workspace.ActiveScene(), m_AssetManager,
                 m_PaletteAssetFiles, camera,
                 [this]( const std::string& folder ) { return ShowFolderInBrowser( folder ); } );
            using Out = std::vector<PaletteCommand>;
            m_Commands.OnBuildBegin( [this] { m_PaletteAssetFiles.Take(); } );
            m_Commands.Register( "Panels", [this]( Out& out ) { AppendPanelCommands( out ); } );
            m_Commands.Register( "Debug (crash)", []( Out& out ) { AppendCrashCommand( out ); } );
            m_Commands.Register( "Assets (selection)",
                                 [this]( Out& out ) { m_AssetCommands->AppendSelectionCommands( out ); } );
            m_Commands.Register( "Panels (maximize), Details", [this]( Out& out ) { AppendMaximizeCommands( out ); } );
            m_Commands.Register( "Clouds", []( Out& out ) { AppendCloudCommands( out ); } );
            m_Commands.Register( "Language", []( Out& out ) { AppendLanguageCommands( out ); } );
            m_Commands.Register( "Documents", [this]( Out& out ) { m_Documents.AppendDocumentCommands( out ); } );
            m_Commands.Register( "Entity", [this]( Out& out ) { m_EntityCommands->Append( out ); } );
            m_Commands.Register( "Modeling (Mesh To Collision)", [this]( Out& out )
                                 { AppendMeshToCollisionCommands( out, m_Workspace.ActiveScene() ); } );
            m_Commands.Register( "Entity (collapse)", [this]( Out& out ) { m_EntityCommands->AppendCollapse( out ); } );
            m_Commands.Register( "Menu", [this]( Out& out ) { m_MainMenu.AppendMenuCommands( out ); } );
            m_Commands.Register( "Level viewport", []( Out& out ) { AppendViewportCommands( out ); } );
            m_Commands.Register( "Modeling (Select Elements)", []( Out& out ) { AppendSelectElementsCommand( out ); } );
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
            m_Commands.Register( "Assets (import)", [this]( Out& out ) { m_AssetCommands->AppendImportCommands( out ); } );
            m_Commands.Register( "Foliage",
                                 [this]( Out& out ) {
                                     AppendFoliageCommands( out, m_Workspace.ActiveScene(), m_AssetManager,
                                                            m_PaletteAssetFiles.Files() );
                                 } );
            m_Commands.Register( "Open", [this]( Out& out )
                                 { m_Documents.AppendOpenCommands( out, m_PaletteAssetFiles.Files() ); } );
            m_Commands.Register( "Assets (folders)", [this]( Out& out ) { m_AssetCommands->AppendFolderCommands( out ); } );
            m_Commands.Register( "Scene", [this]( Out& out ) { AppendSceneCommands( out ); } );
            m_Commands.Register( "Scene (new views)",
                                 [this]( Out& out ) { m_Workspace.AppendNewViewCommands( out ); } );
            m_Commands.Register( "Debug (GPU allocations)", []( Out& out ) { AppendGpuAllocationCommand( out ); } );
            m_Commands.Register( "Scene (view layout)",
                                 [this]( Out& out ) { m_Workspace.AppendViewLayoutCommands( out ); } );
            m_Commands.Register( "Scene (actions)", [this]( Out& out ) { AppendSceneTailCommands( out ); } );
            m_Commands.Register( "Play", [this]( Out& out ) { m_Play.AppendPlayCommands( out ); } );
            m_Commands.Register( "Edit (Undo, Redo)",
                                 []( Out& out ) { MainMenu::AppendUndoRedoCommands( out ); } );
            m_Commands.Register( "Actions, Window", [this]( Out& out ) { AppendWindowCommands( out ); } );
            m_Commands.Register( "Build", []( Out& out ) { AppendBuildCommands( out ); } );
        }
        m_Panels.Add<Editor::SceneHierarchyPanel>( m_Workspace.ActiveScene(), m_AssetManager );
        m_Panels.Add<Editor::ScenePropertiesPanel>( m_Workspace.ActiveScene(), m_AssetManager,
                                                    m_AnimationLibrary.get() );
        m_Panels.Add<Editor::ShaderLibraryPanel>();
        {
            auto primaryViewport =
                 std::make_unique<Editor::ViewportPanel>( m_Workspace.ActiveScene(), m_AssetManager.get() );
            // Focusing the main viewport rebinds the editor back to the primary scene.
            primaryViewport->SetOnActivate( [this] { m_Workspace.SetActiveScene( kPrimarySceneViewId ); } );
            m_Panels.Adopt( std::move( primaryViewport ) );
        }
        {
            auto fileExplorer = std::make_unique<Editor::FileExplorerPanel>(
                 Common::Constants::Path::ASSETS_PATH, &m_Documents.SubjectEditors(), m_AssetManager.get(),
                 m_Workspace.ActiveScene() );
            m_FileExplorerPanel = fileExplorer.get();
            m_Panels.Adopt( std::move( fileExplorer ) );
        }
        m_Panels.Add<Editor::ModelingPanel>( m_Workspace.ActiveScene() );
        m_Panels.Add<Editor::LandscapePanel>( m_Workspace.ActiveScene() );
        m_Panels.Add<Editor::WorldSettingsPanel>( m_Workspace.ActiveScene() );
        m_Panels.Add<Editor::ScalabilityPanel>();
        // Hidden until asked for: the map is only meaningful on a partitioned scene. The streamer is read through
        // the getter each frame, because Stop and a streaming error destroy it from this side.
        m_WorldPartitionPanel = &m_Panels.Add<Editor::WorldPartitionPanel>(
             m_Workspace.ActiveScene(), m_AssetManager.get(), [this] { return m_Play.Streamer(); } );
        m_Panels.Add<Editor::LogsPanel>();
        m_Panels.Add<Editor::CollectionsPanel>( m_AssetManager.get() );
        m_Panels.Add<Editor::HistoryPanel>();
        m_Panels.Add<Editor::SceneValidationPanel>( m_Workspace.ActiveScene(), m_AssetManager.get() );
        m_Panels.Add<Editor::LocalizationPanel>();
        m_Panels.Add<Editor::UIDebuggerPanel>( m_Workspace.ActiveScene() );
        // THE FOUR CLOUD PANELS ARE NOT CONSTRUCTED HERE ANY MORE. They were singletons in this list, each
        // reached from the View menu and bound to whatever file its own combo had last opened; they are now
        // asset DOCUMENTS, built on demand by the registry below. Dropping them from the list is what
        // removes them from the View menu, the command palette and `--open-panel <name>` at once — all three
        // are generic over m_Panels, so there was never a per-panel entry to delete. An asset is opened from
        // the asset, not from a menu (Docs/Clouds/DEV_CONTRACT.md §4).

        // THE NODE GRAPH IS NOT CONSTRUCTED HERE ANY MORE EITHER, and it was the last one: a `.dgraph` is
        // an asset now, so the window is a document over its handle and is built on demand by the registry
        // below. That is U7-2's refusal spent — see NodeGraphPanel.hpp for the four obstacles it named and
        // which of them turned out to be real.
        //
        // THE ANIM GRAPH, THE PARTICLE EDITOR, THE UI EDITOR AND THE SEQUENCER ARE NOT CONSTRUCTED HERE ANY
        // MORE, for the reason the four cloud panels above are not: they edit ONE thing, so they are
        // documents. The difference is what that one thing is — a component on an entity rather than a file
        // — which is what U7 made expressible (Editor/Core/EditorSubject.hpp). Dropping them from this list
        // removes them from the View menu, the command palette and `--open-panel` at once, because all three
        // are generic over m_Panels; they are reached from the component that holds them, in Details.
        m_Panels.Add<Editor::PhotogrammetryPanel>( m_Workspace.ActiveScene(), m_AssetManager.get() );
        m_Panels.Add<Editor::AssetReferencesPanel>( m_Workspace.ActiveScene(), m_AssetManager );
        m_Panels.Add<Editor::LuaConsolePanel>( m_Workspace.ActiveScene().get(), m_AssetManager.get() );
        m_Panels.Add<Editor::ControlRigPanel>( m_Workspace.ActiveScene() );
        m_Panels.Add<Editor::BuildSettingsPanel>();
        m_Panels.Add<Editor::ContentChunksPanel>();
        // THE CLOUDS WINDOW IS A TOOL, and it must be: it is a setting the user keeps (View ▸ Clouds), it
        // edits no subject of its own, and the compiler refuses a document here anyway (PanelRegistry).
        // What it DOES is show the documents that edit the six stages of the sky — asked of
        // m_Documents.Documents(), which is the same container the document well reads, so both windows show the
        // same object and neither knows the other exists. See Editor/Panels/Clouds/CloudsPanel.hpp.
        m_Panels.Add<Editor::CloudsPanel>( m_Workspace.ActiveScene(), m_AssetManager, m_Documents.Documents() );

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
        m_Control.SampleFrameQuiescence( StartupLoading() || ContentSettling() );

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

        // Staged startup loading: run ONE heavy stage per frame. While loading, the scene is NOT rendered
        // at all (shaders/assets aren't there yet — rendering before the preload stage crashed on the
        // missing StaticMeshPBR shader); the frame is ImGui-only and the window it goes to is still hidden.
        //
        // THERE USED TO BE A GATE HERE — "only after one frame with the loading overlay has been
        // presented" — and it existed for the overlay alone: a stage run before that frame froze a blank
        // window. The overlay is gone (the splash, a window of its own, replaced it), and so is the gate.
        //
        // CLOSE ON THE SPLASH ENDS THE START HERE, before the next stage: the editor leaves through the same
        // Application::Close the window frame's close button and the control channel's `quit` take.
        const Splash::StartupStep step = Splash::NextStartupStep( m_Splash && m_Splash->CloseRequested(),
                                                                  m_StartupNext, m_StartupStages.size() );
        if ( step == Splash::StartupStep::Quit && !m_QuitFromSplash )
        {
            m_QuitFromSplash = true;
            LOG_INFO( "[Startup] closed on the splash; {} of {} stage(s) not run",
                      m_StartupStages.size() - m_StartupNext, m_StartupStages.size() );
            m_Application->Close( 0 );
        }
        if ( step == Splash::StartupStep::RunStage )
        {
            {
                DESERT_PROFILE_SCOPE( "Startup stage" );

                // EVERY STAGE IS TIMED, and the reason is a question nobody could answer. A client
                // watching a fresh editor over this project saw the command palette's 'Open' group stay
                // empty for five minutes and had no way to say WHICH of eight stages was spending them:
                // the only startup line the log ever carried was the shader preload's, which runs in
                // OnAttach and is not one of these at all. So "the preload finished" was read as "the
                // startup finished", and the two are minutes apart. Measured here, on an otherwise idle
                // machine, the eight stages cost 6.0 s of a 51 s boot — the other 45 s is OnAttach's
                // shader preload, which is exactly the phase the one existing line already reports.
                //
                // A phase nobody can name is a phase every brief guesses at, and three of this project's
                // timed investigations went looking in the wrong one.
                // THE TIMING AND THE LINE COME FROM `Core::BootTimeline` NOW, not from a chrono pair
                // here — because the shipping runtime needed the same thing and two copies of an
                // accumulation rule is how the two numbers stop being comparable. The SCHEDULER stays
                // here: running one stage per frame behind a progress overlay is this layer's own
                // arrangement and has nothing to do with timing. See Engine/Core/BootTimeline.hpp.
                BeginSplashStage( m_StartupStages[m_StartupNext].ProgressStage );
                const auto stageStart = std::chrono::steady_clock::now();
                m_StartupStages[m_StartupNext].Run();
                const double stageMs =
                     std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - stageStart )
                          .count();
                ++m_StartupNext;
                m_Boot.Record( m_StartupStages[m_StartupNext - 1].Label, stageMs );

                if ( !StartupLoading() )
                {
                    // THE SETTLE PHASE GETS ITS OWN LABEL. A splash that says nothing while it waits is
                    // indistinguishable from an editor that has hung, and this wait is the one the
                    // demand-driven model introduced.
                    const auto& loader = Assets::AsyncAssetLoader::Get();
                    m_SettleBase       = loader.StartedCount() - loader.Outstanding();
                    BeginSplashStage( m_SettleStage, loader.Outstanding() );
                    m_Boot.LogSummary();
                    LOG_INFO( "[Startup] all {} stage(s) done in {:.1f} ms; the editor is now answering "
                              "about a project it has actually read.",
                              m_StartupStages.size(), m_Boot.ElapsedMs() );
                    // AND THE BOOT IS OVER HERE — not at the first frame, which the hidden window has
                    // been presented for the whole of the staged load. Every synchronous asset load
                    // after this line reports itself as a hitch. See Engine/Assets/SyncLoadLedger.hpp.
                    Assets::SyncLoadLedger::NoteBootFinished();
                    LOG_INFO( "[SyncLoad] boot finished — {}", Assets::SyncLoadLedger::Report() );
                    LOG_INFO( "[Memory] boot finished — {}", Graphic::MemoryReadout::Take().Report() );
                    // HOW MANY HANDLES CAN NAME THEIR OWN FILE BY THE TIME THE BOOT IS OVER: the registry's
                    // rows publish them before anything is created (T2.4), so this is the size of the
                    // path->handle inverse the engine holds without having created a single content shell.
                    LOG_INFO( "[AssetPathIndex] boot finished — {} handle(s) can name their own path",
                              Common::AssetPathIndex::Size() );
                    // AND WHAT IT COST TO MINT THEM. The line above is only an achievement next to this
                    // one: the same count reached with directory walks and reached without them are two
                    // different boots, and nothing else in the process can tell them apart (§T2.4).
                    LOG_INFO( "[ContentScan] boot finished — {}", Common::Utils::ContentScanLedger::Report() );

                    // AND ONLY NOW THE EDITOR DOES ITS COOK — after the three lines above, which is not
                    // a tidiness choice. `Refresh` WALKS the content roots (it is the one walk left in
                    // this engine), and a walk before the `[ContentScan]` line would have made the
                    // registry a place the cost moved to rather than a place it stopped being paid:
                    // the number the whole tier is judged by would report the walk it removed.
                    //
                    // What it is for: content that arrived on disk without going through this editor —
                    // a `git pull`, a file dropped into the folder while the editor was closed — has no
                    // row, and since the boot no longer walks, it is content the engine does not have.
                    // This enters it, so it is there the NEXT time the project opens, and says how many
                    // it found. A file authored IN the editor never waits for this: `CreateAsset` notes
                    // its row the moment it exists.
                    //
                    // A REFUSAL IS LOGGED AND THE SESSION CONTINUES, unlike `Load`'s. Nothing in this
                    // session depends on the cook: the editor is already running over the registry it
                    // read, and failing to write the next boot's copy is a reason to say so loudly, not
                    // a reason to stop editing.
                    if ( const auto cooked = Assets::ContentRegistry::Refresh( *m_AssetManager ); !cooked )
                    {
                        LOG_ERROR( "[ContentRegistry] the content registry could not be cooked: {}",
                                   cooked.GetError() );
                    }
                    else
                    {
                        // THE COOK'S OWN WALK COST, READ OUT OF THE SAME LEDGER the boot line above
                        // reports zero from. This is the one number that says what removing the scan
                        // from the boot actually bought, measured rather than argued: the cook runs
                        // exactly the content scans the boot used to run, through the same primitive,
                        // on the same tree, seconds later on the same machine.
                        LOG_INFO( "[ContentRegistry] {}; the cook itself did {}", cooked.GetValue().Describe(),
                                  Common::Utils::ContentScanLedger::Report() );
                    }
                }
            }
            // The browser asked for its opening folder's pictures when it was built (OnAttach): the workers
            // decode them through the stages too, not only through the settle that follows (THUMB2).
            if ( Splash::ThumbnailDiskDecodeAllowed( CurrentRevealState() ) )
                ThumbnailService::TickDiskAndDecode();
            m_Control.SampleFrameQuiescence( StartupLoading() || ContentSettling() );
            return BOOLSUCCESS;
        }

        // The timestep this frame is driven by. Identical to the wall-clock one the application measured,
        // EXCEPT under a `--play` capture, where it is the fixed step from ShotOptions.
        //
        // Substituted for the whole layer update rather than only for the scene: a capture is reproducible
        // only if nothing in it integrates a number that came from a clock, and "the scene is deterministic
        // but the thing above it is not" is the kind of split that holds until the day something above it
        // starts feeding the scene. Outside `--play` this is `ts` itself, so no existing frame moves.
        const Common::Timestep frameTs( ShotOptions::Get().FrameSeconds( ts.GetSeconds() ) );

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
        UpdateContentSettling();
        DrainBackgroundCook();

        // Scene loads wait until the startup stages finished (a scene expects cooked/preloaded assets).
        // A load that ran starts the content settle before the refused-load fallback and New Scene.
        if ( !StartupLoading() )
        {
            if ( m_SceneFiles.ServiceLoadRequest() )
            {
                BeginContentSettle();
                m_SceneFiles.InitializeIfLoadRefused();
            }
            m_SceneFiles.ServiceNewRequest();
        }

        // Opening an extra scene view, a second angle or the four-up grid allocates a SceneRenderer + Init()
        // (WaitDeviceIdle + framebuffer creation) — serviced here, between frames, like scene load/stop above.
        if ( !StartupLoading() )
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

        // First-frame prefs application (needs a live camera) + autosave timer.
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

            // Autosave: Edit mode only, only when something actually changed since the last autosave.
            // Writes a SEPARATE file under <Project>/Saved/Autosaves (Autosave::PathFor) — never the main
            // save, and never anything under the assets root.
            static float    s_AutosaveAccum        = 0.0f;
            static uint64_t s_LastAutosaveRevision = 0;
            const auto&     prefs                  = EditorPreferences::Get();
            if ( prefs.AutosaveMinutes > 0 &&
                 m_Workspace.ActiveScene()->GetState() == ::Desert::Core::Scene::SceneState::Edit )
            {
                s_AutosaveAccum += frameTs.GetSeconds();
                if ( s_AutosaveAccum >= static_cast<float>( prefs.AutosaveMinutes ) * 60.0f )
                {
                    s_AutosaveAccum    = 0.0f;
                    const uint64_t rev = CommandHistory::Get().Revision();
                    if ( rev != s_LastAutosaveRevision )
                    {
                        Desert::Core::SceneSerializer serializer( m_Workspace.ActiveScene().get(),
                                                                  m_AssetManager.get() );
                        const auto                    path = Autosave::PathFor( m_SceneFiles.OpenScenePath(),
                                                                                m_Workspace.ActiveScene()->GetSceneName(),
                                                                                Autosave::kPeriodicSuffix );
                        const auto      dir  = path.parent_path();
                        std::error_code ec;
                        std::filesystem::create_directories( dir, ec );
                        const auto written = ec ? Common::MakeFormattedError( "could not create {}: {}",
                                                                              dir.string(), ec.message() )
                                                : WrittenOrError( Desert::Core::ExternalEntities::WriteSceneText(
                                                       path, serializer.SerializeToJson() ) );
                        if ( written )
                        {
                            // The revision is marked done ONLY on a write that landed. It used to be
                            // marked before the write, so a failed autosave was never retried: the next
                            // tick saw the same revision, decided nothing had changed, and skipped —
                            // and the log said the autosave had happened. A user going for their
                            // autosave after a crash found an old file or none.
                            s_LastAutosaveRevision = rev;
                            LOG_INFO( "[Autosave] {}", path.string() );
                        }
                        else
                        {
                            LOG_ERROR( "[Autosave] {} was NOT written: {}. The next autosave tick will "
                                       "try this revision again.",
                                       path.string(), written.GetError() );
                        }
                    }
                }
            }
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
            for ( auto& doc : m_Workspace.Documents() )
            {
                if ( doc->Registry )
                {
                    doc->Registry->TickRenderTextures( *m_AssetManager, frameTs );
                }
            }
        }

        // ONE thumbnail capture pump for the whole editor. Panels only request; whether the asset browser
        // is open, hidden or closed no longer changes whether previews progress, and a request made by one
        // panel is finished for all of them.
        //
        // Two halves, two gates (Editor/Splash/RevealGate.hpp). A PNG already in the disk cache is decoded
        // on a worker even while the splash is up, so the first frame after the hand-over only uploads it.
        // A CAPTURE is not: it shares the settle's frames and asset loader — the one thing the splash is
        // waiting on — so requests made before the hand-over stay queued and are served after it.
        if ( Splash::ThumbnailDiskDecodeAllowed( CurrentRevealState() ) )
            ThumbnailService::TickDiskAndDecode();
        UploadSplashThumbnails();
        // THE ONE CAPTURE THE SPLASH MAY RUN (THUMB3, THM1m, THM1n-13): the open scene's subjects, then every
        // uncaptured picture of the project, queued by WarmSplashScene. A warmed mesh still being read when
        // the window appears keeps being asked for after it (TickWarmMeshes), first in the queue once queued.
        if ( m_Revealed && m_FileExplorerPanel != nullptr )
            (void)m_FileExplorerPanel->TickWarmMeshes();
        if ( Splash::ThumbnailCaptureAllowed( CurrentRevealState() ) )
            ThumbnailService::Get().TickCapture( ThumbnailWarmup::CaptureScope::Everything );
        else if ( Splash::SceneThumbnailCaptureAllowed( CurrentRevealState() ) &&
                  ThumbnailService::Get().SceneWarmPending() > 0 )
            ThumbnailService::Get().TickCapture( ThumbnailWarmup::CaptureScope::SceneWarmOnly );

        UpdateContextualPanels();

        // Asset hot-reload: pick up edited .demat/.shader files (runs BEFORE scene rendering so
        // a shader-triggered pipeline invalidation never touches an in-recording frame).
        if ( m_AssetManager )
            m_AssetHotReload.Tick( frameTs, *m_AssetManager, m_Workspace.ActiveScene().get() );

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
        m_Shots.BeginPlayIfDue( StartupLoading() );
        m_Shots.PlaceCamera( StartupLoading() );

        // WAS ANYTHING STILL OUTSTANDING WHEN THIS FRAME WAS MADE? Sampled HERE, and the position is the
        // whole of its meaning: after every deferred queue above has drained — scene loads, document
        // closes, asset opens, leaving Play — and before a single pixel of this frame is rendered.
        //
        // Sampled rather than asked for later, because by the time the frame has been presented the
        // answer has moved on, and the question the control channel needs answered is about the picture:
        // "did this frame have everything the command asked for in it, or was some of it still queued?"
        // Editor/Core/Control/ControlPipeline.hpp is where that question is judged.
        m_Control.SampleFrameQuiescence( StartupLoading() || ContentSettling() );

        // Multi-scene editing: drive EVERY open document each frame so all viewports render live. The active
        // one is m_Workspace.ActiveScene() (rebound on viewport focus); RigBuilder / F9 below act on it only. The
        // outline aid + Begin/RegistryRender/OnUpdate/End are folded into UpdateSceneFrame (see below), applied
        // per scene so a secondary viewport is a full, independent render — not a static snapshot.
        if ( auto r = UpdateSceneFrame( *m_Workspace.PrimaryScene(), m_Workspace.PrimaryRegistry(), frameTs ); !r )
            return Common::MakeError( r.GetError() );
        for ( auto& doc : m_Workspace.Documents() )
            if ( auto r = UpdateSceneFrame( *doc->Scene, doc->Registry.get(), frameTs ); !r )
                return Common::MakeError( r.GetError() );

        // Runs a queued "Convert to Skinned" (rig builder) here, outside ImGui component iteration — the swap
        // removes the StaticMeshComponent the Details panel is drawing, so it must not happen mid-render.
        if ( m_Workspace.ActiveScene() && m_AssetManager )
            RigBuilder::ProcessPending( *m_Workspace.ActiveScene(), *m_AssetManager );

        if ( const auto& shot = ShotOptions::Get();
             shot.FlightRoute && m_Workspace.ActiveScene() && !m_SceneFiles.HasPendingLoad() &&
             !StartupLoading() &&
             m_Workspace.ActiveScene()->GetState() == ::Desert::Core::Scene::SceneState::Play )
            RecordFlightFrame( !ContentSettling() );

        // Screenshot mode, SECOND HALF (ShotDirector::CountRenderedFrame). On the capture's last frame the layer
        // adds its own records — the profiler dump, the --flight CSV — and closes with the capture's status.
        if ( m_Shots.CountRenderedFrame( StartupLoading() || ContentSettling() ) )
        {
            const auto& shot = ShotOptions::Get();
            if ( shot.GpuProfile )
                DumpProfilerToLog();
            if ( shot.FlightRoute && !FinishFlight() )
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
    void EditorLayer::OnFramePresented()
    {
        RevealWhenReady();
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
            sr->SetQuality( Common::Settings::MachineSettings::Get() );
        }

        // BEFORE the scene's frame, not between its phases: the scene opens and closes each view's
        // renderer itself now (Scene::OnUpdate), and nothing may sit between a renderer's open and its
        // close. Today this records nothing into the graph anyway — the editor's injected passes execute
        // inside the renderer's own update — so moving it costs the frame nothing.
        if ( registry )
            registry->Render();

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
        if ( StartupLoading() || ContentSettling() )
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
            ImGuiIO&   io       = ::ImGui::GetIO();
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

        static bool               dockspaceOpen  = true;
        static bool               opt_fullscreen = true;
        static ImGuiDockNodeFlags dockspace_flags =
             ImGuiDockNodeFlags_NoWindowMenuButton | ImGuiDockNodeFlags_NoCloseButton;

        // We are using the ImGuiWindowFlags_NoDocking flag to make the parent window not dockable into,
        // because it would be confusing to have two docking targets within each others.
        ImGuiWindowFlags window_flags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking;

        // Menu Bar
        ::ImGui::PushStyleVar( ImGuiStyleVar_WindowBorderSize, 0.0f );
        DrawMenuBar();
        ::ImGui::PopStyleVar();

        if ( opt_fullscreen )
        {
            const ImGuiViewport* viewport = ::ImGui::GetMainViewport();

            auto pos     = viewport->Pos;
            auto size    = viewport->Size;
            bool menuBar = true;
            if ( menuBar )
            {
                const float infoBarSize = ::ImGui::GetFrameHeight();
                pos.y += infoBarSize;
                size.y -= infoBarSize;
            }

            ::ImGui::SetNextWindowPos( pos );
            ::ImGui::SetNextWindowSize( size );
            ::ImGui::SetNextWindowViewport( viewport->ID );

            ::ImGui::PushStyleVar( ImGuiStyleVar_WindowRounding, 0.0f );
            ::ImGui::PushStyleVar( ImGuiStyleVar_WindowBorderSize, 0.0f );
            window_flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                            ImGuiWindowFlags_NoMove;
            window_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
        }

        // When using ImGuiDockNodeFlags_PassthruCentralNode, DockSpace() will render our background
        // and handle the pass-thru hole, so we ask Begin() to not render a background.
        if ( dockspace_flags & ImGuiDockNodeFlags_DockSpace )
            window_flags |= ImGuiWindowFlags_NoBackground;

        // Important: note that we proceed even if Begin() returns false (aka window is collapsed).
        // This is because we want to keep our DockSpace() active. If a DockSpace() is inactive,
        // all active windows docked into it will lose their parent and become undocked.
        // We cannot preserve the docking relationship between an active window and an inactive docking, otherwise
        // any change of dockspace/settings would lead to windows being stuck in limbo and never being visible.
        ::ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( 0.0f, 0.0f ) );
        ::ImGui::Begin( "DockSpace Demo", &dockspaceOpen, window_flags );
        ::ImGui::PopStyleVar();

        if ( opt_fullscreen )
            ::ImGui::PopStyleVar( 2 );

        // Toolbar strip FIRST so it reserves its height at the top; the DockSpace below then fills the
        // remaining area (drawing it after a full-height DockSpace(0,0) would push the bar off-screen).
        m_Toolbar.Draw();

        // Submit the DockSpace
        ImGuiIO& io = ::ImGui::GetIO();

        if ( io.ConfigFlags & ImGuiConfigFlags_DockingEnable )
        {
            // Reserve the bottom status-bar height so the DockSpace fills only the area between the toolbar
            // and the status bar (a full-height DockSpace(0,0) would sit under the status bar).
            // UE's major tabs: "Scene | <asset>" above everything, each asset editor owning the whole area.
            m_Documents.DrawMajorTabStrip();
            const float statusBarHeight = ::ImGui::GetFrameHeight() + 4.0f;
            ImVec2      dockSize        = ::ImGui::GetContentRegionAvail();
            dockSize.y                  = ( dockSize.y > statusBarHeight ) ? dockSize.y - statusBarHeight : 0.0f;

            ImGuiID dockspace_id = ::ImGui::GetID( "MyDockSpace" );

            // One-time auto-relayout: when the default layout's window IDs change (panel-title icons add a
            // ### suffix, changing every window's ImGui ID), old imgui.ini bindings stop matching and panels
            // scatter. Bump kDockLayoutVersion to force a single clean rebuild for everyone, then persist it.
            // 3: the centre is split and documents get a node of their own (layout option B.1).
            // 4: the centre is the level's alone again; documents open as tabs beside it (DocumentPlacement)
            //    and the Documents index moves to the bottom drawer.
            constexpr int kDockLayoutVersion = 4;
            if ( EditorPreferences::Get().DockLayoutVersion < kDockLayoutVersion )
            {
                // SAID OUT LOUD. Every existing imgui.ini is rebuilt once here, and a layout that changes
                // in silence is read as the editor having lost the user's panels — which is the same
                // complaint an area that collapses on its own produces, and the reason B.1 does not
                // collapse. One line naming the old and new versions is the difference between "my layout
                // was reset by the update" and "my layout is gone".
                LOG_INFO( "[Editor] Docking layout rebuilt once: saved layout is version {}, this build lays "
                          "out version {} (asset documents now open as tabs beside the level viewport, and the "
                          "Documents index is a tab in the bottom drawer). Your named layouts under View -> "
                          "Layouts are untouched.",
                          EditorPreferences::Get().DockLayoutVersion, kDockLayoutVersion );

                m_ResetDefaultLayout = true;

                // SaveMigrated, NOT Save: nothing the user did triggered this write. It fires on the
                // first frame after an update, because a stored layout version is being raised to the
                // one this build lays out — the same shape as the metre-era grid snap Load() raises, and
                // the same reason it must be written back (a version that did not persist would rebuild
                // the layout on every launch). Save() means "the user changed a setting" and would have
                // logged this one as if they had.
                const std::string layoutVersions = std::to_string( EditorPreferences::Get().DockLayoutVersion ) +
                                                   " -> " + std::to_string( kDockLayoutVersion );

                EditorPreferences::Get().DockLayoutVersion = kDockLayoutVersion;
                EditorPreferences::SaveMigrated( "docking layout version " + layoutVersions );
            }

            // First run (nothing saved in imgui.ini for this dockspace): lay the panels
            // out into a sensible default instead of leaving them floating in a pile.
            // Checked BEFORE DockSpace() — the call itself creates the node. "Reset to Default
            // Layout" (View -> Layouts) forces the same rebuild on demand.
            const bool buildDefaultLayout =
                 ::ImGui::DockBuilderGetNode( dockspace_id ) == nullptr || m_ResetDefaultLayout;
            m_ResetDefaultLayout = false;

            // While an asset editor's major tab is in front the level's dockspace is KEPT ALIVE but not shown:
            // its windows are not submitted (the panel loop skips them), and KeepAliveOnly keeps them docked
            // where they were, so the Scene tab brings the level layout back untouched.
            const ImVec2 dockOrigin = ::ImGui::GetCursorScreenPos();
            m_Documents.SetMajorTabArea( glm::vec2( dockOrigin.x, dockOrigin.y ),
                                         glm::vec2( dockSize.x, dockSize.y ) );
            ::ImGui::DockSpace( dockspace_id, dockSize,
                                m_Documents.MajorTabActive() ? dockspace_flags | ImGuiDockNodeFlags_KeepAliveOnly
                                                             : dockspace_flags );
            if ( m_Documents.MajorTabActive() )
                ::ImGui::Dummy( dockSize );

            if ( buildDefaultLayout )
            {
                ::ImGui::DockBuilderRemoveNode( dockspace_id );
                ::ImGui::DockBuilderAddNode( dockspace_id, dockspace_flags | ImGuiDockNodeFlags_DockSpace );
                ::ImGui::DockBuilderSetNodeSize( dockspace_id, ( dockSize.x > 0 && dockSize.y > 0 )
                                                                    ? dockSize
                                                                    : ::ImGui::GetMainViewport()->Size );

                //  ┌───────────┬────────────────────────────┬──────────────┐
                //  │ Scene     │ Scene (viewport) + a tab   │ Details      │
                //  │ Outliner  │ per open asset document    ├──────────────┤
                //  ├───────────┤ (DocumentPlacement)        │ SceneSettings│
                //  │Collections├────────────────────────────┤ / Profiler   │
                //  │           │ Assets / Logs / Documents  │              │
                //  └───────────┴────────────────────────────┴──────────────┘
                //
                // THE CENTRE IS WHOLE. An asset document is a tab beside the level, the way Unreal opens an
                // asset editor: it gets the full work area while it is the active tab. The split-off
                // document column this replaced (layout option B.1) left a Material Editor ~400 px wide.
                ImGuiID center = dockspace_id;
                ImGuiID right  = ::ImGui::DockBuilderSplitNode( center, ImGuiDir_Right, 0.20f, nullptr, &center );
                ImGuiID left   = ::ImGui::DockBuilderSplitNode( center, ImGuiDir_Left, 0.22f, nullptr, &center );
                ImGuiID bottom = ::ImGui::DockBuilderSplitNode( center, ImGuiDir_Down, 0.28f, nullptr, &center );
                m_BottomDockId     = bottom; // remembered so the drawer can be collapsed/restored later
                ImGuiID leftBottom = ::ImGui::DockBuilderSplitNode( left, ImGuiDir_Down, 0.40f, nullptr, &left );
                ImGuiID rightBottom =
                     ::ImGui::DockBuilderSplitNode( right, ImGuiDir_Down, 0.50f, nullptr, &right );

                // Panels routed through the central Begin carry an icon (a ### suffix), so dock them by the
                // SAME composed title — otherwise the icon-changed ImGui ID wouldn't match this assignment.
                // Non-panel windows (Profiler / Shader Code) self-Begin with plain names.
                ::ImGui::DockBuilderDockWindow( PanelDisplayTitle( "Scene###scene" ).c_str(), center );
                ::ImGui::DockBuilderDockWindow( PanelDisplayTitle( "Scene Outliner" ).c_str(), left );
                ::ImGui::DockBuilderDockWindow( PanelDisplayTitle( "Collections" ).c_str(), leftBottom );
                ::ImGui::DockBuilderDockWindow( PanelDisplayTitle( "Details" ).c_str(), right );
                ::ImGui::DockBuilderDockWindow( PanelDisplayTitle( "World Settings" ).c_str(), rightBottom );
                ::ImGui::DockBuilderDockWindow( "Profiler", rightBottom );
                // NO LINE FOR "Foliage##FoliagePanel" (FO-UI1): docked here it became a tab behind Scene Settings
                // that entering Foliage mode never showed, in a node ~300 px tall. It floats over the viewport's
                // left edge while Foliage mode is on (FoliagePaintTool::DrawPanel), as UE's mode toolkit sits
                // beside the level.
                ::ImGui::DockBuilderDockWindow( PanelDisplayTitle( "Assets" ).c_str(), bottom );
                ::ImGui::DockBuilderDockWindow( PanelDisplayTitle( "Logs" ).c_str(), bottom );
                ::ImGui::DockBuilderDockWindow( "Shader Code", bottom );

                // Contextual tools (IPanel::IsContextual) get a home too, so the one that opens itself
                // lands where its work belongs instead of floating over the scene: timelines along the
                // bottom next to Assets/Logs, authoring palettes on the right beside Details.
                ::ImGui::DockBuilderDockWindow( PanelDisplayTitle( "Anim Layers" ).c_str(), bottom );
                // No line for "Anim Graph", "Particle Editor", "UI Editor" or "Sequencer": they are
                // documents, and a document does not have a fixed home in the layout — it docks into the
                // document well beside the others (DrawDocuments sets the dock id), which is the whole point
                // of the well existing. A line here would also name a window that no longer exists under
                // that title: a document's ImGui id is "###doc<subject>", so it could never have matched.
                ::ImGui::DockBuilderDockWindow( PanelDisplayTitle( "Modeling" ).c_str(), left );
                ::ImGui::DockBuilderDockWindow( PanelDisplayTitle( "Landscape" ).c_str(), left );
                // UE's World Partition editor is a docked tab whose map fills it. The left column is the
                // tallest node that is not the level, so the map gets a near-square canvas beside the Outliner.
                ::ImGui::DockBuilderDockWindow( PanelDisplayTitle( "World Partition" ).c_str(), left );

                // The well is the INDEX of open documents, not where they open: a document opens as a tab
                // beside the level viewport (DocumentPlacement), so the well is a tab in the drawer and
                // the centre stays whole for the level and the documents.
                ::ImGui::DockBuilderDockWindow( DocumentHost::WellWindowTitle(), bottom );
                m_Documents.Well().ShowWindow(); // the default layout has the well open

                ::ImGui::DockBuilderFinish( dockspace_id );
            }

            // ── THE FOUR-UP GRID, APPLIED ─────────────────────────────────────────────────────────
            //
            // It splits THE NODE THE MAIN VIEWPORT IS IN, not the whole dockspace: the Outliner, Details
            // and the bottom drawer keep their places, and the grid costs exactly the pixels the single
            // viewport had. That is the difference between "a layout command" and "Reset to Default
            // Layout with four viewports in it", and it is why this is not folded into the block above.
            //
            // A FLOATING main viewport has no node to split (DockId 0); the dockspace is the honest
            // fallback, and it is said out loud because the result then displaces the other panels.
            if ( const auto& grid = m_Workspace.PendingViewportGrid(); !grid.empty() )
            {
                ImGuiID node = 0;
                if ( const ::ImGuiWindow* win = ::ImGui::FindWindowByName( PanelDisplayTitle( grid[0] ).c_str() ) )
                    node = win->DockId;
                if ( node == 0 || ::ImGui::DockBuilderGetNode( node ) == nullptr )
                {
                    LOG_WARN( "[Editor] the main viewport is not docked; the grid takes the whole "
                              "dockspace, so other panels move." );
                    node = dockspace_id;
                }

                // Quarters, in the order BuildViewportGrid filled the list: top-left, top-right,
                // bottom-left, bottom-right. A list shorter than four (the slot budget refused a pane)
                // simply leaves that quarter to its neighbours, which is what DockBuilder does with an
                // empty node.
                ImGuiID topLeft = node;
                ImGuiID topRight =
                     ::ImGui::DockBuilderSplitNode( topLeft, ImGuiDir_Right, 0.5f, nullptr, &topLeft );
                const ImGuiID bottomLeft =
                     ::ImGui::DockBuilderSplitNode( topLeft, ImGuiDir_Down, 0.5f, nullptr, &topLeft );
                const ImGuiID bottomRight =
                     ::ImGui::DockBuilderSplitNode( topRight, ImGuiDir_Down, 0.5f, nullptr, &topRight );

                const ImGuiID quarters[4] = { topLeft, topRight, bottomLeft, bottomRight };
                for ( size_t i = 0; i < grid.size() && i < 4; ++i )
                    ::ImGui::DockBuilderDockWindow( PanelDisplayTitle( grid[i] ).c_str(), quarters[i] );

                ::ImGui::DockBuilderFinish( dockspace_id );
                m_Workspace.ClearPendingViewportGrid();
            }
        }

        // THE TOOLS. The document loop is DrawDocuments, below, and the two are separate for the reason the
        // whole task exists: a tool passes &GetVisibility() to Begin, which is right for a setting the user
        // keeps, and a document must not — its false would be read as "destroy this window".
        //
        // The cascade this loop used to carry for documents is gone with them: a document is DOCKED into the
        // well now, so there is no floating window to step down-right from the last one.
        for ( const auto& panel : m_Panels )
        {
            if ( !panel->GetVisibility() || m_Documents.MajorTabActive() )
            {
                continue;
            }

            namespace ImGui = ::ImGui;
            // One padding rule for the whole editor, declared by the panel (the viewport asks for zero).
            // The panel states it as a glm::vec2 -- IPanel.hpp must not name the toolkit -- and this is
            // the line that draws, so this is where it becomes an ImVec2.
            const glm::vec2 padding = panel->GetWindowPadding();
            ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( padding.x, padding.y ) );

            // First-ever open: give the panel its preferred size, centered on the main viewport —
            // floating tools no longer pop up as tiny windows in a corner. imgui.ini keeps the
            // user's layout afterwards (FirstUseEver never fights it).
            if ( const glm::vec2 defSize = panel->GetDefaultSize(); defSize.x > 0.0f && defSize.y > 0.0f )
            {
                ImGui::SetNextWindowSize( ImVec2( defSize.x, defSize.y ), ImGuiCond_FirstUseEver );
                ImGui::SetNextWindowPos( ImGui::GetMainViewport()->GetCenter(), ImGuiCond_FirstUseEver,
                                         ImVec2( 0.5f, 0.5f ) );
            }

            // p_open: the title-bar X closes the panel and stays in sync with the View menu. The display
            // title carries an icon but keeps the ImGui ID == GetName() (see PanelDisplayTitle).
            // A panel that just auto-opened is brought to the front of its dock, otherwise it would
            // appear as a background tab nobody notices.
            if ( !m_FocusPanel.empty() && panel->GetName() == m_FocusPanel )
            {
                ImGui::SetNextWindowFocus();
                m_FocusPanel.clear();
            }

            // Maximize / restore (palette "Panel" group): the dock node is read from the window as it stands,
            // so a panel the user re-docked by hand is simply not maximized any more.
            {
                const ImGuiWindow* window =
                     ImGui::FindWindowByName( PanelDisplayTitle( panel->GetName() ).c_str() );
                const std::uint32_t dockId = window != nullptr ? window->DockId : 0;
                const auto directive       = m_PanelMaximize.Before( PanelShownName( panel->GetName() ), dockId );
                switch ( directive.Kind )
                {
                    case PanelMaximize::Step::Undock:
                    {
                        const ImGuiViewport* viewport = ImGui::GetMainViewport();
                        ImGui::SetNextWindowDockID( 0, ImGuiCond_Always );
                        ImGui::SetNextWindowViewport( viewport->ID );
                        ImGui::SetNextWindowPos( viewport->WorkPos, ImGuiCond_Always );
                        ImGui::SetNextWindowSize( viewport->WorkSize, ImGuiCond_Always );
                        ImGui::SetNextWindowFocus();
                        break;
                    }
                    case PanelMaximize::Step::Redock:
                        ImGui::SetNextWindowDockID( directive.DockId, ImGuiCond_Always );
                        ImGui::SetNextWindowFocus();
                        break;
                    case PanelMaximize::Step::None:
                        break;
                }
            }

            ImGui::Begin( PanelDisplayTitle( panel->GetName() ).c_str(), &panel->GetVisibility() );
            ImGui::PopStyleVar(); // right after Begin: the window kept it, child windows must not inherit
            {
                DESERT_PROFILE_SCOPE_DYNAMIC( panel->GetName().c_str() );
                panel->OnUIRender();
            }
            panel->TrackWindowInteraction();
            ImGui::End();
        }
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
            DrawProfilerWindow();

        m_StatusBar.Draw();

        DrawCommandPalette();
        DrawRecoveryPopup();
        DrawLayoutSavePopup();
        m_Documents.DrawOpenRefusedPopup();
        ImportOptions::DrawWindow(); // a dropped file never imported asks for its options first (THM1l)
        m_Documents.DrawCloseQuestionPopup();

        // Transient bottom-right notifications (save/import/validation). Drawn last so they float on top.
        Editor::ToastManager::Get().Draw();

        ::ImGui::End(); // End dockspace

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

    void EditorLayer::AppendPanelCommands( std::vector<PaletteCommand>& commands )
    {
        // Panels — jump to / reveal any tool window. TOOLS ONLY, and by construction rather than by a
        // filter: m_Panels is a PanelRegistry, which cannot hold a document. Before the split this loop
        // offered "Open M_Crate_Painted###assetdoc..." as a panel, and running it set a visibility flag that
        // the close pass then read as "the user dismissed this window".
        for ( const auto& panel : m_Panels )
        {
            IPanel*     p    = panel.get();
            std::string name = p->GetName();
            if ( const auto hash = name.find( "##" ); hash != std::string::npos )
                name.erase( hash ); // drop the "###id" ImGui suffix for display
            // Through PanelRequests, the one "show that panel" wire: it also brings the panel's tab
            // forward, which setting visibility alone never did for a panel already docked behind another.
            commands.push_back( { "Panel", "Open " + name, [p]
                                  {
                                      Core::PanelRequests::Open( p->GetName() );
                                      return PaletteCommandDone();
                                  } } );
        }
    }

    void EditorLayer::AppendMaximizeCommands( std::vector<PaletteCommand>& commands )
    {
        // Maximize any panel that sits in a dock now; restore the maximized one.
        {
            std::vector<std::string> docked;
            for ( const auto& panel : m_Panels )
            {
                if ( !panel->GetVisibility() )
                    continue;
                const ImGuiWindow* window =
                     ::ImGui::FindWindowByName( PanelDisplayTitle( panel->GetName() ).c_str() );
                if ( window != nullptr && window->DockId != 0 )
                    docked.push_back( PanelShownName( panel->GetName() ) );
            }
            for ( PaletteCommand& command : PanelMaximizePaletteCommands( m_PanelMaximize, docked ) )
                commands.push_back( std::move( command ) );
        }

        // Details: scroll to a field / open an asset picker, as the last Details frame drew them (CTL2).
        for ( PaletteCommand& command : DetailsPaletteCommands( GetDetailsNavigation() ) )
            commands.push_back( std::move( command ) );
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
        m_SceneFiles.AppendOpenSceneCommands( commands );

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
             focused && focused->HasPreview() )
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
                                          if ( !target || !target->HasPreview() )
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
                                          if ( !target )
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
                                          if ( !target )
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
                                      if ( !target )
                                          return Common::MakeError<bool>( "the document to save is no longer "
                                                                          "open." );
                                      return PaletteCommandOutcome(
                                           target->SaveDocument(),
                                           "the document was NOT written. Its own log line says why; this "
                                           "command only knows that no file was produced." );
                                  } } );
        }

        // Actions.
        //
        // RELEASING UNUSED ASSETS BY NAME. The sweep runs by itself on every scene change, which is the
        // policy (Engine/Assets/AssetEviction.hpp) — this is the same request under a name, so that a
        // person profiling a level can ask for it without changing scene, and so that the control channel
        // can. A capability reachable only as a side effect of something else is missing from THE
        // DICTIONARY, and the dictionary is this editor's claim that anything a person can do an agent can
        // do. It goes through the schedule rather than calling Run directly, so a manual sweep lands at the
        // same safe point in the frame as an automatic one.
        commands.push_back( { "Action", "Release unused assets", []
                              {
                                  Assets::AssetEvictionSchedule::Request( "asked for from the command "
                                                                          "palette" );
                                  return PaletteCommandDone();
                              } } );

        // REBUILD CONTENT REGISTRY — the remedy every refusal in this subsystem names, reachable
        // without restarting.
        //
        // Since T2.4 neither host scans the content roots at boot: `Cooked/AssetRegistry.dreg` is the
        // list of what the project has. Content authored IN this editor enters it the moment
        // `AssetManager::CreateAsset` sees the file, and content the cook writes enters it at the
        // write — but a file that arrived on disk with nobody looking (a `git pull`, a drop into the
        // folder while the editor was closed) has no row until something walks. The boot deliberately
        // does not walk; this is what does, on demand.
        //
        // IT IS IN THE DICTIONARY AND NOT ONLY IN A MENU, for the reason "Release unused assets" is:
        // a capability reachable only as a side effect of something else is missing from the palette,
        // and the palette is this editor's claim that anything a person can do an agent can do. The
        // packager refuses to build against a stale registry and its message names this command; a
        // named remedy that cannot be run is worse than no message.
        commands.push_back( { "Action", "Rebuild Content Registry", [this]
                              {
                                  const auto cooked = Assets::ContentRegistry::Refresh( *m_AssetManager );
                                  if ( !cooked )
                                      return Common::MakeFormattedError( "the content registry: {}",
                                                                         cooked.GetError() );
                                  LOG_INFO( "[ContentRegistry] {}", cooked.GetValue().Describe() );
                                  return PaletteCommandDone();
                              } } );

        // SAVE SCENE ANSWERS WHETHER IT SAVED. `(void)SaveOpenScene()` stood here against a
        // `[[nodiscard]] bool` — the attribute was on the declaration and the cast silenced it — so a
        // scene that could not be written came back over the channel as a success. This is the same
        // family as the toast that once said "Saved 'X'" for a file that had not been written
        // (FileSystem.hpp's note on the write primitive that is gone).
        m_SceneFiles.AppendSaveSceneCommand( commands );
    }

    void EditorLayer::AppendWindowCommands( std::vector<PaletteCommand>& commands )
    {
        commands.push_back( { "Action", "Close All Documents", [this]
                              {
                                  m_Documents.RequestCloseAllDocuments();
                                  return PaletteCommandDone();
                              } } );

        // THE WINDOW'S OWN COMMANDS, offered only when this editor owns its frame — with a system frame
        // they would be a second set of buttons for three things the OS already does, and the palette would
        // be offering to press a button that is right there.
        //
        // They are here for the reason Г14 put the palette itself on the channel: a capability reachable
        // only by a mouse does not exist for an unattended run. The title bar's buttons and its
        // double-click call exactly these two window methods, so a client that cannot click can still put
        // the window through maximize and restore and photograph what came out — which is the ONLY way the
        // maximize path in this build has been executed at all, the gesture itself being unsynthesisable
        // on this machine.
        if ( m_WindowChrome )
        {
            const auto& window = m_Application->GetWindow();
            commands.push_back( { "Window", "Maximize", [window]
                                  {
                                      window->Maximize();
                                      return PaletteCommandDone();
                                  } } );
            commands.push_back( { "Window", "Restore", [window]
                                  {
                                      window->Restore();
                                      return PaletteCommandDone();
                                  } } );
            commands.push_back( { "Window", "Minimize", [window]
                                  {
                                      window->Minimize();
                                      return PaletteCommandDone();
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

    void EditorLayer::DrawRecoveryPopup()
    {
        namespace ImGui = ::ImGui;

        if ( !m_ShowRecoveryPrompt )
            return;

        constexpr const char* kId = "Recover unsaved work?##recovery";
        ImGui::OpenPopup( kId );

        const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos( center, ImGuiCond_Appearing, ImVec2( 0.5f, 0.5f ) );

        if ( ImGui::BeginPopupModal( kId, nullptr, ImGuiWindowFlags_AlwaysAutoResize ) )
        {
            ImGui::TextUnformatted( "The previous session ended unexpectedly." );
            ImGui::Spacing();
            ImGui::Text( "Reopen the latest autosave?\n%s", m_RecoveryAutosave.filename().string().c_str() );
            ImGui::Spacing();
            ImGui::TextDisabled( "It opens as an unsaved scene — Save to keep it." );
            ImGui::Separator();

            if ( ImGui::Button( "Reopen autosave", ImVec2( 150.0f, 0.0f ) ) )
            {
                m_SceneFiles.RequestLoad( m_RecoveryAutosave );
                m_ShowRecoveryPrompt = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if ( ImGui::Button( "Ignore", ImVec2( 100.0f, 0.0f ) ) )
            {
                m_ShowRecoveryPrompt = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    void EditorLayer::DrawLayoutSavePopup()
    {
        namespace ImGui = ::ImGui;

        if ( !m_ShowSaveLayoutPopup )
            return;

        constexpr const char* kId = "Save Layout##saveLayout";
        ImGui::OpenPopup( kId );

        const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos( center, ImGuiCond_Appearing, ImVec2( 0.5f, 0.5f ) );

        if ( ImGui::BeginPopupModal( kId, nullptr, ImGuiWindowFlags_AlwaysAutoResize ) )
        {
            ImGui::TextUnformatted( "Layout name:" );
            ImGui::SetNextItemWidth( 260.0f );
            const bool submit = ImGui::InputText( "##layoutName", m_LayoutNameBuf, sizeof( m_LayoutNameBuf ),
                                                  ImGuiInputTextFlags_EnterReturnsTrue );

            const bool valid = !LayoutManager::Sanitize( m_LayoutNameBuf ).empty();
            ImGui::BeginDisabled( !valid );
            if ( ( ImGui::Button( "Save", ImVec2( 110.0f, 0.0f ) ) || submit ) && valid )
            {
                if ( !LayoutManager::Save( m_LayoutNameBuf ) )
                    Editor::ToastManager::Push( "The layout was not saved (see the log)",
                                                Editor::ToastLevel::Error );
                m_ShowSaveLayoutPopup = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if ( ImGui::Button( "Cancel", ImVec2( 110.0f, 0.0f ) ) )
            {
                m_ShowSaveLayoutPopup = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    void EditorLayer::DrawMenuBar()
    {
        namespace ImGui = ::ImGui;

        if ( !ImGui::BeginMainMenuBar() )
            return;

        m_MainMenu.DrawMenus();

        m_Toolbar.DrawProjectSection();
        m_Toolbar.DrawSceneRenameSection();
        // Play/Pause/Stop now live in the toolbar strip (LevelToolbar::Draw), not the menu bar.
        //
        // THIS BAR IS THE WINDOW'S TITLE BAR NOW. It already carried the project, the level, the menus and
        // the stats while the system frame sat above it drawing a second one; the editor asks for a window
        // without a frame (Sandbox.hpp), so the three window commands and the bar's own gestures come here.
        // Both are conditional on the window actually being frameless — with a system frame they would be a
        // second set of buttons for the same three actions.
        const float chromeWidth = m_WindowChrome ? UI::WindowChrome::WindowButtonsWidth() : 0.0f;
        DrawEngineStats( chromeWidth );
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

    void EditorLayer::RebuildCookedAssets()
    {
        // Idle first: re-registering rebuilds GPU textures/materials.
        Graphic::Renderer::GetInstance().WaitDeviceIdle();

        if ( m_ImportManager )
        {
            m_ImportManager->ImportAllFromDirectory( Common::Constants::Path::MESH_PATH, /*force=*/true );
            m_ImportManager->ImportAllFromDirectory( Common::Constants::Path::COLLECTIONS_PATH, /*force=*/true );
            // AND THE TEXTURES DIRECTORY, WHICH THIS COMMAND DID NOT REACH. Loose textures are cooked
            // only as a mesh's dependency or by a drag-and-drop, so `Assets/Textures/` — the checker
            // floor's texture and the sky panoramas — was the one place "Rebuild
            // Cooked Assets" could not rebuild. Found the day the container's version moved: the menu
            // entry whose whole job is "the cooked form is stale, make it again" left `T_Checker.tex`
            // stale, and the only remedy left was to drag the file back into the editor.
            (void)m_ImportManager->ImportLooseTextures();
        }

        // The clip rows may have changed with the re-cook; everything else is re-read on demand.
        if ( m_AnimationLibrary )
            Assets::IndexAnimationClips( *m_AssetManager, *m_AnimationLibrary );

        // Drop cached per-entity material instances so MeshECSSystem rebuilds them from the freshly
        // re-registered runtime materials (which now reference the reloaded texture images).
        if ( m_Workspace.ActiveScene() )
        {
            auto& reg = m_Workspace.ActiveScene()->GetRegistry();
            reg.view<ECS::StaticMeshComponent>().each( []( auto, ECS::StaticMeshComponent& c )
                                                       { c.RuntimeMaterialInstances.clear(); } );
            reg.view<ECS::SkinnedMeshComponent>().each( []( auto, ECS::SkinnedMeshComponent& c )
                                                        { c.RuntimeMaterialInstances.clear(); } );
        }

        if ( m_FileExplorerPanel )
            m_FileExplorerPanel->QueueRefresh();

        LOG_INFO( "[Editor] Rebuilt cooked assets" );
    }

    void EditorLayer::BeginContentSettle()
    {
        m_Content.BeginWorld( Assets::ContentWorkNow().Started );
    }

    // SECONDS PER ITEM, MEASURED — so the bar's share of a stage is the share of the wait it is. A Debug
    // start of this project on the M-series development machine, read off the "[Startup] ... item(s) in"
    // lines below; a machine twice as fast halves every stage alike and the shares do not move.
    void EditorLayer::MakeSplashPlan()
    {
        m_ShaderStage =
             m_Progress.AddStage( "Compiling shaders...", kSecondsPerShader, Assets::EngineShaderCount() );
        for ( StartupStage& stage : m_StartupStages )
            stage.ProgressStage =
                 stage.ItemCosts ? m_Progress.AddStage( stage.Label, stage.SecondsPerItem, stage.ItemCosts() )
                                 : m_Progress.AddStage( stage.Label, stage.SecondsPerItem,
                                                        stage.CountItems ? stage.CountItems() : 1 );
        // The scene's reads are started by the scene load and counted only when the settle begins.
        m_SettleStage =
             m_Progress.AddStage( "Loading scene content...", kSecondsPerSceneRead, 1, kSecondsSceneSettle );
    }

    void EditorLayer::BeginSplashStage( const std::size_t stage, const std::optional<std::size_t> items )
    {
        const double now =
             std::chrono::duration<double>( std::chrono::steady_clock::now() - m_ProgressEpoch ).count();
        if ( const auto finished = m_Progress.BeginStage( stage, now, items ) )
            LOG_INFO( "[Startup] {} {} item(s) in {:.2f} s", finished->Name, finished->Units, finished->Seconds );
        PushSplash();
    }

    void EditorLayer::PushSplash()
    {
        if ( m_Splash && !m_Revealed )
            m_Splash->SetProgress( m_Progress.Snapshot() );
    }

    Assets::ItemProgress EditorLayer::SplashItems()
    {
        return [this]( const std::string& item, const std::size_t done, const std::size_t total )
        {
            m_Progress.Step( item, done, total );
            PushSplash();
        };
    }

    // SHOWN AFTER THE FIRST REAL FRAME IS PRESENTED, NOT BEFORE IT IS DRAWN. The window has been presented
    // loading frames the whole time it was hidden, and a window shown ahead of the first real present would
    // put the last of those — an empty frame — on screen for as long as that frame takes. Shown here, the
    // surface it reveals already holds the editor, and the splash crossfades into it from this instant.
    void EditorLayer::StartBackgroundCook()
    {
        m_BackgroundCook = std::make_unique<BackgroundCookQueue>(
             []( const std::filesystem::path& source )
             {
                 // One cooker per worker thread: Assimp importers are not reentrant.
                 thread_local ImportManager s_ThreadImporter;
                 return s_ThreadImporter.Import( source );
             },
             []( std::function<void()> job ) { Common::JobSystem::Get().Submit( std::move( job ) ); } );
        m_BackgroundCookStart = std::chrono::steady_clock::now();

        const std::array<std::filesystem::path, 2> roots{ Common::Constants::Path::MESH_PATH,
                                                          Common::Constants::Path::COLLECTIONS_PATH };
        for ( const std::filesystem::path& root : roots )
            for ( const std::filesystem::path& source : ImportManager::MeshSources( root ) )
                m_BackgroundCook->Enqueue( source );
        LOG_INFO( "[BackgroundCook] {} mesh source(s) queued on the JobSystem after the reveal; a source whose "
                  "cache entry is missing or stale stays Pending until its cook lands",
                  m_BackgroundCook->Total() );
    }

    void EditorLayer::ReloadRecookedMesh( const std::filesystem::path& source )
    {
        // THE PENDING ASSET AND THE COOKED ONE MUST BE THE SAME HANDLE: the scene already names the Pending one,
        // and nothing rewrites the scene when the cook lands. The handle comes from the asset's path (or a
        // header stated in the file at that path), never from the envelope the cook minted, so it holds — and a
        // drift would be a scene pointing at a mesh that never arrives, so it is checked, not assumed.
        const std::filesystem::path staticPath = CookPaths::MeshAsset( source );
        std::optional<uint64_t>     pendingHandle;
        if ( const auto pending = m_AssetManager->FindByPath<Assets::MeshAsset>( staticPath.generic_string() ) )
        {
            pendingHandle = static_cast<uint64_t>( pending->GetMetadata().Handle );
            // The failed load is dropped with the built GPU mesh; the shell stays, so the next draw reads the
            // fresh entry through the path a first use takes.
            if ( const auto unloaded = pending->Unload(); !unloaded )
                LOG_ERROR( "[BackgroundCook] '{}' was cooked but its Pending asset could not be reset: {}",
                           staticPath.string(), unloaded.GetError() );
            if ( auto* service = Runtime::ResourceRegistry::GetMeshService() )
                (void)service->EvictBuilt( pending->GetMetadata().Handle );
        }
        const auto resolved = MeshDnD::ResolveOrImportMesh( *m_AssetManager, source.string() );
        if ( resolved.Handle.IsNull() )
        {
            LOG_ERROR( "[BackgroundCook] '{}' cooked but its asset did not resolve; it stays Pending",
                       source.string() );
            return;
        }
        if ( pendingHandle && *pendingHandle != static_cast<uint64_t>( resolved.Handle ) )
            LOG_ERROR( "[BackgroundCook] '{}' was Pending as handle {} and resolved as {} after its cook; the "
                       "scene's reference no longer reaches it",
                       source.string(), *pendingHandle, static_cast<uint64_t>( resolved.Handle ) );
        LOG_INFO( "[BackgroundCook] '{}' cooked; its asset now resolves{}", source.string(),
                  pendingHandle ? " under the handle it was Pending as" : "" );
    }

    void EditorLayer::DrainBackgroundCook()
    {
        if ( !m_BackgroundCook )
            return;
        for ( const BackgroundCookQueue::Completed& done : m_BackgroundCook->Drain() )
        {
            switch ( DecideCookCompletion( done.Verdict ) )
            {
                case CookCompletionAction::Nothing:
                    break;
                case CookCompletionAction::ReportFailure:
                    ++m_BackgroundCookFailed;
                    LOG_ERROR( "[BackgroundCook] '{}' did not cook; its asset stays Pending (not drawn)",
                               done.Source.string() );
                    break;
                case CookCompletionAction::Reload:
                    ++m_BackgroundCookChanged;
                    ReloadRecookedMesh( done.Source );
                    break;
            }
        }
        if ( !m_BackgroundCookReported && m_BackgroundCook->AllSettled() )
        {
            m_BackgroundCookReported = true;
            LOG_INFO( "[BackgroundCook] {} mesh source(s) checked after the reveal in {} ms: {} cooked, {} failed",
                      m_BackgroundCook->Total(),
                      std::chrono::duration_cast<std::chrono::milliseconds>( std::chrono::steady_clock::now() -
                                                                             m_BackgroundCookStart )
                           .count(),
                      m_BackgroundCookChanged, m_BackgroundCookFailed );
        }
    }

    void EditorLayer::RevealWhenReady()
    {
        if ( !Splash::MayReveal( CurrentRevealState() ) )
            return;
        const double now =
             std::chrono::duration<double>( std::chrono::steady_clock::now() - m_ProgressEpoch ).count();
        if ( const auto finished = m_Progress.Finish( now ) )
            LOG_INFO( "[Startup] {} {} item(s) in {:.2f} s", finished->Name, finished->Units, finished->Seconds );
        PushSplash();
        m_Revealed = true;
        if ( const auto& window = m_Application->GetWindow() )
            window->Show();
        LOG_INFO( "[Startup] reveal: the scene's content has settled and the editor window is shown" );
        if ( m_FileExplorerPanel != nullptr )
            LOG_INFO( "[Thumbnails] {} thumbnails resident, {} captured on splash",
                      m_FileExplorerPanel->ResidentThumbnails(), m_SplashWarmTotal );
        StartBackgroundCook();
        // Starts the crossfade and returns; the splash object stays until this layer is destroyed.
        m_Splash->Close();
        LOG_INFO( "[Startup] the splash is closed" );
        // The counters are cumulative since process start, so this is every shader and pipeline cost paid before
        // the first real frame, including the pipelines the renderers build after the preload.
        LOG_INFO( "[Startup] shader work before the first frame: {}",
                  ::Desert::Core::FormatShaderPhaseTimes( ::Desert::Core::ReadShaderPhaseTimes() ) );
    }

    Splash::RevealState EditorLayer::CurrentRevealState() const
    {
        Splash::RevealState state;
        state.HasSplash        = m_Splash != nullptr;
        state.Revealed         = m_Revealed;
        state.StartupLoading   = StartupLoading();
        state.SceneLoadPending    = m_SceneFiles.HasPendingLoad();
        state.ContentSettling  = ContentSettling();
        state.RealFrameDrawn   = m_RealFrameDrawn;
        state.ThumbnailsUploading = m_ThumbnailsHoldReveal;
        return state;
    }

    void EditorLayer::UploadSplashThumbnails()
    {
        if ( m_Splash == nullptr || m_Revealed || m_FileExplorerPanel == nullptr )
        {
            m_ThumbnailsHoldReveal = false;
            return;
        }
        WarmSplashScene();
        // A cold mesh still being read counts too: it is a capture that has not been queued YET (THM1m).
        const std::size_t warmPending =
             ThumbnailService::Get().SceneWarmPending() + m_FileExplorerPanel->TickWarmMeshes();
        m_SplashWarmTotal = std::max( m_SplashWarmTotal, warmPending ); // a late resolve queues after the start
        // THE CAPTURES' PICTURES ARE UPLOADED TOO (THM1n-13): once every splash capture has landed, the PNGs they
        // wrote are asked of the workers like the rest, so the window never opens on a picture still on disk.
        if ( m_SplashWarmStarted && warmPending == 0 && !m_SplashPicturesReasked )
        {
            m_SplashPicturesReasked = true;
            m_FileExplorerPanel->RequestProjectPictures();
        }
        // An upload of pixels a worker already decoded from the disk cache: no renderer slot and no capture,
        // which is why it may run before the hand-over while ThumbnailCaptureAllowed is still false.
        const std::size_t pending = m_FileExplorerPanel->UploadPrefetchedThumbnails();
        if ( warmPending != m_SplashWarmShown )
        {
            // The splash says what it is waiting on, as every other stage does.
            m_SplashWarmShown = warmPending;
            m_Progress.Step( "Scene thumbnails", m_SplashWarmTotal - std::min( warmPending, m_SplashWarmTotal ),
                             m_SplashWarmTotal );
            PushSplash();
        }

        Splash::RevealState rest = CurrentRevealState();
        rest.ThumbnailsUploading = false;
        if ( !Splash::MayReveal( rest ) )
        {
            m_RevealOtherwiseReadySince.reset();
            m_ThumbnailsHoldReveal = pending > 0 || warmPending > 0;
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if ( !m_RevealOtherwiseReadySince )
            m_RevealOtherwiseReadySince = now;
        const double waitedMs =
             std::chrono::duration<double, std::milli>( now - *m_RevealOtherwiseReadySince ).count();
        const bool wasHolding  = m_ThumbnailsHoldReveal;
        m_ThumbnailsHoldReveal =
             Splash::ThumbnailsHoldReveal( pending ) || Splash::SceneCapturesHoldReveal( warmPending );
        if ( wasHolding && !m_ThumbnailsHoldReveal )
        {
            LOG_INFO( "[Thumbnails] the opening folder's and the scene's pictures held the hand-over {:.0f} ms",
                      waitedMs );
        }
    }

    void EditorLayer::WarmSplashScene()
    {
        // Once, when the scene the editor opens on is loaded and the renderer is up: the moment a scene
        // capture becomes allowed (Splash::SceneThumbnailCaptureAllowed).
        if ( m_SplashWarmStarted || !m_Workspace.ActiveScene() ||
             !Splash::SceneThumbnailCaptureAllowed( CurrentRevealState() ) )
            return;
        m_SplashWarmStarted = true;

        Assets::AssetRootSet roots;
        ::Desert::Core::CollectAssetRoots( *m_Workspace.ActiveScene(), roots );
        const std::vector<ThumbnailWarmup::WarmItem> scene = ThumbnailWarmup::SceneWarmList(
             roots.Handles(), []( const Common::AssetHandle& handle )
             { return Common::AssetPathIndex::PathFor( static_cast<uint64_t>( handle ) ); } );
        // EVERY PICTURE OF THE PROJECT (THM1n-13, owner 09-29): the content registry's rows of every kind, not the
        // folder the browser opens on — so no folder entered after the hand-over waits for a picture.
        const std::vector<ThumbnailWarmup::WarmItem> project =
             ThumbnailWarmup::ProjectWarmList( &Assets::ContentRegistry::FilesOfKind );
        for ( const ThumbnailWarmup::Unproduced& gap :
              ThumbnailWarmup::UnproducedKinds( &Assets::ContentRegistry::FilesOfKind ) )
            LOG_WARN(
                 "[Thumbnails] {} {} file(s) get no picture on the splash: the kind has no thumbnail producer "
                 "yet ({})",
                 gap.Files, Common::Content::KindName( gap.Kind ), gap.Why );
        m_SplashWarmTotal = m_FileExplorerPanel->WarmProjectThumbnails( scene, project );
        LOG_INFO( "[Thumbnails] the scene uses {} subject(s) of {} root(s), the project has {} picture(s); {} "
                  "picture(s) to capture before the hand-over, the rest decode from the disk cache",
                  scene.size(), roots.Size(), project.size(), m_SplashWarmTotal );
    }

    void EditorLayer::UpdateContentSettling()
    {
        const auto& loader  = Assets::AsyncAssetLoader::Get();
        const auto  work    = Assets::ContentWorkNow();
        const bool  settled = m_Content.Tick( work.Outstanding, work.Started );
        if ( !settled )
        {
            // THE SETTLE SAYS HOW MUCH IS LEFT, not only that it is waiting: a count that moves is the
            // difference between a load and a hang. Pushed only when the count changes.
            if ( ContentSettling() && !m_Revealed && loader.Outstanding() != m_SplashOutstandingShown )
            {
                m_SplashOutstandingShown = loader.Outstanding();
                const std::size_t items  = loader.StartedCount() - m_SettleBase;
                m_Progress.Step( "Scene assets", items - loader.Outstanding(), items );
                PushSplash();
            }
            return;
        }

        LOG_INFO( "[Content] settled after {} frame(s) in {:.1f} ms; {} read(s) have gone to a worker "
                  "this session. This is the cost that used to be a boot stage, and a scene that asks "
                  "for nothing pays none of it.",
                  m_Content.FramesWaited(), m_Content.ElapsedMs(), loader.StartedCount() );
        // PSO1: the content pipelines went to workers; what they still cost the frame is this line.
        const auto& pipelines = Graphic::PipelineBuilds::Get();
        const auto  blocked   = pipelines.CallerBlocked();
        // AL1-12: the gate waited for the engine's; material ones may still be compiling behind the default
        // surface.
        LOG_INFO(
             "[Content] pipelines: {} engine compiled on workers before the reveal, {} material requested "
             "on demand ({} still compiling); the frame was blocked {:.1f} ms in total, {:.2f} ms at most for one",
             pipelines.Started( Graphic::PipelineRole::Engine ),
             pipelines.Started( Graphic::PipelineRole::Material ),
             pipelines.Pending( Graphic::PipelineRole::Material ),
             std::chrono::duration<double, std::milli>( blocked.Total ).count(),
             std::chrono::duration<double, std::milli>( blocked.Max ).count() );
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

        const std::string title = "Desert Engine — " + Editor::ProjectContext::Current().Name + " — " +
                                  m_Workspace.ActiveScene()->GetSceneName();
        if ( window->GetTitle() != title )
            window->SetTitle( title );
    }

    // Collapse/restore the bottom drawer (the dock node holding Assets / Logs / Shader Code).
    //
    // ImGui has no "collapse a dock node" call — a docked window trades its collapse arrow for a tab.
    // So collapsing is done by SIZE: the node is squeezed down to its tab bar and restored to the height
    // it had before. That keeps the tabs on screen, which is the whole point of collapsing rather than
    // closing, and it leaves the user's own resize intact because the height is re-read at collapse time.
    void EditorLayer::DrawBottomDrawerToggle()
    {
        namespace ImGui = ::ImGui;

        // Resolve the drawer node from the Assets window's ACTUAL dock node, not from the id captured while
        // building the default layout: that branch only runs for a fresh layout, so with a restored
        // imgui.ini the id stayed 0 and this control was permanently dead.
        ImGuiDockNode* node = m_BottomDockId ? ImGui::DockBuilderGetNode( m_BottomDockId ) : nullptr;
        if ( !node )
        {
            if ( ImGuiWindow* assets = ImGui::FindWindowByName( PanelDisplayTitle( "Assets" ).c_str() );
                 assets && assets->DockNode )
            {
                node           = assets->DockNode;
                m_BottomDockId = node->ID;
            }
        }
        if ( !node )
        {
            ImGui::TextDisabled( ICON_MDI_CHEVRON_DOWN );
            return;
        }

        // Tab bar height + the node's own padding — what "collapsed" means for this node.
        const float collapsedHeight = ImGui::GetFrameHeight() + ImGui::GetStyle().WindowPadding.y * 2.0f;

        const char* icon = m_BottomCollapsed ? ICON_MDI_CHEVRON_UP : ICON_MDI_CHEVRON_DOWN;
        if ( ImGui::SmallButton( icon ) )
        {
            m_BottomCollapsed = !m_BottomCollapsed;
            if ( m_BottomCollapsed )
            {
                // Remember the CURRENT height, not the default: the user may have dragged the splitter.
                m_BottomHeight = node->Size.y;
                ImGui::DockBuilderSetNodeSize( m_BottomDockId, ImVec2( node->Size.x, collapsedHeight ) );
            }
            else
            {
                const float restore = m_BottomHeight > collapsedHeight
                                           ? m_BottomHeight
                                           : ImGui::GetMainViewport()->Size.y * 0.28f; // the layout default
                ImGui::DockBuilderSetNodeSize( m_BottomDockId, ImVec2( node->Size.x, restore ) );
            }
            ImGui::DockBuilderFinish( m_BottomDockId );
        }
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( m_BottomCollapsed ? "Expand the bottom drawer (Assets / Logs)"
                                                 : "Collapse the bottom drawer (Assets / Logs)" );
    }

    // The profiler table as text. Used by the panel's button AND by --gpu-profile, because a headless shot
    // draws no ImGui and the panel is the only other way these numbers are readable.
    //
    // The GPU column comes from the backend's timestamp queries, so it is device time, not the CPU's wait
    // for it; the two columns disagreeing is the interesting case rather than a fault.
    void EditorLayer::RecordFlightFrame( bool counted )
    {
        const ShotOptions&           shot     = ShotOptions::Get();
        Common::Profiling::Profiler& profiler = Common::Profiling::Profiler::Get();

        double gpuMs    = Flight::kNotMeasured;
        double streamMs = 0.0;
        for ( const Common::Profiling::ScopeResult& scope : profiler.LastFrame() )
        {
            if ( scope.Name == Common::Profiling::kGpuFrameTotalScope && profiler.GpuEnabled() )
                gpuMs = scope.GpuMs;
            else if ( scope.Name == "WorldStreamer::Tick" )
                streamMs = scope.TotalMs;
        }
        m_FlightLog.TimeLast( profiler.LastFrameMs(), gpuMs, streamMs );

        Flight::FrameRow row;
        row.Frame    = m_Shots.Frame();
        row.Kind     = m_Shots.Frame() < Flight::kWarmupFrames ? Flight::Phase::Warmup
                       : counted                               ? Flight::Phase::Flight
                                                               : Flight::Phase::Settling;
        row.Distance = Flight::DistanceAt( m_Shots.Frame(), shot.FlightSpeed, ShotOptions::PlayStepSeconds );
        row.Position = Flight::PoseAt( *shot.FlightRoute, row.Distance ).Position;
        row.Entities = m_Workspace.ActiveScene()->GetAllEntities().size();
        if ( m_Play.Streamer() && m_Play.Streamer()->Streams( *m_Workspace.ActiveScene() ) )
        {
            const auto& report   = m_Play.Streamer()->LastTick();
            row.ResidentRecords  = report.LiveRecords;
            row.UnitsActivated   = report.Tick.UnitsActivated;
            row.UnitsDeactivated = report.Tick.UnitsDeactivated;
            row.RecordsActivated = report.Tick.RecordsActivated;
            row.RecordsDestroyed = report.Tick.RecordsDestroyed;
            row.ActivationMs     = report.ActivationMs;
            row.ActivatedUnits   = report.ActivatedUnits;
        }
        row.AssetGpuBytes = Graphic::ResourceLedger::Take().BytesForOwner( Graphic::ResourceOwner::AssetService );
        m_FlightLog.Append( std::move( row ) );
    }

    bool EditorLayer::FinishFlight()
    {
        const ShotOptions& shot = ShotOptions::Get();
        const auto         rows = m_FlightLog.Rows();
        const auto         written =
             Common::Utils::FileSystem::WriteContentToFileAtomic( shot.FlightCsv, Flight::Csv( rows ) );
        if ( !written.IsSuccess() )
        {
            LOG_ERROR( "[Flight] the CSV was not written to '{}': {}", shot.FlightCsv, written.GetError() );
            return false;
        }
        const auto summary = Flight::Summarise( rows );
        if ( !summary.IsSuccess() )
        {
            LOG_ERROR( "[Flight] '{}': {}", shot.FlightRoute->Spec, summary.GetError() );
            return false;
        }
        LOG_INFO( "[Flight] '{}' at {:.0f} cm/s, {} row(s) in '{}': {}", shot.FlightRoute->Spec, shot.FlightSpeed,
                  rows.size(), shot.FlightCsv, Flight::Describe( summary.GetValue(), rows ) );
        return true;
    }

    void EditorLayer::DumpProfilerToLog()
    {
        auto& prof = ::Common::Profiling::Profiler::Get();

        const double frameMs = prof.LastFrameMs();
        const double fps     = frameMs > 0.0001 ? 1000.0 / frameMs : 0.0;

        const std::string frameTotalScope = ::Common::Profiling::kGpuFrameTotalScope;

        double gpuFrameMs = 0.0;
        double gpuSumMs   = 0.0;
        for ( const auto& s : prof.LastFrame() )
        {
            if ( s.Name == frameTotalScope )
                gpuFrameMs = s.GpuMs;
        }

        LOG_INFO( "[Profiler] ---- per-pass breakdown (averaged over {:.1f} s of frames) ----",
                  prof.AvgWindowSeconds() );
        LOG_INFO( "[Profiler] Frame (wall) {:.3f} ms ({:.0f} FPS), GPU frame {:.3f} ms", frameMs, fps,
                  gpuFrameMs );
        LOG_INFO( "[Profiler] {:<34} {:>10} {:>6} {:>10} {:>10} {:>6}", "scope", "cpu ms", "x", "gpu ms",
                  "gpu self", "x" );

        for ( const auto& s : prof.LastFrame() )
        {
            LOG_INFO( "[Profiler] {:<34} {:>10.3f} {:>6} {:>10.3f} {:>10.3f} {:>6}", s.Name, s.TotalMs, s.Calls,
                      s.GpuMs, s.GpuSelfMs, s.GpuCalls );
            // SELF time is the only summable column — the inclusive one counts a parent's microseconds
            // again in each child. The frame bracket is the denominator, not a pass, so it stays out.
            if ( s.GpuCalls > 0 && s.Name != frameTotalScope )
                gpuSumMs += s.GpuSelfMs;
        }

        LOG_INFO( "[Profiler] GPU self times sum to {:.3f} ms of a {:.3f} ms GPU frame ({:.1f} %); the "
                  "remainder is device work no pass is marked around.",
                  gpuSumMs, gpuFrameMs, gpuFrameMs > 0.0001 ? gpuSumMs / gpuFrameMs * 100.0 : 0.0 );

        // THE DRAW-CALL DETECTOR, READ WITHOUT A WINDOW. The counter itself landed with step 2 of
        // `Docs/World/PROGRAMME.md`, and its only reader was the viewport's perf HUD — which is ImGui,
        // which is drawn into the swapchain, which `--shot` does not read. So the one number step 3 is
        // judged on was unreadable in exactly the mode a measurement is taken in. It joins the profiler
        // dump rather than getting a flag of its own because it answers the same question the dump does
        // — what did this frame cost — and because a second flag would be a second thing to remember.
        const Graphic::DrawCounters drawCounters = Graphic::DrawCounter::LastFrame();
        LOG_INFO( "[Profiler] draws {} / instances {} (an instanced batch is ONE draw and many instances; "
                  "the two are printed apart because culling moves them in different proportions)",
                  drawCounters.Draws, drawCounters.Instances );
        LOG_INFO( "[Profiler] ---- end ----" );
    }

    void EditorLayer::DrawProfilerWindow()
    {
        namespace ImGui = ::ImGui;
        auto& prof      = ::Common::Profiling::Profiler::Get();

        if ( !m_ShowProfiler )
            return;

        const double frameMs = prof.LastFrameMs();
        const double fps     = frameMs > 0.0001 ? 1000.0 / frameMs : 0.0;

        ImGui::SetNextWindowSize( ImVec2( 420, 460 ), ImGuiCond_FirstUseEver );
        ImGui::SetNextWindowPos( ImVec2( 700, 120 ), ImGuiCond_FirstUseEver );
        if ( !ImGui::Begin( "Profiler", &m_ShowProfiler ) ) // X button clears m_ShowProfiler
        {
            ImGui::End();
            return;
        }

        ImGui::Checkbox( "Enabled", &prof.Enabled() );
        ImGui::SameLine();
        ImGui::Checkbox( "Sort by time", &prof.SortByTime() );
        ImGui::SameLine();
        // GPU timestamps are OFF by default: they cost ~8 % of a debug frame on MoltenVK, and an
        // always-on instrument means every later measurement carries the tax. Turning this on is a
        // deliberate act. See Docs/GPU_TIMESTAMPS.md for the measured price.
        ImGui::BeginDisabled( prof.GetGpuSink() == nullptr );
        ImGui::Checkbox( "GPU", &prof.GpuEnabled() );
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Device timestamps around every pass.\n"
                               "Costs about 8%% of the frame it measures, so it is off by default." );
        ImGui::SameLine();
        ImGui::BeginDisabled( !prof.GpuEnabled() );
        ImGui::Checkbox( "per-pass", &prof.GpuPassScopes() );
        ImGui::EndDisabled();
        if ( ImGui::IsItemHovered( ImGuiHoveredFlags_AllowWhenDisabled ) )
            ImGui::SetTooltip( "Off: time the whole frame only (two timestamps, near-free).\n"
                               "On: also time every pass, which is what costs." );
        ImGui::EndDisabled();
        if ( prof.GetGpuSink() == nullptr && ImGui::IsItemHovered( ImGuiHoveredFlags_AllowWhenDisabled ) )
            ImGui::SetTooltip( "This device reports no usable timestamp queries — CPU columns only." );

        ImGui::SetNextItemWidth( 160.0f );
        ImGui::SliderFloat( "Avg window (s)", &prof.AvgWindowSeconds(), 0.1f, 2.0f, "%.1f" );

        // The whole-frame GPU bracket the backend records around the command buffer. It is the denominator
        // the per-pass GPU column is checked against: the passes should tile it, not exceed it.
        double gpuFrameMs = 0.0;
        for ( const auto& s : prof.LastFrame() )
            if ( s.Name == ::Common::Profiling::kGpuFrameTotalScope )
                gpuFrameMs = s.GpuMs;

        ImGui::Text( "Frame: %.3f ms  (%.0f FPS)   [avg]", frameMs, fps );
        if ( gpuFrameMs > 0.0 )
        {
            ImGui::SameLine();
            ImGui::TextColored( ImVec4( 0.55f, 0.80f, 1.0f, 1.0f ), "GPU: %.3f ms", gpuFrameMs );
        }
        ImGui::SameLine();
        if ( ImGui::Button( "Dump to Log" ) )
            DumpProfilerToLog();

        ImGui::Separator();

        if ( ImGui::BeginTable( "##prof", 6,
                                ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
                                     ImGuiTableFlags_SizingStretchProp ) )
        {
            // The numeric columns are FIXED width and the name stretches. With six columns sharing the
            // width proportionally, the panel docked at its usual size truncated every header to
            // "cp... gp... gpu..." — unreadable, and the two GPU columns are the ones a reader has to
            // tell apart. A millisecond figure needs a known number of characters, not a share of the
            // panel, so it gets one.
            const float kNumWidth = ImGui::CalcTextSize( "0000.000" ).x;
            ImGui::TableSetupColumn( "scope", ImGuiTableColumnFlags_WidthStretch );
            ImGui::TableSetupColumn( "cpu", ImGuiTableColumnFlags_WidthFixed, kNumWidth );
            ImGui::TableSetupColumn( "gpu", ImGuiTableColumnFlags_WidthFixed, kNumWidth );
            ImGui::TableSetupColumn( "self", ImGuiTableColumnFlags_WidthFixed, kNumWidth );
            ImGui::TableSetupColumn( "%", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize( "000" ).x );
            ImGui::TableSetupColumn( "x", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize( "000" ).x );
            ImGui::TableHeadersRow();

            for ( const auto& s : prof.LastFrame() )
            {
                const double pct = frameMs > 0.0001 ? ( s.TotalMs / frameMs ) * 100.0 : 0.0;
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted( s.Name.c_str() );
                // Docked at its usual width the name column clips, and "Clouds: Sha" / "Clouds: Exe" are
                // two different passes. The full name on hover costs nothing and settles it.
                if ( ImGui::IsItemHovered() )
                    ImGui::SetTooltip( "%s", s.Name.c_str() );
                ImGui::TableNextColumn();
                ImGui::Text( "%.3f", s.TotalMs );
                ImGui::TableNextColumn();
                // A dash, not 0.000: a scope that records no GPU work and a scope the GPU timer could not
                // reach are different states, and printing zero for both invents a measurement.
                if ( s.GpuCalls > 0 )
                    ImGui::TextColored( ImVec4( 0.55f, 0.80f, 1.0f, 1.0f ), "%.3f", s.GpuMs );
                else
                    ImGui::TextDisabled( "-" );
                ImGui::TableNextColumn();
                // Nested passes subtracted — the column that can be added up.
                if ( s.GpuCalls > 0 )
                    ImGui::TextColored( ImVec4( 0.45f, 0.70f, 0.95f, 1.0f ), "%.3f", s.GpuSelfMs );
                else
                    ImGui::TextDisabled( "-" );
                ImGui::TableNextColumn();
                // Tint hot scopes (>25% of the frame) red.
                if ( pct > 25.0 )
                    ImGui::TextColored( ImVec4( 1.0f, 0.45f, 0.35f, 1.0f ), "%.1f", pct );
                else
                    ImGui::Text( "%.1f", pct );
                ImGui::TableNextColumn();
                ImGui::Text( "%u", s.Calls );
            }
            ImGui::EndTable();
        }

        ImGui::End();
    }

    void EditorLayer::DrawEngineStats( float rightMargin )
    {
        namespace ImGui = ::ImGui;

        const auto text = m_Application->GetEngineStats().GetFormattedStats();
        auto       size = ImGui::CalcTextSize( text.c_str() );

        ImGui::SameLine( ImGui::GetWindowContentRegionMax().x - rightMargin - size.x -
                         ImGui::GetStyle().ItemSpacing.x * 2.0f );

        // TextUnformatted, not Text: ImGui::Text takes a printf FORMAT, so this passed runtime-built
        // engine stats as the format string. Today GetFormattedStats() can only produce
        // "FPS: 60 | Frame: 16.67ms" and contains no '%', so nothing has gone wrong — but the day any
        // percentage is added to that line (a GPU utilisation, a budget fraction — the obvious next
        // additions) ImGui's vsnprintf reads a vararg that was never passed.
        //
        // The example above said "16.6ms" while the function was printing SIX decimals — the argument
        // was right and the sample output was a different program's. It is two decimals now because
        // GetFormattedStats was fixed, not because the comment was made to agree with it.
        ImGui::TextUnformatted( text.c_str() );
    }

    void EditorLayer::DrawPopups()
    {
        m_SceneFiles.DrawDialogs();
        DrawProjectPopup();
        m_Preferences.Draw();
    }

    namespace
    {
        // One static box = mesh (Cube primitive) + Box collider + Static body, as a child of `parent`.
        // The Cube primitive spans 2 units, so the visual size is 2*scale and the collider half-extents == scale
        // (matches the demo ground). Child colliders are placed at their WORLD pose by PhysicsECSSystem.
        void AddHousePart( ::Desert::Core::Scene* scene, ::Desert::ECS::Entity parent, const char* name,
                           const glm::vec3& localPos, const glm::vec3& scale )
        {
            using namespace ::Desert;
            auto& e                                              = scene->CreateNewEntity( std::string( name ) );
            e.AddComponent<ECS::StaticMeshComponent>().Primitive = Geometry::PrimitiveType::Cube;
            auto& t                                              = e.GetComponent<ECS::TransformComponent>();
            t.Translation                                        = localPos * Common::Units::UnitsPerMetre;
            t.Scale                                              = scale;
            auto& col                                            = e.AddComponent<ECS::ColliderComponent>();
            col.Data.Shape                                       = Physics::ShapeType::Box;
            // The Cube primitive spans one metre, so a box of Scale s reaches 50*s units either way.
            col.Data.HalfExtents                                = scale * ( Common::Units::UnitsPerMetre * 0.5f );
            e.AddComponent<ECS::RigidBodyComponent>().Data.Type = Physics::BodyType::Static;
            scene->Attach( parent, e );
        }
    } // namespace

    // Builds a walkable greybox HOUSE (floor-less; sits on the demo ground): 4 walls (front wall has a
    // doorway) + a flat roof, each a static collider so the character walks in through the door and is blocked
    // by walls. All parented under one "House" root (a ready prefab root). 2-unit-cube convention: dims = 2*scale.
    void EditorLayer::DrawProjectPopup()
    {
        // Intentionally empty: the editor never opens/switches projects in-session. All content paths
        // are remapped to the project at startup (--project), so switching would require re-initializing
        // the asset manager, cooked caches and panels — relaunch through the Project Hub instead.
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

        // THE DEVICE DIED, AND THIS IS THE LAST MOMENT THE USER'S WORK EXISTS ANYWHERE.
        //
        // Not left to the autosave timer, which has three separate reasons not to have run recently: it
        // fires every AutosaveMinutes (default 5), it skips when the command revision has not moved, and
        // it runs in Edit mode only. This one runs ONCE, unconditionally, at the moment of loss.
        //
        // It writes a SEPARATE file so that a good periodic autosave is never clobbered by it. The name
        // still contains "_autosave", which is what Autosave::SceneFor matches on, and it is
        // the newest file there, so the recovery prompt offers this one.
        //
        // IN PLAY MODE THE AUTHORED SCENE IS WHAT GETS WRITTEN — PlaySession's snapshot, the same text Stop would
        // have restored. The live scene at that instant holds runtime mutations nobody authored and nobody
        // wants back; saving those under the user's scene name would be the wrong answer wearing the right
        // filename.
        if ( Graphic::DeviceLost::IsLost() && m_Workspace.ActiveScene() )
        {
            using SceneState = ::Desert::Core::Scene::SceneState;
            Desert::Core::SceneSerializer serializer( m_Workspace.ActiveScene().get(), m_AssetManager.get() );
            const std::string             text = m_Workspace.ActiveScene()->GetState() == SceneState::Edit
                                                      ? serializer.SerializeToJson()
                                                      : m_Play.AuthoredSnapshot();
            if ( text.empty() )
            {
                // An empty file under a recovery name is a silent wrong answer: the prompt would offer it
                // and the user would open nothing. Say so instead.
                LOG_ERROR( "[DeviceLost] nothing could be serialized to save — the scene is in {} and its "
                           "authored snapshot is empty. Your periodic autosave, if any, is untouched.",
                           m_Workspace.ActiveScene()->GetState() == SceneState::Edit ? "Edit" : "Play" );
            }
            else
            {
                const auto path =
                     Autosave::PathFor( m_SceneFiles.OpenScenePath(), m_Workspace.ActiveScene()->GetSceneName(),
                                        Autosave::kDeviceLostSuffix );
                const auto      dir  = path.parent_path();
                std::error_code ec;
                std::filesystem::create_directories( dir, ec );
                const auto written =
                     ec ? Common::MakeFormattedError( "could not create {}: {}", dir.string(), ec.message() )
                        : WrittenOrError( Desert::Core::ExternalEntities::WriteSceneText( path, text ) );
                // BRACES ARE REQUIRED ON BOTH ARMS: the LOG_ macros are not single statements, so a
                // braceless if/else here does not compile. The autosave block above is written the same
                // way for the same reason.
                if ( written )
                {
                    LOG_INFO( "[DeviceLost] your work was saved to {} before shutting down; the next start "
                              "will offer it.",
                              path.string() );
                }
                else
                {
                    LOG_ERROR( "[DeviceLost] the emergency save FAILED: {}. The periodic autosave in {} is "
                               "the newest copy that exists.",
                               written.GetError(), dir.string() );
                }
            }
        }

        // Clean shutdown: drop the session lock so the next start doesn't think we crashed. After a device
        // loss CrashRecovery::DisarmSession refuses, on purpose — see its own comment.
        CrashRecovery::DisarmSession();

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
