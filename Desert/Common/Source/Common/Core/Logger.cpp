#include <Common/Core/Logger.hpp>

#include <spdlog/sinks/basic_file_sink.h>

#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <spdlog/sinks/msvc_sink.h>
#endif

namespace Common::Logger
{
    namespace
    {
        constexpr const char* kLogFileName = "engine_log.txt";
    }

    void AddPlatformDebuggerSink()
    {
#ifdef _WIN32
        // check_debugger_present = true: no cost when nothing is attached.
        if ( auto logger = spdlog::default_logger() )
            logger->sinks().push_back( std::make_shared<spdlog::sinks::msvc_sink_mt>( true ) );
#endif
    }

    namespace
    {
        std::mutex            s_PathMutex;
        std::filesystem::path s_LogFilePath;
    } // namespace

    std::filesystem::path LogFilePath()
    {
        std::lock_guard lock( s_PathMutex );
        return s_LogFilePath;
    }

    void RelocateLogFile( const std::filesystem::path& directory )
    {
        auto logger = spdlog::default_logger();
        if ( !logger )
            return;

        const std::filesystem::path to = ( directory / kLogFileName ).lexically_normal();
        {
            std::lock_guard lock( s_PathMutex );
            if ( s_LogFilePath == to )
                return;
        }
        logger->flush();

        // The lines so far: from the early ring on the first placement, from the previous file on a move.
        auto&                    sinks = logger->sinks();
        std::vector<std::string> earlier;
        std::filesystem::path    previous; // the file being moved away from (empty on the first placement)
        auto                     slot = sinks.end();
        for ( auto it = sinks.begin(); it != sinks.end(); ++it )
        {
            if ( auto ring = std::dynamic_pointer_cast<spdlog::sinks::ringbuffer_sink_mt>( *it ) )
            {
                earlier = ring->last_formatted();
                slot    = it;
                break;
            }
            if ( std::dynamic_pointer_cast<spdlog::sinks::basic_file_sink_mt>( *it ) )
            {
                previous = LogFilePath();
                std::ifstream in( previous, std::ios::binary );
                earlier.emplace_back( std::istreambuf_iterator<char>( in ), std::istreambuf_iterator<char>() );
                slot = it;
                break;
            }
        }
        if ( slot == sinks.end() )
            return;

        std::error_code ec;
        std::filesystem::create_directories( directory, ec );
        auto placed = std::make_shared<spdlog::sinks::basic_file_sink_mt>( to.string(), true );
        placed->set_formatter( std::make_unique<spdlog::pattern_formatter>( "%v" ) );
        std::string carried;
        for ( const std::string& line : earlier )
            carried += line;
        while ( !carried.empty() && carried.back() == '\n' )
            carried.pop_back();
        if ( !carried.empty() )
            placed->log( spdlog::details::log_msg( "desert", spdlog::level::info, carried ) );
        placed->set_formatter( std::make_unique<spdlog::pattern_formatter>( "%^[%T.%e][%l][Desert]: %v%$" ) );
        *slot = std::move( placed ); // closes the previous file, so removing it works on Windows too
        if ( !previous.empty() )
            std::filesystem::remove( previous, ec ); // the old copy would be a second, stale log

        std::lock_guard lock( s_PathMutex );
        s_LogFilePath = to;
    }
} // namespace Common::Logger
