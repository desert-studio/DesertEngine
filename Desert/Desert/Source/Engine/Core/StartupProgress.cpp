#include <Engine/Core/StartupProgress.hpp>

#include <Common/Core/Core.hpp>
#include <Common/Core/Logger.hpp>
#include <Common/Json/ReadWrite.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <utility>

namespace Desert::Core
{
    namespace
    {
        [[nodiscard]] double SecondsBetween( const StartupClock::time_point from,
                                             const StartupClock::time_point to )
        {
            return std::chrono::duration<double>( to - from ).count();
        }

        // The history file. A new field is a new version and a migration of the files (there is one per
        // project per machine, regenerated at every reveal).
        struct StartupHistoryFile
        {
            int                           Version = 1;
            std::map<std::string, double> Phases;
        };
        constexpr int kHistoryVersion = 1;

        // The floor of the current phase's estimate: an ETA of 0 while work runs would be a lie, and it would
        // let the bar reach 1 before Finish.
        constexpr double kRemainingFloorSeconds = 0.5;
        // A phase's weight in the bar is its expected seconds, at least this: a phase that took no time on the
        // last start is still a step of the plan.
        constexpr double kMinPhaseWeight = 0.01;

        // An uncounted phase's seconds left after @p elapsed of its @p expected: linear down to its knee (half of
        // it), then the hyperbola knee^2 / elapsed -- equal value and slope at the knee, a quarter of the
        // expectation at the expectation, and still falling through any overrun, so it never stops while work
        // runs and never reaches 0 (the floor).
        [[nodiscard]] double ExpectedLeft( const double expected, const double elapsed )
        {
            const double knee     = 0.5 * expected;
            const double estimate = elapsed <= knee ? expected - elapsed : knee * knee / elapsed;
            return std::max( estimate, kRemainingFloorSeconds );
        }
    } // namespace

    StartupProgress::StartupProgress( std::vector<StartupPhaseSpec>              plan,
                                      std::map<std::string, double, std::less<>> lastSeconds,
                                      const StartupClock::time_point             processStart )
         : m_ProcessStart( processStart )
    {
        m_Phases.reserve( plan.size() );
        for ( StartupPhaseSpec& spec : plan )
        {
            Phase      phase;
            const auto last = lastSeconds.find( spec.Key );
            phase.Expected  = last != lastSeconds.end() ? last->second : spec.DefaultSeconds;
            phase.Spec      = std::move( spec );
            m_Phases.push_back( std::move( phase ) );
        }
    }

    Common::BoolResultStr StartupProgress::BeginPhase( const std::string_view key, LiveSource source )
    {
        const std::lock_guard lock( m_Mutex );
        const auto            found = std::find_if( m_Phases.begin(), m_Phases.end(),
                                                    [key]( const Phase& phase ) { return phase.Spec.Key == key; } );
        if ( found == m_Phases.end() )
            return Common::MakeError( std::format( "phase '{}' is not in the start's plan", key ) );
        const size_t index = static_cast<size_t>( found - m_Phases.begin() );
        if ( m_Finished || ( m_Current && index <= *m_Current ) || found->Ended )
            return Common::MakeError( std::format( "phase '{}' begins after the start has passed it", key ) );

        const StartupClock::time_point now = StartupClock::now();
        EndCurrent( now );
        // A phase of the plan this start did not run (the host skipped it) took no time: ended, at zero, so it
        // stops counting into the time left.
        for ( size_t skipped = m_Current ? *m_Current + 1 : 0; skipped < index; ++skipped )
            if ( !m_Phases[skipped].Ended )
                m_Phases[skipped].Ended = true;
        Phase& phase = *found;
        phase.Begin  = now;
        phase.Source = std::move( source );
        m_Current    = index;
        return BOOLSUCCESS;
    }

    void StartupProgress::SetAmbient( LiveSource source )
    {
        const std::lock_guard lock( m_Mutex );
        m_Ambient = std::move( source );
    }

    void StartupProgress::SetWaitReasons( std::vector<std::string> reasons )
    {
        const std::lock_guard lock( m_Mutex );
        if ( reasons == m_WaitReasons )
            return;
        std::string joined;
        for ( const std::string& reason : reasons )
            joined += joined.empty() ? reason : ", " + reason;
        LOG_INFO( "[Startup] reveal waits for: {}", joined.empty() ? std::string( "nothing" ) : joined );
        m_WaitReasons = std::move( reasons );
    }

    void StartupProgress::Note( std::string text )
    {
        const std::lock_guard lock( m_Mutex );
        NoteLocked( SecondsBetween( m_ProcessStart, StartupClock::now() ), std::move( text ) );
    }

    void StartupProgress::Finish()
    {
        const std::lock_guard lock( m_Mutex );
        if ( m_Finished )
            return;
        const StartupClock::time_point now = StartupClock::now();
        EndCurrent( now );
        for ( Phase& phase : m_Phases )
            phase.Ended = true;
        m_Current.reset();
        m_WaitReasons.clear();
        m_Finished = true;
        m_Shown    = 1.0;

        // THE START'S TABLE: every phase that ran, then the total. The phases are back to back from the first
        // one's begin, so they sum to the wall time after it; what ran before the first phase is the process's
        // own start (images, the splash), said apart.
        double                                  phases = 0.0;
        std::optional<StartupClock::time_point> first;
        for ( const Phase& phase : m_Phases )
        {
            if ( phase.Begin == StartupClock::time_point{} )
                continue;
            if ( !first )
                first = phase.Begin;
            phases += phase.Seconds;
            LOG_INFO( "[Startup] {}: {} item(s) in {:.2f} s (expected {:.2f} s)", phase.Spec.Label, phase.Done,
                      phase.Seconds, phase.Expected );
        }
        const double total = SecondsBetween( m_ProcessStart, now );
        LOG_INFO( "[Startup] boot {:.2f} s: phases {:.2f} s, before the first phase {:.2f} s", total, phases,
                  first ? SecondsBetween( m_ProcessStart, *first ) : total );
    }

    void StartupProgress::Expect( const uint64_t items ) noexcept
    {
        const std::lock_guard lock( m_Mutex );
        if ( m_Current )
            m_Phases[*m_Current].Total += items;
    }

    uint64_t StartupProgress::BeginItem( const std::string_view name ) noexcept
    {
        const std::lock_guard lock( m_Mutex );
        const uint64_t        token = m_NextToken++;
        m_Running.emplace( token, Running{ .Name = std::string( name ), .Begin = StartupClock::now() } );
        return token;
    }

    void StartupProgress::FinishItem( const uint64_t token ) noexcept
    {
        const std::lock_guard lock( m_Mutex );
        const auto            found = m_Running.find( token );
        if ( found == m_Running.end() )
            return;
        const StartupClock::time_point now     = StartupClock::now();
        const double                   seconds = SecondsBetween( found->second.Begin, now );
        if ( m_Current )
            ++m_Phases[*m_Current].Done;
        if ( seconds >= kNoteItemSeconds )
            NoteLocked( SecondsBetween( m_ProcessStart, now ),
                        std::format( "{}  {:.1f} s", found->second.Name, seconds ) );
        m_Running.erase( found );
    }

    bool StartupProgress::StopRequested() const noexcept
    {
        return m_Stop.load( std::memory_order_acquire );
    }

    void StartupProgress::RequestStop() noexcept
    {
        m_Stop.store( true, std::memory_order_release );
    }

    StartupView StartupProgress::Sample( const StartupClock::time_point now )
    {
        const std::lock_guard lock( m_Mutex );
        StartupView           view;
        view.PhaseCount      = m_Phases.size();
        view.ElapsedSeconds  = SecondsBetween( m_ProcessStart, now );
        view.StopRequested   = StopRequested();
        view.Finished        = m_Finished;
        const size_t logFrom = m_Log.size() > kLogShown ? m_Log.size() - kLogShown : 0;
        view.Log.assign( m_Log.begin() + static_cast<std::ptrdiff_t>( logFrom ), m_Log.end() );
        if ( m_Finished )
        {
            view.Fraction = 1.0;
            m_LastSample  = now;
            return view;
        }

        // The running phase: the current one, or before the first BeginPhase the plan's first phase still to run.
        std::optional<size_t> running;
        double                phaseElapsed = 0.0;
        double                phaseLeft    = 0.0;
        if ( !m_Current )
        {
            // Before the first BeginPhase the process is already doing the plan's first phase's work (the
            // window, the instance; the engine always runs it): the splash names it from its first frame and
            // estimates it as running since the process's start, nothing counted yet.
            const auto next = std::find_if( m_Phases.begin(), m_Phases.end(),
                                            []( const Phase& phase ) { return !phase.Ended; } );
            if ( next != m_Phases.end() )
            {
                view.Phase       = next->Spec.Label;
                view.PhaseNumber = static_cast<size_t>( next - m_Phases.begin() ) + 1;
                running          = view.PhaseNumber - 1;
                phaseElapsed     = view.ElapsedSeconds;
                phaseLeft        = ExpectedLeft( next->Expected, phaseElapsed );
            }
        }
        else
        {
            const Phase&    phase = m_Phases[*m_Current];
            const LiveCount count = CountOf( phase, now );
            view.Phase            = phase.Spec.Label;
            view.PhaseNumber      = *m_Current + 1;
            view.Done             = count.Done;
            view.Total            = count.Total;
            view.Active           = count.Active;
            running               = m_Current;
            phaseElapsed          = SecondsBetween( phase.Begin, now );
            phaseLeft             = RemainingOf( phase, count, now );
        }
        // R = the running phase's estimate + every phase still ahead at its expected seconds; W = every phase's
        // weight, D = the ended phases' (a skipped phase ended at zero: its step is behind the start).
        double remaining = phaseLeft;
        double planned   = 0.0;
        double done      = 0.0;
        for ( size_t index = 0; index < m_Phases.size(); ++index )
        {
            const Phase& phase  = m_Phases[index];
            const double weight = std::max( phase.Expected, kMinPhaseWeight );
            planned += weight;
            if ( phase.Ended )
                done += weight;
            else if ( running && index > *running )
                remaining += phase.Expected;
        }

        // THE BAR (UE FScopedSlowTask): done weight / planned weight. The running phase adds its weight times its
        // share done, elapsed / (elapsed + its seconds left) -- its count's done / total when the left is the
        // measured rate's, rising with time when it is the expectation's. The share never goes back (a re-plan
        // that raised the total holds it) and, while the estimate gives nothing newer, it keeps rising by
        // step / (step + max(left, elapsed)) of what is still open: a phase running over its plan slows its
        // part of the bar to the rate it has shown, it never fills it. The bar reaches 1 only at Finish.
        const bool   sampled  = m_LastSample != StartupClock::time_point{};
        const double step     = sampled ? std::max( SecondsBetween( m_LastSample, now ), 0.0 ) : 0.0;
        const double maxShown = std::nextafter( 1.0, 0.0 );
        double       bar      = done;
        if ( running )
        {
            if ( m_SharePhase != running )
            {
                m_SharePhase = running;
                m_PhaseShare = 0.0;
            }
            const double target = phaseElapsed + phaseLeft > 0.0 ? phaseElapsed / ( phaseElapsed + phaseLeft ) : 0.0;
            if ( target > m_PhaseShare )
                m_PhaseShare = target;
            else if ( sampled )
                m_PhaseShare += ( 1.0 - m_PhaseShare ) * step /
                                ( step + std::max( { phaseLeft, phaseElapsed, kRemainingFloorSeconds } ) );
            m_PhaseShare = std::min( m_PhaseShare, maxShown );
            bar += std::max( m_Phases[*running].Expected, kMinPhaseWeight ) * m_PhaseShare;
        }
        if ( planned > 0.0 )
            m_Shown = std::max( m_Shown, bar / planned );
        m_Shown = std::min( m_Shown, maxShown );

        // THE TIME LEFT is R, never rising: each sample it is at most the last one less step * S / (S + elapsed),
        // so it falls in every sample (faster early, slower late) and stays positive. It is not derived from the
        // bar any more: coupling the two pushed the bar to 99.99 % while an overrun phase still ran.
        double left = std::max( remaining, kRemainingFloorSeconds );
        if ( sampled )
            left = std::min( left, std::max( m_ShownRemaining - step * m_ShownRemaining /
                                                                    std::max( m_ShownRemaining + view.ElapsedSeconds,
                                                                              kRemainingFloorSeconds ),
                                             kRemainingFloorSeconds ) );
        m_ShownRemaining      = left;
        m_LastSample          = now;
        view.Fraction         = m_Shown;
        view.RemainingSeconds = left;
        return view;
    }

    std::vector<PhaseTiming> StartupProgress::Timings() const
    {
        const std::lock_guard    lock( m_Mutex );
        std::vector<PhaseTiming> timings;
        timings.reserve( m_Phases.size() );
        for ( const Phase& phase : m_Phases )
            if ( phase.Ended )
                timings.push_back( { .Key             = phase.Spec.Key,
                                     .Label           = phase.Spec.Label,
                                     .Items           = phase.Done,
                                     .Seconds         = phase.Seconds,
                                     .ExpectedSeconds = phase.Expected } );
        return timings;
    }

    LiveCount StartupProgress::CountOf( const Phase& phase, const StartupClock::time_point now ) const
    {
        LiveCount count;
        if ( phase.Source )
            count = phase.Source( now );
        else
        {
            count.Done  = phase.Done;
            count.Total = phase.Total;
        }
        // What the reveal waits for IS what is being done in its phase: the reasons, nothing else.
        if ( !m_WaitReasons.empty() )
        {
            count.Active.clear();
            for ( const std::string& reason : m_WaitReasons )
            {
                if ( count.Active.size() == kActiveShown )
                    break;
                count.Active.push_back( { .Name = reason, .Seconds = 0.0 } );
            }
            return count;
        }
        for ( const auto& [token, running] : m_Running )
            count.Active.push_back( { .Name = running.Name, .Seconds = SecondsBetween( running.Begin, now ) } );
        if ( m_Ambient )
        {
            LiveCount ambient = m_Ambient( now );
            for ( ActiveWork& work : ambient.Active )
                count.Active.push_back( std::move( work ) );
        }
        // Oldest first (the longest running), one row per name: the stalled item is the one worth naming.
        std::stable_sort( count.Active.begin(), count.Active.end(),
                          []( const ActiveWork& a, const ActiveWork& b ) { return a.Seconds > b.Seconds; } );
        std::vector<ActiveWork> shown;
        shown.reserve( kActiveShown );
        for ( ActiveWork& work : count.Active )
        {
            if ( shown.size() == kActiveShown )
                break;
            if ( std::none_of( shown.begin(), shown.end(),
                               [&work]( const ActiveWork& seen ) { return seen.Name == work.Name; } ) )
                shown.push_back( std::move( work ) );
        }
        count.Active = std::move( shown );
        return count;
    }

    double StartupProgress::RemainingOf( const Phase& phase, const LiveCount& count,
                                         const StartupClock::time_point now ) const
    {
        const double elapsed = SecondsBetween( phase.Begin, now );
        // Counted and started: the measured rate (Unity's rule). A cache miss that raised the total raised this.
        if ( count.Total > count.Done && count.Done >= 1 )
            return std::max( elapsed * static_cast<double>( count.Total - count.Done ) /
                                  static_cast<double>( count.Done ),
                             kRemainingFloorSeconds );
        // Otherwise the expectation, decaying through an overrun -- also when every counted item is done and the
        // phase still runs (its main-thread tail, items not asked yet): a count at its total is not a phase at
        // its end, and the floor there sat the time left at 0:01 for 40 s (STARTUP-PROGRESS acceptance #39).
        return ExpectedLeft( phase.Expected, elapsed );
    }

    void StartupProgress::EndCurrent( const StartupClock::time_point now )
    {
        if ( !m_Current )
            return;
        Phase& phase = m_Phases[*m_Current];
        if ( phase.Source )
        {
            // A pulled phase's count is its source's: the last answer is what the phase did.
            const LiveCount count = phase.Source( now );
            phase.Done            = count.Done;
            phase.Total           = count.Total;
            phase.Source          = {};
        }
        phase.Seconds = SecondsBetween( phase.Begin, now );
        phase.Ended   = true;
        m_Current.reset();
        LOG_INFO( "[Startup] phase {}: {} item(s) in {:.2f} s (expected {:.2f} s)", phase.Spec.Label, phase.Done,
                  phase.Seconds, phase.Expected );
        NoteLocked( SecondsBetween( m_ProcessStart, now ),
                    phase.Total > 0 || phase.Done > 0
                         ? std::format( "{}  {} items  {:.1f} s", phase.Spec.Label, phase.Done, phase.Seconds )
                         : std::format( "{}  {:.1f} s", phase.Spec.Label, phase.Seconds ) );
    }

    void StartupProgress::NoteLocked( const double at, std::string text )
    {
        m_Log.push_back( { .AtSeconds = at, .Text = std::move( text ) } );
        // A ring of kLogShown * 4: the splash shows the last kLogShown, the rest is slack for a burst.
        if ( m_Log.size() > kLogShown * 4 )
            m_Log.erase( m_Log.begin() );
    }

    std::string FormatDuration( const double seconds )
    {
        const auto whole = static_cast<long long>( std::llround( std::max( seconds, 0.0 ) ) );
        return std::format( "{}:{:02}", whole / 60, whole % 60 );
    }

    Common::ResultStr<std::map<std::string, double, std::less<>>>
    LoadStartupHistory( const std::filesystem::path& file )
    {
        using History = std::map<std::string, double, std::less<>>;
        std::error_code missing;
        if ( !std::filesystem::exists( file, missing ) )
        {
            if ( missing )
                return Common::MakeError<History>(
                     std::format( "{}: cannot tell whether it exists: {}", file.string(), missing.message() ) );
            return Common::MakeSuccess( History{} );
        }
        auto read = Common::Json::ReadFile<StartupHistoryFile>( file );
        if ( !read )
            return Common::MakeError<History>( read.GetError() );
        const StartupHistoryFile& history = read.GetValue();
        if ( history.Version != kHistoryVersion )
            return Common::MakeError<History>( std::format( "{}: version {}, this build reads version {}",
                                                            file.string(), history.Version, kHistoryVersion ) );
        return Common::MakeSuccess( History( history.Phases.begin(), history.Phases.end() ) );
    }

    Common::BoolResultStr SaveStartupHistory( const std::filesystem::path&    file,
                                              const std::vector<PhaseTiming>& timings )
    {
        StartupHistoryFile history;
        history.Version = kHistoryVersion;
        for ( const PhaseTiming& timing : timings )
            history.Phases[timing.Key] = timing.Seconds;
        return Common::Json::WriteFileAtomic( file, history );
    }
} // namespace Desert::Core
