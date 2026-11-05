#include <Engine/Core/RenderTask.hpp>

#include <Engine/Core/FrameManager.hpp>

#include <Common/Core/EngineThread.hpp>
#include <Common/Core/Logger.hpp>
#include <Common/Core/Profiler.hpp>

#include <string_view>
#include <unordered_set>
#include <utility>

namespace Desert::Engine
{
    namespace
    {
        thread_local bool t_OnRenderThread = false;
    } // namespace

    RenderTask& RenderTask::Get()
    {
        static RenderTask task;
        return task;
    }

    RenderTask::~RenderTask()
    {
        // A run that never reached Shutdown (an early exit) still stops the thread before it is destroyed: a
        // joinable std::thread destroyed is std::terminate.
        {
            const std::lock_guard<std::mutex> lock( m_Mutex );
            m_Stop = true;
        }
        m_Wake.notify_all();
        if ( m_Thread.joinable() )
            m_Thread.join();
    }

    bool RenderTask::OnRenderThread() noexcept
    {
        return t_OnRenderThread;
    }

    bool RenderTask::OnGameThread() const noexcept
    {
        return m_GameThread.load( std::memory_order_relaxed ) == std::this_thread::get_id();
    }

    void RenderTask::Report( const std::string& name, const Common::BoolResultStr& result )
    {
        // The job's failure, reported where it happened: it used to come back through Scene::OnUpdate to the
        // layer stack's "OnUpdate failed" line; that call has returned by the time the job runs.
        if ( !result.IsSuccess() )
            LOG_ERROR( "[RenderTask] {}: {}", name, result.GetError() );
    }

    void RenderTask::EnsureThread()
    {
        if ( m_Thread.joinable() )
            return;
        m_Stop   = false;
        m_Thread = Common::StartEngineThread( [this]() { ThreadMain(); } );
    }

    void RenderTask::ThreadMain()
    {
        t_OnRenderThread = true;
        std::unique_lock<std::mutex> lock( m_Mutex );
        for ( ;; )
        {
            {
                // THE RENDER THREAD WAITING FOR THE GAME THREAD (THREAD-OVERLAP-1): the time from the frame's last
                // job to the next one handed over - the game thread's frame head (events, BeginFrame, the world
                // tick, BeginScene) and tail (the interface graph, submit, present) that no view overlaps. The
                // mirror of "Render task: Join" (the game thread waiting for this one); the two together are the
                // frame's whole cross-thread wait. Closes when the next job arrives, so it is counted in the
                // window of the frame it waited for.
                DESERT_PROFILE_WAIT( "Render task: wait for game thread" );
                m_Wake.wait( lock, [this]() { return m_Stop || !m_Jobs.empty(); } );
            }
            // Stop drains first: a job enqueued before Shutdown still runs, so its renderer's bracket closes.
            if ( m_Jobs.empty() )
                return;
            QueuedJob job = std::move( m_Jobs.front() );
            m_Jobs.pop_front();
            m_Running      = true;
            m_RunningFrame = job.Frame;
            m_RunningOwner = job.Owner;
            lock.unlock();
            // THE JOB'S FRAME (FrameEndSync, from this side). The game thread may count the next frame already
            // (OVERLAP-ON-1: it hands frame N over and ticks N+1); what tears a frame is a job of frame N that
            // starts after N's submission (the queue is in order, and N's submit/present is N's last job), so its
            // command buffers miss that submit. Named once per job name; only this thread reads or writes the set.
            if ( m_SubmittedEnd.load( std::memory_order_acquire ) > job.Frame )
            {
                static std::unordered_set<std::string> torn;
                if ( torn.emplace( job.Name ).second )
                    LOG_ERROR( "[RenderTask] '{}' was enqueued in frame {} and started after that frame was "
                               "submitted: its command buffers missed the submit. A frame's submit/present is "
                               "its last job (Application::Run step 6).",
                               job.Name, job.Frame );
            }
            {
                DESERT_PROFILE_SCOPE( "Render task" );
                // The job records into ITS frame's slot and count, not the game thread's (FrameManager answers
                // job.Slot and job.Frame on this thread for the scope).
                const FrameManager::ScopedRecordingFrame recording( job.Slot, job.Frame );
                Report( job.Name, job.Body() );
            }
            // The job's captures go here, on this thread and before the drain is signalled: a job holds no owner
            // of a world or a renderer (their destructors join, RenderTask::Join), so nothing of the game
            // thread's is destroyed on this one.
            job = QueuedJob{};
            lock.lock();
            m_Running = false;
            if ( m_Jobs.empty() )
                m_InFlight.store( false, std::memory_order_release );
            // Every job: SyncPreviousFrame waits for one frame's jobs, not for the drain.
            m_Drained.notify_all();
        }
    }

    void RenderTask::Enqueue( std::string name, Job job )
    {
        if ( !Overlaps() || !OnGameThread() )
        {
            // Inline: the sequence every caller had before the render task, in serial mode the frame-compare
            // reference. A thread that is not the application's frame loop (a bake, a test) records inline too.
            Report( name, job() );
            return;
        }
        EnsureThread();
        {
            const std::lock_guard<std::mutex> lock( m_Mutex );
            m_InFlight.store( true, std::memory_order_release );
            const FrameManager& frames = FrameManager::GetInstance();
            m_LastEnqueuedFrame        = frames.GetGameFrameCount();
            m_Jobs.push_back( QueuedJob{ std::move( name ), std::move( job ), m_LastEnqueuedFrame,
                                         frames.GetGameFrameIndex(), m_EnqueueOwner } );
        }
        m_Wake.notify_one();
    }

    void RenderTask::WaitDrained()
    {
        if ( !InFlight() )
            return;
        DESERT_PROFILE_WAIT( "Render task: Join" );
        std::unique_lock<std::mutex> lock( m_Mutex );
        m_Drained.wait( lock, [this]() { return m_Jobs.empty() && !m_Running; } );
    }

    void RenderTask::Join()
    {
        if ( OnRenderThread() )
        {
            // A job waiting for the queue it is draining would never return. Nothing on the render thread joins:
            // a job holds no owner whose destructor would (see ThreadMain).
            LOG_ERROR( "[RenderTask] Join called from the render task itself; ignored (it would deadlock)" );
            return;
        }
        if ( m_Joining )
        {
            // A deferred command reached a fence (it recorded, or opened a bracket): what it waits for is work it
            // enqueued itself; the outer Join runs whatever it defers in turn.
            WaitDrained();
            return;
        }
        m_Joining = true;
        for ( ;; )
        {
            WaitDrained();
            if ( m_AfterJoin.empty() )
                break;
            std::vector<Deferred> commands;
            commands.swap( m_AfterJoin );
            for ( Deferred& command : commands )
                command.Body();
        }
        m_Joining = false;
    }

    void RenderTask::JoinOnGameThread()
    {
        if ( InFlight() && OnGameThread() )
            Join();
    }

    void RenderTask::CheckRenderStateWrite( const std::string_view site )
    {
        RenderTask& task = Get();
        if ( !task.OnGameThread() || !task.InFlight() )
            return;
        // Game thread only past this line, so the set needs no lock.
        static std::unordered_set<std::string> named;
        if ( named.emplace( site ).second )
            LOG_ERROR( "[RenderTask] '{}' wrote render-owned state on the game thread while a render job was in "
                       "flight: defer it (RenderTask::AfterJoin) or join first.",
                       site );
    }

    void RenderTask::FrameEndSync()
    {
        if ( !InFlight() || !OnGameThread() )
            return;
        // Game thread only past this line, so the flag needs no lock.
        static bool warned = false;
        if ( !warned )
        {
            warned = true;
            LOG_WARN( "[RenderTask] a frame began while the previous frame's render work was still recording: "
                      "joined it first (FrameEndSync). The frame's submissions and command buffer are per slot, "
                      "but the render graph's frame objects and the GPU profiler still bind one slot at a time." );
        }
        Join();
    }

    void RenderTask::AfterJoin( Command command )
    {
        if ( !InFlight() )
        {
            // THE ORDER OF THE DEFERRED COMMANDS IS THE ORDER THEY WERE ASKED FOR. The views can drain before
            // anyone joins them (InFlight clears on the render thread), and a command asked for then used to run
            // at once, ahead of the ones deferred earlier in the same frame that still wait for the next Join.
            // Those run first; inside a Join the outer loop runs this one after them.
            if ( !m_AfterJoin.empty() && !m_Joining )
                Join();
            if ( m_AfterJoin.empty() )
            {
                command();
                return;
            }
        }
        m_AfterJoin.push_back( Deferred{ std::move( command ), m_LastEnqueuedFrame } );
    }

    const void* RenderTask::FrameOwner() noexcept
    {
        static const char tag = 0;
        return &tag;
    }

    bool RenderTask::FramesBeforeDone( const uint64_t frame ) const
    {
        if ( m_Running && m_RunningFrame < frame )
            return false;
        // The queue is in frame order: Enqueue stamps the game thread's count, which only grows.
        return m_Jobs.empty() || m_Jobs.front().Frame >= frame;
    }

    void RenderTask::RunDueCommands( const uint64_t currentFrame )
    {
        // In order, and only those deferred behind a frame that has finished: a command queued behind this
        // frame's own work waits for it (the next Join or the next SyncPreviousFrame).
        while ( !m_AfterJoin.empty() && ( m_AfterJoin.front().Frame < currentFrame || !InFlight() ) )
        {
            Deferred command = std::move( m_AfterJoin.front() );
            m_AfterJoin.erase( m_AfterJoin.begin() );
            command.Body();
        }
    }

    void RenderTask::SyncPreviousFrame()
    {
        if ( OnRenderThread() || !OnGameThread() || m_Joining )
            return;
        const uint64_t current = FrameManager::GetInstance().GetGameFrameCount();
        if ( InFlight() )
        {
            DESERT_PROFILE_WAIT( "Render task: FrameEndSync (frame N-1)" );
            std::unique_lock<std::mutex> lock( m_Mutex );
            m_Drained.wait( lock, [this, current]() { return FramesBeforeDone( current ); } );
        }
        m_Joining = true;
        RunDueCommands( current );
        m_Joining = false;
    }

    void RenderTask::JoinUnlessOwnedBy( const void* owner )
    {
        if ( !InFlight() || !OnGameThread() )
            return;
        const auto foreign = [owner]( const void* jobOwner )
        { return jobOwner != owner && jobOwner != FrameOwner(); };
        bool mustJoin = false;
        {
            const std::lock_guard<std::mutex> lock( m_Mutex );
            mustJoin = m_Running && foreign( m_RunningOwner );
            for ( const QueuedJob& queued : m_Jobs )
                mustJoin = mustJoin || foreign( queued.Owner );
        }
        if ( mustJoin )
            Join();
    }

    void RenderTask::NoteFrameSubmitted( const uint64_t frame ) noexcept
    {
        uint64_t end = m_SubmittedEnd.load( std::memory_order_relaxed );
        while ( end < frame + 1 &&
                !m_SubmittedEnd.compare_exchange_weak( end, frame + 1, std::memory_order_acq_rel ) )
        {
        }
    }

    void RenderTask::RecordGuard( const std::string_view site )
    {
        RenderTask& task = Get();
        if ( !task.InFlight() || OnRenderThread() || !task.OnGameThread() )
            return;
        // Game thread only past this line, so the set needs no lock.
        static std::unordered_set<std::string> warned;
        if ( warned.emplace( site ).second )
            LOG_WARN(
                 "[RenderTask] '{}' recorded on the game thread while the scene views were recording: joined "
                 "them first, so the interface lost the overlap on this frame. Defer it (RenderTask::AfterJoin) "
                 "or move it before the views (OnPreUpdate).",
                 site );
        task.Join();
    }

    void RenderTask::SetOverlapped( const bool overlapped )
    {
        Join();
        if ( overlapped )
            m_GameThread.store( std::this_thread::get_id(), std::memory_order_relaxed );
        m_Overlapped.store( overlapped, std::memory_order_relaxed );
    }

    void RenderTask::SetSerial( const bool serial )
    {
        Join();
        m_Serial.store( serial, std::memory_order_relaxed );
        LOG_INFO( "[RenderTask] scene views record {}", serial ? "inline on the game thread (serial)"
                                                               : "on the render task, overlapping the interface" );
    }

    void RenderTask::Shutdown()
    {
        Join();
        m_Overlapped.store( false, std::memory_order_relaxed );
        {
            const std::lock_guard<std::mutex> lock( m_Mutex );
            m_Stop = true;
        }
        m_Wake.notify_all();
        if ( m_Thread.joinable() )
            m_Thread.join();
    }
} // namespace Desert::Engine
