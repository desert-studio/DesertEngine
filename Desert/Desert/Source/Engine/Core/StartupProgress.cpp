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

        // The history file. A new field is a new version and a migration of the files (one per project per
        // machine, rewritten at every reveal). Version 1 kept one run per key; it is read once as that run.
        struct StartupHistoryVersion
        {
            int Version = 0;
        };
        struct StartupHistoryFileV1
        {
            int                           Version = 1;
            std::map<std::string, double> Phases;
        };
        struct StartupHistoryFile
        {
            int                                        Version = 2;
            std::map<std::string, std::vector<double>> Phases;
        };
        constexpr int kHistoryVersion = 2;

        // Rule 1: the longest of the last starts. A cold disk is 12x a warm one; an over-estimate is caught up
        // downwards, an under-estimate would run the start "longer than expected".
        [[nodiscard]] double ExpectedOf( const StartupPhaseSpec& spec, const StartupHistory& history )
        {
            const auto found = history.find( spec.Key );
            if ( found == history.end() || found->second.empty() )
                return spec.DefaultSeconds;
            return *std::max_element( found->second.begin(), found->second.end() );
        }
    } // namespace

    StartupProgress::StartupProgress( std::vector<StartupPhaseSpec> plan, StartupHistory history,
                                      const StartupClock::time_point processStart )
         : m_ProcessStart( processStart )
    {
        m_Phases.reserve( plan.size() );
        for ( StartupPhaseSpec& spec : plan )
        {
            Phase phase;
            phase.Expected = ExpectedOf( spec, history );
            phase.Spec     = std::move( spec );
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
        // A phase of the plan this start did not run (the host skipped it) took no time: ended at zero, so it
        // stops counting into the time left.
        for ( size_t skipped = 0; skipped < index; ++skipped )
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

    void StartupProgress::SetWaits( std::vector<StartupWait> waits )
    {
        const std::lock_guard          lock( m_Mutex );
        const StartupClock::time_point now = StartupClock::now();
        std::vector<Waiting>           next;
        next.reserve( waits.size() );
        bool namesChanged = waits.size() != m_Waits.size();
        for ( size_t i = 0; i < waits.size(); ++i )
        {
            // A name already waited for keeps its age; only its Detail moves.
            const auto seen = std::find_if( m_Waits.begin(), m_Waits.end(), [&]( const Waiting& waiting )
                                            { return waiting.Wait.Name == waits[i].Name; } );
            if ( !namesChanged && m_Waits[i].Wait.Name != waits[i].Name )
                namesChanged = true;
            next.push_back(
                 { .Wait = std::move( waits[i] ), .Since = seen != m_Waits.end() ? seen->Since : now } );
        }
        if ( namesChanged )
        {
            std::string joined;
            for ( const Waiting& waiting : next )
                joined += joined.empty() ? waiting.Wait.Name : ", " + waiting.Wait.Name;
            LOG_INFO( "[Startup] waits for: {}", joined.empty() ? std::string( "nothing" ) : joined );
        }
        m_Waits = std::move( next );
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
        m_Finished      = true;
        m_ShownFraction = 1.0;
        m_ShownEta      = 0.0;

        // THE START'S TABLE: every phase that ran, then the total. The phases are back to back from the first
        // one's begin; what ran before the first phase is the process's own start (images, the splash).
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

    void StartupProgress::BeginStep( const std::string_view key ) noexcept
    {
        if ( const auto begun = BeginPhase( key ); !begun )
            LOG_ERROR( "[Startup] step '{}' could not begin: {}", key, begun.GetError() );
    }

    void StartupProgress::Expect( const uint64_t items ) noexcept
    {
        const std::lock_guard lock( m_Mutex );
        if ( !m_Current )
        {
            LOG_ERROR( "[Startup] Expect( {} ) refused: no phase is running", items );
            return;
        }
        Phase& phase = m_Phases[*m_Current];
        // The count on screen never moves (the contract's rule 2): one Expect, before the first item, of a
        // Planned phase. Anything else is a defect of the caller, said once per phase.
        if ( phase.Spec.Count != PhaseCountKind::Planned || phase.TotalGiven || phase.ItemBegun )
        {
            if ( !phase.RefusalSaid )
                LOG_ERROR( "[Startup] Expect( {} ) refused in phase '{}': {}", items, phase.Spec.Key,
                           phase.Spec.Count != PhaseCountKind::Planned ? "the phase is not Planned"
                           : phase.TotalGiven                          ? "its count was already given"
                                                                       : "an item has already begun" );
            phase.RefusalSaid = true;
            return;
        }
        phase.Total      = items;
        phase.TotalGiven = true;
    }

    uint64_t StartupProgress::BeginItem( const std::string_view name ) noexcept
    {
        const std::lock_guard lock( m_Mutex );
        const uint64_t        token = m_NextToken++;
        m_Running.emplace( token, Running{ .Name = std::string( name ), .Begin = StartupClock::now() } );
        if ( m_Current )
            m_Phases[*m_Current].ItemBegun = true;
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

    StartupView StartupProgress::Sample( const StartupClock::time_point now, const double minVisibleStep )
    {
        const std::lock_guard lock( m_Mutex );
        StartupView           view;
        view.PhaseCount      = m_Phases.size();
        view.ElapsedSeconds  = SecondsBetween( m_ProcessStart, now );
        view.StopRequested   = StopRequested();
        view.Finished        = m_Finished;
        view.EtaKnown        = m_EtaKnown;
        const size_t logFrom = m_Log.size() > kLogShown ? m_Log.size() - kLogShown : 0;
        view.Log.assign( m_Log.begin() + static_cast<std::ptrdiff_t>( logFrom ), m_Log.end() );
        if ( m_Finished )
        {
            view.Fraction = 1.0;
            m_LastSample  = now;
            return view;
        }

        // The running phase: the current one, or before the first BeginPhase the plan's first phase still to
        // run, which the process is already doing (the window): named from the splash's first frame, running
        // since the process started.
        std::optional<size_t> running = m_Current;
        if ( !running )
        {
            const auto next = std::find_if( m_Phases.begin(), m_Phases.end(),
                                            []( const Phase& phase ) { return !phase.Ended; } );
            if ( next != m_Phases.end() )
                running = static_cast<size_t>( next - m_Phases.begin() );
        }
        double remaining = 0.0; // R (rule 3)
        if ( running )
        {
            const Phase& phase   = m_Phases[*running];
            const double elapsed = SecondsBetween( m_Current ? phase.Begin : m_ProcessStart, now );
            LiveCount    count;
            if ( m_Current )
                count = CountOf( phase, now );
            else
                count.Active.push_back( { .Name = phase.Spec.Label, .Seconds = elapsed } );
            view.Phase       = phase.Spec.Label;
            view.PhaseNumber = *running + 1;
            view.Count       = phase.Spec.Count;
            view.Done        = phase.Spec.Count == PhaseCountKind::Timed ? 0 : count.Done;
            view.Total       = phase.Spec.Count == PhaseCountKind::Planned && phase.TotalGiven ? phase.Total : 0;
            view.Active      = std::move( count.Active );
            remaining        = RemainingOf( phase, count, elapsed );
            for ( size_t ahead = *running + 1; ahead < m_Phases.size(); ++ahead )
                if ( !m_Phases[ahead].Ended )
                    remaining += m_Phases[ahead].Expected;
        }

        // Rules 4 and 5: the shown ETA and bar are regulators integrated over the time since the last sample,
        // so neither stands still while work runs and neither jumps. The first sample starts them at the target.
        const double step    = std::clamp( minVisibleStep, 1e-6, 0.1 );
        const double barCap  = 1.0 - step;
        const double target  = view.ElapsedSeconds + remaining > 0.0
                                    ? view.ElapsedSeconds / ( view.ElapsedSeconds + remaining )
                                    : 0.0;
        const bool   sampled = m_LastSample != StartupClock::time_point{};
        const double dt      = sampled ? std::max( SecondsBetween( m_LastSample, now ), 0.0 ) : 0.0;
        if ( !sampled )
        {
            m_ShownEta      = remaining;
            m_ShownFraction = std::min( target, barCap );
        }
        else
        {
            const double etaRate =
                 std::clamp( 1.0 + ( m_ShownEta - remaining ) / kEtaCatchUpSeconds, kEtaMinRate, kEtaMaxRate );
            m_ShownEta -= etaRate * dt;
            const double minBarRate = kBarStepMargin * step / kBarMaxStallSeconds;
            const double barRate =
                 std::clamp( ( target - m_ShownFraction ) / kBarCatchUpSeconds, minBarRate, kBarMaxRate );
            m_ShownFraction = std::min( m_ShownFraction + barRate * dt, std::max( barCap, m_ShownFraction ) );
        }
        // Rule 4's floor: below it the number would be a lie while work remains; the rest of the start says
        // "longer than expected" instead.
        if ( m_EtaKnown && m_ShownEta <= kEtaFloorSeconds )
        {
            m_EtaKnown = false;
            LOG_INFO( "[Startup] the start runs longer than expected ({:.1f} s left by the plan, at {:.1f} s)",
                      remaining, view.ElapsedSeconds );
        }
        m_ShownEta      = std::max( m_ShownEta, 0.0 );
        m_LastSample    = now;
        view.Fraction   = m_ShownFraction;
        view.EtaSeconds = m_ShownEta;
        view.EtaKnown   = m_EtaKnown;
        return view;
    }

    std::vector<PhaseTiming> StartupProgress::Timings() const
    {
        const std::lock_guard    lock( m_Mutex );
        std::vector<PhaseTiming> timings;
        timings.reserve( m_Phases.size() );
        for ( const Phase& phase : m_Phases )
            if ( phase.Ended && phase.Begin != StartupClock::time_point{} )
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
        std::vector<ActiveWork> work;
        if ( phase.Source )
        {
            count = phase.Source( now );
            work  = std::move( count.Active );
            count.Active.clear();
        }
        else
            count.Done = phase.Done;
        // Rule 6, in this order: what the phase waits for (aged from the first call that named it), then its
        // work oldest first (its items, its source's, the ambient pipelines), one row per name.
        for ( const Waiting& waiting : m_Waits )
            count.Active.push_back( { .Name    = waiting.Wait.Name,
                                      .Detail  = waiting.Wait.Detail,
                                      .Seconds = SecondsBetween( waiting.Since, now ) } );
        for ( const auto& [token, item] : m_Running )
            work.push_back( { .Name = item.Name, .Seconds = SecondsBetween( item.Begin, now ) } );
        if ( m_Ambient )
            for ( ActiveWork& ambient : m_Ambient( now ).Active )
                work.push_back( std::move( ambient ) );
        std::stable_sort( work.begin(), work.end(),
                          []( const ActiveWork& a, const ActiveWork& b ) { return a.Seconds > b.Seconds; } );
        for ( ActiveWork& item : work )
            count.Active.push_back( std::move( item ) );

        std::vector<ActiveWork> shown;
        shown.reserve( kActiveShown );
        for ( ActiveWork& item : count.Active )
        {
            if ( shown.size() == kActiveShown )
                break;
            if ( std::none_of( shown.begin(), shown.end(),
                               [&item]( const ActiveWork& seen ) { return seen.Name == item.Name; } ) )
                shown.push_back( std::move( item ) );
        }
        if ( shown.empty() )
            shown.push_back( { .Name = phase.Spec.Label, .Seconds = SecondsBetween( phase.Begin, now ) } );
        count.Active = std::move( shown );
        return count;
    }

    double StartupProgress::RemainingOf( const Phase& phase, const LiveCount& count, const double elapsed ) const
    {
        // Rule 3.
        const double reserve  = std::max( kReserveSeconds, kReserveShare * elapsed );
        const double expected = std::max( phase.Expected - elapsed, reserve );
        const double done     = static_cast<double>( count.Done );
        switch ( phase.Spec.Count )
        {
            case PhaseCountKind::Planned:
                if ( phase.TotalGiven && phase.Total > 0 )
                {
                    const double total = static_cast<double>( phase.Total );
                    if ( count.Done >= phase.Total )
                        return 0.0;
                    if ( done >= std::max( 3.0, total / 50.0 ) )
                        return ( total - done ) * elapsed / done;
                    return phase.Expected * ( 1.0 - done / total );
                }
                return expected;
            case PhaseCountKind::Discovered:
                if ( count.Queued > 0 && count.Done >= 20 )
                    return std::max( static_cast<double>( count.Queued ) * elapsed / done, expected );
                return expected;
            case PhaseCountKind::Timed:
                break;
        }
        return expected;
    }

    void StartupProgress::EndCurrent( const StartupClock::time_point now )
    {
        m_Waits.clear();
        if ( !m_Current )
            return;
        Phase& phase = m_Phases[*m_Current];
        if ( phase.Source )
        {
            // A pulled phase's count is its source's: the last answer is what the phase did.
            phase.Done   = phase.Source( now ).Done;
            phase.Source = {};
        }
        phase.Seconds = SecondsBetween( phase.Begin, now );
        phase.Ended   = true;
        m_Current.reset();
        LOG_INFO( "[Startup] phase {}: {} item(s) in {:.2f} s (expected {:.2f} s)", phase.Spec.Label, phase.Done,
                  phase.Seconds, phase.Expected );
        NoteLocked( SecondsBetween( m_ProcessStart, now ),
                    phase.Spec.Count != PhaseCountKind::Timed
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

    Common::ResultStr<StartupHistory> LoadStartupHistory( const std::filesystem::path& file )
    {
        std::error_code missing;
        if ( !std::filesystem::exists( file, missing ) )
        {
            if ( missing )
                return Common::MakeError<StartupHistory>(
                     std::format( "{}: cannot tell whether it exists: {}", file.string(), missing.message() ) );
            return Common::MakeSuccess( StartupHistory{} );
        }
        const auto version = Common::Json::ReadFile<StartupHistoryVersion>( file );
        if ( !version )
            return Common::MakeError<StartupHistory>( version.GetError() );
        StartupHistory history;
        switch ( version.GetValue().Version )
        {
            case 1:
            {
                // Version 1: one run per key, read as that one run; the next save writes version 2.
                const auto read = Common::Json::ReadFile<StartupHistoryFileV1>( file );
                if ( !read )
                    return Common::MakeError<StartupHistory>( read.GetError() );
                for ( const auto& [key, seconds] : read.GetValue().Phases )
                    history[key] = { seconds };
                return Common::MakeSuccess( std::move( history ) );
            }
            case kHistoryVersion:
            {
                const auto read = Common::Json::ReadFile<StartupHistoryFile>( file );
                if ( !read )
                    return Common::MakeError<StartupHistory>( read.GetError() );
                for ( const auto& [key, runs] : read.GetValue().Phases )
                    history[key] = runs;
                return Common::MakeSuccess( std::move( history ) );
            }
            default:
                return Common::MakeError<StartupHistory>(
                     std::format( "{}: version {}, this build reads versions 1 and {}", file.string(),
                                  version.GetValue().Version, kHistoryVersion ) );
        }
    }

    Common::BoolResultStr SaveStartupHistory( const std::filesystem::path& file, const StartupHistory& previous,
                                              const std::vector<PhaseTiming>& timings )
    {
        StartupHistoryFile history;
        history.Version = kHistoryVersion;
        for ( const auto& [key, runs] : previous )
            history.Phases[key] = runs;
        for ( const PhaseTiming& timing : timings )
        {
            std::vector<double>& runs = history.Phases[timing.Key];
            runs.push_back( timing.Seconds );
            if ( runs.size() > StartupProgress::kHistoryRuns )
                runs.erase( runs.begin(),
                            runs.end() - static_cast<std::ptrdiff_t>( StartupProgress::kHistoryRuns ) );
        }
        return Common::Json::WriteFileAtomic( file, history );
    }
} // namespace Desert::Core
