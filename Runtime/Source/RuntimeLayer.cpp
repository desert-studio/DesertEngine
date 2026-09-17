#include "RuntimeLayer.hpp"
#include <Engine/Graphic/MemoryReadout.hpp>
#include <Engine/Assets/SyncLoadLedger.hpp>

#include <Engine/Core/Scene.hpp>
#include <Engine/Core/EngineContext.hpp>
#include <Engine/Core/Serialize/SceneSerializer.hpp>
#include <Engine/Core/Serialize/SceneFormat.hpp>
#include <Engine/Project/ProjectContext.hpp>
#include <Engine/Graphic/SceneRenderer.hpp>

#include <Common/Settings/MachineSettings.hpp>
#include <Engine/Graphic/UICacheTexture.hpp>
#include <Engine/Graphic/Image.hpp>
#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/AssetPreloader.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <Engine/Core/SceneRenderCollectors.hpp>
#include <Engine/ECS/System/MeshECSSystem.hpp>
#include <Engine/ECS/System/TextECSSystem.hpp>
#include <Engine/ECS/System/SkyboxECSSystem.hpp>
#include <Engine/ECS/System/HeightFogECSSystem.hpp>
#include <Engine/ECS/System/VolumetricCloudECSSystem.hpp>
#include <Engine/ECS/System/TimeOfDayECSSystem.hpp>
#include <Engine/ECS/System/TerrainECSSystem.hpp>
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

#include <Common/Utilities/FileSystem.hpp>
#include <Common/Core/Logger.hpp>

#include <Engine/Graphic/Texture.hpp>
#include <Engine/Graphic/Image.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/Framebuffer.hpp>
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
#include <cstdlib>
#include <filesystem>
#include <string_view>

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
        // No ImGui: the runtime presents the frame + draws UI/splash with the engine's own Render2D (set up
        // lazily on the first present, once the swapchain framebuffer exists).

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
        m_Boot.Run( "Preloading shaders", [this] { m_AssetPreloader->PreloadShaders(); } );
        m_Boot.Run( "Preloading meshes, textures and materials",
                    [this] { m_AssetPreloader->PreloadCookedAssetsAndMaterials(); } );
        m_Boot.Run( "Preloading skyboxes", [this] { m_AssetPreloader->PreloadSkyboxes(); } );
        m_Boot.Run( "Preloading cloud noise volumes",
                    [this] { m_AssetPreloader->PreloadCloudNoiseVolumes(); } );
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
        // Order-free. A packaged game reads its `.destrings` out of Content.dpak through the same VFS as
        // everything else, so the player sees the language the build boots in with no extra plumbing.
        m_Boot.Run( "Preloading string tables", [this] { m_AssetPreloader->PreloadStringTables(); } );

        // Same system set + order as the editor's Play mode — and it is the SAME LIST, not a copy of it
        // (Engine/Core/SceneRenderCollectors.hpp). "Same as the editor" was a comment above a hand-copied
        // block, which is the arrangement that let a sixth caller omit the whole thing silently (Ю16).
        m_Boot.Run( "Building the render collectors and gameplay systems", [this] {
            Desert::Core::AddSceneRenderCollectors( *m_Scene );
            m_Scene->AddSystem<ECS::AnimationECSSystem>( m_AnimationLibrary.get(), m_AssetManager.get() );
            m_Scene->AddSystem<ECS::AttachmentSystem>( m_Scene.get() );
            m_Scene->AddSystem<ECS::ScriptSystem>( m_Scene.get(), m_AssetManager.get() );
            m_Scene->AddSystem<ECS::PhysicsECSSystem>( m_Scene.get() );
            m_Scene->AddSystem<ECS::LocomotionSystem>( m_Scene.get() );
            m_Scene->AddSystem<ECS::AudioECSSystem>( m_Scene.get() );
        } );

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
            const auto sceneJson = m_Boot.Run( "Reading the scene file", [&scenePath] {
                return Common::Utils::FileSystem::ReadFileContent( scenePath );
            } );
            if ( !sceneJson )
                return Common::MakeError( sceneJson.GetError() );
            if ( const auto loaded = m_Boot.Run( "Deserialising the scene",
                                                 [&] {
                                                     return serializer.DeserializeFromJson(
                                                          sceneJson.GetValue(), scenePath );
                                                 } );
                 !loaded )
                return Common::MakeError( loaded.GetError() );
            if ( const auto init =
                      m_Boot.Run( "Initialising the loaded scene", [this] { return m_Scene->Init(); } );
                 !init )
                return init;
            LOG_INFO( "[Runtime] Scene loaded: {}", scenePath );
        }
        else
        {
            LOG_WARN( "[Runtime] No scene to load ('{}') — starting empty. Set DefaultScene in the "
                      ".deproj or pass --scene <path>.",
                      scenePath );
        }

        // Straight into gameplay: scripts tick, physics runs, the main CameraComponent drives the view.
        m_Scene->SetState( Core::Scene::SceneState::Play );
        TriggerSplash(); // the boot scene's splash is the game's startup splash

        m_Boot.LogSummary();
        // AND HERE THE BOOT IS OVER, which is what turns every later synchronous load into a reported
        // hitch. This line and not `Renderer::BeginFrame`: the runtime presents splash frames while it
        // boots, and a phase keyed on the first frame would have marked the whole preload as in-frame.
        // See Engine/Assets/SyncLoadLedger.hpp.
        Assets::SyncLoadLedger::NoteBootFinished();
        LOG_INFO( "[SyncLoad] boot finished — {}", Assets::SyncLoadLedger::Report() );
        LOG_INFO( "[Memory] boot finished — {}", Graphic::MemoryReadout::Take().Report() );
        return BOOLSUCCESS;
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
        const auto jsonRead = Common::Utils::FileSystem::ReadFileContent( path );
        if ( !jsonRead )
        {
            LOG_ERROR( "[Runtime] Scene switch refused, the running scene is untouched: {}", jsonRead.GetError() );
            return;
        }
        const std::string& json = jsonRead.GetValue();
        if ( const auto loadable = Core::ParseLoadableScene( path, json ); !loadable )
        {
            LOG_ERROR( "[Runtime] Scene switch refused, the running scene is untouched: {}", loadable.GetError() );
            return;
        }

        EngineContext::GetInstance().GetDevice()->WaitIdle(); // scene teardown frees GPU resources
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
        m_Scene->SetState( Core::Scene::SceneState::Play );
        TriggerSplash();
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

    Common::BoolResultStr RuntimeLayer::OnUpdate( const Common::Timestep& ts )
    {
        // The startup boundary, logged once: everything before this line (preloads, shader compiles,
        // scene load) is what a player waits through — the millisecond timestamps upstream attribute
        // that wait to its phases, this line marks where it ended.
        if ( !m_LoggedFirstUpdate )
        {
            m_LoggedFirstUpdate = true;
            LOG_INFO( "[Runtime] first update — startup work is done, the game is presenting" );
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

        if ( const auto begin = m_Scene->BeginScene(); !begin )
            return Common::MakeError( begin.GetError() );

        m_Scene->OnUpdate( ts );

        if ( const auto end = m_Scene->EndScene(); !end )
            return Common::MakeError( end.GetError() );

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

    // The runtime's frame present — no ImGui. Opens the swapchain pass, blits the scene's final image
    // fullscreen, then draws the UI canvas + splash with the engine's Render2D batcher. (Named OnImGuiRender
    // only because that's the per-frame Layer hook the Application invokes between BeginFrame and Present.)
    Common::BoolResultStr RuntimeLayer::OnImGuiRender()
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
                // 1) Present the scene: blit its final (tonemapped) image over the whole swapchain.
                if ( const auto image = m_Scene->GetFinalImage() )
                {
                    if ( auto tp = m_BlitExecutor->GetTexture2DProperty( "u_Texture" ) )
                        tp->SetImage( image.get() );
                    renderer.SubmitFullscreenQuad( m_BlitPipeline.get(), m_BlitExecutor.get() );
                }

                // 2) UI + splash via Render2D, on top.
                m_Render2D->BeginFrame( { 0.0f, 0.0f, w, h } );
                auto& dl = m_Render2D->GetDrawList();

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
                input.ScrollDelta   = m_ScrollAccum;
                input.TypedText     = m_TypedText;
                input.Backspace     = m_Backspace;
                input.Tab           = m_TabPressed;
                input.Submit        = m_SubmitPressed;
                m_PrevMouseDown     = down;
                m_ScrollAccum       = 0.0f;
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
                m_UIView.Materials = &m_Render2D->Materials();
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
                    if ( auto* tex = Runtime::ResourceRegistry::GetTextureService()->Get( m_SplashSprite ) )
                    {
                        auto* img = static_cast<Graphic::Image2D*>(
                             Runtime::ResourceRegistry::GetImageService()->Resolve( tex->GetImageHandle() ) );
                        if ( img && img->GetWidth() > 0 && img->GetHeight() > 0 )
                        {
                            const float iw  = static_cast<float>( img->GetWidth() );
                            const float ih  = static_cast<float>( img->GetHeight() );
                            const float fit = std::min( w / iw, h / ih );
                            const float sw = iw * fit, sh = ih * fit;
                            const float cx = w * 0.5f, cy = h * 0.5f;
                            dl.AddImage( img, { cx - sw * 0.5f, cy - sh * 0.5f },
                                         { cx + sw * 0.5f, cy + sh * 0.5f }, { 0.0f, 0.0f }, { 1.0f, 1.0f },
                                         glm::vec4( 1.0f, 1.0f, 1.0f, a ) );
                        }
                    }
                }

                m_Render2D->Flush();
            }
        }

        renderer.EndRenderPass();

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
