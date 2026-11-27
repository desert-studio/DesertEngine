#pragma once

// THE START'S PROGRESS, ONE OWNER (STARTUP-PROGRESS). The host makes it before the splash and the Application and
// hands it down: the Application and EditorStartup write it (phases, items), the splash reads it on its own thread
// (Sample) so the text moves while the main thread is blocked. Phases are weighted by the seconds they took on the
// previous start on this machine (the host's history file); a phase in progress is re-estimated from its measured
// rate; the shown fraction is monotonic and rises while work runs (see Sample). Thread-safe: one mutex.

#include <Common/Core/ResultStr.hpp>
#include <Engine/Core/WorkSink.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::Core
{
    using StartupClock = std::chrono::steady_clock;

    struct StartupPhaseSpec
    {
        std::string Key;              // "shaders.maps": the history file's key
        std::string Label;            // "Building shader maps": what the splash says
        double      DefaultSeconds{}; // the expected time with no history (the first start on a machine)
    };

    struct ActiveWork
    {
        std::string Name;
        double      Seconds = 0.0; // how long it has run
    };

    // What a pulled source says now. Done/Total 0/0 = uncounted.
    struct LiveCount
    {
        uint64_t                Done  = 0;
        uint64_t                Total = 0;
        std::vector<ActiveWork> Active;
    };
    // Called on the SPLASH thread under the progress's mutex: it must be thread-safe and must not call back.
    using LiveSource = std::function<LiveCount( StartupClock::time_point now )>;

    struct StartupLogLine
    {
        double      AtSeconds = 0.0; // since the process started
        std::string Text;
    };

    struct PhaseTiming
    {
        std::string Key;
        std::string Label;
        uint64_t    Items           = 0;
        double      Seconds         = 0.0;
        double      ExpectedSeconds = 0.0;
    };

    // One sample, for the splash.
    struct StartupView
    {
        std::string                 Phase; // the label; empty before the first phase
        size_t                      PhaseNumber = 0; // 1-based
        size_t                      PhaseCount  = 0;
        uint64_t                    Done        = 0;
        uint64_t                    Total       = 0; // 0 = uncounted
        std::vector<ActiveWork>     Active;          // oldest first, at most kActiveShown (wait reasons in scene.reveal)
        double                      Fraction         = 0.0; // 0..1, monotonic across samples, 1 only after Finish
        double                      ElapsedSeconds   = 0.0;
        double                      RemainingSeconds = 0.0;
        std::vector<StartupLogLine> Log; // newest last, at most kLogShown
        bool                        StopRequested = false;
        bool                        Finished      = false;
    };

    class StartupProgress final : public WorkSink
    {
    public:
        static constexpr size_t kActiveShown = 3;
        static constexpr size_t kLogShown    = 6;
        static constexpr double kNoteItemSeconds = 1.0; // a finished item at least this long goes into the log

        // @p plan is every phase in start order (the engine's then the host's); @p lastSeconds the history
        // (key -> seconds; a key absent takes its DefaultSeconds); @p processStart the process's start, so elapsed
        // includes what ran before this object.
        StartupProgress( std::vector<StartupPhaseSpec> plan, std::map<std::string, double, std::less<>> lastSeconds,
                         StartupClock::time_point processStart );

        // MAIN THREAD. Ends the current phase (its timing is logged "[Startup] phase <label>: N item(s) in S s
        // (expected E s)" and noted) and begins @p key. @p source, when set, is the phase's count (pulled per
        // sample) instead of the pushed items. A key not in the plan is refused, nothing changes.
        [[nodiscard]] Common::BoolResultStr BeginPhase( std::string_view key, LiveSource source = {} );
        // Active items shown in every phase (in-flight pipeline builds), never counted.
        void SetAmbient( LiveSource source );
        // MAIN THREAD, scene.reveal: what the reveal still waits for, shown as the active items. A change is logged
        // "[Startup] reveal waits for: a, b".
        void SetWaitReasons( std::vector<std::string> reasons );
        // ANY THREAD: one line into the detail log.
        void Note( std::string text );
        // MAIN THREAD, at the reveal: ends the last phase, fraction 1, logs the per-phase table and the total.
        void Finish();

        // WorkSink, ANY THREAD: into the current phase.
        void               Expect( uint64_t items ) noexcept override;
        [[nodiscard]] uint64_t BeginItem( std::string_view name ) noexcept override;
        void               FinishItem( uint64_t token ) noexcept override;
        [[nodiscard]] bool StopRequested() const noexcept override;
        // The splash's close button (its own thread).
        void RequestStop() noexcept;

        // SPLASH THREAD, per repaint. Advances the monotonic fraction (the only mutation), so one reader only.
        [[nodiscard]] StartupView Sample( StartupClock::time_point now );
        // After Finish: what the host writes into its history.
        [[nodiscard]] std::vector<PhaseTiming> Timings() const;

    private:
        struct Phase
        {
            StartupPhaseSpec         Spec;
            double                   Expected = 0.0;
            StartupClock::time_point Begin{};
            double                   Seconds = 0.0;
            uint64_t                 Done = 0, Total = 0;
            LiveSource               Source;
            bool                     Ended = false;
        };
        struct Running
        {
            std::string              Name;
            StartupClock::time_point Begin{};
        };
        [[nodiscard]] LiveCount CountOf( const Phase& phase, StartupClock::time_point now ) const;
        [[nodiscard]] double    RemainingOf( const Phase& phase, const LiveCount& count,
                                             StartupClock::time_point now ) const;
        void                    EndCurrent( StartupClock::time_point now );
        void                    NoteLocked( double at, std::string text );

        mutable std::mutex              m_Mutex;
        std::vector<Phase>              m_Phases;
        std::optional<size_t>           m_Current;
        std::map<uint64_t, Running>     m_Running; // token -> item, ordered = oldest first
        uint64_t                        m_NextToken = 1;
        LiveSource                      m_Ambient;
        std::vector<std::string>        m_WaitReasons;
        std::vector<StartupLogLine>     m_Log; // ring of kLogShown * 4
        StartupClock::time_point        m_ProcessStart;
        StartupClock::time_point        m_LastSample{};
        double                          m_Shown    = 0.0;
        bool                            m_Finished = false;
        std::atomic<bool>               m_Stop{ false };
    };

    // "0:42", "12:05".
    [[nodiscard]] std::string FormatDuration( double seconds );

    // The host's history file {"Version":1,"Phases":{key:seconds}}. A missing file = empty map (no error).
    [[nodiscard]] Common::ResultStr<std::map<std::string, double, std::less<>>>
                                        LoadStartupHistory( const std::filesystem::path& file );
    [[nodiscard]] Common::BoolResultStr SaveStartupHistory( const std::filesystem::path& file,
                                                            const std::vector<PhaseTiming>& timings );
} // namespace Desert::Core
