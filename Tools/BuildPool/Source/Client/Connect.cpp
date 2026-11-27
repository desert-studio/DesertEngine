// BuildPool client: the connection to the daemon of a pool home. The first client that finds no daemon starts
// one detached (SpawnDetached); every client that loses its daemon connects again, and the first of them starts
// the next one. Starting is idempotent: a second daemon finds the endpoint taken and exits ("already running").
#include "Client.hpp"

#include <chrono>
#include <thread>
#include <variant>

namespace buildpool::client
{
    namespace
    {
        constexpr uint32_t kConnectAttemptMs = 2000;
        // How long a client keeps trying to reach a daemon it started (or that is restarting) before it fails.
        constexpr uint64_t kReachMs = 30'000;
        // A daemon started by this client that still does not listen after this long is started again.
        constexpr uint64_t kRespawnMs = 5'000;
        // How long an older daemon that refused this client is given to drain its leases and exit.
        constexpr uint64_t kDrainMs   = 15 * 60'000;
        constexpr uint32_t kWelcomeMs = 30'000;
        // The daemon's own stdout and stderr (its log proper is daemon.log, written by the daemon).
        constexpr const char* kConsoleLog = "daemon.console.log";

        void Pause( uint32_t ms )
        {
            std::this_thread::sleep_for( std::chrono::milliseconds( ms ) );
        }

        Status SpawnDaemon( const std::filesystem::path& home ) noexcept
        {
            auto self = SelfExecutable();
            if ( !self )
                return self.error();
            std::error_code error;
            std::filesystem::create_directories( home, error );
            if ( error )
                return Fail( "cannot create the pool home " + PathText( home ) + ": " + error.message() );
            return SpawnDetached( { PathText( self.value() ), "daemon", "--home", PathText( home ) },
                                  home / kConsoleLog );
        }

        // The daemon's own protocol number a version refusal names, when it names one. The reason reads
        // "the client speaks protocol <client>, this daemon protocol <daemon>" (Daemon/Scheduler.cpp), so the
        // number after "daemon protocol " is the daemon's; the first "protocol <n>" is this client's own.
        std::optional<uint64_t> RefusedProtocol( std::string_view reason )
        {
            constexpr std::string_view kWord = "daemon protocol ";
            const size_t               at    = reason.find( kWord );
            if ( at == std::string_view::npos )
                return std::nullopt;
            size_t   i      = at + kWord.size();
            uint64_t number = 0;
            bool     digits = false;
            for ( ; i < reason.size() && reason[i] >= '0' && reason[i] <= '9'; ++i )
            {
                number = number * 10 + static_cast<uint64_t>( reason[i] - '0' );
                digits = true;
            }
            return digits ? std::optional<uint64_t>( number ) : std::nullopt;
        }
    } // namespace

    DaemonLink::DaemonLink( std::filesystem::path home, Hello hello, Start start ) noexcept
         : m_home( std::move( home ) ), m_hello( std::move( hello ) ), m_start( start )
    {
    }

    Result<DaemonLink> DaemonLink::Open( std::filesystem::path home, Hello hello, Start start ) noexcept
    {
        DaemonLink link( std::move( home ), std::move( hello ), start );
        if ( Status connected = link.Connect(); !connected )
            return connected.error();
        return Result<DaemonLink>( std::move( link ) );
    }

    Status DaemonLink::Reopen() noexcept
    {
        return Connect();
    }

    Status DaemonLink::Connect() noexcept
    {
        m_connection.reset();
        const std::string       endpoint      = EndpointFor( m_home );
        const std::string       home          = PathText( m_home );
        const uint64_t          begin         = MonotonicMs();
        uint64_t                reachDeadline = begin + kReachMs;
        const uint64_t          drainDeadline = begin + kDrainMs;
        std::optional<uint64_t> spawnedAt;

        for ( ;; )
        {
            const uint64_t now        = MonotonicMs();
            auto           connection = Connection::Connect( endpoint, kConnectAttemptMs );
            if ( !connection )
            {
                const std::string& reason = connection.error().message;
                const bool         absent = reason.find( "no daemon" ) != std::string::npos;
                if ( absent && m_start == Start::Never )
                    return Fail( reason + " (pool home " + home + ")" );
                if ( now >= reachDeadline )
                    return Fail( "cannot reach the daemon of " + home + ": " + reason + "; its output is in " +
                                 home + "/" + kConsoleLog + " and " + home + "/daemon.log" );
                if ( absent && ( !spawnedAt || now - *spawnedAt >= kRespawnMs ) )
                {
                    if ( Status spawned = SpawnDaemon( m_home ); !spawned )
                        return Fail( "cannot start the daemon of " + home + ": " + spawned.error().message );
                    spawnedAt = now;
                }
                Pause( 50 );
                continue;
            }

            Connection                   open  = std::move( connection ).take();
            const std::vector<std::byte> hello = Encode( Message{ m_hello } );
            if ( Status sent = open.Send( hello ); !sent )
            {
                if ( MonotonicMs() >= reachDeadline )
                    return Fail( "the daemon of " + home + " closed before Hello: " + sent.error().message );
                Pause( 50 ); // the daemon went away between accept and Hello: connect again
                continue;
            }
            auto reply = open.Receive( kWelcomeMs );
            if ( !reply )
            {
                if ( MonotonicMs() >= reachDeadline )
                    return Fail( "the daemon of " + home + " closed before Welcome: " + reply.error().message );
                Pause( 50 );
                continue;
            }
            if ( !reply.value() )
                return Fail( "the daemon of " + home + " did not answer Hello within " +
                             std::to_string( kWelcomeMs / 1000 ) + " s" );
            auto message = Decode( *reply.value() );
            if ( !message )
                return Fail( "the daemon of " + home +
                             " answered Hello with bytes that do not decode: " + message.error().message );

            if ( const auto* welcome = std::get_if<Welcome>( &message.value() ) )
            {
                m_welcome = *welcome;
                m_connection.emplace( std::move( open ) );
                return Ok();
            }
            if ( const auto* refused = std::get_if<Refused>( &message.value() ) )
            {
                // A daemon of an older protocol drains and exits when a newer client says Hello (Protocol.hpp
                // kProtocolVersion); this client waits for it and starts the new one. A daemon of the same or a
                // newer protocol refuses for good.
                const std::optional<uint64_t> theirs = RefusedProtocol( refused->reason );
                const bool olderDaemon               = refused->reason.find( "protocol" ) != std::string::npos &&
                                         ( !theirs || *theirs < kProtocolVersion );
                if ( olderDaemon && MonotonicMs() < drainDeadline )
                {
                    Pause( 1000 );
                    reachDeadline = MonotonicMs() + kReachMs;
                    continue;
                }
                return Fail( "the daemon of " + home + " refused: " + refused->reason );
            }
            return Fail( "the daemon of " + home + " answered Hello with neither Welcome nor Refused" );
        }
    }

    bool DaemonLink::Send( const Message& message ) noexcept
    {
        if ( !m_connection )
            return false;
        const std::vector<std::byte> bytes = Encode( message );
        if ( !m_connection->Send( bytes ) )
        {
            m_connection.reset();
            return false;
        }
        return true;
    }

    Result<std::optional<Message>> DaemonLink::Receive( uint32_t timeoutMs ) noexcept
    {
        if ( !m_connection )
            return Fail( "the daemon is gone" );
        auto bytes = m_connection->Receive( timeoutMs );
        if ( !bytes )
        {
            m_connection.reset();
            return bytes.error();
        }
        if ( !bytes.value() )
            return std::optional<Message>{};
        auto message = Decode( *bytes.value() );
        if ( !message )
            return Fail( "a message from the daemon does not decode: " + message.error().message );
        return std::optional<Message>( std::move( message ).take() );
    }
} // namespace buildpool::client
