#include "Scheduler.hpp"

#include "Config.hpp"

#include <algorithm>
#include <sstream>

namespace buildpool::daemon
{
    namespace
    {
        std::string Mb( uint64_t bytes )
        {
            return std::to_string( bytes / kMiB );
        }
        std::string SignedMb( int64_t bytes )
        {
            return std::to_string( bytes / static_cast<int64_t>( kMiB ) );
        }

        std::string BuildKeyOf( const std::string& root, const std::string& label )
        {
            return root + '\t' + label;
        }

        const char* StageName( Stage stage ) noexcept
        {
            switch ( stage )
            {
                case Stage::Front:
                    return "front";
                case Stage::Back:
                    return "back";
                case Stage::Control:
                    return "control";
            }
            return "?";
        }
    } // namespace

    void Outbox::Clear()
    {
        sends.clear();
        drops.clear();
        log.clear();
        finishedRuns.clear();
        exit = false;
    }

    Scheduler::Scheduler( const PoolConfig& config, Ledger& ledger, uint32_t logicalCpus, std::string identity,
                          uint64_t firstSession )
         : m_config( config ), m_ledger( ledger ), m_identity( std::move( identity ) ),
           m_nextSession( firstSession )
    {
        // LoadConfig refuses a cpu_reserve that leaves no slot.
        m_slots = logicalCpus > config.cpuReserve ? logicalCpus - config.cpuReserve : 1;
    }

    // ---- small helpers
    // -------------------------------------------------------------------------------------------

    Scheduler::Build* Scheduler::FindBuild( const std::string& key )
    {
        const auto it = m_builds.find( key );
        return it == m_builds.end() ? nullptr : &it->second;
    }

    uint64_t Scheduler::FreeNow() const noexcept
    {
        return m_memory ? FreeBytes( *m_memory ) : 0;
    }

    int64_t Scheduler::HeadroomNow() const
    {
        int64_t headroom = static_cast<int64_t>( FreeNow() ) - static_cast<int64_t>( m_config.reserveBytes );
        for ( const auto& [lease, id] : m_leases )
        {
            const auto it = m_clients.find( id );
            if ( it == m_clients.end() || !it->second.action )
                continue;
            const Action& action = *it->second.action;
            if ( action.estimate > action.currentBytes )
                headroom -= static_cast<int64_t>( action.estimate - action.currentBytes );
        }
        return headroom;
    }

    void Scheduler::ObserveAllBuilds()
    {
        const MemoryState* memory = m_memory ? &*m_memory : nullptr;
        for ( auto& [key, build] : m_builds )
            build.run.ObserveMachine( m_running, memory );
    }

    void Scheduler::Refuse( ClientId id, std::string reason, Outbox& out )
    {
        out.log.push_back( "refused client " + std::to_string( id ) + ": " + reason );
        out.sends.emplace_back( id, Message{ Refused{ std::move( reason ) } } );
        out.drops.push_back( id );
    }

    void Scheduler::FreeLease( Client& client )
    {
        if ( !client.action || client.action->state != Action::State::Running )
            return;
        m_leases.erase( client.action->lease );
        if ( Build* build = FindBuild( client.buildKey ) )
            build->running -= build->running > 0 ? 1 : 0;
        m_running -= m_running > 0 ? 1 : 0;
        client.action->lease = 0;
    }

    // ---- events
    // --------------------------------------------------------------------------------------------------

    void Scheduler::OnConnected( ClientId client, Now, Outbox& )
    {
        m_clients.try_emplace( client );
    }

    void Scheduler::OnMessage( ClientId id, const Message& message, Now now, Outbox& out )
    {
        auto it = m_clients.find( id );
        if ( it == m_clients.end() )
            it = m_clients.try_emplace( id ).first;
        Client& client = it->second;

        if ( const auto* hello = std::get_if<Hello>( &message ) )
        {
            OnHello( id, client, *hello, now, out );
            return;
        }
        if ( !client.greeted )
        {
            Refuse( id, "the first message of a connection must be Hello", out );
            return;
        }

        if ( const auto* acquire = std::get_if<Acquire>( &message ) )
            OnAcquire( id, client, *acquire, now, out );
        else if ( const auto* note = std::get_if<Note>( &message ) )
        {
            if ( const auto session = m_sessions.find( client.session ); session != m_sessions.end() )
                session->second.note = note->text;
        }
        else if ( const auto* heartbeat = std::get_if<Heartbeat>( &message ) )
        {
            if ( client.action && client.action->state == Action::State::Running )
            {
                client.action->currentBytes  = heartbeat->currentBytes;
                client.action->observedBytes = std::max( client.action->observedBytes, heartbeat->currentBytes );
                client.action->cpuMs         = heartbeat->cpuMs;
            }
        }
        else if ( const auto* released = std::get_if<Released>( &message ) )
            OnReleased( id, client, *released, now, out );
        else if ( const auto* exited = std::get_if<CacheExited>( &message ) )
            OnCacheExited( id, client, *exited, now, out );
        else if ( const auto* query = std::get_if<Query>( &message ) )
            OnQuery( id, client, *query, now, out );
        else
            Refuse( id,
                    "a client sent a message only the daemon sends (variant index " +
                         std::to_string( message.index() ) + ")",
                    out );
    }

    void Scheduler::OnHello( ClientId id, Client& client, const Hello& hello, Now now, Outbox& out )
    {
        if ( client.greeted )
        {
            Refuse( id, "a second Hello on one connection", out );
            return;
        }
        if ( hello.protocol != kProtocolVersion )
        {
            const std::string reason = "the client speaks protocol " + std::to_string( hello.protocol ) +
                                       ", this daemon protocol " + std::to_string( kProtocolVersion );
            if ( hello.protocol > kProtocolVersion )
                StartDrain( reason + " (a newer client: this daemon drains and exits)", out );
            Refuse( id, reason, out );
            CheckDrained( out );
            return;
        }
        client.greeted = true;
        client.stage   = hello.stage;
        client.process = hello.process;

        if ( hello.stage == Stage::Control )
        {
            out.sends.emplace_back( id, Message{ Welcome{ m_nextSession++, {} } } );
            return;
        }
        if ( hello.buildRoot.empty() )
        {
            Refuse( id, std::string( "a " ) + StageName( hello.stage ) + " client named no build root", out );
            return;
        }
        const ClassPolicy* policy = FindClass( m_config, hello.className );
        if ( policy == nullptr )
        {
            std::string known;
            for ( const ClassPolicy& c : m_config.classes )
                known += ( known.empty() ? "" : ", " ) + c.name;
            Refuse( id, "unknown build class '" + hello.className + "'; pool.ini has: " + known, out );
            return;
        }

        const std::string key   = BuildKeyOf( hello.buildRoot, hello.runLabel );
        auto [buildIt, created] = m_builds.try_emplace( key );
        Build& build            = buildIt->second;
        if ( created )
        {
            build.classIndex    = static_cast<size_t>( policy - m_config.classes.data() );
            build.startMs       = now.ms;
            build.run.startUnix = now.unixSeconds;
            build.run.buildRoot = hello.buildRoot;
            build.run.runLabel  = hello.runLabel;
            build.run.className = policy->name;
            build.run.ObserveMachine( m_running, m_memory ? &*m_memory : nullptr );
            out.log.push_back( "build opened: " + hello.buildRoot + " run=" +
                               ( hello.runLabel.empty() ? "-" : hello.runLabel ) + " class=" + policy->name );
        }
        else if ( &ClassOf( build ) != policy )
        {
            out.log.push_back( "build " + hello.buildRoot + ": a client named class " + policy->name +
                               ", the build keeps " + ClassOf( build ).name + " (its first client's)" );
        }
        build.clients += 1;
        client.buildKey = key;

        if ( hello.stage == Stage::Front )
        {
            const uint64_t session = m_nextSession++;
            Session&       state   = m_sessions[session];
            state.front            = id;
            state.buildKey         = key;
            client.session         = session;
            out.sends.emplace_back( id, Message{ Welcome{ session, {} } } );
            return;
        }

        // Back
        const auto session = m_sessions.find( hello.session );
        if ( hello.session != 0 && session != m_sessions.end() )
        {
            client.session = hello.session;
            if ( session->second.adopted )
                session->second.backs += 1;
            out.sends.emplace_back( id, Message{ Welcome{ hello.session, session->second.note } } );
        }
        else if ( hello.session != 0 )
        {
            // A Back whose Front this daemon never saw (the daemon restarted under a running build): the session is
            // opened fresh under the Front's id. Its Front talks to the daemon that is gone, so the session lives
            // while its Backs are connected and ends with the last of them (OnClosed).
            Session& adopted = m_sessions[hello.session];
            adopted.buildKey = key;
            adopted.adopted  = true;
            adopted.backs    = 1;
            client.session   = hello.session;
            if ( m_nextSession <= hello.session )
                m_nextSession = hello.session + 1;
            out.log.push_back( "back of unknown session " + std::to_string( hello.session ) + " (" +
                               hello.buildRoot + "): session opened fresh" );
            out.sends.emplace_back( id, Message{ Welcome{ hello.session, {} } } );
        }
        else
        {
            Refuse( id, "a back client named no session", out );
        }
    }

    void Scheduler::OnAcquire( ClientId id, Client& client, const Acquire& acquire, Now now, Outbox& out )
    {
        if ( client.stage == Stage::Control )
        {
            Refuse( id, "a control client cannot acquire a lease", out );
            return;
        }
        if ( client.action )
        {
            Refuse( id, "one action per connection: Acquire while the previous action is not released", out );
            return;
        }
        Build* build = FindBuild( client.buildKey );
        if ( build == nullptr )
        {
            Refuse( id, "Acquire from a client of no build", out );
            return;
        }

        Action action;
        action.request       = acquire;
        action.order         = m_nextArrival++;
        action.queuedSinceMs = now.ms;

        // Dedup: the first session under a key leads it; later sessions wait without a slot.
        if ( !acquire.inflightKey.empty() && !acquire.retried && client.session != 0 )
        {
            const auto session = m_sessions.find( client.session );
            if ( session != m_sessions.end() )
            {
                auto [entry, inserted] = m_inflight.try_emplace( acquire.inflightKey );
                if ( inserted )
                {
                    entry->second.leaderSession   = client.session;
                    entry->second.leaderBuildRoot = build->run.buildRoot;
                    session->second.leads.push_back( acquire.inflightKey );
                }
                else if ( entry->second.leaderSession != client.session )
                {
                    action.state = Action::State::Awaiting;
                    entry->second.waiters.push_back( id );
                    out.sends.emplace_back( id, Message{ Await{ entry->second.leaderBuildRoot } } );
                }
            }
        }
        client.action = std::move( action );
        Admit( now, out );
    }

    void Scheduler::OnReleased( ClientId id, Client& client, const Released& released, Now now, Outbox& out )
    {
        if ( !client.action || client.action->state != Action::State::Running )
        {
            Refuse( id, "Released without a lease", out );
            return;
        }
        Build*         build  = FindBuild( client.buildKey );
        Action&        action = *client.action;
        const uint64_t lease  = action.lease;
        FreeLease( client );

        if ( released.preempted )
        {
            const uint64_t observed =
                 std::max( { action.observedBytes, action.currentBytes, released.peakBytes } );
            const auto raised = static_cast<uint64_t>( static_cast<double>( observed ) * 1.5 );
            if ( !action.request.costKey.empty() )
                m_ledger.Raise( action.request.costKey, raised );
            action.preemptions += 1;
            action.preemptSent   = false;
            action.state         = Action::State::Queued;
            action.order         = m_nextHead--; // the head of the queue
            action.queuedSinceMs = now.ms;
            action.currentBytes  = 0;
            action.cpuMs         = 0;
            if ( build != nullptr )
                build->run.preemptions += 1;
            out.log.push_back(
                 "preempted lease " + std::to_string( lease ) + " released: " + action.request.costKey +
                 " observed " + Mb( observed ) + " MB, estimate raised to >= " + Mb( raised ) +
                 " MB, back at the head of the queue (preemption " + std::to_string( action.preemptions ) + ")" );
        }
        else
        {
            if ( !action.request.costKey.empty() && released.peakBytes > 0 )
                m_ledger.Record( action.request.costKey, released.peakBytes, released.wallMs, released.cpuMs );
            if ( build != nullptr )
                build->run.actions += 1;
            if ( !action.request.inflightKey.empty() && client.session != 0 )
            {
                const auto entry = m_inflight.find( action.request.inflightKey );
                if ( entry != m_inflight.end() && entry->second.leaderSession == client.session )
                    entry->second.leaderSucceeded = released.exitCode == 0;
            }
            client.action.reset();
        }
        Admit( now, out );
        CheckDrained( out );
    }

    void Scheduler::OnCacheExited( ClientId id, Client& client, const CacheExited&, Now now, Outbox& out )
    {
        if ( client.stage != Stage::Front )
        {
            Refuse( id, "CacheExited from a client that is not a Front", out );
            return;
        }
        // Every CacheExited of a Front gets exactly one Verdict, whatever is left of its session or lease: a
        // session already ended (a second CacheExited, a lease lost with its Back) answers "do not run again".
        const auto session  = m_sessions.find( client.session );
        const bool runAgain = session != m_sessions.end() && session->second.retryAnswered;
        out.sends.emplace_back( id, Message{ Verdict{ runAgain } } );
        if ( session == m_sessions.end() )
            return;
        if ( runAgain )
        {
            session->second.retryAnswered = false;
            if ( Build* build = FindBuild( client.buildKey ) )
                build->run.cacheReruns += 1;
            return;
        }
        const uint64_t ended = client.session;
        client.session       = 0;
        EndSession( ended, now, out );
        Admit( now, out );
    }

    void Scheduler::OnQuery( ClientId id, Client& client, const Query& query, Now now, Outbox& out )
    {
        switch ( query.what )
        {
            case Query::What::Status:
                out.sends.emplace_back( id, Message{ Text{ StatusText( now ) } } );
                return;
            case Query::What::Window:
                out.sends.emplace_back( id, Message{ Text{ std::to_string( m_slots * m_config.windowFactor ) } } );
                return;
            case Query::What::Stop:
                client.waitsForStop = true;
                StartDrain( "stop requested by pid " + std::to_string( client.process.pid ), out );
                CheckDrained( out );
                return;
        }
        Refuse( id, "unknown query", out );
    }

    void Scheduler::OnClosed( ClientId id, Now now, Outbox& out )
    {
        const auto it = m_clients.find( id );
        if ( it == m_clients.end() )
            return;
        Client client = std::move( it->second );
        m_clients.erase( it );

        if ( client.action )
        {
            switch ( client.action->state )
            {
                case Action::State::Running:
                    out.log.push_back( "lease " + std::to_string( client.action->lease ) +
                                       " lost with its client (" + client.action->request.costKey + ")" );
                    FreeLease( client );
                    break;
                case Action::State::Awaiting:
                {
                    const auto entry = m_inflight.find( client.action->request.inflightKey );
                    if ( entry != m_inflight.end() )
                    {
                        auto& waiters = entry->second.waiters;
                        waiters.erase( std::remove( waiters.begin(), waiters.end(), id ), waiters.end() );
                    }
                    break;
                }
                case Action::State::Queued:
                    break;
            }
        }
        if ( client.stage == Stage::Front && client.session != 0 )
            EndSession( client.session, now, out );
        if ( client.stage == Stage::Back && client.session != 0 )
        {
            // An adopted session (its Front talks to a daemon that is gone) ends with its last Back.
            const auto session = m_sessions.find( client.session );
            if ( session != m_sessions.end() && session->second.adopted )
            {
                session->second.backs -= session->second.backs > 0 ? 1 : 0;
                if ( session->second.backs == 0 )
                    EndSession( client.session, now, out );
            }
        }
        if ( !client.buildKey.empty() )
            if ( Build* build = FindBuild( client.buildKey ) )
            {
                build->clients -= build->clients > 0 ? 1 : 0;
                if ( build->clients == 0 )
                    build->idleSinceMs = now.ms;
            }
        Admit( now, out );
        CheckDrained( out );
    }

    void Scheduler::OnTick( const MemoryState& memory, Now now, Outbox& out )
    {
        if ( !m_memoryError.empty() )
            out.log.push_back( "memory reading restored" );
        m_memory = memory;
        m_memoryError.clear();
        ObserveAllBuilds();
        Guard( now, out );
        Admit( now, out );
        RetireIdleBuilds( now, out );
        CheckDrained( out );
    }

    void Scheduler::OnMemoryUnavailable( const std::string& reason, Now now, Outbox& out )
    {
        if ( reason != m_memoryError )
            out.log.push_back( "memory reading failed, nothing is admitted until it succeeds: " + reason );
        m_memory.reset();
        m_memoryError = reason;
        RetireIdleBuilds( now, out );
        CheckDrained( out );
    }

    // ---- sessions and dedup
    // ---------------------------------------------------------------------------------------

    void Scheduler::EndSession( uint64_t sessionId, Now now, Outbox& out )
    {
        const auto it = m_sessions.find( sessionId );
        if ( it == m_sessions.end() )
            return;
        const std::vector<std::string> leads = std::move( it->second.leads );
        m_sessions.erase( it );
        for ( const std::string& key : leads )
        {
            const auto entry = m_inflight.find( key );
            if ( entry != m_inflight.end() && entry->second.leaderSession == sessionId )
                ResolveInflight( key, now, out );
        }
    }

    void Scheduler::ResolveInflight( const std::string& key, Now now, Outbox& out )
    {
        const auto entryIt = m_inflight.find( key );
        if ( entryIt == m_inflight.end() )
            return;
        Inflight& entry = entryIt->second;

        if ( entry.leaderSucceeded )
        {
            for ( const ClientId waiterId : entry.waiters )
            {
                const auto waiter = m_clients.find( waiterId );
                if ( waiter == m_clients.end() || !waiter->second.action ||
                     waiter->second.action->state != Action::State::Awaiting )
                    continue;
                Client& client = waiter->second;
                if ( Build* build = FindBuild( client.buildKey ) )
                    build->run.waitedMsTotal += now.ms - client.action->queuedSinceMs;
                if ( const auto session = m_sessions.find( client.session ); session != m_sessions.end() )
                    session->second.retryAnswered = true;
                client.action.reset();
                out.sends.emplace_back( waiterId, Message{ Retry{} } );
            }
            m_inflight.erase( entryIt );
            return;
        }

        // The leader failed or vanished: the oldest live waiter leads now and joins the queue in its arrival
        // place.
        while ( !entry.waiters.empty() )
        {
            const ClientId waiterId = entry.waiters.front();
            entry.waiters.erase( entry.waiters.begin() );
            const auto waiter = m_clients.find( waiterId );
            if ( waiter == m_clients.end() || !waiter->second.action ||
                 waiter->second.action->state != Action::State::Awaiting )
                continue;
            Client&    client  = waiter->second;
            const auto session = m_sessions.find( client.session );
            if ( session == m_sessions.end() )
            {
                // Its own session has ended meanwhile: it is queued, but cannot lead.
                client.action->state = Action::State::Queued;
                continue;
            }
            client.action->state  = Action::State::Queued;
            entry.leaderSession   = client.session;
            entry.leaderSucceeded = false;
            const Build* build    = FindBuild( client.buildKey );
            entry.leaderBuildRoot = build != nullptr ? build->run.buildRoot : std::string{};
            session->second.leads.push_back( key );
            out.log.push_back( "dedup: the leader of a key did not succeed; " + entry.leaderBuildRoot +
                               " leads it now (" + std::to_string( entry.waiters.size() ) + " still waiting)" );
            return;
        }
        m_inflight.erase( entryIt );
    }

    // ---- admission
    // ------------------------------------------------------------------------------------------------

    void Scheduler::GrantLease( ClientId id, Client& client, uint64_t estimate, Now now, Outbox& out )
    {
        Action& action         = *client.action;
        Build*  build          = FindBuild( client.buildKey );
        action.state           = Action::State::Running;
        action.lease           = m_nextLease++;
        action.grantMs         = now.ms;
        action.estimate        = estimate;
        action.currentBytes    = 0;
        action.cpuMs           = 0;
        action.preemptSent     = false;
        m_leases[action.lease] = id;
        m_running += 1;
        OsPriority priority = OsPriority::Normal;
        if ( build != nullptr )
        {
            build->running += 1;
            build->run.waitedMsTotal += now.ms - action.queuedSinceMs;
            priority = ClassOf( *build ).osPriority;
        }
        out.sends.emplace_back( id, Message{ buildpool::Grant{ action.lease, priority, m_config.sampleMs } } );
        ObserveAllBuilds();
    }

    void Scheduler::Admit( Now now, Outbox& out )
    {
        if ( m_draining || !m_memory )
            return;

        const uint64_t free   = FreeNow();
        const bool     closed = free < m_config.reserveBytes || m_memory->osPressure;
        if ( closed != m_guardClosed )
        {
            m_guardClosed = closed;
            out.log.push_back( closed ? "guard: admission stopped, free " + Mb( free ) + " MB, reserve " +
                                             Mb( m_config.reserveBytes ) + " MB, os_pressure " +
                                             ( m_memory->osPressure ? "yes" : "no" )
                                      : "guard: admission resumed, free " + Mb( free ) + " MB" );
        }
        if ( closed )
            return;

        int64_t headroom = HeadroomNow();

        // Slow start: a cost class never recorded runs one action at a time.
        std::unordered_map<std::string, uint32_t> runningPerCostClass;
        for ( const auto& [lease, id] : m_leases )
        {
            const auto it = m_clients.find( id );
            if ( it != m_clients.end() && it->second.action )
                runningPerCostClass[std::string( CostClassOf( it->second.action->request.costKey ) )] += 1;
        }
        const auto slowStartBlocked = [&]( const std::string& costKey )
        {
            if ( m_ledger.ClassSeen( costKey ) )
                return false;
            const auto it = runningPerCostClass.find( std::string( CostClassOf( costKey ) ) );
            return it != runningPerCostClass.end() && it->second > 0;
        };

        struct Candidate
        {
            ClientId id           = 0;
            Client*  client       = nullptr;
            bool     guaranteed   = false;
            int      priority     = 0;
            uint32_t buildRunning = 0;
            int64_t  order        = 0;
            uint64_t estimate     = 0;
        };
        std::vector<Candidate> candidates;
        const uint32_t         guaranteeCap = std::max<uint32_t>( 1, m_slots / 2 );

        // Order: a Queued leader of an inflightKey is served at min(own class priority, the best class priority
        // among that key's Awaiting waiters), so dedup never inverts the order. Read from the waiter list on
        // every pass; only the priority term is lent.
        const auto orderPriority = [&]( const Client& client, int ownPriority )
        {
            const std::string& key = client.action->request.inflightKey;
            if ( key.empty() || client.session == 0 )
                return ownPriority;
            const auto entry = m_inflight.find( key );
            if ( entry == m_inflight.end() || entry->second.leaderSession != client.session )
                return ownPriority;
            int best = ownPriority;
            for ( const ClientId waiterId : entry->second.waiters )
            {
                const auto waiter = m_clients.find( waiterId );
                if ( waiter == m_clients.end() )
                    continue;
                const Build* waiterBuild = FindBuild( waiter->second.buildKey );
                if ( waiterBuild != nullptr )
                    best = std::min( best, ClassOf( *waiterBuild ).priority );
            }
            return best;
        };

        while ( m_running < m_slots )
        {
            uint32_t guaranteedInUse = 0;
            for ( const auto& [key, build] : m_builds )
                guaranteedInUse += std::min( build.running, ClassOf( build ).guaranteedSlots );

            candidates.clear();
            for ( auto& [id, client] : m_clients )
            {
                if ( !client.action || client.action->state != Action::State::Queued )
                    continue;
                const Build* build = FindBuild( client.buildKey );
                if ( build == nullptr )
                    continue;
                const std::string& costKey = client.action->request.costKey;
                if ( slowStartBlocked( costKey ) )
                    continue;
                const ClassPolicy& policy = ClassOf( *build );
                Candidate          candidate;
                candidate.id           = id;
                candidate.client       = &client;
                candidate.guaranteed   = build->running < policy.guaranteedSlots && guaranteedInUse < guaranteeCap;
                candidate.priority     = orderPriority( client, policy.priority );
                candidate.buildRunning = build->running;
                candidate.order        = client.action->order;
                candidate.estimate     = m_ledger.Estimate( costKey );
                candidates.push_back( candidate );
            }
            if ( candidates.empty() )
                return;
            std::sort( candidates.begin(), candidates.end(),
                       []( const Candidate& a, const Candidate& b )
                       {
                           if ( a.guaranteed != b.guaranteed )
                               return a.guaranteed;
                           if ( a.priority != b.priority )
                               return a.priority < b.priority;
                           if ( a.buildRunning != b.buildRunning )
                               return a.buildRunning < b.buildRunning;
                           return a.order < b.order;
                       } );

            const Candidate& head         = candidates.front();
            const auto       headEstimate = static_cast<int64_t>( head.estimate );
            if ( headEstimate <= headroom || m_running == 0 )
            {
                if ( headEstimate > headroom )
                    out.log.push_back(
                         "admitted over headroom with nothing running: " + head.client->action->request.costKey +
                         " planned " + Mb( head.estimate ) + " MB, headroom " + SignedMb( headroom ) + " MB" );
                runningPerCostClass[std::string( CostClassOf( head.client->action->request.costKey ) )] += 1;
                GrantLease( head.id, *head.client, head.estimate, now, out );
                headroom -= headEstimate;
                continue;
            }

            // EASY backfilling: plan the head's start from the running leases' wall times; a later action goes
            // first only if it fits now and ends before that start or leaves the head's estimate free at it.
            struct Ending
            {
                uint64_t atMs         = 0;
                int64_t  returnsBytes = 0;
            };
            std::vector<Ending> endings;
            endings.reserve( m_leases.size() );
            for ( const auto& [lease, id] : m_leases )
            {
                const auto it = m_clients.find( id );
                if ( it == m_clients.end() || !it->second.action )
                    continue;
                const Action&  running = *it->second.action;
                const uint64_t end =
                     std::max( now.ms, running.grantMs + m_ledger.WallMs( running.request.costKey ) );
                endings.push_back(
                     { end, static_cast<int64_t>( std::max( running.estimate, running.currentBytes ) ) } );
            }
            std::sort( endings.begin(), endings.end(),
                       []( const Ending& a, const Ending& b ) { return a.atMs < b.atMs; } );
            int64_t  available = headroom;
            uint64_t shadowMs  = now.ms;
            bool     headFits  = false;
            for ( const Ending& ending : endings )
            {
                available += ending.returnsBytes;
                shadowMs = ending.atMs;
                if ( available >= headEstimate )
                {
                    headFits = true;
                    break;
                }
            }
            // When the head fits only once everything has ended, it starts then (with nothing running it is
            // admitted whatever its estimate), and nothing may be planned into its memory.
            int64_t extra = headFits ? available - headEstimate : 0;

            for ( size_t i = 1; i < candidates.size() && m_running < m_slots; ++i )
            {
                const Candidate& candidate = candidates[i];
                const auto       estimate  = static_cast<int64_t>( candidate.estimate );
                if ( estimate > headroom )
                    continue;
                const std::string& costKey = candidate.client->action->request.costKey;
                if ( slowStartBlocked( costKey ) )
                    continue;
                const bool endsBeforeHead = now.ms + m_ledger.WallMs( costKey ) <= shadowMs;
                if ( !endsBeforeHead && estimate > extra )
                    continue;
                runningPerCostClass[std::string( CostClassOf( costKey ) )] += 1;
                GrantLease( candidate.id, *candidate.client, candidate.estimate, now, out );
                headroom -= estimate;
                if ( !endsBeforeHead )
                    extra -= estimate;
            }
            return;
        }
    }

    void Scheduler::Guard( Now now, Outbox& out )
    {
        if ( !m_memory )
            return;
        const uint64_t free = FreeNow();
        if ( free >= m_config.reserveBytes / 2 )
            return;

        Client*  victim         = nullptr;
        int      victimPriority = 0;
        uint64_t victimCpuMs    = 0;
        ClientId victimId       = 0;
        for ( const auto& [lease, id] : m_leases )
        {
            const auto it = m_clients.find( id );
            if ( it == m_clients.end() || !it->second.action )
                continue;
            const Action& action = *it->second.action;
            if ( action.preemptSent )
                return; // one Preempt at a time: wait for its Released
            if ( !action.request.restartable || action.preemptions >= 2 )
                continue;
            const Build* build    = FindBuild( it->second.buildKey );
            const int    priority = build != nullptr ? ClassOf( *build ).priority : 0;
            const bool better = victim == nullptr || priority > victimPriority ||
                                ( priority == victimPriority && action.cpuMs < victimCpuMs );
            if ( better )
            {
                victim         = &it->second;
                victimPriority = priority;
                victimCpuMs    = action.cpuMs;
                victimId       = id;
            }
        }
        if ( victim == nullptr )
        {
            if ( now.ms - m_lastNoVictimLogMs >= kRunIdleMs )
            {
                m_lastNoVictimLogMs = now.ms;
                out.log.push_back( "guard: free " + Mb( free ) + " MB is below reserve/2 (" +
                                   Mb( m_config.reserveBytes / 2 ) + " MB) and no lease may be preempted" );
            }
            return;
        }
        Action& action     = *victim->action;
        action.preemptSent = true;
        out.sends.emplace_back( victimId, Message{ Preempt{ action.lease } } );
        out.log.push_back( "preempt lease " + std::to_string( action.lease ) + ": " + action.request.costKey +
                           " (class priority " + std::to_string( victimPriority ) + ", cpu " +
                           std::to_string( victimCpuMs ) + " ms, now " + Mb( action.currentBytes ) +
                           " MB, planned " + Mb( action.estimate ) + " MB); free " + Mb( free ) +
                           " MB below reserve/2 " + Mb( m_config.reserveBytes / 2 ) + " MB" );
    }

    // ---- runs and draining
    // ----------------------------------------------------------------------------------------

    void Scheduler::RetireIdleBuilds( Now now, Outbox& out )
    {
        for ( auto it = m_builds.begin(); it != m_builds.end(); )
        {
            Build& build = it->second;
            if ( build.clients == 0 && build.running == 0 && now.ms - build.idleSinceMs >= kRunIdleMs )
            {
                build.run.endUnix = now.unixSeconds;
                out.log.push_back( "build closed: " + build.run.buildRoot +
                                   " actions=" + std::to_string( build.run.actions ) +
                                   " waited_ms=" + std::to_string( build.run.waitedMsTotal ) +
                                   " preemptions=" + std::to_string( build.run.preemptions ) );
                out.finishedRuns.push_back( std::move( build.run ) );
                it = m_builds.erase( it );
            }
            else
                ++it;
        }
    }

    void Scheduler::CloseAllRuns( Now now, Outbox& out )
    {
        for ( auto& [key, build] : m_builds )
        {
            build.run.endUnix = now.unixSeconds;
            out.finishedRuns.push_back( std::move( build.run ) );
        }
        m_builds.clear();
    }

    void Scheduler::StartDrain( const std::string& reason, Outbox& out )
    {
        if ( m_draining )
            return;
        m_draining    = true;
        m_drainReason = reason;
        out.log.push_back( "draining: " + reason + "; nothing new is admitted, " + std::to_string( m_running ) +
                           " lease(s) to finish" );
    }

    void Scheduler::CheckDrained( Outbox& out )
    {
        if ( !m_draining || m_running != 0 || out.exit )
            return;
        for ( const auto& [id, client] : m_clients )
            if ( client.waitsForStop )
                out.sends.emplace_back( id, Message{ Text{ "buildpool daemon " + m_identity + " stopped" } } );
        out.log.push_back( "drained: exiting" );
        out.exit = true;
    }

    // ---- status
    // ---------------------------------------------------------------------------------------------------

    std::string Scheduler::StatusText( Now now ) const
    {
        std::ostringstream text;
        text << "daemon " << m_identity << "  protocol " << kProtocolVersion << "\n";
        text << "slots " << m_running << "/" << m_slots << "  window " << m_slots * m_config.windowFactor;
        if ( m_memory )
            text << "  free " << Mb( FreeNow() ) << " MB  reserve " << Mb( m_config.reserveBytes )
                 << " MB  headroom " << SignedMb( HeadroomNow() ) << " MB  os_pressure "
                 << ( m_memory->osPressure ? "yes" : "no" );
        text << "\nadmission ";
        if ( m_draining )
            text << "draining (" << m_drainReason << ")";
        else if ( !m_memory )
            text << "stopped: no memory reading (" << m_memoryError << ")";
        else if ( m_guardClosed )
            text << "stopped by the guard (free memory below reserve or OS pressure)";
        else
            text << "open";
        text << "\n";

        text << "builds " << m_builds.size() << "\n";
        for ( const auto& [key, build] : m_builds )
        {
            uint32_t queued   = 0;
            uint32_t awaiting = 0;
            for ( const auto& [id, client] : m_clients )
                if ( client.buildKey == key && client.action )
                {
                    queued += client.action->state == Action::State::Queued ? 1 : 0;
                    awaiting += client.action->state == Action::State::Awaiting ? 1 : 0;
                }
            text << "  " << build.run.buildRoot
                 << "  run=" << ( build.run.runLabel.empty() ? "-" : build.run.runLabel )
                 << "  class=" << ClassOf( build ).name << "  clients=" << build.clients
                 << "  running=" << build.running << "  queued=" << queued << "  awaiting=" << awaiting
                 << "  actions=" << build.run.actions << "  cache_reruns=" << build.run.cacheReruns
                 << "  waited_ms=" << build.run.waitedMsTotal << "  preemptions=" << build.run.preemptions
                 << "  age_s=" << ( now.ms - build.startMs ) / 1000 << "\n";
        }

        text << "leases " << m_leases.size() << "\n";
        std::vector<std::pair<uint64_t, ClientId>> leases( m_leases.begin(), m_leases.end() );
        std::sort( leases.begin(), leases.end() );
        for ( const auto& [lease, id] : leases )
        {
            const auto it = m_clients.find( id );
            if ( it == m_clients.end() || !it->second.action )
                continue;
            const Action& action = *it->second.action;
            const auto    build  = m_builds.find( it->second.buildKey );
            text << "  #" << lease << "  "
                 << ( build != m_builds.end() ? build->second.run.buildRoot : std::string( "?" ) ) << "  "
                 << action.request.costKey << "  planned=" << Mb( action.estimate )
                 << " MB  now=" << Mb( action.currentBytes ) << " MB  peak=" << Mb( action.observedBytes )
                 << " MB  age_s=" << ( now.ms - action.grantMs ) / 1000
                 << ( action.request.restartable ? "" : "  not-restartable" )
                 << ( action.preemptSent ? "  PREEMPTING" : "" ) << "\n";
        }

        size_t waiters = 0;
        for ( const auto& [key, entry] : m_inflight )
            waiters += entry.waiters.size();
        text << "inflight keys " << m_inflight.size() << ", awaiting " << waiters << "\n";
        text << "ledger keys " << m_ledger.KeyCount() << "\n";
        return text.str();
    }
} // namespace buildpool::daemon
