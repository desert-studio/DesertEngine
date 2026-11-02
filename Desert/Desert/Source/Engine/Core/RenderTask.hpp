#pragma once

#include <Common/Core/ResultStr.hpp>

#include <atomic>
#include <condition_variable>
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
     *    graphics API): a graph a panel records during its draw lands after the scene, not inside its segment.
     * After the wait it runs, in order, the commands the game thread deferred with AfterJoin().
     *
     * WHAT A JOB MAY READ. Only what the game thread handed it before Enqueue and does not write until the next
     * Join: the renderer's own per-frame copies (BeginScene snapshots the camera, the world parameters and the
     * replayed draws), never the registry, the live camera, the editor's selection or the OS. A pass that must
     * read the world reads it on the game thread (ExtensionPass::Gather).
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
        };

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
        bool                    m_Stop    = false;
        std::thread             m_Thread;

        std::atomic<bool>            m_InFlight{ false };
        std::atomic<bool>            m_Overlapped{ false };
        std::atomic<bool>            m_Serial{ false };
        std::atomic<std::thread::id> m_GameThread{};

        // Game thread only.
        std::vector<Command> m_AfterJoin;
        bool                 m_Joining = false;
    };
} // namespace Desert::Engine
