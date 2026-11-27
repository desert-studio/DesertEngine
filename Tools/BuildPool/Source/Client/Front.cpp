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

        Result<CacheRun> PrepareCache( const Options& options, const std::filesystem::path& compiler,
                                       uint64_t session ) noexcept
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
            // The compiler's identity is read only when a --cache-env value asks for it.
            std::optional<std::string> compilerId;
            for ( const auto& [name, value] : options.cacheEnv )
            {
                if ( value.find( "{compiler_id}" ) != std::string::npos && !compilerId )
                {
                    auto id = CompilerId( compiler );
                    if ( !id )
                        return Fail( "cannot identify the compiler " + PathText( compiler ) + ": " +
                                     id.error().message );
                    compilerId = std::move( id ).take();
                }
                run.environment.emplace_back( name, compilerId ? ExpandCompilerId( value, *compilerId ) : value );
            }
            return run;
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
        {
            std::vector<std::string> command = options.command;
            command.front()                  = PathText( compiler.value() );
            auto parsed                      = ParseInvocation( command );
            if ( !parsed )
                return Report( parsed.error().message );
            Acquire acquire;
            acquire.kind        = parsed.value().action;
            acquire.costKey     = CostKey( parsed.value(), options.roots );
            acquire.restartable = parsed.value().restartable;
            ChildSpec spec;
            spec.argv = std::move( command );
            return RunLeased( link, acquire, spec, nullptr );
        }

        auto run = PrepareCache( options, compiler.value(), link.GetWelcome().session );
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
