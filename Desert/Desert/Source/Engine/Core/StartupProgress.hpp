#pragma once

// THE START'S PROGRESS, ONE OWNER, ONE MODEL (SP-MODEL). The host makes it before the splash and the Application
// and hands it down: the main thread writes it (phases, items), the splash reads it on its own thread (Sample).
//
// THE MODEL, whole (nothing else decides what the splash shows):
//  1. PLAN. Every phase has a key, a label, a count kind and a planned time: the MEDIAN of its last
//     kHistoryRuns starts on this machine (the host's history file), else its DefaultSeconds.
//     PACE p of this run = the seconds of its finished phases / their planned seconds (1 before any finished).
//     A phase's expected time is planned * p: a warm start after cold ones is scaled down by its own first
//     phases, a cold one after warm ones up.
//  2. COUNT. Planned: n / N, N given once by Expect before the first item, never changed. Discovered: n only
//     (work found while it runs; no N is shown). Timed: no count.
//  3. PHASE REMAINDER r, the work left. Planned with n >= max(3, N/50): (N - n) * elapsed / n; before that
//     expected * (1 - n/N). Discovered with n >= 20: Queued * elapsed / n. Otherwise max(expected - elapsed, 0).
//     R = r + the expected time of every phase ahead. It reaches 0 when the last phase's work is done.
//  4. ETA (shown). Its target R' is R with the running phase's r raised to the reserve
//     max(kReserveSeconds, kReserveShare * elapsed), so the number never claims the end while work runs.
//     E starts at R' and follows it: dE/dt = -(1 + (E - R') / kEtaCatchUpSeconds), solved over the sample gap
//     in closed form on the assumption that R' falls in real time across it (one second per second), so an
//     over-estimate decays in a few kEtaCatchUpSeconds whatever its size and a long gap does not jump. E
//     falls at least at kEtaMinRate (a whole-second ETA changes at least every 4 s) and never below R': when
//     the plan rises, E rises with it. E >= R' >= kReserveSeconds while a phase runs, so 0:01 and 0:00 are
//     never shown while work remains.
//     OVERRUN: when the run outlives its plan (R = 0 and the running phase past its expected time) before
//     Finish, EtaKnown turns false for the rest of the start (the splash says "longer than expected" in place
//     of the number); it is never decided from the smoothed E.
//  5. BAR (shown). T = elapsed / (elapsed + R), 1 after Finish. Per sample B moves to T by at most kBarMaxStep
//     and by at least minRate * dt (minRate = kBarStepMargin * minVisibleStep / kBarMaxStallSeconds), so it
//     follows the estimate without lag, never jumps, never stalls; B <= 1 - minVisibleStep until Finish, after
//     which the same path carries it to 1 with the per-sample limit raised to close the gap left at Finish in
//     kBarFinishSeconds (the splash holds its fade out until the bar shows 1).
//  6. ITEM. While a phase runs Active is never empty: the waits, the running items oldest first, the ambient
//     work; when there is none, the phase itself (its label, its elapsed seconds).
// Thread-safe: one mutex.

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

    enum class PhaseCountKind : uint8_t
    {
        Timed,      // no count: the item line and the clock
        Planned,    // n / N, N from one Expect before the first item
        Discovered, // n only: the work is found while it runs
    };

    struct StartupPhaseSpec
    {
        std::string    Key;              // "shaders.maps": the history file's key
        std::string    Label;            // "Building shader maps": what the splash says
        double         DefaultSeconds{}; // the expected time with no history (a measured start, see the plan)
        PhaseCountKind Count = PhaseCountKind::Timed;
    };

    struct ActiveWork
    {
        std::string Name;          // stable for the life of the item
        std::string Detail;        // may change per sample ("357 in flight"); empty = none
        double      Seconds = 0.0; // how long it has run
    };

    // What a pulled source says now.
    struct LiveCount
    {
        uint64_t                Done   = 0;
        uint64_t                Queued = 0; // found and not done (Discovered only: the remainder the rate is applied to)
        std::vector<ActiveWork> Active;
    };
    // Called on the SPLASH thread under the progress mutex: it must be thread-safe and must not call back.
    using LiveSource = std::function<LiveCount( StartupClock::time_point now )>;

    // Something the running phase waits for. Its seconds run from the first SetWaits that named Name.
    struct StartupWait
    {
        std::string Name;   // stable: "textures streaming"
        std::string Detail; // changing: "357 in flight"
    };

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

    // One sample, for the splash. Everything shown comes from here; the splash derives nothing.
    struct StartupView
    {
        std::string                 Phase; // the label; before the first BeginPhase the plan's first phase
        size_t                      PhaseNumber = 0; // 1-based
        size_t                      PhaseCount  = 0;
        PhaseCountKind              Count       = PhaseCountKind::Timed;
        uint64_t                    Done        = 0; // Planned, Discovered
        uint64_t                    Total       = 0; // Planned only, fixed for the phase; 0 until Expect
        std::vector<ActiveWork>     Active;          // 1..kActiveShown while a phase runs (rule 6)
        double                      Fraction       = 0.0; // rule 5; 1 only after Finish
        double                      ElapsedSeconds = 0.0;
        double                      EtaSeconds     = 0.0; // rule 4; meaningful while EtaKnown
        bool                        EtaKnown       = true;
        std::vector<StartupLogLine> Log; // newest last, at most kLogShown
        bool                        StopRequested = false;
        bool                        Finished      = false;
    };

    // key -> the phase seconds on the last starts, oldest first, at most StartupProgress::kHistoryRuns.
    using StartupHistory = std::map<std::string, std::vector<double>, std::less<>>;

    class StartupProgress final : public WorkSink
    {
    public:
        static constexpr size_t kActiveShown        = 3;
        static constexpr size_t kLogShown           = 6;
        static constexpr size_t kHistoryRuns        = 5;
        static constexpr double kNoteItemSeconds    = 1.0;  // a finished item at least this long goes into the log
        static constexpr double kReserveSeconds     = 2.0;  // rule 4
        static constexpr double kReserveShare       = 0.15; // rule 4
        static constexpr double kEtaCatchUpSeconds  = 5.0;  // rule 4
        static constexpr double kEtaMinRate         = 0.25; // s of ETA per s: a whole second at least every 4 s
        static constexpr double kEtaMaxRise         = 2.0;  // s of ETA a rising plan adds per sample (rule 4)
        static constexpr double kBarMaxStep         = 0.004; // of the bar per sample (rule 5): under a 0.5 % step
        static constexpr double kBarMaxStallSeconds = 2.0;
        static constexpr double kBarFinishSeconds   = 0.3; // rule 5: after Finish the bar's gap closes in this
        static constexpr double kBarStepMargin      = 1.25;

        // @p plan is every phase in start order (the engine's then the host's); @p history the previous starts
        // (a key absent takes its DefaultSeconds); @p processStart the process start.
        StartupProgress( std::vector<StartupPhaseSpec> plan, StartupHistory history,
                         StartupClock::time_point processStart );

        // MAIN THREAD. Ends the current phase (logged "[Startup] phase <label>: N item(s) in S s (expected E s)"
        // and noted) and begins @p key. @p source, when set, is the phase count (pulled per sample) instead of
        // the pushed items. A key not in the plan, or one the start has passed, is refused; nothing changes.
        [[nodiscard]] Common::BoolResultStr BeginPhase( std::string_view key, LiveSource source = {} );
        // Active items shown in every phase (in-flight pipeline builds), never counted.
        void SetAmbient( LiveSource source );
        // MAIN THREAD: what the running phase waits for now (empty = nothing). Shown as active items aged from
        // the first call that named them. A change of the NAMES is logged "[Startup] waits for: a, b"; a change
        // of a Detail alone is not logged. Cleared when the phase ends.
        void SetWaits( std::vector<StartupWait> waits );
        // ANY THREAD: one line into the detail log.
        void Note( std::string text );
        // MAIN THREAD, at the reveal: ends the last phase, fraction 1, logs the per-phase table and the total.
        void Finish();

        // WorkSink. BeginStep = BeginPhase( key ) with the refusal logged. Expect: once per phase, before its
        // first item, Planned phases only; otherwise refused and logged once per phase.
        void                   BeginStep( std::string_view key ) noexcept override;
        void                   Expect( uint64_t items ) noexcept override;
        [[nodiscard]] uint64_t BeginItem( std::string_view name ) noexcept override;
        void                   FinishItem( uint64_t token ) noexcept override;
        [[nodiscard]] bool     StopRequested() const noexcept override;
        // The splash close button (its own thread).
        void RequestStop() noexcept;

        // SPLASH THREAD, per repaint; advances the shown bar and ETA, so one reader only. @p minVisibleStep is
        // the smallest change of Fraction the reader can show (1 / the bar width in device pixels), in (0, 0.1].
        [[nodiscard]] StartupView Sample( StartupClock::time_point now, double minVisibleStep );
        // After Finish: what the host writes into its history.
        [[nodiscard]] std::vector<PhaseTiming> Timings() const;

    private:
        struct Phase
        {
            StartupPhaseSpec         Spec;
            double                   Expected = 0.0; // planned (rule 1); the run's expectation is Expected * pace
            StartupClock::time_point Begin{};
            double                   Seconds = 0.0;
            uint64_t                 Done = 0, Total = 0;
            bool                     TotalGiven  = false; // Expect was taken
            bool                     ItemBegun   = false;
            bool                     RefusalSaid = false;
            LiveSource               Source;
            bool                     Ended = false;
        };
        struct Running
        {
            std::string              Name;
            StartupClock::time_point Begin{};
        };
        struct Waiting
        {
            StartupWait              Wait;
            StartupClock::time_point Since{};
        };
        [[nodiscard]] LiveCount CountOf( const Phase& phase, StartupClock::time_point now ) const;
        [[nodiscard]] double    PaceOf() const; // rule 1
        [[nodiscard]] double    RemainingOf( const Phase& phase, const LiveCount& count, double elapsed,
                                             double pace ) const;
        void                    EndCurrent( StartupClock::time_point now );
        void                    NoteLocked( double at, std::string text );

        mutable std::mutex          m_Mutex;
        std::vector<Phase>          m_Phases;
        std::optional<size_t>       m_Current;
        std::map<uint64_t, Running> m_Running; // token -> item, ordered = oldest first
        uint64_t                    m_NextToken = 1;
        LiveSource                  m_Ambient;
        std::vector<Waiting>        m_Waits;
        std::vector<StartupLogLine> m_Log; // ring of kLogShown * 4
        StartupClock::time_point    m_ProcessStart;
        StartupClock::time_point    m_LastSample{};
        double                      m_ShownFraction = 0.0;
        double                      m_FinishGap     = 0.0; // 1 - the shown bar at Finish (rule 5)
        double                      m_ShownEta      = 0.0;
        bool                        m_EtaKnown      = true;
        bool                        m_Finished      = false;
        std::atomic<bool>           m_Stop{ false };
    };

    // "0:42", "12:05".
    [[nodiscard]] std::string FormatDuration( double seconds );

    // The host history file, version 2: {"Version":2,"Phases":{key:[seconds,...]}}. A missing file = empty.
    // A version 1 file ({key:seconds}) is read as one run per key; the next save writes version 2.
    [[nodiscard]] Common::ResultStr<StartupHistory> LoadStartupHistory( const std::filesystem::path& file );
    // Appends @p timings to @p previous (keeping the newest kHistoryRuns per key) and writes the file.
    [[nodiscard]] Common::BoolResultStr SaveStartupHistory( const std::filesystem::path&    file,
                                                            const StartupHistory&           previous,
                                                            const std::vector<PhaseTiming>& timings );
} // namespace Desert::Core
