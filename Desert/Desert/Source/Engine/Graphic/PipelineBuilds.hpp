#pragma once

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>

// Graphics pipelines whose driver compile is running on the JobSystem (PSO1; the pattern of UE 5.8
// FGraphicsPipelineStateInitializer precaching: the compile leaves the render thread, the draw waits for
// the pipeline instead of the frame waiting for the driver).
//
// ONE PROCESS-WIDE COUNT PER ROLE, because the question it answers is process-wide: "is anything the world
// asked for still being compiled?". The two hosts add the ENGINE count to what their ContentGate waits for, so
// the splash / the loading screen does not open on a frame that skips a system pass whose pipeline is still in
// the driver. MATERIAL pipelines are not waited for (AL1-12, UE's PSO precache): a draw whose material pipeline
// is still compiling is drawn with the engine's default surface, so nothing is missing from the revealed frame
// and the window no longer waits seconds for the driver on a cold pipeline cache.
// Pure std (no device), so the rule is testable in `Desert/Tests/Engine/PipelineCacheFile`.
namespace Desert::Graphic
{
    // Who asked for a pipeline, which decides whether the reveal waits for it.
    enum class PipelineRole : uint8_t
    {
        Engine,   ///< a system pass (G-buffer, shadows, the default surface): the reveal waits for it
        Material, ///< a content material's own shader: compiled on demand, drawn with the default until ready
    };
} // namespace Desert::Graphic

namespace Desert::Graphic::PipelineBuilds
{
    class Tracker
    {
    public:
        // A compile was handed to a worker. Called on the thread that submits it, BEFORE the submit.
        void OnStarted( PipelineRole role )
        {
            const std::lock_guard lock( m_Mutex );
            ++m_Pending[Index( role )];
            ++m_Started[Index( role )];
        }

        // The worker is done with it (built or failed): nothing of the pipeline's inputs is read any more.
        void OnFinished( PipelineRole role )
        {
            {
                const std::lock_guard lock( m_Mutex );
                --m_Pending[Index( role )];
            }
            m_Idle.notify_all();
        }

        // Compiles of @p role still running.
        size_t Pending( PipelineRole role ) const
        {
            const std::lock_guard lock( m_Mutex );
            return m_Pending[Index( role )];
        }

        // Monotonic: compiles ever started. ContentGate's second condition ("the frame just rendered
        // started nothing new") needs a count that moves on START, not only a queue depth.
        uint64_t Started( PipelineRole role ) const
        {
            const std::lock_guard lock( m_Mutex );
            return m_Started[Index( role )];
        }

        // Blocks until no compile is running. Shader hot-reload calls it before it replaces a shader's
        // modules, which a running compile is still reading.
        void WaitIdle()
        {
            std::unique_lock lock( m_Mutex );
            m_Idle.wait( lock, [this] { return m_Pending[0] == 0 && m_Pending[1] == 0; } );
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
        static constexpr size_t  Index( PipelineRole role )
        {
            return static_cast<size_t>( role );
        }
        std::array<size_t, 2>   m_Pending{};
        std::array<uint64_t, 2> m_Started{};
    };

    inline Tracker& Get()
    {
        static Tracker tracker;
        return tracker;
    }
} // namespace Desert::Graphic::PipelineBuilds
