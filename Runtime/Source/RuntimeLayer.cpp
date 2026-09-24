#include "RuntimeLayer.hpp"
#include "RuntimeShot.hpp"

#include <Engine/Assets/ContentRegistry.hpp>

#include <Common/Core/AssetPathIndex.hpp>
#include <Common/Utilities/ContentScanLedger.hpp>
#include <Engine/Graphic/MemoryReadout.hpp>
#include <Engine/Assets/SyncLoadLedger.hpp>

#include <Engine/Core/Scene.hpp>
#include <Engine/Core/EngineContext.hpp>
#include <Engine/Core/Serialize/SceneSerializer.hpp>
#include <Engine/Core/Serialize/SceneFormat.hpp>
#include <Engine/Project/ProjectContext.hpp>
#include <Engine/Graphic/SceneRenderer.hpp>

#include <Common/Settings/MachineSettings.hpp>
#include <Engine/Graphic/Image.hpp>
#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/AssetPreloader.hpp>
#include <Engine/Assets/AsyncAssetLoader.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <Engine/Core/SceneRenderCollectors.hpp>
#include <Engine/ECS/System/MeshECSSystem.hpp>
#include <Engine/ECS/System/TextECSSystem.hpp>
#include <Engine/ECS/System/SkyboxECSSystem.hpp>
#include <Engine/ECS/System/HeightFogECSSystem.hpp>
#include <Engine/ECS/System/VolumetricCloudECSSystem.hpp>
#include <Engine/ECS/System/TimeOfDayECSSystem.hpp>
#include <Engine/ECS/System/PointLightSystem.hpp>
#include <Engine/ECS/System/SpotLightSystem.hpp>
#include <Engine/ECS/System/AnimationECSSystem.hpp>
#include <Engine/ECS/System/AttachmentSystem.hpp>
#include <Engine/ECS/System/ScriptSystem.hpp>
#include <Engine/UI/UIDataStore.hpp>
#include <Engine/UI/UIOverlay.hpp>
#include <Engine/ECS/System/PhysicsECSSystem.hpp>
#include <Engine/ECS/System/LocomotionSystem.hpp>
#include <Engine/ECS/System/AudioECSSystem.hpp>

// STB_IMAGE_WRITE_IMPLEMENTATION is already compiled into Desert.lib (stb_image.obj); declare only.
// Only the capture writes a PNG from this host, so a shipping build does not need the declaration either.
#if DESERT_DEV_INSTRUMENTS
#include <stb_image/stb_image_write.h>
#endif

#include <Common/Utilities/FileSystem.hpp>
#include <Common/Core/Logger.hpp>

#include <Engine/Graphic/Texture.hpp>
#include <Engine/Graphic/Image.hpp>
#include <Engine/Graphic/SwapChain.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/Framebuffer.hpp>
#include <Engine/Graphic/Render2D/DrawList2D.hpp>
#include <Engine/Graphic/Render2D/Render2D.hpp>
#include <Engine/Graphic/Render2D/UIRenderTextureCache.hpp>
#include <Engine/Graphic/Materials/MaterialExecutor.hpp>
#include <Engine/Graphic/Materials/Properties/Texture2DProperty.hpp>
#include <Engine/UI/UICanvasLayout.hpp>
#include <Engine/UI/UICanvasRenderer2D.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/Shader/ShaderService.hpp>
#include <Engine/Core/Input.hpp>

#include <Common/Core/Events/Event.hpp>
#include <Common/Core/Events/MouseEvents.hpp>
#include <Common/Core/Events/KeyEvents.hpp>
#include <Common/Core/KeyCodes.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <string_view>

namespace
{
    /// The device image behind an authored sprite handle, or nullptr if the project has none. Both the
    /// loading cover and the authored splash ask this, and they used to ask it in two hand-copied
    /// blocks -- which is how the two of them would have drifted apart the first time either was fixed.
    Desert::Graphic::Image2D* ResolveSpriteImage( const Desert::Assets::AssetHandle& handle )
    {
        auto* textures = Desert::Runtime::ResourceRegistry::GetTextureService();
        if ( textures == nullptr )
            return nullptr;
        auto* tex = textures->Get( handle );
        if ( tex == nullptr )
            return nullptr;
        auto* images = Desert::Runtime::ResourceRegistry::GetImageService();
        if ( images == nullptr )
            return nullptr;
        auto* img = dynamic_cast<Desert::Graphic::Image2D*>( images->Resolve( tex->GetImageHandle() ) );
        if ( img == nullptr || img->GetWidth() == 0 || img->GetHeight() == 0 )
            return nullptr;
        return img;
    }

    /// Centre @p img over a @p w x @p h surface, scaled to FIT (letterboxed, never cropped, never
    /// stretched): a splash authored at one aspect must not be distorted by the player's window.
    void DrawFittedSprite( Desert::Graphic::Render2D::DrawList2D& dl, Desert::Graphic::Image2D& img, float w,
                           float h, float alpha )
    {
        const auto  iw  = static_cast<float>( img.GetWidth() );
        const auto  ih  = static_cast<float>( img.GetHeight() );
        const float fit = std::min( w / iw, h / ih );
        const float sw  = iw * fit;
        const float sh  = ih * fit;
        const float cx  = w * 0.5f;
        const float cy  = h * 0.5f;
        dl.AddImage( &img, { cx - sw * 0.5f, cy - sh * 0.5f }, { cx + sw * 0.5f, cy + sh * 0.5f }, { 0.0f, 0.0f },
                     { 1.0f, 1.0f }, glm::vec4( 1.0f, 1.0f, 1.0f, alpha ) );
    }
} // namespace

namespace Desert::Player
{
    RuntimeLayer::RuntimeLayer( std::string scenePathOverride, Engine::Application* application )
         : Common::Layer( "RuntimeLayer" ), m_ScenePathOverride( std::move( scenePathOverride ) ),
           m_Application( application )
    {
        m_AssetManager = std::make_shared<Assets::AssetManager>();
        // BEFORE the preloader, which now takes it: the preloader is what fills it, at the tail of the
        // scan that finds the clips. This layer used to build a library, hand it to AnimationECSSystem and
        // never put a single clip in it — `AnimationLibrary` appeared exactly twice in this whole file and
        // neither occurrence was a Register — so every skinned character in a packaged game stood in its
        // bind pose, and no editor session could reproduce it.
        m_AnimationLibrary = std::make_unique<Animation::AnimationLibrary>( m_AssetManager.get() );
        m_AssetPreloader   = std::make_unique<Assets::AssetPreloader>( m_AssetManager, *m_AnimationLibrary );
        m_SceneRenderer    = std::make_unique<Graphic::SceneRenderer>();
        m_Scene            = std::make_shared<Core::Scene>( "Game", m_SceneRenderer.get() );
    }

    RuntimeLayer::~RuntimeLayer() = default;

    Common::BoolResultStr RuntimeLayer::OnAttach()
    {
        // The runtime presents the frame + draws UI/splash with the engine's own Render2D (set up lazily on
        // the first present, once the swapchain framebuffer exists).

        // The runtime does NOT cook: it plays what the editor cooked. Assets load from the project's
        // Cooked/ tree (missing cooked content = open the project in the editor once).
        // EVERY PRELOAD IS A NAMED, TIMED STAGE — and until now this host had no startup timing at all.
        //
        // The editor has had it since a client watched a boot for five minutes without being able to say
        // which stage was spending them. The shipping runtime, which is the process a player actually
        // starts, had thirteen preload calls in a flat sequence and one log line, `[Runtime] Scene
        // loaded`, which arrives after all of it. §0.4 of the world programme makes "start-up time does
        // not grow with the size of the map" one of four acceptance criteria, so the host the criterion
        // is about was the host nobody could measure. The stage LABELS deliberately match the editor's
        // wording where the work is the same, so two logs can be put side by side.
        // FIRST, AND IT IS THE STAGE EVERY OTHER ONE NOW DEPENDS ON. The preloads below used to begin
        // with a `recursive_directory_iterator` over a content root — sixteen of them, every boot — and
        // that walk was the only thing in this process that minted handles wholesale. It reads one file
        // instead, and publishes every row's identity into `Common::AssetPathIndex` before a single
        // asset exists, so a handle read out of a `.desce` names its file with nothing having been
        // walked. `[ContentScan] boot finished` at the tail of this function is what says so.
        //
        // RETURNED, not logged and stepped over: a cooked game whose registry will not parse has no
        // content it can find, and starting with an empty project would be the silent substitution §1.4
        // forbids — a black screen whose reason is one line up in a log the player does not have.
        const auto registry = m_Boot.Run( "Reading the cooked asset registry",
                                          []
                                          {
                                              // A packaged game carries the registry its packager cooked; a loose
                                              // run (a development checkout) has no cooked registry and gathers
                                              // one like the editor.
                                              return Common::Utils::FileSystem::Exists(
                                                          Common::Utils::AssetRegistry::DefaultPath() )
                                                          ? Assets::ContentRegistry::LoadCooked()
                                                          : Assets::ContentRegistry::Gather();
                                          } );
        if ( !registry )
            return Common::MakeError( registry.GetError() );
        LOG_INFO( "[ContentRegistry] {} row(s), {} handle(s) bound before anything was loaded",
                  Assets::ContentRegistry::Get().Count(), registry.GetValue() );

        m_Boot.Run( "Preloading shaders", [this] { m_AssetPreloader->PreloadShaders(); } );
        m_Boot.Run( "Preloading meshes, textures and materials",
                    [this] { m_AssetPreloader->PreloadCookedAssetsAndMaterials(); } );
        m_Boot.Run( "Preloading skyboxes", [this] { m_AssetPreloader->PreloadSkyboxes(); } );
        m_Boot.Run( "Preloading cloud noise volumes", [this] { m_AssetPreloader->PreloadCloudNoiseVolumes(); } );
        // MUST follow the volumes: a type binds the one it names.
        m_Boot.Run( "Preloading cloud types", [this] { m_AssetPreloader->PreloadCloudTypes(); } );
        // order-free: a body names nothing and is named by nothing but a scene
        m_Boot.Run( "Preloading cloud modelling volumes",
                    [this] { m_AssetPreloader->PreloadCloudModellingVolumes(); } );
        // Missing here too, and for the same reason it was missing from the editor: PreloadCloudLayouts
        // was written, tested and never called, so a packaged game rendered every painted sky
        // procedurally. Order-free like the line above.
        m_Boot.Run( "Preloading cloud layouts", [this] { m_AssetPreloader->PreloadCloudLayouts(); } );
        // The UI themes (Ю13). HERE AND NOT ONLY IN THE EDITOR, because this is the process that ships:
        // a canvas whose theme the player's build never scanned draws every element's own colour, which
        // is a game that looks right in the editor and wrong on the player's machine — the worst shape a
        // missing preload can take. Order-free: a theme names only font paths, which FontService
        // registers on demand, and nothing else names a theme.
        m_Boot.Run( "Preloading UI themes", [this] { m_AssetPreloader->PreloadUIThemes(); } );
        // AND HERE TOO, which the editor's copy alone would not have given us: AssetPreloadCensus caught
        // exactly this omission on A12's first sweep. A rig that loads in the editor and silently does not
        // in the packaged game is worse than no rig — the scene names it, one line goes to the log, and the
        // character poses from its clips.
        m_Boot.Run( "Preloading control rigs", [this] { m_AssetPreloader->PreloadControlRigs(); } );
        m_Boot.Run( "Preloading anim graphs", [this] { m_AssetPreloader->PreloadAnimGraphs(); } );
        // AND HERE TOO, for the rig's reason four lines up. Not order-free: a retarget binds its source
        // rig while loading, so it must follow the cooked scan that registers the `.skeleton` files.
        m_Boot.Run( "Preloading retargets", [this] { m_AssetPreloader->PreloadRetargets(); } );
        // Order-free. A packaged game reads its `.destrings` out of Content.dpak through the same VFS as
        // everything else, so the player sees the language the build boots in with no extra plumbing.
        m_Boot.Run( "Preloading string tables", [this] { m_AssetPreloader->PreloadStringTables(); } );

        // Same system set + order as the editor's Play mode — and it is the SAME LIST, not a copy of it
        // (Engine/Core/SceneRenderCollectors.hpp). "Same as the editor" was a comment above a hand-copied
        // block, which is the arrangement that let a sixth caller omit the whole thing silently (Ю16).
        m_Boot.Run( "Building the render collectors and gameplay systems", [this] { BuildGameplaySystems(); } );

        if ( const auto init = m_Boot.Run( "Initialising the systems", [this] { return m_Scene->Init(); } );
             !init )
            return init;

        // Scene: --scene override, else the project's default scene.
        std::string scenePath = m_ScenePathOverride;
        if ( scenePath.empty() )
            scenePath = Project::ProjectContext::DefaultScenePath();

        if ( !scenePath.empty() && Common::Utils::FileSystem::Exists( scenePath ) ) // VFS-aware
        {
            Core::SceneSerializer serializer( m_Scene.get(), m_AssetManager.get() );
            // RETURNED, not logged and stepped over: a cooked game whose boot scene will not load has
            // nothing to run, and starting on an empty world would be the silent substitution §1.4 forbids
            // - the player would see a black screen and the reason would be one line up in a log they do
            // not have. The read's / loader's error already names the file (and the version and the fix).
            // THE READ AND THE PARSE ARE SEPARATE STAGES. On the 12 MB, 50 179-entity world scene these
            // two are the boot, and they fail for different reasons — one is the disk (or the pak), the
            // other is the JSON. A single "Loading scene" stage would have made the two indistinguishable
            // in the one log that gets sent back from a player's machine.
            // A COOKED WORLD BESIDE THE SCENE IS WHAT A GAME LOADS (WorldStreamer.hpp): its index and its
            // always-loaded cell, and nothing else before the first frame — the cells are read on workers as
            // the camera wants them. The .desce is read only when no cook sits beside it.
            auto cooked = m_Boot.Run( "Reading the cooked world (index, always-loaded cell)",
                                      [&scenePath] { return Core::ReadCookedWorld( scenePath ); } );
            if ( !cooked )
                return Common::MakeError( cooked.GetError() );
            std::optional<Core::CookedWorldStart> world = cooked.ExtractValue();
            std::string                           sceneJson;
            if ( world.has_value() )
                sceneJson = std::move( world->AlwaysLoadedJson );
            else
            {
                auto read = m_Boot.Run( "Reading the scene file", [&scenePath]
                                        { return Common::Utils::FileSystem::ReadFileContent( scenePath ); } );
                if ( !read )
                    return Common::MakeError( read.GetError() );
                sceneJson = read.ExtractValue();
            }
            if ( const auto loaded =
                      m_Boot.Run( "Deserialising the scene",
                                  [&] { return serializer.DeserializeFromJson( sceneJson, scenePath ); } );
                 !loaded )
                return Common::MakeError( loaded.GetError() );
            if ( const auto init =
                      m_Boot.Run( "Initialising the loaded scene", [this] { return m_Scene->Init(); } );
                 !init )
                return init;
            // A game has no Edit mode and never saves its world, so a partitioned one streams from its first
            // frame: only the camera's neighbourhood is ever entities.
            auto streamer =
                 m_Boot.Run( "Starting world streaming",
                             [&]
                             {
                                 return world.has_value()
                                             ? Core::WorldStreamer::BeginCooked( *m_Scene, *m_AssetManager,
                                                                                 std::move( *world ) )
                                             : Core::WorldStreamer::Begin( *m_Scene, *m_AssetManager, sceneJson );
                             } );
            if ( !streamer )
                return Common::MakeError( streamer.GetError() );
            m_WorldStreamer = streamer.ExtractValue();
            LOG_INFO( "[Runtime] Scene loaded: {}", scenePath );
        }
        else
        {
            LOG_WARN( "[Runtime] No scene to load ('{}') — starting empty. Set DefaultScene in the "
                      ".deproj or pass --scene <path>.",
                      scenePath );
        }

        // PLAY, BUT NOT YET PRESENTED. The state is Play from here because Play is what binds the view to
        // the scene's own CameraComponent (`Scene::UpdateActiveCameraSource`): a loading frame rendered
        // from anywhere else would ask the renderer for the content of a view the game is never going to
        // show, and the gate below would then open on the wrong answer. What the loading state suspends
        // is TIME, not the render — see the zero timestep in OnUpdate.
        m_Scene->SetState( Core::Scene::SceneState::Play );
        // AND THE WORLD IS NOW THE GATE'S SUBJECT. Nothing it wants has been asked for yet: the cloud
        // kinds are demand-driven, so the first frame is where the asking happens.
        // TriggerSplash() is NOT called here any more. It used to start the authored splash at the top of
        // the boot, so the duration a designer picked was spent racing a file read instead of being seen;
        // it is armed on the tick the gate opens (OnContentReady), over a world that is actually there.
        m_Content.BeginWorld( Assets::AsyncAssetLoader::Get().StartedCount() );

        m_Boot.LogSummary();
        // AND HERE THE BOOT IS OVER, which is what turns every later synchronous load into a reported
        // hitch. This line and not `Renderer::BeginFrame`: the runtime presents splash frames while it
        // boots, and a phase keyed on the first frame would have marked the whole preload as in-frame.
        // See Engine/Assets/SyncLoadLedger.hpp.
        Assets::SyncLoadLedger::NoteBootFinished();
#if DESERT_DEV_INSTRUMENTS
        LOG_INFO( "[SyncLoad] boot finished — {}", Assets::SyncLoadLedger::Report() );
        LOG_INFO( "[Memory] boot finished — {}", Graphic::MemoryReadout::Take().Report() );
#endif
        // HOW MANY HANDLES CAN NAME THEIR OWN FILE BY THE TIME THE BOOT IS OVER. The eager preloader's
        // directory walk is what mints them, so this number IS the size of the path->handle inverse the
        // engine has at that moment — and therefore the exact quantity the demand-driven model has to
        // reproduce some other way once the walk stops happening (GAP_ANALYSIS T2.4). Beside the two
        // lines above because it answers the same question they do: what did the boot buy.
        LOG_INFO( "[AssetPathIndex] boot finished — {} handle(s) can name their own path",
                  Common::AssetPathIndex::Size() );
        // AND WHAT IT COST TO MINT THEM. The line above is only an achievement next to this one: the
        // same count reached with directory walks and reached without them are two different boots, and
        // nothing else in the process can tell them apart (§T2.4).
        LOG_INFO( "[ContentScan] boot finished — {}", Common::Utils::ContentScanLedger::Report() );
        return BOOLSUCCESS;
    }

    void RuntimeLayer::BuildGameplaySystems()
    {
        // A METHOD RATHER THAN THE LAMBDA BODY IT WAS. A parameter-less multi-line lambda is the one
        // construct on which clang-format 18.1.3 (what CI runs) and 18.1.8 (what this machine has)
        // disagree — `[this] {` against `[this]\n{` — so the changed-lines gate goes red for code that
        // is locally clean, and the repair anybody reaches for is to reformat by hand until CI stops
        // complaining. A named method keeps the lambda on one line and says what the stage is.
        Desert::Core::AddSceneRenderCollectors( *m_Scene );
        m_Scene->AddSystem<ECS::AnimationECSSystem>( m_AnimationLibrary.get(), m_AssetManager.get() );
        m_Scene->AddSystem<ECS::AttachmentSystem>( m_Scene.get() );
        m_Scene->AddSystem<ECS::ScriptSystem>( m_Scene.get(), m_AssetManager.get() );
        m_Scene->AddSystem<ECS::PhysicsECSSystem>( m_Scene.get() );
        m_Scene->AddSystem<ECS::LocomotionSystem>( m_Scene.get() );
        m_Scene->AddSystem<ECS::AudioECSSystem>( m_Scene.get() );
    }

    Common::BoolResultStr RuntimeLayer::OnDetach()
    {
        // Release the present GPU resources while the device is still alive (before engine teardown).
        m_Render2D.reset();
        m_UIRenderTextures.reset(); // destroying the captures is what returns their renderer slots
        m_BlitExecutor.reset();
        m_BlitPipeline.reset();
        m_PresentReady = false;
        return BOOLSUCCESS;
    }

    void RuntimeLayer::LoadSceneInternal( const std::string& path )
    {
        if ( !Common::Utils::FileSystem::Exists( path ) ) // VFS-aware
        {
            LOG_WARN( "[Runtime] Scene switch target not found: '{}' (a button's OnClickMessage is "
                      "'scene:<path>' — the path must resolve in the cooked project)",
                      path );
            return;
        }

        // ASKED BEFORE ANYTHING IS TORN DOWN. Clear() below drops every entity of the scene that is running,
        // so finding out afterwards that the target will not load would leave the game in an empty world it
        // cannot get out of. The loader asks the same question again and its answer is the authoritative
        // one; this is only the difference between a switch that does not happen and a game that ends.
        // The same choice as the boot's: a cooked world beside the scene, else the .desce.
        auto cooked = Core::ReadCookedWorld( path );
        if ( !cooked )
        {
            LOG_ERROR( "[Runtime] Scene switch refused, the running scene is untouched: {}", cooked.GetError() );
            return;
        }
        std::optional<Core::CookedWorldStart> world = cooked.ExtractValue();
        std::string                           json;
        if ( world.has_value() )
            json = std::move( world->AlwaysLoadedJson );
        else
        {
            auto jsonRead = Common::Utils::FileSystem::ReadFileContent( path );
            if ( !jsonRead )
            {
                LOG_ERROR( "[Runtime] Scene switch refused, the running scene is untouched: {}",
                           jsonRead.GetError() );
                return;
            }
            json = jsonRead.ExtractValue();
        }
        if ( const auto loadable = Core::ParseLoadableScene( path, json ); !loadable )
        {
            LOG_ERROR( "[Runtime] Scene switch refused, the running scene is untouched: {}", loadable.GetError() );
            return;
        }

        EngineContext::GetInstance().GetDevice()->WaitIdle(); // scene teardown frees GPU resources
        m_WorldStreamer.reset();                              // streams the world about to be cleared
        m_Scene->Clear();                                     // keeps the gameplay systems, drops the entities

        // A notification raised by the level being left names an overlay canvas of THAT level. Carrying it
        // across would either pop up in a world it says nothing about or, more likely, name an overlay the
        // new level does not have and log a refusal for something nobody did. The view's own overlay state
        // needs no help here — BeginUIFrame rebinds and resets it when the registry changes.
        UI::UIOverlayRequests::Get().Clear();

        // AND EVERY RENDER-TEXTURE CAPTURE, for the reason the line above does not cover: a capture is
        // keyed by the ELEMENT ENTITY, and an entity id is unique only inside its registry. Carrying one
        // across a scene switch would hand the new level's entity 7 the world the old level's entity 7 was
        // showing — the same false identity UICanvasContext::Registry warns about, with a whole scene
        // behind it. Destroying them is also what returns their renderer slots to the level being loaded.
        if ( m_UIRenderTextures )
        {
            m_UIRenderTextures->Reset();
        }

        Core::SceneSerializer serializer( m_Scene.get(), m_AssetManager.get() );
        if ( const auto loaded = serializer.DeserializeFromJson( json, path ); !loaded )
        {
            LOG_ERROR( "[Runtime] Scene switch failed after teardown: {}", loaded.GetError() );
            return;
        }
        if ( const auto init = m_Scene->Init(); !init )
        {
            LOG_ERROR( "[Runtime] Scene switch init failed: {}", init.GetError() );
            return;
        }
        auto streamer = world.has_value()
                             ? Core::WorldStreamer::BeginCooked( *m_Scene, *m_AssetManager, std::move( *world ) )
                             : Core::WorldStreamer::Begin( *m_Scene, *m_AssetManager, json );
        if ( !streamer )
        {
            LOG_ERROR( "[Runtime] Scene switch could not stream the world: {}", streamer.GetError() );
            return;
        }
        m_WorldStreamer = streamer.ExtractValue();
        m_Scene->SetState( Core::Scene::SceneState::Play );
        // THE SAME GATE AS THE BOOT'S, and this is the half that would have been forgotten. A level switch
        // is a second world handed over at run time — its clouds, its layouts, its themes are read on
        // demand exactly like the first one's — so a loading state that covered only the boot would ship
        // the defect back into the game the moment a door was opened.
        m_Content.BeginWorld( Assets::AsyncAssetLoader::Get().StartedCount() );
        m_LoadingFramesPresented = 0;
        LOG_INFO( "[Runtime] Switched scene: {}", path );
    }

    void RuntimeLayer::TriggerSplash()
    {
        const auto& s = m_Scene->GetSettings();
        if ( s.SplashDuration > 0.0f )
        {
            m_SplashSprite   = s.SplashSprite;
            m_SplashDuration = s.SplashDuration;
            m_SplashFade     = s.SplashFade;
            m_SplashTimer    = s.SplashDuration;
        }
    }

#if DESERT_DEV_INSTRUMENTS
    void RuntimeLayer::RecordShotIfDue()
    {
        const auto& shot = RuntimeShot::Get();
        if ( !shot.Active() || m_ShotRecorded )
            return;
        // Counted in PRESENTED frames, from 1: m_PresentedFrames still holds the count BEFORE this one.
        if ( m_PresentedFrames + 1 != shot.Frames )
            return;

        const auto swapChain = EngineContext::GetInstance().GetWindow()->GetWindowSwapChain();
        if ( !swapChain )
        {
            LOG_ERROR( "[Shot] there is no swapchain to capture the presented frame from." );
            m_ShotRecorded = true; // refused, but decided: OnFramePresented ends the run with a status
            return;
        }
        if ( const auto recorded = swapChain->RecordFrameCapture(); !recorded )
        {
            LOG_ERROR( "[Shot] {}", recorded.GetError() );
            m_ShotRecorded = true;
            return;
        }
        m_ShotRecorded = true;
    }

#endif // DESERT_DEV_INSTRUMENTS

    void RuntimeLayer::OnFramePresented()
    {
        ++m_PresentedFrames;

#if !DESERT_DEV_INSTRUMENTS
        // A shipping player counts its presented frames and does nothing else here: the capture that used
        // to end the process on a chosen frame is not in this binary. Early-returning keeps the counter
        // above as the only statement, rather than leaving a function whose body is all preprocessor.
        return;
#else
        const auto& shot = RuntimeShot::Get();
        if ( !shot.Active() || !m_ShotRecorded )
            return;

        int exitCode = 0;

        const auto swapChain = EngineContext::GetInstance().GetWindow()->GetWindowSwapChain();
        if ( !swapChain )
        {
            LOG_ERROR( "[Shot] the swapchain went away before the captured frame could be collected." );
            exitCode = 1;
        }
        else
        {
            const std::filesystem::path file( shot.Output );
            if ( file.has_parent_path() && !file.parent_path().empty() )
            {
                std::error_code ec;
                std::filesystem::create_directories( file.parent_path(), ec );
                if ( ec && !std::filesystem::exists( file.parent_path() ) )
                    LOG_ERROR( "[Shot] could not create '{}': {}", file.parent_path().string(), ec.message() );
            }

            // The copy was submitted with this frame; waiting is what makes the staging buffer readable.
            Graphic::Renderer::GetInstance().WaitDeviceIdle();

            uint32_t   width  = 0;
            uint32_t   height = 0;
            const auto pixels = swapChain->TakeCapturedFrameRGBA8( width, height );
            if ( !pixels )
            {
                LOG_ERROR( "[Shot] {}", pixels.GetError() );
                exitCode = 1;
            }
            else
            {
                stbi_flip_vertically_on_write( 0 );
                const bool written =
                     stbi_write_png( shot.Output.c_str(), static_cast<int>( width ), static_cast<int>( height ), 4,
                                     pixels.GetValue().data(), static_cast<int>( width ) * 4 ) != 0;
                if ( !written )
                {
                    LOG_ERROR( "[Shot] stb_image_write refused to write '{}'.", shot.Output );
                    exitCode = 1;
                }
                else
                {
                    LOG_INFO( "[Shot] wrote presented frame {} of the GAME -> {} ({}x{}); the world was "
                              "{} at that frame.",
                              shot.Frames, shot.Output, width, height,
                              m_Content.Loading() ? "still loading" : "complete" );
                }
            }
        }

        m_Application->Close( exitCode );
#endif // DESERT_DEV_INSTRUMENTS
    }

    void RuntimeLayer::OnContentReady()
    {
        LOG_INFO( "[Runtime] the world is complete after {} settling frame(s) / {} presented loading "
                  "frame(s), {:.1f} ms; {} read(s) have gone to a worker this session. Until this tick the "
                  "swapchain carried the loading screen, not the scene -- which is the difference between "
                  "a lazy loader and a game that shows a sky it has not read yet.",
                  m_Content.FramesWaited(), m_LoadingFramesPresented, m_Content.ElapsedMs(),
                  Assets::AsyncAssetLoader::Get().StartedCount() );

        // THE AUTHORED SPLASH STARTS HERE, over a world that exists. Armed at the top of the boot (where
        // it used to be) its duration was spent on top of frames the player was not going to see anyway,
        // so a two-second splash was two seconds of nothing in particular.
        TriggerSplash();
    }

    void RuntimeLayer::DrawLoadingScreen( Graphic::Render2D::DrawList2D& dl, float w, float h )
    {
        // FULLY OPAQUE AND FULL-SCREEN, FIRST. The scene's blit is skipped while this is up, so the
        // swapchain image underneath is whatever the previous frame left; an alpha < 1 here would show the
        // last frame of the level being left, which is the frame a level switch exists to replace.
        dl.AddRectFilled( { 0.0f, 0.0f }, { w, h }, glm::vec4( 0.0f, 0.0f, 0.0f, 1.0f ) );

        // The world's own splash image, if it names one. Read from the scene's settings rather than from
        // m_SplashSprite: that field belongs to the authored splash ANIMATION, which does not start until
        // the gate opens, and borrowing it would have coupled "what the loading screen shows" to "has the
        // splash been armed yet" -- two facts with one field, and the loading screen would have been blank
        // for exactly the scenes that bothered to author an image.
        const Assets::AssetHandle sprite = m_Scene->GetSettings().SplashSprite;
        if ( auto* img = ResolveSpriteImage( sprite ) )
            DrawFittedSprite( dl, *img, w, h, 1.0f );

        // AND SOMETHING THAT MOVES. A still loading screen is indistinguishable from a hung game -- the
        // editor's overlay solves this with a label, and this host has no font it can rely on (fonts are
        // assets, and the point of this screen is that the assets are not here yet). So: a track and a
        // block sliding along it, driven by the PRESENTED frame count rather than by a clock, so that an
        // unattended capture of frame N is reproducible.
        constexpr float kTrackFraction = 0.34f;
        constexpr float kBlockFraction = 0.18f;
        constexpr float kPeriodFrames  = 48.0f;

        const float trackW = w * kTrackFraction;
        const float blockW = trackW * kBlockFraction;
        const float x0     = ( w - trackW ) * 0.5f;
        const float y0     = h * 0.82f;
        const float thick  = std::max( 2.0f, h * 0.004f );

        dl.AddRectFilled( { x0, y0 }, { x0 + trackW, y0 + thick }, glm::vec4( 1.0f, 1.0f, 1.0f, 0.16f ) );

        // A ping-pong rather than a wrap, so the block is never cut in half at the ends of the track.
        const float phase = std::fmod( static_cast<float>( m_LoadingFramesPresented ), kPeriodFrames * 2.0f );
        const float tri   = phase < kPeriodFrames ? phase / kPeriodFrames : 2.0f - phase / kPeriodFrames;
        const float bx    = x0 + tri * ( trackW - blockW );
        dl.AddRectFilled( { bx, y0 }, { bx + blockW, y0 + thick }, glm::vec4( 1.0f, 1.0f, 1.0f, 0.85f ) );
    }

    Common::BoolResultStr RuntimeLayer::OnUpdate( const Common::Timestep& ts )
    {
        // ONE PUMP PER TICK, and without this line the shipping host would read a cloud volume on a
        // worker and never hear that it landed -- the completion delegate is what uploads it. The
        // editor's copy is at the head of its own OnUpdate for the same reason and is asserted beside
        // it by `AsyncAssetPump`.
        Assets::AsyncAssetLoader::Get().Pump();

        // THE LOADING STATE, TICKED HERE AND NOWHERE ELSE. The rule it applies -- nothing outstanding AND
        // the frame just rendered asked for nothing new, and never before the second frame -- is one
        // implementation shared with the editor (Engine/Assets/ContentGate.hpp), because two hosts
        // holding two copies of a two-condition rule is how one of them ends up holding one condition.
        //
        // The marker this replaced said the same thing to the LOG and to nothing else. A log line is not
        // a state: nothing could branch on it, so the frames it described were presented anyway.
        {
            const auto& loader = Assets::AsyncAssetLoader::Get();
            if ( m_Content.Tick( loader.Outstanding(), loader.StartedCount() ) )
                OnContentReady();
        }

        if ( m_SplashTimer > 0.0f )
            m_SplashTimer -= ts.GetMilliseconds() * 0.001f;

        // A UI button requested a scene switch last frame: apply it here, between frames, before any
        // recording starts (Clear() destroys GPU resources — same rule as the resize below).
        if ( m_PendingSceneLoad )
        {
            const std::string path = *m_PendingSceneLoad;
            m_PendingSceneLoad.reset();
            LoadSceneInternal( path );
        }

        // Window-size changes resize the scene target here — before any recording starts (destroying
        // framebuffers mid-frame is a device loss).
        if ( m_PendingResize )
        {
            m_Scene->Resize( m_PendingResize->first, m_PendingResize->second );
            if ( auto cam = m_Scene->GetMainCamera().lock() )
                cam->UpdateProjectionMatrix( static_cast<float>( m_PendingResize->first ),
                                             static_cast<float>( m_PendingResize->second ) );
            m_PendingResize.reset();
        }

        // Same safe-point garbage collection as the editor (scripts can invalidate materials live).
        if ( auto* materialService = ::Desert::Runtime::ResourceRegistry::GetMaterialService() )
            materialService->CollectGarbage();

        // Advance + upload any playing UI videos here, at a safe point between frames (SetData flushes its
        // own transfer) — the UI walk during present then just samples the freshly-updated frame texture.
        if ( auto* videoService = ::Desert::Runtime::ResourceRegistry::GetVideoService() )
            videoService->UpdateAll();

        // WHAT THIS PLAYER'S MACHINE CAN AFFORD, pushed before BeginScene reads it. Until К3 these five
        // values were fields of the LEVEL file, so a player could not turn the picture down at all — the
        // only place the answer existed was a `.desce` shipped inside the game's content archive. They
        // are in the machine store now, loaded at startup from this player's own directory
        // (Runtime/Source/Main.cpp), and this is the line that makes the dial reach the renderer.
        m_SceneRenderer->SetQuality( Common::Settings::MachineSettings::Get() );

        // THE WORLDS INSIDE UI RENDER-TEXTURE ELEMENTS (Ю16), advanced before the game world opens its
        // pass. A capture records a whole scene render and Vulkan has no nested render pass — and the walk
        // that samples the result runs later still, inside the swapchain pass at present time. Same
        // constraint the editor obeys at the same point in its own frame.
        if ( m_UIRenderTextures && m_AssetManager )
        {
            m_UIRenderTextures->Tick( *m_AssetManager, ts );
        }

        // ZERO WHILE THE LOADING SCREEN IS UP, and the render still runs. The render is what ASKS -- a
        // frame that is not drawn requests nothing, so a host that skipped it would wait for content
        // nobody had ordered and the gate would never open. What must not run is TIME: without this the
        // player's first visible frame is already several frames into the game, with the physics stepped
        // and every script's OnUpdate called against a world they could not be seen reacting to.
        // BEFORE the systems run, so no system sees an entity whose cell has just left.
        if ( m_WorldStreamer )
        {
            m_WorldStreamClock += ts.GetSeconds();
            if ( auto streamed = m_WorldStreamer->Tick( m_WorldStreamClock ); !streamed )
            {
                // The world stays as it is now and the game goes on in it; streaming does not.
                LOG_ERROR( "[Runtime] world streaming stopped: {}", streamed.GetError() );
                m_WorldStreamer.reset();
            }
        }

        if ( const auto frame = m_Scene->OnUpdate( m_Content.Loading() ? Common::Timestep( 0.0f ) : ts ); !frame )
            return Common::MakeError( frame.GetError() );

        return BOOLSUCCESS;
    }

    Common::BoolResultStr RuntimeLayer::InitPresent( const std::shared_ptr<Graphic::Framebuffer>& swapFb )
    {
        auto* shaderService = Runtime::ResourceRegistry::GetShaderService();
        if ( !shaderService )
            return Common::MakeError( "InitPresent: no shader service" );
        auto blitShader = shaderService->GetByName( "SwapchainBlit" );
        if ( !blitShader )
            return Common::MakeError( "InitPresent: missing shader 'SwapchainBlit'" );

        // Fullscreen blit pipeline (vertexless: the VS synthesizes the quad), opaque, into the swapchain.
        Graphic::GraphicsPipelineSpecification spec;
        spec.DebugName         = "SwapchainBlitPipeline";
        spec.Shader            = blitShader;
        spec.Framebuffer       = swapFb;
        spec.DepthTestEnabled  = false;
        spec.DepthWriteEnabled = false;
        spec.CullMode          = Graphic::CullMode::None;
        const auto blitPipeline = Graphic::GraphicsPipeline::Create( spec );
        if ( !blitPipeline )
            return Common::MakeError( "InitPresent: " + blitPipeline.GetError() );
        m_BlitPipeline = blitPipeline.GetValue();
        m_BlitExecutor = Graphic::MaterialExecutor::Create( "SwapchainBlit", blitShader );

        m_UIRenderTextures = std::make_unique<Graphic::Render2D::UIRenderTextureCache>();
        m_Render2D = std::make_unique<Graphic::Render2D::Render2D>();
        if ( const auto r = m_Render2D->Init( swapFb ); !r )
            return r;

        m_PresentReady = true;
        return BOOLSUCCESS;
    }

    // The runtime's frame present. Opens the swapchain pass, blits the scene's final image fullscreen,
    // then draws the UI canvas + splash with the engine's Render2D batcher. No interface toolkit is in
    // this process at all — Desert/Tests/Engine/ImGuiBoundary is the census that keeps it that way.
    Common::BoolResultStr RuntimeLayer::OnUIRender()
    {
        auto& renderer = Graphic::Renderer::GetInstance();
        renderer.BeginSwapChainRenderPass();

        std::string clicked;
        if ( const auto swapFb = renderer.GetCompositeFramebuffer() )
        {
            if ( !m_PresentReady )
                if ( const auto r = InitPresent( swapFb ); !r )
                    LOG_ERROR( "[Runtime] present init failed: {}", r.GetError() );

            const uint32_t width  = swapFb->GetFramebufferWidth();
            const uint32_t height = swapFb->GetFramebufferHeight();
            if ( width > 0 && height > 0 && ( width != m_LastWidth || height != m_LastHeight ) )
            {
                m_LastWidth     = width;
                m_LastHeight    = height;
                m_PendingResize = { width, height };
            }
            const float w = static_cast<float>( width );
            const float h = static_cast<float>( height );

            if ( m_PresentReady )
            {
                // THE ONE LINE THAT KEEPS A HALF-READ WORLD OFF THE SCREEN.
                //
                // The scene IS rendered while the gate is shut -- rendering is what asks the services for
                // the cloud kinds, and a frame that is not drawn orders nothing -- but the result does not
                // reach the swapchain. Skipping the blit rather than drawing an opaque rectangle over it
                // is deliberate: a cover is only as opaque as whoever edits it next leaves it, and this way
                // the undercooked image is not in the swapchain to begin with.
                const bool loading = m_Content.Loading();

                // 1) Present the scene: blit its final (tonemapped) image over the whole swapchain.
                if ( !loading )
                {
                    if ( const auto image = m_Scene->GetFinalImage() )
                    {
                        if ( auto tp = m_BlitExecutor->GetTexture2DProperty( "u_Texture" ) )
                            tp->SetImage( image.get() );
                        renderer.SubmitFullscreenQuad( m_BlitPipeline.get(), m_BlitExecutor.get() );
                    }
                }

                // 2) UI + splash via Render2D, on top.
                m_Render2D->BeginFrame( { 0.0f, 0.0f, w, h } );
                auto& dl = m_Render2D->GetDrawList();

                if ( loading )
                {
                    ++m_LoadingFramesPresented;
                    DrawLoadingScreen( dl, w, h );

                    // KEYS AND CLICKS MADE AT A LOADING SCREEN ARE NOT INPUT TO THE GAME. Without this the
                    // accumulators keep filling while the cover is up and empty themselves into the first
                    // frame the player can see -- a character that starts the level already walking, from
                    // a key held down during the wait.
                    m_PrevMouseDown = Input::Mouse::Get().IsMouseButtonPressed( Common::MouseButton::Left );
                    m_ScrollAccum   = 0.0f;
                    m_TypedText.clear();
                    m_Backspace     = false;
                    m_TabPressed    = false;
                    m_SubmitPressed = false;
                    m_EscapePressed = false;
                }
                else
                {
                    glm::mat4        vp( 1.0f );
                    const glm::mat4* vpPtr = nullptr;
                    if ( auto cam = m_Scene->GetMainCamera().lock() )
                    {
                        vp    = cam->GetProjectionMatrix() * cam->GetViewMatrix();
                        vpPtr = &vp;
                    }

                    // Fullscreen: window mouse px == framebuffer px. MouseReleased is the down->up edge.
                    const auto [mx, my] = Input::Mouse::Get().GetMousePosition();
                    const bool  down    = Input::Mouse::Get().IsMouseButtonPressed( Common::MouseButton::Left );
                    UI::UIInput input;
                    input.MousePx       = { mx, my };
                    input.MouseDown     = down;
                    input.MouseReleased = m_PrevMouseDown && !down;
                    // The right button is reported HELD, not as an edge: the frame derives the edge itself from
                    // UIViewContext::PrevRightDown, exactly as it does for the left one, so the press a context
                    // menu opens on is computed in one place.
                    input.MouseRightDown = Input::Mouse::Get().IsMouseButtonPressed( Common::MouseButton::Right );
                    input.Escape         = m_EscapePressed;
                    input.ScrollDelta    = m_ScrollAccum;
                    input.TypedText      = m_TypedText;
                    input.Backspace      = m_Backspace;
                    input.Tab            = m_TabPressed;
                    input.Submit         = m_SubmitPressed;
                    m_PrevMouseDown      = down;
                    m_ScrollAccum        = 0.0f;
                    m_TypedText.clear();
                    m_Backspace     = false;
                    m_TabPressed    = false;
                    m_SubmitPressed = false;
                    m_EscapePressed = false;

                    // Pointer events / drops can fire several times in one frame, so they come back in their
                    // own list; a button action still arrives through `clicked`.
                    std::vector<std::string> uiMessages;
                    // The player has exactly one view, but its UI state still belongs to that view rather
                    // than to the process — a scene change rebinds it, and nothing else can reach it.
                    //
                    // EVERY CANVAS OF THE LEVEL, in authored Sort Order. This used to ask UI::SoleCanvas and
                    // refuse a level with two canvases, because a view could hold the runtime state of one
                    // canvas only; Ю4 keys that state by (canvas x view), so a HUD and a pause menu are simply
                    // two canvases and both are drawn. A level with none draws nothing and says nothing — a
                    // game without UI is legitimate, and it was only ever a refusal because of the limit.
                    m_UIView.Materials      = &m_Render2D->Materials();
                    m_UIView.RenderTextures = m_UIRenderTextures.get();
                    UI::BeginUIFrame( m_UIView, m_Scene->GetRegistry(), UI::Rect{ 0.0f, 0.0f, w, h } );
                    for ( const entt::entity canvas : UI::CanvasesInDrawOrder( m_Scene->GetRegistry() ) )
                        if ( const auto drawn = UI::RenderCanvas2D( m_UIView, m_Scene->GetRegistry(), canvas, dl,
                                                                    vpPtr, &input, &clicked, &m_FocusedUI );
                             !drawn )
                            LOG_ERROR( "[Runtime] {}", drawn.GetError() );
                    UI::EndUIFrame( m_UIView, m_Scene->GetRegistry(), dl, &input, &m_FocusedUI, &clicked,
                                    &uiMessages );

                    // Queue them for gameplay: ScriptSystem drains this and calls OnUIMessage on every script.
                    for ( const std::string& msg : uiMessages )
                        UI::UIMessageQueue::Get().Push( msg );

                    if ( m_SplashTimer > 0.0f )
                    {
                        const float elapsed = m_SplashDuration - m_SplashTimer;
                        float       a       = 1.0f;
                        if ( m_SplashFade > 0.0f )
                        {
                            if ( elapsed < m_SplashFade )
                                a = elapsed / m_SplashFade; // fade in
                            else if ( m_SplashTimer < m_SplashFade )
                                a = m_SplashTimer / m_SplashFade; // fade out
                        }
                        a = std::clamp( a, 0.0f, 1.0f );

                        dl.AddRectFilled( { 0.0f, 0.0f }, { w, h }, glm::vec4( 0.0f, 0.0f, 0.0f, a ) ); // fade
                        if ( auto* img = ResolveSpriteImage( m_SplashSprite ) )
                            DrawFittedSprite( dl, *img, w, h, a );
                    }
                }

                m_Render2D->Flush();
            }
        }

        renderer.EndRenderPass();

        // THE CAPTURE IS RECORDED WHILE THE FRAME IS STILL BEING BUILT, and it has to be: a swapchain
        // image may only be touched between its acquire and its present, and reading it back afterwards
        // is a Vulkan violation that looks perfect in the resulting PNG -- only the validation layer
        // objects. So the copy goes into THIS frame's command buffer, and the bytes are collected in
        // OnFramePresented once the present that carried it has gone out.
#if DESERT_DEV_INSTRUMENTS
        RecordShotIfDue();
#endif

        // Dispatch the clicked button's action AFTER the pass (scene switch queued for next OnUpdate; quit /
        // URL are process-level). Same encoding the UI walker produces.
        if ( !clicked.empty() )
        {
            constexpr std::string_view kScene = "scene:";
            constexpr std::string_view kUrl   = "url:";
            if ( clicked.rfind( kScene, 0 ) == 0 )
            {
                m_PendingSceneLoad = clicked.substr( kScene.size() );
            }
            else if ( clicked == "quit" )
            {
                LOG_INFO( "[Runtime] UI quit requested" );
                // NOT std::exit(). This runs inside the frame, with the job system's workers live and the
                // device alive: exit() runs the static destructors under them and the process aborts on
                // destroyed mutexes instead of quitting. Close() ends the loop after this frame and the
                // game shuts down exactly as it does when the window is closed.
                m_Application->Close( 0 );
            }
            else if ( clicked.rfind( kUrl, 0 ) == 0 )
            {
                const std::string url = clicked.substr( kUrl.size() );
#if defined( _WIN32 )
                const std::string cmd = "start \"\" \"" + url + "\"";
#elif defined( __APPLE__ )
                const std::string cmd = "open \"" + url + "\"";
#else
                const std::string cmd = "xdg-open \"" + url + "\"";
#endif
                if ( std::system( cmd.c_str() ) != 0 )
                    LOG_WARN( "[Runtime] OpenURL failed: {}", url );
            }
            else
            {
                // ON THE SAME QUEUE AS THE POINTER EVENTS ABOVE, and that is the whole point. A button's
                // SendMessage action is documented as "gameplay event name (Lua/scripts)" and three
                // comments said it reached Lua, but this branch only LOGGED it: the canvas raised the
                // message, ScriptEngine::BroadcastUIMessage was ready to deliver it, and the host in
                // between dropped it on the floor. An author who typed a name into "Action Target" got a
                // line in the log and no handler, with nothing to distinguish that from a typo.
                UI::UIMessageQueue::Get().Push( clicked );
                LOG_INFO( "[Runtime] UI message: '{}' (delivered to every script defining OnUIMessage)", clicked );
            }
        }

        return BOOLSUCCESS;
    }

    void RuntimeLayer::OnEvent( Common::Event& e )
    {
        Common::EventManager mgr( e );

        // Mouse wheel -> ScrollView. Consumed + reset each present.
        mgr.Notify<Common::MouseScrolledEvent>(
             [this]( Common::MouseScrolledEvent& ev )
             {
                 m_ScrollAccum += ev.GetYOffset();
                 return false;
             } );

        // Text input -> the focused InputField. Encode the codepoint as UTF-8 (the default SDF atlas covers
        // ASCII; other codepoints are stored but render as blanks until the atlas is extended).
        mgr.Notify<Common::KeyTypedEvent>(
             [this]( Common::KeyTypedEvent& ev )
             {
                 const unsigned int cp = ev.GetCodepoint();
                 if ( cp < 0x80 )
                     m_TypedText += static_cast<char>( cp );
                 else if ( cp < 0x800 )
                 {
                     m_TypedText += static_cast<char>( 0xC0 | ( cp >> 6 ) );
                     m_TypedText += static_cast<char>( 0x80 | ( cp & 0x3F ) );
                 }
                 else if ( cp < 0x10000 )
                 {
                     m_TypedText += static_cast<char>( 0xE0 | ( cp >> 12 ) );
                     m_TypedText += static_cast<char>( 0x80 | ( ( cp >> 6 ) & 0x3F ) );
                     m_TypedText += static_cast<char>( 0x80 | ( cp & 0x3F ) );
                 }
                 else
                 {
                     m_TypedText += static_cast<char>( 0xF0 | ( cp >> 18 ) );
                     m_TypedText += static_cast<char>( 0x80 | ( ( cp >> 12 ) & 0x3F ) );
                     m_TypedText += static_cast<char>( 0x80 | ( ( cp >> 6 ) & 0x3F ) );
                     m_TypedText += static_cast<char>( 0x80 | ( cp & 0x3F ) );
                 }
                 return false;
             } );

        mgr.Notify<Common::KeyPressedEvent>(
             [this]( Common::KeyPressedEvent& ev )
             {
                 switch ( ev.GetKeyCode() )
                 {
                     case Common::KeyCode::Backspace:
                         m_Backspace = true;
                         break;
                     case Common::KeyCode::Tab:
                         m_TabPressed = true;
                         break;
                     case Common::KeyCode::Enter:
                         m_SubmitPressed = true;
                         break;
                     case Common::KeyCode::Escape:
                         m_EscapePressed = true;
                         break;
                     default:
                         break;
                 }
                 return false;
             } );
    }
} // namespace Desert::Player
