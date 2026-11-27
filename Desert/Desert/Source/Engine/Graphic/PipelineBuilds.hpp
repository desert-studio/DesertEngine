#pragma once

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

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
    // One compile running now: what the splash names and how long it has run.
    struct InFlightBuild
    {
        std::string                           Name;
        std::chrono::steady_clock::time_point Begin;
        PipelineRole                          Role;
    };

    class Tracker
    {
    public:
        // A compile begins, on the thread that runs or submits it — sync (CompileOn::CallingThread) and async
        // alike, so every graphics-pipeline compile is counted and named. Returns the ticket OnFinished takes.
        [[nodiscard]] uint64_t OnStarted( PipelineRole role, std::string_view name );
        // The compile is done (built or failed): nothing of the pipeline's inputs is read any more.
        void OnFinished( uint64_t ticket );

        // Compiles of @p role still running.
        size_t Pending( PipelineRole role ) const;
        // Monotonic: compiles ever started. ContentGate's second condition ("the frame just rendered
        // started nothing new") needs a count that moves on START, not only a queue depth.
        uint64_t Started( PipelineRole role ) const;
        // Monotonic: compiles ever finished.
        uint64_t Finished( PipelineRole role ) const;
        // The oldest @p max compiles running now (all roles), oldest first.
        std::vector<InFlightBuild> InFlight( size_t max ) const;

        // Blocks until no compile is running. Shader hot-reload calls it before it replaces a shader's
        // modules, which a running compile is still reading.
        void WaitIdle();
        // Blocks until @p done is true; re-asked after every OnFinished (DeferredLighting's submit-then-wait).
        // MAIN THREAD ONLY: it does not help the pool, so a JobSystem worker waiting here is the
        // "body waits for an Async" cycle JobSystem.hpp forbids.
        void WaitUntil( const std::function<bool()>& done );

        // What a content pipeline request cost the CALLING thread (the frame): spec checks, layout, state
        // setup and the hand-off. The driver compile is not in it any more; this is the number that says so.
        void RecordCallerBlock( std::chrono::nanoseconds blocked );

        struct CallerCost
        {
            std::chrono::nanoseconds Total{};
            std::chrono::nanoseconds Max{};
        };
        CallerCost CallerBlocked() const;

    private:
        static constexpr size_t Index( PipelineRole role )
        {
            return static_cast<size_t>( role );
        }

        mutable std::mutex                    m_Mutex;
        std::condition_variable               m_Idle;
        std::chrono::nanoseconds              m_CallerBlocked{};
        std::chrono::nanoseconds              m_CallerBlockedMax{};
        std::array<size_t, 2>                 m_Pending{};
        std::array<uint64_t, 2>               m_Started{};
        std::array<uint64_t, 2>               m_Finished{};
        std::map<uint64_t, InFlightBuild>     m_InFlight;
        uint64_t                              m_NextTicket = 1;
    };

    Tracker& Get();
} // namespace Desert::Graphic::PipelineBuilds
