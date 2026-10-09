#include "Editor/LevelEditor/EditorStartup.hpp"

#include "Editor/Import/CookPaths.hpp"
#include "Editor/Import/ImportManager.hpp"
#include "Editor/Import/MeshDeriver.hpp"
#include "Editor/Import/TextureImporter.hpp"
#include "Editor/LevelEditor/AssetCompiling.hpp"
#include "Editor/LevelEditor/SceneFiles.hpp"
#include "Editor/LevelEditor/SceneWorkspace.hpp"
#include "Editor/Panels/FileExplorer/FileExplorerPanel.hpp"
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
#include <Engine/Assets/SyncLoadLedger.hpp>
#include <Engine/Assets/TextureSourceAsset.hpp>
#include <Engine/Core/SceneAssetRoots.hpp>
#include <Engine/Core/ShaderCompiler/ShaderSpirvCache.hpp>
#include <Engine/Desert.hpp>
#include <Engine/Graphic/MemoryReadout.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <algorithm>

namespace Desert::Editor
{
    namespace
    {
        // The splash's cost per item of each weighed stage (EditorStartup::MakeSplashPlan), read off the
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

    EditorStartup::EditorStartup( Engine::Application*                          application,
                                  std::shared_ptr<Assets::AssetManager>&        assetManager,
                                  std::unique_ptr<ImportManager>&               importManager,
                                  std::unique_ptr<Animation::AnimationLibrary>& animationLibrary,
                                  SceneWorkspace& workspace, SceneFiles& sceneFiles,
                                  AssetCompiling& assetCompiling, const bool& realFrameDrawn,
                                  std::unique_ptr<Splash::SplashScreen> splash )
         : m_Application( application ), m_AssetManager( assetManager ), m_ImportManager( importManager ),
           m_AnimationLibrary( animationLibrary ), m_Workspace( workspace ), m_SceneFiles( sceneFiles ),
           m_AssetCompiling( assetCompiling ), m_RealFrameDrawn( realFrameDrawn ), m_Splash( std::move( splash ) )
    {
        // Cook only what's missing/stale (skips the expensive Assimp re-parse on every launch). Collections
        // hold packs (a character + its animation FBXs), so they're imported too — their skinned assets land
        // beside each source, where the content registry gathers them (see CookPaths::SkinnedAsset).
        //
        // STAGED: this used to run inline here and froze the window for seconds before the first frame.
        // The stages now execute one-per-frame from OnUpdate, each announced on the splash.
        // NOTE: shaders are NOT staged — they load synchronously in OnAttach, because the render systems
        // (MeshECSSystem's default lit materials) resolve their shaders in their constructors.
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
        m_StartupStages.push_back(
             { "Importing textures...",
               [this]
               {
                   // The editor derives texture platform data on a DDC miss; a packaged game
                   // has no builder.
                   Assets::SetTexturePlatformDataBuilder( &TextureImporter::BuildPlatformData );
                   Assets::SetMeshPlatformDataBuilder( &Editor::BuildMeshPlatformData );
                   (void)m_ImportManager->ImportLooseTextures( SplashItems() );
               },
               kSecondsPerTextureCheck, [] { return LooseTextureSources().size(); }, nullptr, 0 } );
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
    }

    void EditorStartup::BeginShaderStage()
    {
        MakeSplashPlan();
        BeginSplashStage( m_ShaderStage );
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
        const Splash::StartupStep step = Splash::NextStartupStep( m_Splash && m_Splash->CloseRequested(),
                                                                  m_StartupNext, m_StartupStages.size() );
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
            // THE TIMING AND THE LINE COME FROM `::Desert::Core::BootTimeline` NOW, not from a chrono pair
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
                // The count itself starts at the scene load (BeginContentSettle), from the loader's counters.
                BeginSplashStage( m_SettleStage );
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
        const bool changed = line.Done != m_ContentProgress.Done || line.Total != m_ContentProgress.Total ||
                             line.Item != m_ContentProgress.Item;
        m_ContentProgress = line;
        if ( changed && !m_Revealed )
        {
            m_Progress.Step( line.Item.empty() ? std::string( "Scene assets" ) : line.Item, line.Done,
                             std::max<std::size_t>( line.Total, 1 ) );
            PushSplash();
        }
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

    // SECONDS PER ITEM, MEASURED — so the bar's share of a stage is the share of the wait it is. A Debug
    // start of this project on the M-series development machine, read off the "[Startup] ... item(s) in"
    // lines below; a machine twice as fast halves every stage alike and the shares do not move.
    void EditorStartup::MakeSplashPlan()
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

    void EditorStartup::BeginSplashStage( const std::size_t stage, const std::optional<std::size_t> items )
    {
        const double now =
             std::chrono::duration<double>( std::chrono::steady_clock::now() - m_ProgressEpoch ).count();
        if ( const auto finished = m_Progress.BeginStage( stage, now, items ) )
            LOG_INFO( "[Startup] {} {} item(s) in {:.2f} s", finished->Name, finished->Units, finished->Seconds );
        PushSplash();
    }

    void EditorStartup::PushSplash()
    {
        if ( m_Splash && !m_Revealed )
            m_Splash->SetProgress( m_Progress.Snapshot() );
    }

    Assets::ItemProgress EditorStartup::SplashItems()
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
    void EditorStartup::RevealWhenReady()
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
        if ( m_FileExplorer != nullptr )
            LOG_INFO( "[Thumbnails] {} thumbnails resident at the hand-over; the rest are made as they are shown",
                      m_FileExplorer->ResidentThumbnails() );
        ReportUnproducedThumbnailKinds();
        m_AssetCompiling.StartBackgroundCook();
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
        return state;
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
