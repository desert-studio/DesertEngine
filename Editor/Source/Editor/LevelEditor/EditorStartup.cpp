#include "Editor/LevelEditor/EditorStartup.hpp"

#include "Editor/Import/ImportManager.hpp"
#include "Editor/Import/MeshDeriver.hpp"
#include "Editor/Import/TextureImporter.hpp"
#include "Editor/LevelEditor/AssetCompiling.hpp"
#include "Editor/LevelEditor/SceneFiles.hpp"
#include "Editor/LevelEditor/SceneWorkspace.hpp"
#include "Editor/LevelEditor/ShotDirector.hpp"
#include "Editor/Core/ProjectContext.hpp"
#include "Editor/Core/StartupRecord.hpp"
#include "Editor/Core/ToastManager.hpp"
#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/Constants.hpp>
#include <Common/Utilities/FileSystem.hpp>
#include <Engine/Graphic/SceneRenderState.hpp>
#include <Engine/Assets/GrassCoverLook.hpp>
#include <Editor/Import/GrassCoverLookBuilder.hpp>
#include <Engine/Assets/Serialization/MeshBinary.hpp>
#include <Engine/Core/Application.hpp>
#include <filesystem>
#include <iterator>
#include <format>
#include "Editor/Panels/FileExplorer/AssetThumbnailPool.hpp"
#include "Editor/Splash/SplashControls.hpp"
#include "Editor/Widgets/ThumbnailService.hpp"
#include "Editor/Widgets/ThumbnailCensus.hpp"

#include <Common/Core/AssetPathIndex.hpp>
#include <Common/Utilities/ContentScanLedger.hpp>
#include <Engine/Animation/AnimationLibrary.hpp>
#include <Engine/Assets/BootContent.hpp>
#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/ContentWork.hpp>
#include <Engine/Assets/MeshDerivedData.hpp>
#include <Engine/Assets/VertexAnimationDerivedData.hpp>
#include <Engine/Assets/MeshCardRepresentation.hpp>
#include <Engine/Assets/MeshDisplacementTopology.hpp>
#include <Engine/Assets/MeshDistanceField.hpp>
#include <Engine/Assets/SyncLoadLedger.hpp>
#include <Engine/Assets/TextureSourceAsset.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/Graphic/ReflectionCaptures/ReflectionCaptureRenderer.hpp>
#include <Engine/Core/ShaderCompiler/ShaderSpirvCache.hpp>
#include <Engine/Assets/AssetManager.hpp>
#include <Common/Core/Logger.hpp>
#include <Engine/Graphic/MemoryReadout.hpp>
#include <Engine/Graphic/PipelineBuilds.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <algorithm>
#include <chrono>
#include <Engine/Runtime/Services/Texture/TextureService.hpp>

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
                std::format_to( std::back_inserter( named ), "{}{}", named.empty() ? "" : ", ", key );
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

    void EditorStartup::ChooseInitialLevel( ShotDirector& shots )
    {
        // Launched with --project (Project Hub): adopt the project's name and queue its default scene
        // (loaded through the normal deferred path on the first frame, when the renderer is ready).
        // Startup content is DATA: a template's DefaultScene ships in its Payload (Templates/Starter), and the
        // launcher refuses a template that names a scene it does not carry. A DefaultScene that is not on disk
        // is therefore an error naming the path, never a scene built in code. With no scene to open, the
        // editor opens the Basic level template as an untitled scene (UE: EditorStartupMap / TemplateMapInfos).
        // Screenshot mode names its own scene; it is the whole point of the flag.
        if ( ShotDirector::NamesScene() )
        {
            if ( const auto refused = shots.QueueScene() )
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
    }

    Common::BoolResultStr EditorStartup::BootContent()
    {
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
        BeginPhase( "content.gather" );
        const auto registry = Assets::ContentRegistry::Gather( nullptr, &unboxedMeshes );
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
        // start, and one call: it begins its own three phases (shaders.load, shaders.maps, shaders.register) and
        // names every file and program it works through; the splash's close, pressed during it, stops it
        // between programs (the progress's one stop flag).
        if ( m_Splash )
            m_Splash->PumpMainThread();
        const auto shaders = Assets::CompileShaders( m_AssetManager, m_Startup.get() );
        if ( m_Splash )
            m_Splash->PumpMainThread();
        if ( !shaders )
            return Common::MakeFormattedError( "the shaders: {}", shaders.GetError() );
        // The imported materials choose among these shaders' Import blocks; every cook below comes after.
        LOG_INFO( "[Import] {} import template(s) published from the loaded shaders",
                  ImportManager::PublishImportTemplates( *m_AssetManager ) );

        return m_Workspace.CreateSystems( *m_Workspace.ActiveScene() );
    }

    // The 11-26 warm Bistro start, phase by phase (with the engine's app.* phases about 44 s). Only the first
    // start on a machine uses these; every later one weighs its phases by the history the previous wrote.
    std::vector<::Desert::Core::StartupPhaseSpec> EditorStartupPhases()
    {
        return {
             { "content.gather", "Indexing content", 1.5 },
             { "shaders.load", "Loading shaders", 0.1 },
             { "shaders.maps", "Building shader maps", 0.7 },
             { "shaders.register", "Creating shader programs", 9.6 },
             { "textures.import", "Importing textures", 0.05 },
             { "clips.index", "Indexing animation clips", 0.01 },
             { "strings.request", "Requesting string tables", 0.01 },
             { "content.refresh", "Refreshing the content registry", 1.5 },
             { "scene.load", "Loading the scene", 8.4 },
             { "scene.reveal", "Finishing the first frame", 15.8 },
        };
    }

    namespace
    {
        // The pipelines compiling now (every role), oldest first, with how long each has run: what the splash
        // names in every phase while a compile it does not count is running.
        std::vector<::Desert::Core::ActiveWork>
        PipelinesInFlight( const ::Desert::Core::StartupClock::time_point now )
        {
            std::vector<::Desert::Core::ActiveWork> active;
            for ( const auto& build : Graphic::PipelineBuilds::Get().InFlight( 3 ) )
                active.push_back( { build.Name, std::chrono::duration<double>( now - build.Begin ).count() } );
            return active;
        }
    } // namespace

    EditorStartup::EditorStartup( Engine::Application*                          application,
                                  std::shared_ptr<Assets::AssetManager>&        assetManager,
                                  std::unique_ptr<ImportManager>&               importManager,
                                  std::unique_ptr<Animation::AnimationLibrary>& animationLibrary,
                                  SceneWorkspace& workspace, SceneFiles& sceneFiles,
                                  AssetCompiling& assetCompiling, const bool& realFrameDrawn,
                                  std::unique_ptr<Splash::SplashScreen>            splash,
                                  std::shared_ptr<::Desert::Core::StartupProgress> startup )
         : m_Application( application ), m_AssetManager( assetManager ), m_ImportManager( importManager ),
           m_AnimationLibrary( animationLibrary ), m_Workspace( workspace ), m_SceneFiles( sceneFiles ),
           m_AssetCompiling( assetCompiling ), m_RealFrameDrawn( realFrameDrawn ),
           m_Startup( std::move( startup ) ), m_Splash( std::move( splash ) )
    {
        // THE AMBIENT SOURCE: a pipeline compiled inside any phase (a renderer's Register, an Initialize) is
        // named on the splash while it runs, with its age. It counts into no phase. Pulled on the splash
        // thread; the tracker is mutex-safe.
        m_Startup->SetAmbient(
             []( const ::Desert::Core::StartupClock::time_point now )
             {
                 ::Desert::Core::LiveCount count;
                 count.Active = PipelinesInFlight( now );
                 return count;
             } );

        // STAGED: this used to run inline here and froze the window for seconds before the first frame.
        // The stages now execute one-per-frame from OnUpdate, each announced on the splash.
        // NOTE: shaders are NOT staged — they load synchronously in OnAttach, because the render systems
        // (MeshECSSystem's default lit materials) resolve their shaders in their constructors.
        //
        // NOTHING IS COOKED AT STARTUP (DDC-ON-DEMAND; UE's editor builds derived data when an asset is first
        // loaded, never by walking the content tree). A mesh source's envelope, a texture's or mesh's platform
        // data, a distance field and the cards are each built by the builder registered below when a load misses
        // the DDC, so opening a scene cooks exactly what its dependency closure reaches.
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
        m_StartupStages.push_back(
             { "textures.import", [this]( ::Desert::Core::WorkSink* sink )
               {
                   // The editor derives texture platform data on a DDC miss; a packaged game
                   // has no builder.
                   Assets::SetTexturePlatformDataBuilder( &TextureImporter::BuildPlatformData );
                   // The builder's encode effort is the project's (Config/Engine.json, Texture Encoding), read at
                   // every miss so a Project Settings change applies to the next derive.
                   TextureImporter::UseProjectTextureEncoding();
                   Assets::SetMeshPlatformDataBuilder( &Editor::BuildMeshPlatformData );
                   // A static mesh's envelope missing from the DDC is cooked from its source (UE: a DDC miss
                   // rebuilds from the package), so a fresh checkout's scene draws without a re-import.
                   Assets::SetMeshSourceBuilder( &Editor::BuildMeshSourceOnMiss );
                   // A mesh's distance field (UE: built beside the render data on a DDC miss, editor only).
                   Assets::SetMeshDistanceFieldBuilder( &Assets::BuildMeshDistanceFieldBytes );
                   // A far crowd's vertex animation missing from the DDC is derived on demand (WP-FAR-12B; the
                   // package cook derives every pair, a packaged game only loads).
                   Assets::SetVertexAnimationDeriving( true );
                   // Its surface-cache cards, the same way (UE CARDREPRESENTATION_DERIVEDDATA_VER).
                   Assets::SetMeshCardsBuilder( &Assets::BuildMeshCardsBytes );
                   // A Runtime-displaced mesh's cooked topology (clusters, welds), the same way.
                   Assets::SetDisplacementTopologyBuilder( &Assets::BuildDisplacementTopologyBytes );
                   // A displaced mesh's bake-level surface (Baked draw, collision; CPU height decode is editor-only).
                   Assets::SetDisplacedMeshBuilder( &Editor::BuildDisplacedMeshBytes );
                   // A grass variety's cover look: its albedo map averaged over the texels its blades draw.
                   Assets::SetGrassCoverLookBuilder( &Editor::BuildGrassCoverLook );
                   if ( const size_t imported = m_ImportManager->ImportLooseTextures( sink ); imported > 0 )
                       LOG_INFO( "[Startup] {} loose texture(s) imported", imported );
               } } );
        // THE ONLY CONTENT STAGES LEFT (AL1-9): nothing here creates an asset of any kind. Textures, materials,
        // meshes, skyboxes and the cloud kinds are created from their content-registry rows when something
        // names them, and the scene settle below waits for the ones the scene names.
        m_StartupStages.push_back( { "clips.index", [this]( ::Desert::Core::WorkSink* )
                                     { Assets::IndexAnimationClips( *m_AssetManager, *m_AnimationLibrary ); } } );
        // Order-free, and early among the optional stages on purpose: a missing translation shows up on
        // the very first frame drawn, and its log line is far easier to read before the rest of the
        // content's lines arrive.
        m_StartupStages.push_back( { "strings.request", [this]( ::Desert::Core::WorkSink* )
                                     { Assets::RequestStringTables( m_AssetManager ); } } );
    }

    void EditorStartup::BeginPhase( const std::string_view key, ::Desert::Core::LiveSource source )
    {
        if ( const auto begun = m_Startup->BeginPhase( key, std::move( source ) ); !begun )
            LOG_ERROR( "[Startup] the phase '{}' could not begin: {}", key, begun.GetError() );
    }

    bool EditorStartup::RunStartupFrame()
    {
        // Staged startup loading: run ONE heavy stage per frame. While loading, the scene is NOT rendered
        // at all (shaders/assets aren't there yet — rendering before the preload stage crashed on the
        // missing StaticMeshLit shader); the frame is ImGui-only and the window it goes to is still hidden.
        //
        // THERE USED TO BE A GATE HERE — "only after one frame with the loading overlay has been
        // presented" — and it existed for the overlay alone: a stage run before that frame froze a blank
        // window. The overlay is gone (the splash, a window of its own, replaced it), and so is the gate.
        //
        // CLOSE ON THE SPLASH ENDS THE START HERE, before the next stage: the editor leaves through the same
        // Application::Close the window frame's close button and the control channel's `quit` take.
        //
        // The splash's main-thread duty first (the Mac minimise; nothing on Windows), once per startup frame.
        if ( m_Splash )
            m_Splash->PumpMainThread();
        const Splash::StartupStep step =
             Splash::NextStartupStep( m_Startup->StopRequested(), m_StartupNext, m_StartupStages.size() );
        if ( step == Splash::StartupStep::Quit && !m_QuitFromSplash )
        {
            m_QuitFromSplash = true;
            LOG_INFO( "[Startup] closed on the splash; {} of {} stage(s) not run",
                      m_StartupStages.size() - m_StartupNext, m_StartupStages.size() );
            m_Application->Close( 0 );
        }
        if ( step != Splash::StartupStep::RunStage )
            return false;
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
            // THE TIMING AND THE TABLE ARE THE PROGRESS'S NOW (Engine/Core/StartupProgress.hpp): each stage is
            // one of its phases, timed from its begin to the next phase's, and StartupProgress::Finish logs the
            // table at the reveal. The SCHEDULER stays here: running one stage per frame behind the splash is
            // this layer's own arrangement and has nothing to do with timing.
            StartupStage& stage = m_StartupStages[m_StartupNext];
            BeginPhase( stage.Key );
            stage.Run( m_Startup.get() );
            ++m_StartupNext;

            if ( !StartupLoading() )
            {
                LOG_INFO( "[Startup] all {} stage(s) done; the editor is now answering about a project it has "
                          "actually read.",
                          m_StartupStages.size() );
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
                BeginPhase( "content.refresh" );
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

                // THE SCENE LOAD GETS ITS OWN PHASE. A splash that says nothing while it waits is
                // indistinguishable from an editor that has hung, and this wait is the one the demand-driven
                // model introduced. Its count is pulled on the splash thread, so it moves while a blocking load
                // holds the main thread.
                BeginPhase( "scene.load", SceneLoadSource() );
                m_SceneLoadPhase = true;
            }
        }
        // The browser asked for its opening folder's pictures when it was built (OnAttach): the workers
        // decode them through the stages too, not only through the settle that follows (THUMB2).
        if ( Splash::ThumbnailDiskDecodeAllowed( CurrentRevealState() ) )
            ThumbnailService::TickDiskAndDecode();
        return true;
    }

    void EditorStartup::TickThumbnails()
    {
        // ONE thumbnail capture pump for the whole editor. Panels only request; whether the asset browser
        // is open, hidden or closed no longer changes whether previews progress, and a request made by one
        // panel is finished for all of them.
        //
        // Two halves, two gates (Editor/Splash/RevealGate.hpp). A PNG already in the disk cache is decoded
        // on a worker even while the splash is up, so the first frame after the hand-over only uploads it.
        // A CAPTURE is not: it shares the settle's frames and asset loader — the one thing the splash is
        // waiting on — so captures start after the hand-over, for what is shown then.
        //
        // NOTHING IS WARMED AND NOTHING WAITS (THUMB-LAZY, UE FAssetThumbnailPool): a picture is captured only
        // while a shower draws it (ThumbnailService::TickCapture drops the rest), after the hand-over, one at a
        // time within CaptureBudget. The start-up, a scene load and a --shot never wait on a thumbnail.
        if ( Splash::ThumbnailDiskDecodeAllowed( CurrentRevealState() ) )
            ThumbnailService::TickDiskAndDecode();
        if ( Splash::ThumbnailCaptureAllowed( CurrentRevealState() ) )
            ThumbnailService::Get().TickCapture();
    }

    void EditorStartup::ShowContentLine( const Assets::ContentProgressLine& line )
    {
        // The splash reads the scene load from its own source (SceneLoadSource); this line is the gate's and
        // the post-reveal overlay's.
        m_ContentProgress = line;
    }

    ::Desert::Core::LiveSource EditorStartup::SceneLoadSource()
    {
        // Both bases are taken now, on the main thread, before the scene load asks for anything: the phase
        // counts only the reads and the engine pipelines started after its begin.
        const Assets::LoadProgress base          = Assets::AsyncAssetLoader::Get().Progress();
        const uint64_t             readsBase     = base.Finished;
        const uint64_t             readsStarted  = base.Started;
        const auto&                pipelines     = Graphic::PipelineBuilds::Get();
        const uint64_t             pipesStarted  = pipelines.Started( Graphic::PipelineRole::Engine );
        const uint64_t             pipesFinished = pipelines.Finished( Graphic::PipelineRole::Engine );
        // What the item line ages: the read named now and since when, and when anything was last in flight. The
        // source is called under the progress's mutex only (the splash's samples, the phase's end), so this
        // per-source state needs no lock of its own.
        struct Watch
        {
            std::string                            Read;
            ::Desert::Core::StartupClock::time_point ReadSince{};
            ::Desert::Core::StartupClock::time_point LastInFlight = ::Desert::Core::StartupClock::now();
        };
        return [readsBase, readsStarted, pipesStarted, pipesFinished,
                watch = Watch{}]( const ::Desert::Core::StartupClock::time_point now ) mutable
        {
            // The loader and the tracker only (both mutex-safe); nothing of the gate, which is main-thread.
            const Assets::LoadProgress reads  = Assets::AsyncAssetLoader::Get().Progress();
            const auto&                builds = Graphic::PipelineBuilds::Get();
            const auto                 since  = []( const uint64_t value, const uint64_t base ) -> uint64_t
            { return value > base ? value - base : 0; };
            ::Desert::Core::LiveCount count;
            count.Total = since( reads.Started, readsStarted ) +
                          since( builds.Started( Graphic::PipelineRole::Engine ), pipesStarted );
            count.Done = std::min( count.Total,
                                   since( reads.Finished, readsBase ) +
                                        since( builds.Finished( Graphic::PipelineRole::Engine ), pipesFinished ) );
            // WHAT RUNS NOW, WITH ITS SECONDS: the engine/material pipelines in the driver, and the newest read
            // only while a read is in flight -- the loader's `Current` outlives its read, and a finished file's
            // name held for 20 s (Starter_Ground.demat at 123 / 123, build #38) said a stall where there was none.
            for ( ::Desert::Core::ActiveWork& build : PipelinesInFlight( now ) )
                count.Active.push_back( std::move( build ) );
            if ( reads.Started > reads.Finished && !reads.Current.empty() )
            {
                if ( reads.Current != watch.Read )
                {
                    watch.Read      = reads.Current;
                    watch.ReadSince = now;
                }
                count.Active.push_back(
                     { std::format( "{} {}", Assets::AssetTypeName( reads.CurrentType ),
                                    std::filesystem::path( reads.Current ).filename().string() ),
                       std::chrono::duration<double>( now - watch.ReadSince ).count() } );
            }
            // Nothing on a worker or in the driver while the phase runs: the main thread is doing the scene's work
            // (instancing what landed, asking the next reads). Said as that, aged from the last thing in flight.
            if ( count.Active.empty() )
                count.Active.push_back(
                     { "Main thread, nothing in flight",
                       std::chrono::duration<double>( now - watch.LastInFlight ).count() } );
            else
                watch.LastInFlight = now;
            return count;
        };
    }

    Assets::AsyncAssetLoader::WaitFeedback EditorStartup::SceneLoadFeedback( const uint64_t finishedBefore )
    {
        return [this, finishedBefore]( const Assets::LoadProgress& now )
        {
            ShowContentLine( Assets::ContentProgressSince(
                 now, finishedBefore, Graphic::PipelineBuilds::Get().Pending( Graphic::PipelineRole::Engine ) ) );
        };
    }

    void EditorStartup::BeginContentSettle( const uint64_t finishedBefore )
    {
        m_SettleBase      = finishedBefore;
        m_ContentProgress = Assets::ContentProgressNow( m_SettleBase );
        m_Content.BeginWorld( Assets::ContentWorkNow().Started );
    }

    void EditorStartup::ReportRevealWait( const Splash::RevealState& state )
    {
        // scene.reveal begins once the scene's content has settled (and covers the start whose scene asked for
        // nothing, which has no settle at all).
        if ( m_StartupFinished || !m_SceneLoadPhase || state.SceneLoadPending || state.ContentSettling )
            return;
        if ( !m_RevealPhase )
        {
            m_RevealPhase = true;
            BeginPhase( "scene.reveal" );
        }
        // What the reveal still waits for, read here because the capture state is main-thread-only.
        std::vector<std::string> reasons;
        if ( state.TexturesStreaming )
            reasons.push_back( std::format( "textures streaming: {} in flight",
                                            Runtime::ResourceRegistry::GetTextureService()->InFlight() ) );
        if ( state.ReflectionCapturesFilling )
            reasons.push_back( "reflection captures: first fill" );
        if ( !state.RealFrameDrawn )
            reasons.push_back( "first real frame" );
        m_Startup->SetWaitReasons( std::move( reasons ) );
    }

    void EditorStartup::FinishStartup()
    {
        m_StartupFinished = true;
        // The progress's table ("[Startup] boot ... ms" and a line per phase), then the history the next start
        // weighs its phases by. A history that cannot be written costs the next start its estimate, not this
        // session anything: said loudly, and the editor goes on.
        m_Startup->Finish();
        const std::filesystem::path history = StartupHistoryFile();
        if ( const auto saved = ::Desert::Core::SaveStartupHistory( history, m_Startup->Timings() ); !saved )
            LOG_ERROR( "[Startup] the start's phase timings could not be written to '{}': {}", history.string(),
                       saved.GetError() );
    }

    // SHOWN AFTER THE FIRST REAL FRAME IS PRESENTED, NOT BEFORE IT IS DRAWN. The window has been presented
    // loading frames the whole time it was hidden, and a window shown ahead of the first real present would
    // put the last of those — an empty frame — on screen for as long as that frame takes. Shown here, the
    // surface it reveals already holds the editor, and the splash crossfades into it from this instant.
    void EditorStartup::RevealWhenReady()
    {
        const Splash::RevealState state = CurrentRevealState();
        ReportRevealWait( state );
        if ( !m_Splash )
        {
            // A RUN WITH NO SPLASH (a builder run) has nothing to hand over, but its start ends at the same
            // point: the same gate, as if a splash were up, finishes the progress once.
            Splash::RevealState asIfShown = state;
            asIfShown.HasSplash           = true;
            // Its timings are logged by Finish and kept out of the history, which is the splash start's.
            if ( !m_StartupFinished && Splash::MayReveal( asIfShown ) )
            {
                m_StartupFinished = true;
                m_Startup->Finish();
            }
            return;
        }
        if ( !Splash::MayReveal( state ) )
            return;
        FinishStartup();
        m_Revealed = true;
        if ( const auto& window = m_Application->GetWindow() )
            window->Show();
        LOG_INFO( "[Startup] reveal: the scene's content has settled and the editor window is shown" );
        if ( m_ThumbnailPool != nullptr )
            LOG_INFO( "[Thumbnails] {} thumbnails resident at the hand-over; the rest are made as they are shown",
                      m_ThumbnailPool->ResidentThumbnails() );
        ReportUnproducedThumbnailKinds();
        // Starts the crossfade and returns; the splash object stays until this layer is destroyed.
        m_Splash->Close();
        LOG_INFO( "[Startup] the splash is closed" );
        // The counters are cumulative since process start, so this is every shader and pipeline cost paid before
        // the first real frame, including the pipelines the renderers build after the preload.
        LOG_INFO( "[Startup] shader work before the first frame: {}",
                  ::Desert::Core::FormatShaderPhaseTimes( ::Desert::Core::ReadShaderPhaseTimes() ) );
    }

    Splash::RevealState EditorStartup::CurrentRevealState() const
    {
        Splash::RevealState state;
        state.HasSplash           = m_Splash != nullptr;
        state.Revealed            = m_Revealed;
        state.StartupLoading      = StartupLoading();
        state.SceneLoadPending    = m_SceneFiles.HasPendingLoad();
        state.ContentSettling     = ContentSettling();
        state.RealFrameDrawn      = m_RealFrameDrawn;
        state.TexturesStreaming   = Runtime::ResourceRegistry::GetTextureService()->InFlight() > 0;
        state.ReflectionCapturesFilling = ReflectionCapturesFilling();
        return state;
    }

    bool EditorStartup::ReflectionCapturesFilling() const
    {
        const auto& scene = m_Workspace.ActiveScene();
        return scene && scene->GetRenderState() != nullptr &&
               Graphic::RenderStateOf( *scene ).GetReflectionCaptures().FirstFillPending();
    }

    void EditorStartup::ReportUnproducedThumbnailKinds()
    {
        for ( const ThumbnailCensus::Unproduced& gap :
              ThumbnailCensus::UnproducedKinds( &Assets::ContentRegistry::FilesOfKind ) )
            LOG_WARN( "[Thumbnails] {} {} file(s) get no picture in the browser: the kind has no thumbnail "
                      "producer yet ({})",
                      gap.Files, Common::Content::KindName( gap.Kind ), gap.Why );
    }

    void EditorStartup::UpdateContentSettling()
    {
        const auto& loader  = Assets::AsyncAssetLoader::Get();
        const auto  work    = Assets::ContentWorkNow();
        const bool  settled = m_Content.Tick( work.Outstanding, work.Started );
        if ( !settled )
        {
            // THE SETTLE SAYS WHAT IS BEING READ AND HOW MUCH IS DONE (LOAD-SHOW), every frame, from the
            // loader's counters: "Mesh SM_Wall_A.demesh (123 / 622)" — a count that moves is the difference
            // between a load and a hang. The splash is pushed when the line changes; after the reveal the
            // editor's own overlay draws the same line (EditorLayer::OnUIRender).
            if ( ContentSettling() )
                ShowContentLine( Assets::ContentProgressNow( m_SettleBase ) );
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
} // namespace Desert::Editor
