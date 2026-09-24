#include <Engine/Core/Application.hpp>
#include <Engine/Core/EngineContext.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/DeviceLost.hpp>
#include <Engine/Assets/AssetEviction.hpp>
#include <Engine/Core/SceneAssetRoots.hpp>

#include <Common/Core/EventRegistry.hpp>
#include <Common/Core/Profiler.hpp>

#include <GLFW/glfw3.h>

namespace Desert::Engine
{
    Application::Application( const ApplicationInfo& appInfo ) : m_ApplicationInfo( appInfo )
    {
        WindowSpecification windowSpec;
        windowSpec.Title     = appInfo.Title;
        windowSpec.VSync     = appInfo.VSync;
        windowSpec.Decorated = appInfo.Decorated;
        windowSpec.Visible   = appInfo.Visible;
        if ( appInfo.Width.has_value() && appInfo.Height.has_value() )
        {
            windowSpec.Width      = *appInfo.Width;
            windowSpec.Height     = *appInfo.Height;
            windowSpec.Fullscreen = false;
        }
        else
        {
            // No explicit size -> fullscreen at the monitor's native resolution (WindowsWindow::Init fills
            // Width/Height from the primary monitor's video mode).
            windowSpec.Fullscreen             = true;
            windowSpec.FullscreenCoverTaskbar = appInfo.FullscreenCoverTaskbar;
        }

        m_Window = Window::Create( windowSpec );
        m_Window->Init();

        // 1. Create RendererContext (Vulkan Instance)
        m_RendererContext = Graphic::RendererContext::Create( m_Window );
        
        // 2. Partially initialize EngineContext
        EngineContext::CreateInstance().Initialize( m_Window, nullptr, m_RendererContext );

        // 3. Create Device
        m_Device = Device::Create();
        
        // 4. Register Device
        EngineContext::GetInstance().SetDevice( m_Device );

        // 5. Init Context (Allocators)
        m_RendererContext->Init();

        // 6. Setup SwapChain
        auto swapChainResult = m_Window->SetupSwapChain();
        DESERT_VERIFY( swapChainResult.IsSuccess(), "Failed to setup SwapChain" );

        // 7. Initialize Global Renderer
        //
        // ITS RESULT WAS DISCARDED. Renderer::Init returns BoolResultStr and can fail for two reasons that
        // matter — no rendering API was selected, and the BRDF LUT could not be created — after either of
        // which every frame below is drawn against half a renderer. Same instrument as the swapchain line
        // above it, and for the same reason: there is no partially-working renderer to continue with.
        const auto rendererReady = Graphic::Renderer::CreateInstance().Init();
        DESERT_VERIFY( rendererReady.IsSuccess(), "Failed to initialize the renderer: {}",
                       rendererReady.GetError() );

        // A DEVICE CAN BE LOST BEFORE THE FIRST FRAME. Everything above uploads to the GPU — the BRDF LUT,
        // the fallback textures, the swapchain's images — and on this machine another process's GPU reset
        // is as likely during startup as at any other moment. Without this the engine would enter Run()
        // with a latched device and half-built resources, and the loop's own check would only stop it one
        // frame later, after a stream of refusals. Refusing HERE names the cause while it is still the
        // only thing that has gone wrong.
        DESERT_VERIFY( !Graphic::DeviceLost::IsLost(),
                       "The GPU device was lost while the engine was starting up; see the [DeviceLost] "
                       "block above. Nothing was drawn and nothing was open, so there is nothing to "
                       "recover — start again." );

        m_Window->SetEventCallback( [this]( Common::Event& e ) { ProcessEvents( e ); } );
    }

    Application::~Application()
    {
        // Nothing below may be recorded into, or referenced by, a command buffer the GPU has not finished
        // with. Run() has already presented its last frame, but presentation only queues the work.
        if ( m_Device )
            m_Device->WaitIdle();

        // Everything the renderer generated at Init() (the BRDF LUT, the fallback textures, the API
        // object) and every GPU resource the registries handed out lives in a static that outlives this
        // object. Released here, while the device and the allocator are still alive, because a static
        // destructor cannot be ordered against them.
        Graphic::Renderer::GetInstance().Shutdown();

        // Members then die window -> device -> context; see the note on the declarations.
    }

    void Application::ProcessEvents( Common::Event& e )
    {
        Common::EventManager eventManager( e );
        eventManager.Notify<Common::EventWindowClose>( [this]( Common::EventWindowClose& e )
                                                       { return this->OnClose( e ); } );

        for ( auto it = m_LayerStack.end(); it != m_LayerStack.begin(); )
        {
            ( *--it )->OnEvent( e );
            if ( e.m_Handled )
                break;
        }
    }

    void Application::ReportLayerFailure( const char* stage, Common::Layer* layer, const std::string& error )
    {
        // ONCE PER DISTINCT MESSAGE, and the deduplication is the point rather than tidiness. `OnUpdate`
        // and `OnUIRender` run every frame, so a failure that persists — a scene that will not begin,
        // a pipeline that will not build — is not one event but sixty a second. Logging each one makes
        // the log unreadable, which is the same outcome as not logging at all; these four results were
        // dropped on the floor before this commit and the cure must not be a flood.
        const std::string key = std::string( stage ) + '|' + layer->GetName() + '|' + error;
        if ( !m_ReportedLayerFailures.insert( key ).second )
            return;

        LOG_ERROR( "[Application] layer '{}' failed in {}: {}", layer->GetName(), stage, error );
    }

    void Application::PushLayer( std::unique_ptr<Common::Layer> layer )
    {
        // Borrowed back out of the stack, which now owns it: attaching must not need a second claim.
        Common::Layer* pushed = m_LayerStack.PushLayer( std::move( layer ) );
        if ( !pushed )
            return;

        const auto attached = pushed->OnAttach();
        if ( !attached.IsSuccess() )
        {
            // A layer that did not attach has no resources, and the loop below is about to call OnUpdate
            // on it sixty times a second. Refusing to start is the only answer that does not turn a
            // startup failure into a stream of consequences with a nonzero exit code nowhere in sight —
            // Close's own comment already says why exit 0 on a failed run is the worse error.
            LOG_ERROR( "[Application] layer '{}' failed to attach: {}", pushed->GetName(), attached.GetError() );
            Close( 1 );
        }
    }

    bool Application::EndRunOnDeviceLoss( const char* stage )
    {
        if ( !Graphic::DeviceLost::IsLost() )
            return false;

        // The explanation was already printed, once, by whoever first met the loss. This line only says
        // which stage of the loop is standing down, so that the log reads as one event with an ending
        // rather than as a second, unrelated failure.
        LOG_ERROR( "[Application] the device is lost; ending the run while {}. The [DeviceLost] block "
                   "above is the cause, and the {} Vulkan calls refused since then are what did NOT go to "
                   "a dead device.",
                   stage, Graphic::DeviceLost::RefusedCalls() );
        Close( kExitDeviceLost );
        return true;
    }

    void Application::Run()
    {
        float m_LastFrameTime = 0.0f;
        while ( m_IsRunningApplication )
        {
            // Frame boundary for the profiler (publishes last frame, flips Optick's frame). Placed at the
            // very top so every scope below — acquire, update, UI, present — is attributed to this frame.
            DESERT_PROFILE_FRAME( "Frame" );

            float    time     = (float)glfwGetTime();
            float    timestep = time - m_LastFrameTime;
            m_LastFrameTime   = time;

            m_EngineStats.Update();

            // 1. Pump GLFW events before touching any GPU resources.
            // Resize/close callbacks can destroy descriptor sets and framebuffers; they must
            // fire outside of a recording session, before PrepareNextFrame acquires the image.
            {
                DESERT_PROFILE_SCOPE( "ProcessEvents" );
                m_Window->ProcessEvents();
            }

            // 2. Prepare Frame (Acquire next image) — CPU blocks here if the GPU is behind / vsync-gated.
            {
                DESERT_PROFILE_SCOPE( "PrepareNextFrame (Acquire)" );
                // ITS RESULT USED TO BE DISCARDED — by this line, and by the two links beneath it, which
                // both declared themselves void over a backend that returns one. A frame that could not
                // acquire an image was therefore recorded and submitted anyway.
                const auto prepared = m_Window->PrepareNextFrame();
                if ( !prepared.IsSuccess() )
                {
                    if ( EndRunOnDeviceLoss( "acquiring the next image" ) )
                        break;
                    // A failure here that is NOT a device loss has no other cause today — the Vulkan
                    // backend's acquire path reports only that — so this branch is currently unreachable
                    // and is a log rather than an ending. Reporting it and carrying on is what this line
                    // did before, with the difference that it now says so.
                    LOG_ERROR( "[Application] PrepareNextFrame failed: {}", prepared.GetError() );
                }
            }

            // 2b. RELEASE WHAT NOTHING NEEDS ANY MORE, if a scene change asked for it.
            //
            // HERE AND NOT AT THE SCENE CHANGE ITSELF, for two reasons that are both about what is alive
            // at the moment of the sweep: the scene being replaced is destroyed by the time this frame
            // starts (a sweep during the swap would see its assets as reachable and free nothing), and no
            // command buffer is open yet, so dropping a built material cannot invalidate one that is
            // recording. The material graveyard's own collector runs a few lines later inside the layer
            // update, which is what actually returns the descriptor pools.
            //
            // Free when nothing is due: RunIfDue tests one bool and does not build the root set.
            Assets::AssetEvictionSchedule::RunIfDue( [] { return Core::CollectAssetRootsFromLiveScenes(); } );

            // 3. Start recording commands for this frame
            const auto frameBegun = Graphic::Renderer::GetInstance().BeginFrame();
            if ( !frameBegun.IsSuccess() )
            {
                // END THE RUN, and neither of the two cheaper answers is available here.
                //
                // Recording anyway is what this line did before: everything below writes into the command
                // buffer BeginFrame was supposed to open, and with no open buffer PresentFinalImage still
                // submits and presents — a stale or torn image with no error anywhere, which is the
                // failure shape this project has paid for more than once.
                //
                // `continue` is worse still, and less obviously so: PrepareNextFrame has ALREADY acquired
                // a swapchain image, and skipping the present never gives it back. A few iterations of
                // that and the next acquire blocks forever, so a reported error becomes a hang.
                //
                // Both of BeginFrame's failures are terminal anyway — the window is gone, or
                // vkBeginCommandBuffer refused. The SECOND of those used to be described here as "which
                // means the device is lost", and that reading is now a branch of its own rather than a
                // guess: a lost device leaves by the line below with its own exit code, and everything
                // still reaching exit 1 is a failure that really is ours.
                if ( EndRunOnDeviceLoss( "beginning the frame" ) )
                    break;

                LOG_ERROR( "[Application] BeginFrame failed, ending the run: {}", frameBegun.GetError() );
                Close( 1 );
                break;
            }

            // 4. Update layers (Scene rendering to offscreen buffers)
            for ( const auto& layer : m_LayerStack )
            {
                const auto updated = layer->OnUpdate( Common::Timestep( timestep ) );
                if ( !updated.IsSuccess() )
                    ReportLayerFailure( "OnUpdate", layer.get(), updated.GetError() );
            }

            // 5. UI Rendering
            {
                DESERT_PROFILE_SCOPE( "UI Render" );
                for ( const auto& layer : m_LayerStack )
                {
                    const auto rendered = layer->OnUIRender();
                    if ( !rendered.IsSuccess() )
                        ReportLayerFailure( "OnUIRender", layer.get(), rendered.GetError() );
                }
            }

            // 6. Submit all recorded commands and Present — CPU blocks here on submit/present (GPU-bound/vsync).
            {
                DESERT_PROFILE_SCOPE( "PresentFinalImage (Submit)" );
                const auto presented = m_Window->PresentFinalImage();
                if ( !presented.IsSuccess() )
                {
                    // THE FIRST PLACE A LOST DEVICE IS NORMALLY NOTICED. A submit executes asynchronously,
                    // so the frame that killed the device has already gone by the time anyone is told, and
                    // this is where the news lands. Breaking HERE — rather than one iteration later — is
                    // what keeps step 7 below from asking the dead device for a screenshot.
                    if ( EndRunOnDeviceLoss( "submitting and presenting the frame" ) )
                        break;
                    LOG_ERROR( "[Application] PresentFinalImage failed: {}", presented.GetError() );
                }
            }

            // 7. The frame is OUT. The only point in this loop at which the composited picture — scene and
            // interface together — exists as bytes on the device; everything above is still building it.
            // A layer that must read the frame back, or must not answer a question before a frame has
            // answered it, gets its instant here. Default is a no-op (Common::Layer::OnFramePresented).
            {
                DESERT_PROFILE_SCOPE( "OnFramePresented" );
                for ( const auto& layer : m_LayerStack )
                    layer->OnFramePresented();
            }

            // 8. Pipelines the driver built this frame go to disk now rather than only at a clean exit.
            {
                DESERT_PROFILE_SCOPE( "PersistPipelineCache" );
                if ( const auto persisted = m_Device->PersistPipelineCache(); !persisted )
                    LOG_WARN( "[PipelineCache] not written: {}", persisted.GetError() );
            }
        }

        // Window closed: detach layers (top-down) so each releases its resources and clears its
        // session state — e.g. EditorLayer::OnDetach removes the crash-recovery lock. Nothing else
        // calls OnDetach (the LayerStack dtor is empty), so without this a normal quit looked like
        // an unclean exit and the recovery prompt reappeared on every launch.
        for ( auto it = m_LayerStack.end(); it != m_LayerStack.begin(); )
        {
            Common::Layer* layer    = ( *--it ).get();
            const auto     detached = layer->OnDetach();
            if ( !detached.IsSuccess() )
                ReportLayerFailure( "OnDetach", layer, detached.GetError() );
        }
    }

    void Application::Init()
    {
    }

    void Application::Destroy()
    {
    }

} // namespace Desert::Engine
