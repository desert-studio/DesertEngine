// BuildPool client: the Back - the process the cache starts as "the compiler" - and RunLeased, the one way any
// stage runs a tool under a lease. A preprocess Back leaves the digest of its text on the session (Note); the
// compile Back that follows asks for its lease under the in-flight key built from it, and when another session
// is already compiling the same thing it waits without a slot and exits with kRetryExitCode, so the Front runs
// the cache again and gets the other session's result as an ordinary hit.
#include "Client.hpp"

#include "BuildPool/Daemon.hpp"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <fstream>
#include <variant>

namespace buildpool::client
{
    namespace
    {
        // How long one Receive blocks while an action waits for its lease (the wait itself has no limit).
        constexpr uint32_t kLeaseWaitMs = 60'000;
        constexpr size_t   kStreamChunk = 1 << 20;

        struct RunEnd
        {
            int  exitCode  = 0;
            bool preempted = false;
        };

        // The daemon's answer to an Acquire: a Grant, or empty for Retry. A daemon lost while the action waits
        // is replaced (DaemonLink::Reopen) and asked again.
        Result<std::optional<Grant>> AwaitLease( DaemonLink& link, const Acquire& acquire,
                                                 bool sendAcquire ) noexcept
        {
            for ( ;; )
            {
                if ( sendAcquire )
                {
                    if ( !link.Send( Message{ acquire } ) )
                    {
                        if ( Status reopened = link.Reopen(); !reopened )
                            return reopened.error();
                        continue;
                    }
                    sendAcquire = false;
                }
                auto received = link.Receive( kLeaseWaitMs );
                if ( !received )
                {
                    if ( link.Connected() )
                        return received.error();
                    if ( Status reopened = link.Reopen(); !reopened )
                        return reopened.error();
                    sendAcquire = true;
                    continue;
                }
                if ( !received.value() )
                    continue;
                const Message& message = *received.value();
                if ( const auto* grant = std::get_if<Grant>( &message ) )
                    return std::optional<Grant>( *grant );
                if ( std::holds_alternative<Retry>( message ) )
                    return std::optional<Grant>{};
                if ( const auto* refused = std::get_if<Refused>( &message ) )
                    return Fail( "the daemon refused the action: " + refused->reason );
                // Await only informs; a Preempt naming a lease this client already released is stale.
                if ( std::holds_alternative<Await>( message ) || std::holds_alternative<Preempt>( message ) )
                    continue;
                return Fail( "the daemon sent an unexpected message while the action waited for its lease" );
            }
        }

        Status WriteStdout( std::span<const std::byte> bytes ) noexcept
        {
            if ( std::fwrite( bytes.data(), 1, bytes.size(), stdout ) != bytes.size() )
                return Fail( "cannot pass the preprocessed text on to stdout" );
            return Ok();
        }

        // Runs until the job exits or the daemon preempts it: a Heartbeat every `sampleMs` and a look for Preempt
        // right after it. A daemon that is gone does not stop the job; it runs on unaccounted.
        Result<RunEnd> Supervise( DaemonLink& link, ChildJob& job, uint64_t lease, uint32_t sampleMs,
                                  PreprocessedDigest* tee ) noexcept
        {
            std::vector<std::byte> buffer( tee != nullptr ? kStreamChunk : 0 );
            bool                   streamOpen = tee != nullptr;
            uint64_t               nextBeat   = MonotonicMs() + sampleMs;
            for ( ;; )
            {
                const uint64_t now  = MonotonicMs();
                const uint32_t wait = nextBeat > now ? static_cast<uint32_t>( nextBeat - now ) : 0;
                if ( streamOpen )
                {
                    auto read = job.ReadStdout( buffer, wait );
                    if ( !read )
                        return Fail( "cannot read the tool's output: " + read.error().message );
                    if ( read.value() )
                    {
                        const size_t count = *read.value();
                        if ( count == 0 )
                            streamOpen = false;
                        else
                        {
                            const std::span<const std::byte> chunk( buffer.data(), count );
                            if ( Status written = WriteStdout( chunk ); !written )
                                return written.error();
                            tee->Feed( chunk );
                        }
                    }
                }
                else
                {
                    auto waited = job.Wait( wait );
                    if ( !waited )
                        return Fail( "cannot wait for the tool: " + waited.error().message );
                    if ( waited.value() )
                        return RunEnd{ *waited.value(), false };
                }

                if ( MonotonicMs() < nextBeat )
                    continue;
                nextBeat = MonotonicMs() + sampleMs;
                if ( !link.Connected() )
                    continue;
                link.Send( Message{ Heartbeat{ job.Usage().currentBytes } } );
                for ( ;; )
                {
                    auto received = link.Receive( 0 );
                    if ( !received || !received.value() )
                        break;
                    const auto* preempt = std::get_if<Preempt>( &*received.value() );
                    if ( preempt != nullptr && preempt->lease == lease )
                    {
                        job.Terminate();
                        return RunEnd{ 1, true };
                    }
                }
            }
        }

        Status DigestFile( const std::filesystem::path& path, PreprocessedDigest& digest ) noexcept
        {
            std::ifstream file( path, std::ios::binary );
            if ( !file )
                return Fail( "cannot read the preprocessed output " + PathText( path ) );
            std::vector<char> buffer( kStreamChunk );
            while ( file )
            {
                file.read( buffer.data(), static_cast<std::streamsize>( buffer.size() ) );
                const std::streamsize count = file.gcount();
                if ( count > 0 )
                    digest.Feed( std::span<const std::byte>( reinterpret_cast<const std::byte*>( buffer.data() ),
                                                             static_cast<size_t>( count ) ) );
            }
            if ( file.bad() )
                return Fail( "cannot read the preprocessed output " + PathText( path ) );
            return Ok();
        }

        Result<std::string> StageVariable( const char* name ) noexcept
        {
            std::optional<std::string> value = GetEnv( name );
            if ( !value || value->empty() )
                return Fail( std::string( "BUILDPOOL_STAGE=back without " ) + name +
                             ": a Back is started only by the cache a buildpool Front runs" );
            return std::move( *value );
        }

        int RunPreprocess( DaemonLink& link, const Acquire& acquire, const ChildSpec& spec,
                           const Invocation& invocation, const TreeRoots& roots ) noexcept
        {
            if ( invocation.preprocessToStdout )
            {
#if defined( _WIN32 )
                // Platform.hpp has no binary stdout: the C runtime's text-mode stdout would turn every "\n" of the
                // passed-on text into "\r\n". The child writes to the inherited stdout itself; this session gets
                // no Note, so its compile runs without dedup (ccache's MSVC mode preprocesses to a file, /P /Fi).
                return RunLeased( link, acquire, spec, nullptr );
#else
                PreprocessedDigest digest( roots );
                const int          exitCode = RunLeased( link, acquire, spec, &digest );
                std::fflush( stdout );
                if ( exitCode == 0 )
                    link.Send( Message{ Note{ digest.Finish() } } );
                return exitCode;
#endif
            }
            const int exitCode = RunLeased( link, acquire, spec, nullptr );
            if ( exitCode != 0 )
                return exitCode;
            PreprocessedDigest digest( roots );
            if ( Status read = DigestFile( PathFrom( invocation.preprocessOutput ), digest ); !read )
                return Report( read.error().message );
            link.Send( Message{ Note{ digest.Finish() } } );
            return 0;
        }
    } // namespace

    int RunLeased( DaemonLink& link, const Acquire& acquire, const ChildSpec& spec,
                   PreprocessedDigest* tee ) noexcept
    {
        // The client never reads <home>/pool.ini: the daemon is its only reader and writer. The Heartbeat period
        // is the contract's PoolConfig::sampleMs default; a Heartbeat only refreshes the daemon's memory sample of
        // the lease, the daemon's own Guard tick runs on the configured value. The configured value reaches the
        // client once Grant carries it (NEEDS: Grant::sampleMs in Protocol.hpp).
        const uint32_t kSampleMs   = std::max<uint32_t>( PoolConfig{}.sampleMs, 1 );
        bool           sendAcquire = true;
        for ( ;; )
        {
            auto lease = AwaitLease( link, acquire, sendAcquire );
            if ( !lease )
                return Report( lease.error().message );
            if ( !lease.value() )
                return kRetryExitCode;
            const Grant grant = *lease.value();

            ChildSpec run        = spec;
            run.priority         = grant.priority;
            run.captureStdout    = tee != nullptr;
            const uint64_t start = MonotonicMs();
            auto           job   = ChildJob::Spawn( run );
            if ( !job )
            {
                link.Send( Message{ Released{ 1, 0, MonotonicMs() - start, 0, false } } );
                return Report( "cannot start " + spec.argv.front() + ": " + job.error().message );
            }

            auto end = Supervise( link, job.value(), grant.lease, kSampleMs, tee );
            if ( !end )
                job.value().Terminate();
            const JobUsage usage = job.value().Usage();
            Released       released;
            released.exitCode  = end ? end.value().exitCode : 1;
            released.peakBytes = usage.peakBytes;
            released.wallMs    = MonotonicMs() - start;
            released.cpuMs     = usage.cpuMs;
            released.preempted = end && end.value().preempted;
            link.Send( Message{ released } );
            if ( !end )
                return Report( end.error().message );
            if ( !released.preempted )
                return released.exitCode;
            // Preempted: the daemon has put the action back at the head of its build's queue; wait for the next
            // Grant without asking again.
            sendAcquire = false;
        }
    }

    int RunBack( int argc, char** argv ) noexcept
    {
        auto home       = StageVariable( kEnvHome );
        auto session    = StageVariable( kEnvSession );
        auto compiler   = StageVariable( kEnvCompiler );
        auto sourceRoot = StageVariable( kEnvSourceRoot );
        auto buildRoot  = StageVariable( kEnvBuildRoot );
        for ( const Result<std::string>* variable : { &home, &session, &compiler, &sourceRoot, &buildRoot } )
        {
            if ( !*variable )
                return Report( variable->error().message );
        }
        uint64_t           sessionId   = 0;
        const std::string& sessionText = session.value();
        const auto [end, error] =
             std::from_chars( sessionText.data(), sessionText.data() + sessionText.size(), sessionId );
        if ( error != std::errc() || end != sessionText.data() + sessionText.size() )
            return Report( std::string( kEnvSession ) + "=" + sessionText + " is not a session id" );

        const TreeRoots          roots{ sourceRoot.value(), buildRoot.value() };
        std::vector<std::string> command;
        command.reserve( static_cast<size_t>( argc ) );
        command.push_back( compiler.value() );
        for ( int i = 1; i < argc; ++i )
            command.emplace_back( argv[i] );

        // A command the parser cannot read (an unreadable response file) still compiles: it is admitted as Other,
        // the classification only steers scheduling.
        Invocation invocation;
        if ( auto parsed = ParseInvocation( command ); parsed )
            invocation = std::move( parsed ).take();
        else
            Report( "cannot classify the command, running it as Other: " + parsed.error().message );

        Hello hello   = FrontHello( roots.build );
        hello.stage   = Stage::Back;
        hello.session = sessionId;
        auto opened =
             DaemonLink::Open( PathFrom( home.value() ), std::move( hello ), DaemonLink::Start::IfMissing );
        if ( !opened )
            return Report( opened.error().message );
        DaemonLink& link = opened.value();

        Acquire acquire;
        acquire.kind        = invocation.action;
        acquire.costKey     = CostKey( invocation, roots );
        acquire.restartable = invocation.restartable;
        acquire.retried     = GetEnv( kEnvRetried ).value_or( std::string() ) == "1";
        ChildSpec spec;
        spec.argv = command;

        if ( invocation.action == ActionKind::Preprocess )
            return RunPreprocess( link, acquire, spec, invocation, roots );

        const bool compiles =
             invocation.action == ActionKind::Compile || invocation.action == ActionKind::PchCreate;
        const std::string& note = link.GetWelcome().note;
        if ( compiles && !note.empty() )
        {
            // The inflight key is a scheduling hint, never trusted for correctness: without the compiler's
            // identity the action asks without one and compiles on its own.
            if ( auto id = CompilerId( PathFrom( compiler.value() ) ); id )
                acquire.inflightKey = InflightKey( command, roots, id.value(), note );
            else
                Report( "cannot identify the compiler " + compiler.value() +
                        ", compiling without the inflight key: " + id.error().message );
        }
        return RunLeased( link, acquire, spec, nullptr );
    }
} // namespace buildpool::client
