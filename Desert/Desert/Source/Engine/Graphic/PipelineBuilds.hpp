#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>

// Graphics pipelines whose driver compile is running on the JobSystem (PSO1; the pattern of UE 5.8
// FGraphicsPipelineStateInitializer precaching: the compile leaves the render thread, the draw waits for
// the pipeline instead of the frame waiting for the driver).
//
// ONE PROCESS-WIDE COUNT, because the question it answers is process-wide: "is anything the world asked
// for still being compiled?". The two hosts add it to what their ContentGate waits for, so the splash / the
// loading screen does not open on a frame that skips a mesh whose pipeline is still in the driver.
// Pure std (no device), so the rule is testable in `Desert/Tests/Engine/PipelineCacheFile`.
namespace Desert::Graphic::PipelineBuilds
{
    class Tracker
    {
    public:
        // A compile was handed to a worker. Called on the thread that submits it, BEFORE the submit.
        void OnStarted()
        {
            const std::lock_guard lock( m_Mutex );
            ++m_Pending;
            ++m_Started;
        }

        // The worker is done with it (built or failed): nothing of the pipeline's inputs is read any more.
        void OnFinished()
        {
            {
                const std::lock_guard lock( m_Mutex );
                --m_Pending;
            }
            m_Idle.notify_all();
        }

        // Compiles still running.
        size_t Pending() const
        {
            const std::lock_guard lock( m_Mutex );
            return m_Pending;
        }

        // Monotonic: compiles ever started. ContentGate's second condition ("the frame just rendered
        // started nothing new") needs a count that moves on START, not only a queue depth.
        uint64_t Started() const
        {
            const std::lock_guard lock( m_Mutex );
            return m_Started;
        }

        // Blocks until no compile is running. Shader hot-reload calls it before it replaces a shader's
        // modules, which a running compile is still reading.
        void WaitIdle()
        {
            std::unique_lock lock( m_Mutex );
            m_Idle.wait( lock, [this] { return m_Pending == 0; } );
        }

        // What a content pipeline request cost the CALLING thread (the frame): spec checks, layout, state
        // setup and the hand-off. The driver compile is not in it any more; this is the number that says so.
        void RecordCallerBlock( const std::chrono::nanoseconds blocked )
        {
            const std::lock_guard lock( m_Mutex );
            m_CallerBlocked += blocked;
            if ( blocked > m_CallerBlockedMax )
                m_CallerBlockedMax = blocked;
        }

        struct CallerCost
        {
            std::chrono::nanoseconds Total{};
            std::chrono::nanoseconds Max{};
        };
        CallerCost CallerBlocked() const
        {
            const std::lock_guard lock( m_Mutex );
            return { m_CallerBlocked, m_CallerBlockedMax };
        }

    private:
        mutable std::mutex       m_Mutex;
        std::chrono::nanoseconds m_CallerBlocked{};
        std::chrono::nanoseconds m_CallerBlockedMax{};
        std::condition_variable  m_Idle;
        size_t                   m_Pending = 0;
        uint64_t                 m_Started = 0;
    };

    inline Tracker& Get()
    {
        static Tracker tracker;
        return tracker;
    }
} // namespace Desert::Graphic::PipelineBuilds
