#include <Engine/Core/Application.hpp>
#include <Engine/Core/EngineContext.hpp>
#include <Engine/Core/FrameManager.hpp>
#include <Engine/Core/RenderTask.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/DeviceLost.hpp>

#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include <Engine/Assets/AssetEviction.hpp>
#include <Engine/Core/SceneAssetRoots.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Graphic/ViewMemory.hpp>
#include <Engine/Graphic/ViewResources.hpp>

#include <Common/Core/EngineThread.hpp>
#include <Common/Core/Profiler.hpp>
#include <Common/Settings/MachineSettings.hpp>

#include <Engine/Core/Glfw.hpp>

#include <algorithm>
#include <chrono>
#include <format>
#include <thread>
#include <utility>

namespace Desert::Engine
{
    Application::Application( const ApplicationInfo& appInfo ) : m_ApplicationInfo( appInfo )
    {
        // THE GAME THREAD is the thread that builds the application and runs its loop (UE: GGameThreadId, the
        // thread FEngineLoop::PreInit runs on): named and raised here, and the process opts out of EcoQoS so a
        // run without focus keeps the clock of one with it (OS-SCHED-HYGIENE).
        Common::NameCurrentThread( "Game Thread", Common::ThreadPriority::AboveNormal );
        if ( const auto optedOut = Common::OptOutOfPowerThrottling(); !optedOut )
            LOG_WARN( "[Threads] EcoQoS not opted out: {}", optedOut.GetError() );
        WindowSpecification windowSpec;
        windowSpec.Title     = appInfo.Title;
        windowSpec.VSync     = appInfo.Display.VSync;
        windowSpec.Decorated = appInfo.Decorated;
        windowSpec.Visible   = appInfo.Visible;
        windowSpec.Mode      = appInfo.Display.Mode;
        // The two monitor-sized modes take their size from the monitor in the platform window's Init.
        if ( appInfo.Display.UsesResolution() )
        {
            windowSpec.Width  = appInfo.Display.ResolutionX;
            windowSpec.Height = appInfo.Display.ResolutionY;
        }
        m_FramePacer.SetLimit( appInfo.Display.FrameRateLimit );

        m_Window = Window::Create( windowSpec );
        m_WindowEventNode = m_Events.Attach<Window>( m_ApplicationEventNode, *m_Window );
        m_Window->SetEventTree( m_Events );
        m_Window->Init();

        // 1. Create RendererContext (Vulkan Instance)
        m_RendererContext = Graphic::RendererContext::Create( m_Window );
        
        // 2. Partially initialize EngineContext
        EngineContext::CreateInstance().Initialize( m_Window, nullptr, m_RendererContext );

        // 3. The machine's store, then the device: the device is created from what machine.json withholds
        // (HideRayTracing), so the store cannot wait for QualityBoot, which runs after the device exists.
        if ( !appInfo.MachineSettingsFile.empty() )
            ::Common::Settings::MachineSettings::Load( appInfo.MachineSettingsFile );

        // 3b. Create Device
        // A REFUSED DEVICE IS AN ERROR WITH A LIST, NOT A CRASH. DeviceCaps names every required capability
        // the GPU lacks; construction stops here, EntryPoint sees StartupRefusal() and skips OnCreate/Run,
        // and the process ends with that message and a failing exit code. Not std::exit: worker threads
        // already exist (TeardownOrder.StdExitAppearsOnlyBeforeTheApplicationExists).
        auto device = Device::Create();
        if ( !device )
        {
            m_StartupRefusal = device.GetError();
            LOG_ERROR( "[Device] cannot start: {}", m_StartupRefusal );
            Close( EXIT_FAILURE );
            return;
        }
        m_Device = device.ExtractValue();

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

        m_EngineSubsystems.emplace( *this, m_Events, m_ApplicationEventNode );
    }

    Application::~Application()
    {
        // A refused start built no device and no renderer, so there is nothing below to wait on or release.
        if ( !m_StartupRefusal.empty() )
            return;

        // Nothing below may be recorded into, or referenced by, a command buffer the GPU has not finished
        // with. Run() has already presented its last frame, but presentation only queues the work.
        if ( m_Device )
            m_Device->WaitIdle();

        m_EngineSubsystems.reset();

        // The render caches the worlds parked as their components died (FrameManager::RetireAfterFrames) are
        // released while the device that owns them is alive: the queue is a static and would outlive it.
        FrameManager::GetInstance().ReleaseRetiredNow();

        // Everything the renderer generated at Init() (the BRDF LUT, the fallback textures, the API
        // object) and every GPU resource the registries handed out lives in a static that outlives this
        // object. Released here, while the device and the allocator are still alive, because a static
        // destructor cannot be ordered against them.
        Graphic::Renderer::GetInstance().Shutdown();

        // Members then die window -> device -> context; see the note on the declarations.
    }

    bool Application::OnWindowClosed( Common::EventWindowClose& /*close*/ )
    {
        if ( m_CloseGate.StopsNow() )
            m_IsRunningApplication = false;
        return true;
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

    void Application::AttachLayer( std::unique_ptr<Common::Layer> layer )
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
        // Loop state rather than a member: nothing outside this loop has a use for it, and the whole of
        // what it remembers is whether the PREVIOUS iteration stood down (see 1b below).
        Graphic::DrawableAreaGate drawableArea;
        // The '[Threads]' census is written once, after the first frame: the render, submit and pool threads run.
        bool threadsCensused = false;
        // A shared view resource forgotten by the game thread while a view records on the render task waits for
        // it (ViewResourceRegistry::SetForgetFence).
        Graphic::ViewResourceRegistry::SetForgetFence( [] { ::Desert::Engine::RenderTask::Get().JoinOnGameThread(); } );
        // THE SCENE VIEWS RECORD ON THE RENDER TASK, overlapping the interface (UE: the rendering thread records
        // the view family while the game thread ticks and paints Slate). What the views read from the world is
        // gathered on this thread first (ExtensionPass::Gather, BeginScene's snapshots); the palette's
        // "Debug.Scene record: serial" puts them back inline for a frame compare.
        RenderTask::Get().SetOverlapped( true );
        // A failure of the frame's render-task half (its begin, its submit and present), handed back to this loop
        // (OVERLAP-ON-1): the job notes it, the loop acts on it at the next check, as it acted on the result.
        struct RenderFailure
        {
            std::mutex  Mutex;
            bool        Failed  = false;
            bool        EndsRun = false;
            std::string Stage;
            std::string Error;
        };
        RenderFailure renderFailure;
        const auto    noteFailure =
             [&renderFailure]( const char* stage, const bool endsRun, const std::string& error )
        {
            const std::lock_guard<std::mutex> lock( renderFailure.Mutex );
            if ( renderFailure.Failed )
                return;
            renderFailure.Failed  = true;
            renderFailure.EndsRun = endsRun;
            renderFailure.Stage   = stage;
            renderFailure.Error   = error;
        };
        // True: the run ends (the caller breaks).
        const auto endRunOnRenderFailure = [this, &renderFailure]() -> bool
        {
            std::string stage;
            std::string error;
            bool        endsRun = false;
            {
                const std::lock_guard<std::mutex> lock( renderFailure.Mutex );
                if ( !renderFailure.Failed )
                    return false;
                renderFailure.Failed = false;
                stage                = renderFailure.Stage;
                error                = renderFailure.Error;
                endsRun              = renderFailure.EndsRun;
            }
            if ( EndRunOnDeviceLoss( stage.c_str() ) )
                return true;
            LOG_ERROR( "[Application] {} failed: {}", stage, error );
            if ( !endsRun )
                return false;
            Close( 1 );
            return true;
        };
        // The presented notifications of a frame handed to the render task (step 7), delivered once it is out.
        std::vector<std::function<void()>> presentedPending;
        const auto                         deliverPresented = [&presentedPending]()
        {
            std::vector<std::function<void()>> due;
            due.swap( presentedPending );
            // A lost device is asked for no screenshot (the run ends at the failure check).
            if ( Graphic::DeviceLost::IsLost() )
                return;
            for ( const std::function<void()>& notify : due )
                notify();
        };
        while ( m_IsRunningApplication )
        {
            // Frame boundary for the profiler (publishes last frame, flips Optick's frame). Placed at the
            // very top so every scope below — acquire, update, UI, present — is attributed to this frame.
            DESERT_PROFILE_FRAME( "Frame" );

            float    time     = (float)glfwGetTime();
            const float held     = std::exchange( m_ExcludedSeconds, 0.0f );
            const float timestep = m_FixedDeltaTime.has_value() ? *m_FixedDeltaTime
                                                                : std::max( 0.0f, time - m_LastFrameTime - held );
            m_LastFrameTime   = time;

            {
                DESERT_PROFILE_SCOPE( "EngineStats" );
                m_EngineStats.Update();
            }

            // 1. Pump GLFW events before touching any GPU resources.
            // Resize/close callbacks can destroy descriptor sets and framebuffers; they must
            // fire outside of a recording session, before PrepareNextFrame applies a requested rebuild.
            {
                DESERT_PROFILE_SCOPE( "ProcessEvents" );
                m_Window->ProcessEvents();
            }

            // 1b. STAND THE WHOLE FRAME DOWN WHILE THERE IS NOTHING TO DRAW INTO.
            //
            // A minimised window reports a 0x0 framebuffer, and no image can be acquired from a surface
            // with no area. Everything below then runs on a frame that cannot finish: the acquire fails,
            // Submit still waits on the PresentComplete semaphore that acquire never signalled
            // (VUID-vkQueueSubmit-pWaitSemaphores-03238) and Present hands back whatever image index was
            // left from the last real frame. Measured on Windows, three minimise/restore cycles: 72
            // acquire failures and 72 of that VUID, one pair per frame.
            //
            // HERE, BEFORE THE FRAME IS BEGUN, WHICH IS WHAT MAKES `continue` SAFE. Skipping the rest of the
            // loop after BeginFrame would leave a frame open, and skipping it after the late acquire (step 5,
            // RendererAPI::ImportBackBuffer) would leak an acquired image every iteration until the next
            // acquire blocked forever (see BeginFrame's failure branch below, which cannot use `continue` for
            // exactly that reason). Nothing has been begun or acquired at this point in the loop.
            //
            // The event pump above keeps running, which is what eventually delivers the restore.
            const auto drawable = drawableArea.Observe( m_Window->HasDrawableArea() );
            if ( drawable.SkipFrame )
            {
                if ( drawable.AnnounceStop )
                    LOG_INFO( "[Application] the window has no drawable area (minimised); standing the "
                              "frame loop down until it comes back. Nothing is acquired, recorded, "
                              "submitted or presented until then." );

                // NOT A SPIN. Present is what paced this loop, and skipping it removes the only thing that
                // made an iteration cost anything — a minimised editor would otherwise burn a core doing
                // nothing. Long enough to idle, short enough that the restore is acted on in one frame's
                // worth of time.
                // A frame handed to the render task finishes, and is announced, before the loop stands down.
                RenderTask::Get().Join();
                deliverPresented();
                std::this_thread::sleep_for( std::chrono::milliseconds( 16 ) );
                continue;
            }
            if ( drawable.AnnounceResume )
            {
                LOG_INFO( "[Application] the window has a drawable area again; frames resume." );
            }

            // 2. Prepare Frame: the frame boundary (a requested swapchain rebuild is applied, a lost device is
            // reported). NOT the acquire any more: the swapchain image is acquired late, when the frame's final
            // pass imports the back buffer during step 5 (RendererAPI::ImportBackBuffer, PRESENT-COST), so the
            // CPU no longer blocks on the display (vsync, a full present queue) before it has built the frame.
            {
                DESERT_PROFILE_SCOPE( "PrepareNextFrame (frame boundary)" );
                // ITS RESULT USED TO BE DISCARDED — by this line, and by the two links beneath it, which
                // both declared themselves void over a backend that returns one. A frame that could not
                // acquire an image was therefore recorded and submitted anyway.
                // A REQUESTED SWAPCHAIN REBUILD IS APPLIED HERE, ON THE WINDOW'S THREAD, with nothing on the
                // render task (RendererContext::BeginFrame): it releases the images the frame in flight presents
                // and the capture staging that frame's window shot reads, and on Windows the swapchain's create
                // and destroy talk to the window, whose messages only this thread pumps. So the loop joins, and
                // announces that frame (the serial order), only when a rebuild is pending (OVERLAP-ON-3; UE:
                // RHIResizeViewport runs on the game thread behind FlushRenderingCommands). Every other frame
                // boundary overlaps the frame in flight.
                if ( const std::shared_ptr<Graphic::SwapChain> swapChain = m_Window->GetWindowSwapChain();
                     swapChain && swapChain->RebuildPending() )
                {
                    RenderTask::Get().Join();
                    deliverPresented();
                }
                const auto prepared = m_Window->PrepareNextFrame();
                if ( !prepared.IsSuccess() )
                {
                    if ( EndRunOnDeviceLoss( "preparing the next frame" ) )
                        break;
                    // A failure here that is NOT a device loss has no other cause today — the Vulkan
                    // backend's frame boundary reports only that — so this branch is currently unreachable
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
            {
                DESERT_PROFILE_SCOPE( "Assets: Eviction" );
                Assets::AssetEvictionSchedule::RunIfDue( [] { return Core::CollectAssetRootsFromLiveScenes(); } );
            }

            // The meshes and textures a sweep dropped are freed here, frames later, once no frame in flight can
            // still draw them (Assets::FrameRetireQueue). Every frame, because the margin is counted in frames
            // begun.
            {
                DESERT_PROFILE_SCOPE( "Assets: Retire Evicted" );
                Runtime::ResourceRegistry::GetMeshService()->RetireEvicted();
                Runtime::ResourceRegistry::GetTextureService()->RetireEvicted();
            }

            // Textures whose cook finished on a worker get their GPU image here, within the per-frame upload
            // budget (TextureUploadSettings), before any command buffer of this frame is open.
            {
                DESERT_PROFILE_SCOPE( "Textures: Pump Uploads" );
                Runtime::ResourceRegistry::GetTextureService()->PumpUploads();
            }
            {
                DESERT_PROFILE_SCOPE( "Textures: Update Streaming" );
                Runtime::ResourceRegistry::GetTextureService()->UpdateStreaming();
            }

            // 3. Start recording commands for this frame. Its first statement is the frame's one CPU wait on the GPU
            // (row "Wait for GPU": the fence of frame N-k, k = frames in flight, VulkanRendererAPI::BeginFrame), so
            // everything above it in this iteration overlaps the GPU still executing earlier frames.
            // OVERLAP-ON-1: the GPU wait stays here, on the game thread, which writes this slot's per-frame data
            // from now on; the slot's recording begins on the render task (job "Frame: Begin" below), behind frame
            // N-1's submit, so the render graph's frame objects are begun in frame order on the recording thread.
            // The fence of frame N-k is waited only once N-k was submitted: with one frame of lag and k >= 2 it
            // always was (the last iteration's SyncPreviousFrame); a swapchain of one image joins first.
            {
                const FrameManager& frames = FrameManager::GetInstance();
                const uint64_t      frame  = frames.GetGameFrameCount();
                const uint32_t      slots  = frames.GetMaxFramesInFlight();
                if ( frame >= slots && !RenderTask::Get().FrameSubmitted( frame - slots ) )
                    RenderTask::Get().Join();
            }
            const auto frameBegun = []
            {
                DESERT_PROFILE_SCOPE( "Renderer: BeginFrame" );
                return Graphic::Renderer::GetInstance().WaitFrameSlot();
            }();
            if ( !frameBegun.IsSuccess() )
            {
                // END THE RUN, and neither of the two cheaper answers is available here.
                //
                // Recording anyway is what this line did before: everything below writes into the command
                // buffer BeginFrame was supposed to open, and with no open buffer PresentFinalImage still
                // submits and presents — a stale or torn image with no error anywhere, which is the
                // failure shape this project has paid for more than once.
                //
                // `continue` is worse still, and less obviously so: it skips the frame's submit, and the
                // frame slot's state (and, if an import ever ran before this, an acquired swapchain image that
                // only a present gives back) would carry into the next iteration unfinished.
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
            {
                const RenderTask::ScopedOwner frameWork( RenderTask::FrameOwner() );
                RenderTask::Get().Enqueue( "Frame: Begin",
                                           [&noteFailure]() -> Common::BoolResultStr
                                           {
                                               DESERT_PROFILE_SCOPE( "Renderer: BeginFrame (recording)" );
                                               Common::BoolResultStr begun =
                                                    Graphic::Renderer::GetInstance().BeginFrameRecording();
                                               if ( !begun.IsSuccess() )
                                                   noteFailure( "beginning the frame", true, begun.GetError() );
                                               return begun;
                                           } );
            }
            if ( endRunOnRenderFailure() )
                break;

            // 4. Update layers (Scene rendering to offscreen buffers)
            for ( const auto& layer : m_LayerStack )
            {
                DESERT_PROFILE_SCOPE( "Layers: OnUpdate" );
                const auto updated = layer->OnUpdate( Common::Timestep( timestep ) );
                if ( !updated.IsSuccess() )
                    ReportLayerFailure( "OnUpdate", layer.get(), updated.GetError() );
            }

            // 4b. THE FRAME BOUNDARY (UE FFrameEndSync with one frame of lag, OVERLAP-ON-1). Frame N-1's render
            // work - views, interface, submit and present - has finished before this frame's interface is painted,
            // and N-1 is announced (its presented notifications, then the commands deferred behind it). The head
            // and the tick above overlapped it; a world's views joined it already before their own bracket.
            RenderTask::Get().SyncPreviousFrame();
            deliverPresented();
            if ( endRunOnRenderFailure() )
                break;

            // 5. UI Rendering
            {
                DESERT_PROFILE_SCOPE( "UI Render" );
                for ( const auto& layer : m_LayerStack )
                {
                    const auto rendered = layer->OnUIRender();
                    if ( !rendered.IsSuccess() )
                        ReportLayerFailure( "OnUIRender", layer.get(), rendered.GetError() );
                }
                // NOT THE FRAME'S FENCE ANY MORE (OVERLAP-ON-1): the frame's views and interface are jobs, and its
                // submit and present follow them on the same queue (step 6). Deferred events change what those
                // jobs read, so a frame that has some joins its jobs first (today's order); one without any
                // does not.
                if ( m_Events.DeferredCount() > 0 )
                {
                    RenderTask::Get().Join();
                    m_Events.RouteDeferred();
                }
            }

            // Whether this frame is handed to the render task (steps 6 and 7, OVERLAP-ON-1).
            const bool handedOver = RenderTask::Get().Overlaps();
            // Each layer captures what its presented notification needs of THIS frame (the serial that built it,
            // its shot record). Handed over, BEFORE step 6 enqueues the submit and present (OVERLAP-ON-5): a job a
            // layer enqueues in its capture runs in this frame's order, after its views and before its submit and
            // present (EditorLayer's "Editor: frame census"). Inline, at step 7 as before (the serial order).
            std::vector<std::function<void()>> captured;
            const auto                         captureFramePresented = [this, handedOver, &captured]()
            {
                DESERT_PROFILE_SCOPE( "CaptureFramePresented" );
                for ( const auto& layer : m_LayerStack )
                    if ( std::function<void()> notify = layer->CaptureFramePresented( handedOver ) )
                        captured.push_back( std::move( notify ) );
            };
            if ( handedOver )
                captureFramePresented();
            // 6. Submit all recorded commands and Present. No fence wait here (GPU-COST-11): the only block left in
            // this row is vkQueuePresentKHR's own ("Vulkan: Present", a full presentation queue under FIFO vsync);
            // the GPU throttle is step 3's "Wait for GPU".
            {
                DESERT_PROFILE_SCOPE( "PresentFinalImage (Submit)" );
                // The frame's LAST job (RenderTask::ThreadMain names a job of this frame that starts after it):
                // the render task submits and presents frame N while this thread goes on to frame N+1's head
                // and tick
                // (UE: the render thread presents, the game thread waits only for N-1). Inline in serial mode and
                // with the overlap off, where Renderer::PresentFinalImage moves the frame count itself. A lost
                // device is noticed here inline, else at step 4b; either way before any screenshot (step 7).
                {
                    const RenderTask::ScopedOwner frameWork( RenderTask::FrameOwner() );
                    RenderTask::Get().Enqueue( "Frame: Submit/Present",
                                               [this, &noteFailure]() -> Common::BoolResultStr
                                               {
                                                   Common::BoolResultStr sent = m_Window->PresentFinalImage();
                                                   if ( !sent.IsSuccess() )
                                                       noteFailure( "submitting and presenting the frame", false,
                                                                    sent.GetError() );
                                                   return sent;
                                               } );
                }
                if ( handedOver )
                    FrameManager::GetInstance().NextFrame();
                // Inline the result is here already; handed over, at the next check (step 4b).
                if ( endRunOnRenderFailure() )
                    break;
            }

            // 7. The frame is OUT. The only point in this loop at which the composited picture — scene and
            // interface together — exists as bytes on the device; everything above is still building it.
            // A layer that must read the frame back, or must not answer a question before a frame has
            // answered it, gets its instant here. Default is a no-op (Common::Layer::OnFramePresented).
            {
                DESERT_PROFILE_SCOPE( "OnFramePresented" );
                // The notifications captured above (handed over) or now (inline) run once the frame is out: now
                // when it was presented inline, else at the next iteration's frame boundary (step 4b), behind its
                // submit and present. A run that ended at step 6 announces nothing of this frame, as before.
                if ( !handedOver )
                    captureFramePresented();
                for ( std::function<void()>& notify : captured )
                    presentedPending.push_back( std::move( notify ) );
                if ( !handedOver )
                    deliverPresented();
            }

            // 8. Pipelines the driver built this frame go to disk now rather than only at a clean exit.
            {
                DESERT_PROFILE_SCOPE( "PersistPipelineCache" );
                if ( const auto persisted = m_Device->PersistPipelineCache(); !persisted )
                    LOG_WARN( "[PipelineCache] not written: {}", persisted.GetError() );
            }

            if ( !threadsCensused )
            {
                threadsCensused = true;
                LOG_INFO( "[Threads] {}", Common::DescribeEngineThreads() );
            }

            // 9. The frame rate limit (GameUserSettings::Display.FrameRateLimit): the next frame starts on
            // the pacer's schedule. Returns at once when there is no limit.
            {
                DESERT_PROFILE_WAIT( "FramePacer" );
                m_FramePacer.WaitForNextFrame();
            }
        }

        // The last frame handed over is out and announced before the render task's thread ends with the loop,
        // and that before the layers release what its jobs recorded with.
        RenderTask::Get().Join();
        deliverPresented();
        RenderTask::Get().Shutdown();
        Graphic::ViewResourceRegistry::SetForgetFence( nullptr );

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

    Common::BoolResultStr PresentInterimFrame( const Window& window )
    {
        DESERT_PROFILE_SCOPE( "PresentInterimFrame" );
        if ( const auto presented = window.PresentFinalImage(); !presented.IsSuccess() )
            return Common::MakeError( std::format( "presenting the interim frame: {}", presented.GetError() ) );
        if ( const auto prepared = window.PrepareNextFrame(); !prepared.IsSuccess() )
            return Common::MakeError(
                 std::format( "the frame boundary after the interim frame: {}", prepared.GetError() ) );
        if ( const auto begun = Graphic::Renderer::GetInstance().BeginFrame(); !begun.IsSuccess() )
            return Common::MakeError( std::format( "beginning after the interim frame: {}", begun.GetError() ) );
        return BOOLSUCCESS;
    }

} // namespace Desert::Engine
