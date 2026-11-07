#pragma once

#include <Common/Core/ResultStr.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace Desert::Engine
{
    /**
     * @brief THE RENDER TASK: the scene views of a frame record on one long-lived thread while the game thread
     * builds the interface (RENDER-TASK-1, step 2a of the Wiki contract RENDER-THREADS).
     *
     * UE: the game thread enqueues the scene render (BeginRenderingViewFamily) and goes on to tick and paint
     * Slate while the rendering thread records the scene; one FRenderCommandFence joins them before the
     * interface's draw buffer is recorded after the scene. With r.OneFrameThreadLag=0 nothing lags a frame, and
     * neither does this: a job enqueued in frame N finishes in frame N.
     *
     * THE ORDER. Jobs run one at a time, in the order they were enqueued, on ONE thread, so the frame's command
     * buffers come out in the same order as the old inline path (RendererAPI::ExecuteGraph appends each graph's
     * submissions to the frame's list in call order).
     *
     * THE FENCE. Join() is the one place the game thread waits for the render task. It is called
     *  - before the interface graph is recorded (VulkanImGui::End): the interface samples the scene's image;
     *  - before the frame is submitted (Application::Run, PresentFinalImage), for an application without one;
     *  - before the game thread opens another view's bracket (SceneRenderer::BeginScene), ticks another world
     *    (Scene::OnUpdate) or destroys a world or a renderer: two brackets are never open at once, and nothing
     *    a job reads is freed under it;
     *  - before the game thread records anything itself (RecordGuard, from every recording entry point of the
     *    graphics API): a graph a panel records during its draw lands after the scene, not inside its segment;
     *  - at the frame boundary, with one frame of lag (SyncPreviousFrame, Application::Run step 4b): frame N-1's
     *    jobs - its begin, views, interface, submit and present - are done before frame N's interface is
     *    painted; and before a swapchain rebuild, only when one is pending (step 2, SwapChain::RebuildPending).
     * After the wait it runs, in order, the commands the game thread deferred with AfterJoin().
     *
     * WHAT A JOB MAY READ. Only what the game thread handed it before Enqueue and does not write until the next
     * Join: the renderer's own per-frame copies (BeginScene snapshots the camera and the world parameters; the
     * view's job replays the recorded draws read-only - their intake ran at Scene::OnUpdate's fence,
     * RenderCommand::Prepare), never the registry, the live camera, the editor's selection
     * or the OS. A pass that must read the world reads it on the game thread (ExtensionPass::Gather).
     *
     * SERIAL. Off by default — a test, a tool or a bake that drives Scene::OnUpdate itself gets the inline
     * sequence it always had — and the application switches it on (Application::Run). SetSerial(true) runs every
     * job inline on the caller, the frame-compare switch (palette Debug."Scene record: serial").
     */
    class RenderTask
    {
    public:
        using Job     = std::function<Common::BoolResultStr()>;
        using Command = std::function<void()>;

        static RenderTask& Get();

        RenderTask( const RenderTask& )            = delete;
        RenderTask& operator=( const RenderTask& ) = delete;
        ~RenderTask();

        // GAME THREAD. Appends a job to the frame's render work and wakes the render thread; in serial mode runs
        // it now. A failure is logged here with @p name (the job's own report, one fence later than before).
        void Enqueue( std::string name, Job job );

        // GAME THREAD. THE FENCE: returns when every job enqueued so far has finished and every deferred command
        // has run (a command may enqueue work again; Join waits for that too). No-op while nothing is in flight.
        void Join();

        // GAME THREAD. Runs @p command at the next Join, after the render work in flight, or now when nothing is
        // in flight: what the interface changes in the renderer while the views record (a LOD preview lease, a
        // preview render) waits for them instead of racing them.
        void AfterJoin( Command command );

        // GAME THREAD. Enqueue for a job that holds a VIEW'S BRACKET (Scene::OnUpdate's "Scene view": a renderer's
        // replay, OnUpdate and EndScene). What JoinViews waits for; every other job is the frame's own work.
        void EnqueueView( std::string name, Job job );

        // GAME THREAD. AfterJoin for a command whose fence is the views alone (CPU-GT-OVERLAP): what it writes is
        // read by view jobs and by nothing else on the render task - not the interface's draw, not the frame's
        // begin, submit or present. It runs at the next JoinViews (or Join), in order with every other command.
        void AfterViews( Command command );

        // GAME THREAD. THE BRACKET'S FENCE (SceneRenderer::BeginScene; UE: the game thread opens view N+1's
        // family while the rendering thread still submits N). Returns when no view job is queued or running and
        // the deferred commands have run - and only when every deferred command is an AfterViews one; any other
        // makes it a Join. The frame's own jobs in flight (frame N-1's interface draw, census, submit and
        // present; frame N's begin; the surface cache's frame statement) keep running under the caller. Nested
        // in a fence (a capture face opened by a deferred command), off the game thread, or with nothing in
        // flight, it is Join.
        void JoinViews();

        // THE FENCE FOR A MUTATION OF WHAT A JOB READS, from code that runs on more than one thread (a shared view
        // resource forgotten, an extension pass registered, a view resized): joins when the caller is the game
        // thread and a job is in flight, and returns at once anywhere else. The render task changing what it
        // reads itself is sequential with its own jobs, and a worker never fences (the render task may be
        // waiting on that worker: a join there would deadlock).
        void JoinOnGameThread();

        // THE OWNERSHIP CHECK (CPU-ONE-SYNC). Render-owned state - what a job in flight may read (a material's
        // pools, a retire queue, a surface cache) - is written on the game thread only while no job is in flight
        // (inside a deferred command, AfterJoin, or after a Join) or on the render task itself. A game-thread
        // write that breaks it is named once per @p site (an error, so a scenario's log check goes red).
        static void CheckRenderStateWrite( std::string_view site );

        // GAME THREAD, THE FRAME BOUNDARY'S FENCE (UE FFrameEndSync): the first statement of
        // Renderer::PresentFinalImage (frame N's submission takes its slot's list as it is, so N's jobs have
        // recorded), and VulkanRendererAPI::BeginFrame right after the slot's GPU wait (frame N+1 binds the
        // render graph's frame objects - frame loop queue objects, transient heaps, pass descriptors, backend -
        // and the GPU profiler, which hold ONE slot at a time). The frame's own state - its submission list, its
        // recording command buffer - is per slot (VulkanRendererAPI::FrameSlotState), and a job records into the
        // slot it was enqueued for (QueuedJob::Slot, FrameManager::ScopedRecordingFrame), so the game thread
        // moving to the next slot (NextFrame) no longer tears it. On the render task, where the frame's begin and
        // its submit run as jobs (OVERLAP-ON-1), it returns at once; the game thread's fence, with one frame of
        // lag, is SyncPreviousFrame (Application::Run step 4b). ThreadMain checks the rule from the other side: a
        // job of frame F that starts after F was submitted is an error.
        void FrameEndSync();
        // GAME THREAD. THE FRAME BOUNDARY WITH ONE FRAME OF LAG (UE FFrameEndSync, bAllowOneFrameThreadLag):
        // returns when every job of a frame BEFORE the game thread's current one (GetGameFrameCount) has
        // finished - frame N-1's views, interface, submit and present - while frame N's own jobs (its frame begin)
        // keep running, then runs the commands deferred behind that work (AfterJoin) in order. Application::Run
        // calls it once per iteration after the layers' update, so the head and the tick of frame N overlap frame
        // N-1's render work. No-op off the game thread.
        void SyncPreviousFrame();
        // GAME THREAD. Scene::OnUpdate's fence for ANOTHER world's views: joins unless every job in flight was
        // enqueued under @p owner (ScopedOwner) or is the frame's own work (FrameOwner). This world's views of the
        // previous frame keep recording under its tick; a job with no owner is treated as foreign.
        void JoinUnlessOwnedBy( const void* owner );
        // ANY THREAD. Frame @p frame was handed to the queue (Renderer::PresentFinalImage). A job of that frame
        // that starts later missed its submission, and ThreadMain names it.
        void NoteFrameSubmitted( uint64_t frame ) noexcept;
        // Whether frame @p frame has been submitted (its fence may be waited: Application::Run step 3).
        [[nodiscard]] bool FrameSubmitted( uint64_t frame ) const noexcept
        {
            return m_SubmittedEnd.load( std::memory_order_acquire ) > frame;
        }
        // GAME THREAD. Every job enqueued in the scope is stamped with @p owner (the world, Scene::OnUpdate; the
        // frame's own work, FrameOwner). Scopes nest.
        class ScopedOwner
        {
        public:
            explicit ScopedOwner( const void* owner ) : m_Previous( Get().m_EnqueueOwner )
            {
                Get().m_EnqueueOwner = owner;
            }
            ~ScopedOwner()
            {
                Get().m_EnqueueOwner = m_Previous;
            }
            ScopedOwner( const ScopedOwner& )            = delete;
            ScopedOwner& operator=( const ScopedOwner& ) = delete;

        private:
            const void* m_Previous;
        };
        // The owner of the frame's own work: its begin, its interface, its submit and present.
        [[nodiscard]] static const void* FrameOwner() noexcept;

        // True from an Enqueue until the job queue drains (the render thread clears it).
        [[nodiscard]] bool InFlight() const noexcept
        {
            return m_InFlight.load( std::memory_order_acquire );
        }

        // True on the render task's own thread.
        [[nodiscard]] static bool OnRenderThread() noexcept;

        // Every recording entry point of the graphics API calls this (VulkanRendererAPI::IsRecording,
        // Renderer::ExecuteGraph). On the game thread while the views record it joins first and says so once per
        // @p site; on the render thread and on any worker it returns at once.
        static void RecordGuard( std::string_view site );

        // The application's switch: false (the default) keeps the inline sequence for everything that is not
        // the application's frame loop.
        void SetOverlapped( bool overlapped );
        // The frame-compare switch: true runs every job inline even in the application.
        void               SetSerial( bool serial );
        [[nodiscard]] bool IsSerial() const noexcept
        {
            return m_Serial.load( std::memory_order_relaxed );
        }
        // Whether Enqueue runs its job on the render thread (overlapped and not serial).
        [[nodiscard]] bool Overlaps() const noexcept
        {
            return m_Overlapped.load( std::memory_order_relaxed ) && !IsSerial();
        }

        // GAME THREAD, at the end of the run: joins, stops and joins the thread.
        void Shutdown();

    private:
        RenderTask() = default;

        struct QueuedJob
        {
            std::string Name;
            Job         Body;
            // FrameManager::GetAbsoluteFrameCount at Enqueue: the frame whose slot the job records into.
            uint64_t Frame = 0;
            // FrameManager::GetGameFrameIndex at Enqueue: that slot. ThreadMain binds it as the thread's recording
            // frame (FrameManager::ScopedRecordingFrame), so the job reads and writes its own slot's frame state
            // even after the game thread has submitted its frame and moved on.
            uint32_t Slot = 0;
            // ScopedOwner at Enqueue (JoinUnlessOwnedBy).
            const void* Owner = nullptr;
            // EnqueueView: the job holds a view's bracket (m_ViewsInFlight, JoinViews).
            bool View = false;
        };
        // A command deferred to the next Join, with the frame of the last job enqueued before it:
        // SyncPreviousFrame runs it once that frame's work has finished.
        struct Deferred
        {
            Command  Body;
            uint64_t Frame = 0;
            // AfterViews: the views are its whole fence (JoinViews may run it with frame jobs in flight).
            bool ViewsOnly = false;
        };
        void Push( std::string name, Job job, bool view );
        void Defer( Command command, bool viewsOnly );
        // Game thread: every deferred command is an AfterViews one.
        [[nodiscard]] bool OnlyViewCommandsDeferred() const noexcept;
        // Under m_Mutex: every job of a frame before @p frame has finished.
        [[nodiscard]] bool FramesBeforeDone( uint64_t frame ) const;
        void               RunDueCommands( uint64_t currentFrame );

        void               ThreadMain();
        void               EnsureThread();
        static void        Report( const std::string& name, const Common::BoolResultStr& result );
        [[nodiscard]] bool OnGameThread() const noexcept;
        void               WaitDrained();

        std::mutex              m_Mutex;
        std::condition_variable m_Wake;    // render thread: a job arrived or stop
        std::condition_variable m_Drained; // game thread: the queue drained
        std::deque<QueuedJob>   m_Jobs;
        bool                    m_Running = false; // a job is executing (under m_Mutex)
        uint32_t                m_ViewsInFlight = 0;     // EnqueueView jobs queued or running (under m_Mutex)
        bool                    m_Stop    = false;
        std::thread             m_Thread;

        std::atomic<bool>            m_InFlight{ false };
        std::atomic<bool>            m_Overlapped{ false };
        std::atomic<bool>            m_Serial{ false };
        std::atomic<std::thread::id> m_GameThread{};

        // Game thread only.
        std::vector<Deferred> m_AfterJoin;
        bool                  m_Joining           = false;
        // JoinViews is running AfterViews commands (CheckRenderStateWrite: the views are drained).
        bool m_InViewFence = false;
        const void*           m_EnqueueOwner      = nullptr;
        uint64_t              m_LastEnqueuedFrame = 0;
        // Under m_Mutex: the running job's frame and owner.
        uint64_t    m_RunningFrame = 0;
        const void* m_RunningOwner = nullptr;
        // One past the last frame submitted (NoteFrameSubmitted); 0: none yet.
        std::atomic<uint64_t> m_SubmittedEnd{ 0 };
    };
} // namespace Desert::Engine
