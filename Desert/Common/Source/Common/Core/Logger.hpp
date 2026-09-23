#pragma once

#include <spdlog/spdlog.h>

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <Common/Core/ResultStr.hpp>

#include <memory>
#include <utility>

namespace Common::Logger
{
    inline void LogInit()
    {
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>("engine_log.txt", true);

        spdlog::set_default_logger(std::make_shared<spdlog::logger>("desert", spdlog::sinks_init_list{console_sink, file_sink}));
        // Millisecond timestamps (%e): startup-phase costs — a shader compile, an atlas bake — are
        // tens-to-hundreds of ms each, and a 1-second clock cannot attribute them to anything.
        spdlog::set_pattern( "%^[%T.%e][%l][Desert]: %v%$" );
        spdlog::set_level( spdlog::level::trace );
        spdlog::flush_on( spdlog::level::trace );

        // THE RESULT TYPE LIVES IN A REPOSITORY WITH NO LOGGER, ON PURPOSE — the shared-format library
        // must build without the engine, so it reports a failed unwrap through a function the HOST
        // installs. This is that host. Without this line the message still appears, on stderr; with it
        // the message lands in engine_log.txt beside the error that caused it, which is the only place
        // a reader will look. Installed here rather than in a target's main() so that every executable
        // that initialises the logger gets it — the editor, the runtime, and every tool.
        Common::SetResultUnwrapReporter( []( const char* message ) { spdlog::error( "{}", message ); } );
    }

    // THE FORMAT STRING IS CHECKED BY THE COMPILER, and that is the whole point of this signature.
    //
    // These functions used to take a `std::string_view` and call `fmt::vformat`, which is fmt's RUNTIME
    // entry point: a format string with one more `{}` than it has arguments compiles clean and throws
    // `fmt::format_error` when the line is reached. Nothing catches it, so the process dies — and it dies
    // at the exact moment the code was trying to REPORT that something had already gone wrong. That is not
    // hypothetical: a developer's error message with one extra brace killed the editor from inside the
    // draw loop, and no test could have caught it because no test compiles a `{}`.
    //
    // `fmt::format_string<Args...>` makes the same mistake a compile error. A genuinely runtime format
    // string is still possible and still allowed — it just has to say so, by wrapping itself in
    // `fmt::runtime(...)`, which is exactly the visibility that was missing.
    //
    // KNOWN AND DELIBERATELY NOT CHANGED HERE: every one of these calls `spdlog::set_level` to its OWN
    // level before logging, so the global level is whatever the last message happened to be and filtering
    // can never do anything. That is a separate defect with a visible consequence (fixing it starts
    // suppressing output that is printed today), so it is named rather than smuggled into this change.
    template <typename... Args>
    void LogDebug( fmt::format_string<Args...> inMessage, Args&&... InArgs )
    {
        spdlog::set_level( spdlog::level::debug );
        spdlog::debug( fmt::format( inMessage, std::forward<Args>( InArgs )... ) );
    }

    template <typename... Args>
    void LogInfo( fmt::format_string<Args...> inMessage, Args&&... InArgs )
    {
        spdlog::set_level( spdlog::level::info );
        spdlog::info( fmt::format( inMessage, std::forward<Args>( InArgs )... ) );
    }

    template <typename... Args>
    void LogWarn( fmt::format_string<Args...> inMessage, Args&&... InArgs )
    {
        spdlog::set_level( spdlog::level::warn );
        spdlog::warn( fmt::format( inMessage, std::forward<Args>( InArgs )... ) );
    }

    template <typename... Args>
    void LogError( fmt::format_string<Args...> inMessage, Args&&... InArgs )
    {
        spdlog::set_level( spdlog::level::err );
        spdlog::error( fmt::format( inMessage, std::forward<Args>( InArgs )... ) );
    }

    template <typename... Args>
    void LogCritical( fmt::format_string<Args...> inMessage, Args&&... InArgs )
    {
        spdlog::set_level( spdlog::level::critical );
        spdlog::critical( fmt::format( inMessage, std::forward<Args>( InArgs )... ) );
    }

    template <typename... Args>
    void LogTrace( fmt::format_string<Args...> inMessage, Args&&... InArgs )
    {
        spdlog::set_level( spdlog::level::trace );
        spdlog::trace( fmt::format( inMessage, std::forward<Args>( InArgs )... ) );
    }

    // The runtime door, named so it cannot be walked through by accident. A message assembled at runtime —
    // a compiler's diagnostic, a file's contents, a stringified macro argument — is DATA and must be logged
    // as an argument (`LOG_ERROR( "{}", text )`), because braces inside it would otherwise be parsed as
    // placeholders. Use this only when the format string itself is genuinely chosen at runtime, and say
    // why at the call site.
    using fmt::runtime;
} // namespace Common::Logger

#define LOG_INFO( ... ) Common::Logger::LogInfo( __VA_ARGS__ );
#define LOG_WARN( ... ) Common::Logger::LogWarn( __VA_ARGS__ );
#define LOG_ERROR( ... ) Common::Logger::LogError( __VA_ARGS__ );
#define LOG_CRITICAL( ... ) Common::Logger::LogCritical( __VA_ARGS__ );
#define LOG_TRACE( ... ) Common::Logger::LogTrace( __VA_ARGS__ );
#define LOG_DEBUG( ... ) Common::Logger::LogDebug( __VA_ARGS__ );
