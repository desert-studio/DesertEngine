#include <Common/Core/Logger.hpp>

#include <spdlog/sinks/basic_file_sink.h>

#include <fstream>
#include <iterator>
#include <string>
#include <system_error>

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

    void RelocateLogFile( const std::filesystem::path& directory )
    {
        auto logger = spdlog::default_logger();
        if ( !logger )
            return;

        std::error_code             ec;
        const std::filesystem::path from = std::filesystem::absolute( kLogFileName, ec );
        const std::filesystem::path to   = directory / kLogFileName;
        if ( ec || std::filesystem::equivalent( from.parent_path(), directory, ec ) )
            return;

        auto& sinks = logger->sinks();
        for ( auto& sink : sinks )
        {
            if ( !std::dynamic_pointer_cast<spdlog::sinks::basic_file_sink_mt>( sink ) )
                continue;
            logger->flush();
            std::string earlier;
            {
                std::ifstream in( from, std::ios::binary );
                earlier.assign( std::istreambuf_iterator<char>( in ), std::istreambuf_iterator<char>() );
            }
            auto moved = std::make_shared<spdlog::sinks::basic_file_sink_mt>( to.string(), true );
            moved->set_formatter( std::make_unique<spdlog::pattern_formatter>( "%v" ) );
            if ( !earlier.empty() && earlier.back() == '\n' )
                earlier.pop_back();
            if ( !earlier.empty() )
                moved->log( spdlog::details::log_msg( "desert", spdlog::level::info, earlier ) );
            moved->set_formatter( std::make_unique<spdlog::pattern_formatter>( "%^[%T.%e][%l][Desert]: %v%$" ) );
            sink = std::move( moved );
            std::filesystem::remove( from, ec ); // the old copy would be a second, stale log
            return;
        }
    }
} // namespace Common::Logger
