// BuildPool client: RunClient (BuildPool/Invocation.hpp) - the launcher's command line, and the small helpers
// every stage shares.
#include "Client.hpp"

#include "BuildPool/Daemon.hpp"

#include <cstdio>
#include <system_error>

namespace buildpool
{
    namespace client
    {
        namespace
        {
            constexpr const char* kUsage =
                 "usage: buildpool cc --home <dir> --source-root <dir> --build-root <dir> [--cache <program> "
                 "[<argument>...]] [--cache-env <NAME=VALUE>]... -- <compiler> <arguments...>\n"
                 "       buildpool link --home <dir> --source-root <dir> --build-root <dir> -- <command...>\n"
                 "       buildpool daemon|status|window|stop --home <dir>";

            bool IsOption( std::string_view word )
            {
                return word == "--" || word == "--home" || word == "--source-root" || word == "--build-root" ||
                       word == "--cache" || word == "--cache-env";
            }

            Result<Options> ParseOptions( int argc, char** argv, int first )
            {
                Options options;
                bool home = false;
                for ( int i = first; i < argc; ++i )
                {
                    const std::string_view word  = argv[i];
                    const auto             value = [&]() -> Result<std::string>
                    {
                        if ( i + 1 >= argc )
                            return Fail( std::string( word ) + " needs a value" );
                        return std::string( argv[++i] );
                    };
                    if ( word == "--" )
                    {
                        for ( ++i; i < argc; ++i )
                            options.command.emplace_back( argv[i] );
                        break;
                    }
                    if ( word == "--home" || word == "--source-root" || word == "--build-root" )
                    {
                        auto text = value();
                        if ( !text )
                            return text.error();
                        if ( word == "--home" )
                        {
                            options.home = PathFrom( RootText( text.value() ) );
                            home         = true;
                        }
                        else if ( word == "--source-root" )
                        {
                            options.roots.source = RootText( text.value() );
                        }
                        else
                        {
                            options.roots.build = RootText( text.value() );
                        }
                    }
                    else if ( word == "--cache" )
                    {
                        auto program = value();
                        if ( !program )
                            return program.error();
                        options.cache.push_back( program.value() );
                        while ( i + 1 < argc && !IsOption( argv[i + 1] ) )
                            options.cache.emplace_back( argv[++i] );
                    }
                    else if ( word == "--cache-env" )
                    {
                        auto pair = value();
                        if ( !pair )
                            return pair.error();
                        const size_t equals = pair.value().find( '=' );
                        if ( equals == std::string::npos || equals == 0 )
                            return Fail( "--cache-env " + pair.value() + " is not NAME=VALUE" );
                        options.cacheEnv.emplace_back( pair.value().substr( 0, equals ),
                                                       pair.value().substr( equals + 1 ) );
                    }
                    else
                        return Fail( "unknown option " + std::string( word ) );
                }
                if ( !home )
                    return Fail( "--home is missing" );
                return options;
            }

            Status RequireTree( const Options& options )
            {
                if ( options.roots.source.empty() || options.roots.build.empty() )
                    return Fail( "--source-root and --build-root are required" );
                if ( options.command.empty() )
                    return Fail( "nothing to run after \"--\"" );
                return Ok();
            }
        } // namespace

        int Report( std::string_view message ) noexcept
        {
            std::fprintf( stderr, "buildpool: %.*s\n", static_cast<int>( message.size() ), message.data() );
            std::fflush( stderr );
            return 1;
        }

        std::string PathText( const std::filesystem::path& path )
        {
            const std::u8string text = path.generic_u8string();
            return std::string( text.begin(), text.end() );
        }

        std::filesystem::path PathFrom( std::string_view utf8 )
        {
            return std::filesystem::path( std::u8string( utf8.begin(), utf8.end() ) );
        }

        std::string RootText( std::string_view directory )
        {
            std::error_code       error;
            std::filesystem::path path = std::filesystem::absolute( PathFrom( directory ), error );
            if ( error )
                path = PathFrom( directory );
            std::string text = PathText( path.lexically_normal() );
            // "S:/BuildPool/" -> "S:/BuildPool", but a root ("/", "S:/") keeps its slash.
            while ( text.size() > 1 && text.back() == '/' && text[text.size() - 2] != ':' )
                text.pop_back();
            return text;
        }

        Result<std::filesystem::path> ResolveProgram( std::string_view name ) noexcept
        {
            const std::filesystem::path program = PathFrom( name );
            std::error_code             error;
            if ( program.has_parent_path() )
            {
                std::filesystem::path absolute = std::filesystem::absolute( program, error );
                if ( error )
                    return Fail( "cannot make " + std::string( name ) + " absolute: " + error.message() );
                return absolute;
            }
#if defined( _WIN32 )
            constexpr char kListSeparator = ';';
            const bool     addExe         = !program.has_extension();
#else
            constexpr char kListSeparator = ':';
            const bool     addExe         = false;
#endif
            const std::string path = GetEnv( "PATH" ).value_or( std::string() );
            size_t            from = 0;
            while ( from <= path.size() )
            {
                size_t to = path.find( kListSeparator, from );
                if ( to == std::string::npos )
                    to = path.size();
                const std::string_view directory = std::string_view( path ).substr( from, to - from );
                from                             = to + 1;
                if ( directory.empty() )
                    continue;
                for ( int variant = 0; variant < ( addExe ? 2 : 1 ); ++variant )
                {
                    std::filesystem::path candidate = PathFrom( directory ) / program;
                    if ( variant == 1 )
                        candidate += ".exe";
                    if ( std::filesystem::is_regular_file( candidate, error ) )
                        return candidate;
                }
            }
            return Fail( "cannot find " + std::string( name ) + " on PATH" );
        }

        Hello FrontHello( const std::string& buildRoot ) noexcept
        {
            Hello hello;
            hello.stage     = Stage::Front;
            hello.process   = SelfProcess();
            hello.buildRoot = buildRoot;
            hello.className = GetEnv( kEnvClass ).value_or( std::string() );
            hello.runLabel  = GetEnv( kEnvRun ).value_or( std::string() );
            return hello;
        }
    } // namespace client

    int RunClient( int argc, char** argv ) noexcept
    {
        using namespace client;

        if ( GetEnv( kEnvStage ).value_or( std::string() ) == "back" )
            return RunBack( argc, argv );

        if ( argc < 2 )
            return Report( kUsage );
        const std::string_view mode   = argv[1];
        auto                   parsed = ParseOptions( argc, argv, 2 );
        if ( !parsed )
            return Report( parsed.error().message + "\n" + kUsage );
        const Options& options = parsed.value();

        if ( mode == "cc" || mode == "link" )
        {
            if ( Status complete = RequireTree( options ); !complete )
                return Report( complete.error().message + "\n" + kUsage );
            return mode == "cc" ? RunFront( options ) : RunLink( options );
        }
        if ( mode == "daemon" )
        {
            // A daemon lives in its own home: the spawner's directory (ninja's build dir, the tree) would stay
            // open for the daemon's whole life and block deleting or renaming it.
            std::error_code       error;
            std::filesystem::path home = std::filesystem::absolute( options.home, error );
            if ( !error )
                std::filesystem::create_directories( home, error );
            if ( !error )
                std::filesystem::current_path( home, error );
            if ( error )
                return Report( "cannot enter the pool home " + PathText( options.home ) + ": " + error.message() );
            return RunDaemon( home );
        }
        if ( mode == "status" )
            return RunControl( Query::What::Status, options.home );
        if ( mode == "window" )
            return RunControl( Query::What::Window, options.home );
        if ( mode == "stop" )
            return RunControl( Query::What::Stop, options.home );
        return Report( "unknown mode " + std::string( mode ) + "\n" + kUsage );
    }
} // namespace buildpool
