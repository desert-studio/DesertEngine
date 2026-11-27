// RunDaemon: the one thread that owns the pool. It reads the policy and the ledger, takes the endpoint (the
// single-instance lock), and then only moves events into the Scheduler and its answers onto the wire.
#include "BuildPool/Daemon.hpp"

#include "Config.hpp"
#include "Ledger.hpp"
#include "Runs.hpp"
#include "Scheduler.hpp"

#include "BuildPool/Platform.hpp"
#include "BuildPool/Protocol.hpp"

#include <cstdio>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <vector>

namespace buildpool
{
    namespace
    {
        constexpr uint64_t kLedgerFlushMs = 30'000; // Daemon.hpp: ledger.tsv "rewritten every 30 s and on stop"

        // <home>/daemon.log: one line per event, the Unix time first, flushed at once (the scenario reads it
        // live).
        class DaemonLog
        {
        public:
            explicit DaemonLog( const std::filesystem::path& file )
                 : m_out( file, std::ios::app | std::ios::binary )
            {
            }

            void Line( std::string_view text )
            {
                if ( !m_out )
                {
                    std::fprintf( stderr, "buildpool daemon: %.*s\n", static_cast<int>( text.size() ),
                                  text.data() );
                    return;
                }
                m_out << UnixSeconds() << "  " << text << '\n';
                m_out.flush();
            }

        private:
            std::ofstream m_out;
        };

        daemon::Now Clock() noexcept
        {
            return daemon::Now{ MonotonicMs(), UnixSeconds() };
        }
    } // namespace

    int RunDaemon( const std::filesystem::path& home ) noexcept
    {
        std::error_code ec;
        std::filesystem::create_directories( home, ec );
        if ( ec )
        {
            std::fprintf( stderr, "buildpool daemon: cannot create the pool home %s: %s\n", home.string().c_str(),
                          ec.message().c_str() );
            return 1;
        }
        DaemonLog log( home / "daemon.log" );

        const ProcessIdentity self = SelfProcess();
        const std::string     identity =
             "pid " + std::to_string( self.pid ) + " started " + std::to_string( UnixSeconds() );
        log.Line( "daemon " + identity + " starting; home " + home.string() + "; protocol " +
                  std::to_string( kProtocolVersion ) );

        const bool         hadConfig = std::filesystem::exists( home / "pool.ini", ec );
        Result<PoolConfig> loaded    = LoadConfig( home );
        if ( !loaded )
        {
            log.Line( "refused to start: " + loaded.error().message );
            return 1;
        }
        const PoolConfig config = std::move( loaded ).take();
        if ( !hadConfig )
            log.Line( "pool.ini was missing: written with the defaults" );
        for ( const std::string& line : daemon::DescribeConfig( config ) )
            log.Line( "config " + line );

        Result<daemon::Ledger> ledgerLoaded = daemon::Ledger::Load( home / "ledger.tsv", config.estimateMargin );
        if ( !ledgerLoaded )
        {
            log.Line( "refused to start: " + ledgerLoaded.error().message );
            return 1;
        }
        daemon::Ledger ledger = std::move( ledgerLoaded ).take();
        log.Line( "ledger: " + std::to_string( ledger.KeyCount() ) + " cost keys" );

        const std::string endpointName = EndpointFor( home );
        Result<Server>    listened     = Server::Listen( endpointName );
        if ( !listened )
        {
            const std::string& reason = listened.error().message;
            if ( reason.find( "already running" ) != std::string::npos )
            {
                log.Line( "another daemon holds " + endpointName + "; this one exits (" + reason + ")" );
                return 2;
            }
            log.Line( "cannot listen on " + endpointName + ": " + reason );
            return 1;
        }
        Server endpoint = std::move( listened ).take();

        // Session ids continue above every id an earlier daemon could have issued, so a Back that outlived its
        // daemon never names a session of this one.
        daemon::Scheduler scheduler( config, ledger, LogicalCpuCount(), identity, UnixSeconds() * 1'000'000ull );
        log.Line( "listening on " + endpointName + "; slots " + std::to_string( scheduler.Slots() ) + ", window " +
                  std::to_string( scheduler.Slots() * config.windowFactor ) );

        daemon::Outbox               out;
        std::unordered_set<ClientId> dropped;
        const std::filesystem::path  runsFile = home / "runs.tsv";

        // Moves the outbox onto the wire, the log and runs.tsv; a dropped client is closed in the scheduler at
        // once, which may produce more to deliver. Returns true when the scheduler asked to exit.
        const auto deliver = [&]() -> bool
        {
            bool exit = false;
            while ( true )
            {
                for ( const auto& [client, message] : out.sends )
                    if ( dropped.count( client ) == 0 )
                        (void)endpoint.Send( client,
                                             Encode( message ) ); // a failure is followed by its Closed event
                for ( const std::string& line : out.log )
                    log.Line( line );
                for ( const daemon::RunRecord& run : out.finishedRuns )
                {
                    Status appended = daemon::AppendRun( runsFile, run );
                    if ( !appended )
                        log.Line( "runs.tsv: " + appended.error().message );
                }
                exit                              = exit || out.exit;
                const std::vector<ClientId> drops = std::move( out.drops );
                out.Clear();
                if ( drops.empty() )
                    return exit;
                for ( const ClientId id : drops )
                    if ( dropped.insert( id ).second )
                    {
                        endpoint.Drop( id );
                        scheduler.OnClosed( id, Clock(), out );
                    }
            }
        };

        const auto saveLedger = [&]( const char* when )
        {
            if ( !ledger.Dirty() )
                return;
            Status saved = ledger.Save();
            if ( !saved )
                log.Line( std::string( "ledger.tsv not written " ) + when + ": " + saved.error().message );
        };

        int                      exitCode  = 0;
        bool                     leaving   = false;
        uint64_t                 nextTick  = MonotonicMs();
        uint64_t                 nextFlush = nextTick + kLedgerFlushMs;
        std::vector<ServerEvent> events;
        while ( !leaving )
        {
            uint64_t t = MonotonicMs();
            if ( t >= nextTick )
            {
                Result<MemoryState> memory = QueryMemory();
                if ( memory )
                    scheduler.OnTick( memory.value(), Clock(), out );
                else
                    scheduler.OnMemoryUnavailable( memory.error().message, Clock(), out );
                nextTick = t + config.sampleMs;
                if ( deliver() )
                    break;
            }
            if ( t >= nextFlush )
            {
                saveLedger( "on the 30 s flush" );
                nextFlush = t + kLedgerFlushMs;
            }

            events.clear();
            t                 = MonotonicMs();
            const auto wait   = static_cast<uint32_t>( nextTick > t ? nextTick - t : 0 );
            Status     polled = endpoint.Poll( wait, events );
            if ( !polled )
            {
                log.Line( "endpoint lost: " + polled.error().message );
                exitCode = 1;
                break;
            }
            for ( ServerEvent& event : events )
            {
                if ( dropped.count( event.client ) != 0 )
                    continue;
                switch ( event.kind )
                {
                    case ServerEvent::Kind::Connected:
                        scheduler.OnConnected( event.client, Clock(), out );
                        break;
                    case ServerEvent::Kind::Message:
                    {
                        Result<Message> message = Decode( event.message );
                        if ( message )
                            scheduler.OnMessage( event.client, message.value(), Clock(), out );
                        else
                        {
                            const std::string reason =
                                 "a message that does not decode: " + message.error().message;
                            out.log.push_back( "refused client " + std::to_string( event.client ) + ": " +
                                               reason );
                            out.sends.emplace_back( event.client, Message{ Refused{ reason } } );
                            out.drops.push_back( event.client );
                        }
                        break;
                    }
                    case ServerEvent::Kind::Closed:
                        scheduler.OnClosed( event.client, Clock(), out );
                        break;
                }
                if ( deliver() )
                {
                    leaving = true;
                    break;
                }
            }
        }

        scheduler.CloseAllRuns( Clock(), out );
        (void)deliver();
        saveLedger( "on stop" );
        log.Line( "daemon " + identity + " stopped (exit code " + std::to_string( exitCode ) + ")" );
        return exitCode;
    }
} // namespace buildpool
