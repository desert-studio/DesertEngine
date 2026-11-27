// BuildPool client: the Front - what ninja runs for one compile step. It owns the step's session, starts the
// cache with this executable as "the compiler" (every Back the cache starts joins the session), and runs the
// cache a second time when the daemon says one of its Backs was answered Retry: the other session's result is
// in the cache by then, and the second run is an ordinary hit. Without a cache the Front runs the compiler
// itself under a lease.
#include "Client.hpp"

#include <optional>
#include <string>
#include <variant>

namespace buildpool::client
{
    namespace
    {
        constexpr uint32_t kVerdictWaitMs = 60'000;

        struct CacheRun
        {
            std::vector<std::string>                         argv;
            std::vector<std::pair<std::string, std::string>> environment;
        };

        Result<int> RunCache( const CacheRun& run, bool retried ) noexcept
        {
            ChildSpec spec;
            spec.argv        = run.argv;
            spec.environment = run.environment;
            spec.environment.emplace_back( kEnvRetried, retried ? "1" : "" );
            auto job = ChildJob::Spawn( spec );
            if ( !job )
                return Fail( "cannot start the cache " + run.argv.front() + ": " + job.error().message );
            for ( ;; )
            {
                auto waited = job.value().Wait( kVerdictWaitMs );
                if ( !waited )
                    return Fail( "cannot wait for the cache: " + waited.error().message );
                if ( waited.value() )
                    return *waited.value();
            }
        }

        // Whether the cache must run again. A daemon that is gone answers nothing: the cache's own exit code
        // tells, since only a Back answered Retry ends with kRetryExitCode.
        bool RunAgain( DaemonLink& link, int exitCode ) noexcept
        {
            if ( !link.Send( Message{ CacheExited{ exitCode } } ) )
                return exitCode == kRetryExitCode;
            for ( ;; )
            {
                auto received = link.Receive( kVerdictWaitMs );
                if ( !received )
                    return exitCode == kRetryExitCode;
                if ( !received.value() )
                    continue;
                if ( const auto* verdict = std::get_if<Verdict>( &*received.value() ) )
                    return verdict->runCacheAgain;
            }
        }

        // Whether a --cache-env value asks for the compiler's identity.
        bool NeedsCompilerId( const Options& options ) noexcept
        {
            for ( const auto& [name, value] : options.cacheEnv )
            {
                if ( value.find( "{compiler_id}" ) != std::string::npos )
                    return true;
            }
            return false;
        }

        Result<CacheRun> PrepareCache( const Options& options, const std::filesystem::path& compiler,
                                       uint64_t session, std::string_view compilerId ) noexcept
        {
            auto self = SelfExecutable();
            if ( !self )
                return self.error();

            CacheRun run;
            run.argv = options.cache;
            run.argv.push_back( PathText( self.value() ) );
            for ( size_t i = 1; i < options.command.size(); ++i )
                run.argv.push_back( options.command[i] );

            run.environment = {
                 { kEnvStage, "back" },
                 { kEnvSession, std::to_string( session ) },
                 { kEnvCompiler, PathText( compiler ) },
                 { kEnvHome, PathText( options.home ) },
                 { kEnvSourceRoot, options.roots.source },
                 { kEnvBuildRoot, options.roots.build },
            };
            for ( const auto& [name, value] : options.cacheEnv )
                run.environment.emplace_back( name, ExpandCompilerId( value, compilerId ) );
            return run;
        }

        // The compiler under a lease of this Front, without the cache. A command the parser cannot read (an
        // unreadable response file) still compiles: it is admitted as Other.
        int RunDirect( DaemonLink& link, const Options& options, const std::filesystem::path& compiler ) noexcept
        {
            std::vector<std::string> command = options.command;
            command.front()                  = PathText( compiler );
            Invocation invocation;
            if ( auto parsed = ParseInvocation( command ); parsed )
                invocation = std::move( parsed ).take();
            else
                Report( "cannot classify the command, running it as Other: " + parsed.error().message );
            Acquire acquire;
            acquire.kind        = invocation.action;
            acquire.costKey     = CostKey( invocation, options.roots );
            acquire.restartable = invocation.restartable;
            ChildSpec spec;
            spec.argv = std::move( command );
            return RunLeased( link, acquire, spec, nullptr );
        }
    } // namespace

    int RunFront( const Options& options ) noexcept
    {
        auto compiler = ResolveProgram( options.command.front() );
        if ( !compiler )
            return Report( compiler.error().message );

        auto opened =
             DaemonLink::Open( options.home, FrontHello( options.roots.build ), DaemonLink::Start::IfMissing );
        if ( !opened )
            return Report( opened.error().message );
        DaemonLink& link = opened.value();

        if ( options.cache.empty() )
            return RunDirect( link, options, compiler.value() );

        // The identity keys the cache; a cache run without it could hit another compiler's object, so a compiler
        // that cannot be identified compiles without the cache.
        std::string compilerId;
        if ( NeedsCompilerId( options ) )
        {
            auto id = CompilerId( compiler.value() );
            if ( !id )
            {
                Report( "cannot identify the compiler " + PathText( compiler.value() ) +
                        ", compiling without the cache: " + id.error().message );
                return RunDirect( link, options, compiler.value() );
            }
            compilerId = std::move( id ).take();
        }

        auto run = PrepareCache( options, compiler.value(), link.GetWelcome().session, compilerId );
        if ( !run )
            return Report( run.error().message );

        auto first = RunCache( run.value(), false );
        if ( !first )
            return Report( first.error().message );
        if ( !RunAgain( link, first.value() ) )
            return first.value();

        auto second = RunCache( run.value(), true );
        if ( !second )
            return Report( second.error().message );
        // The daemon closes the session's accounting on this CacheExited; its Verdict changes nothing now.
        RunAgain( link, second.value() );
        return second.value();
    }
} // namespace buildpool::client
