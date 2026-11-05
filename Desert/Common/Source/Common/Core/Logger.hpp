#pragma once

#include <spdlog/spdlog.h>

#include <spdlog/sinks/ringbuffer_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <Common/Core/ResultStr.hpp>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Common::Logger
{
    // Defined in Logger.cpp so that <Windows.h> (spdlog's msvc_sink) stays out of every includer.
    // Windows: the debugger's Output pane gets every line (the editor is a windowed application, so
    // without `--console` stdout goes nowhere and "logs are not written" is what a developer sees).
    void AddPlatformDebuggerSink();

    // THE LOG FILE IS NEVER OPENED IN THE WORKING DIRECTORY. A process started from Finder is in `/`
    // (read-only), one started from a shell in /tmp would leave its log where nobody looks. LogInit keeps
    // the lines written before the host knows where its log belongs in memory (a ring, Detail::EarlyLines);
    // the host then calls this with that directory and the file is opened there, beginning with them
    // (UE's layout: editor and tools <ProjectDir>/Saved/Logs, the shipped runtime the game's user directory
    // /Logs since the install folder is read-only). A second call moves the file, carrying its lines.
    void RelocateLogFile( const std::filesystem::path& directory );

    // Where the log file is — empty until RelocateLogFile has placed it (the Logs panel reads it here,
    // never by a bare file name).
    std::filesystem::path LogFilePath();

    // THE LOG NEVER BLOCKS THE THREAD THAT LOGS ON I/O (BEGINGRAPH-STALL-1; UE: FOutputDeviceFile writes through
    // FAsyncWriter's own thread, the game and render threads only append to its buffer). @p target - the
    // console, the log file - is written by a writer thread of its own: a line is copied into the writer's queue
    // and the call returns; the writer writes what is queued in order and flushes after every batch, so the file
    // is as current as before, but a write the disk or a full pipe holds for a second holds the writer, never a
    // frame (a TURN frame waited 0.9 s inside a LOG_INFO in RDG BeginGraph while a build loaded the disk). An
    // error or critical line waits until it is written (what an abort follows is on disk); the queue holds at
    // most kWriterQueueBytes of lines, past that a caller waits for the writer (bounded, never dropped).
    std::shared_ptr<spdlog::sinks::sink> MakeAsyncWriterSink( std::shared_ptr<spdlog::sinks::sink> target );

    // Blocks until every line logged before the call is written and flushed by every writer of the default
    // logger (RelocateLogFile before it reads the file it moves; a host before it hands its log to a reader).
    void DrainLog();

    // THE LEVEL GATE (UE: a log category's verbosity, checked before the message is built). A line below the
    // process' level is dropped at the call: no format, no lock, no queue. Default info outside Debug (a
    // trace line per frame per view - [PreExposure] - cost the Release editor a formatted, queued and written
    // line each), trace in Debug; `--log-level <trace|debug|info|warn|error|critical>` moves it at start.
    void SetLevel( spdlog::level::level_enum level );
    [[nodiscard]] spdlog::level::level_enum Level();
    // The level named @p name (spdlog's names: trace, debug, info, warning or warn, error, critical), or none.
    [[nodiscard]] std::optional<spdlog::level::level_enum> LevelFromName( std::string_view name );

    // THE PARSED LINE RING. The log file's writer thread hands every line it writes, formatted as the file
    // holds it and with its own level, to a bounded ring; a reader (the editor's Logs panel) takes the lines
    // numbered @p next and later, and never re-reads or re-parses the file. Lines are numbered from 0 in the
    // order the writer wrote them; a reader that fell more than kLineRingCapacity behind skips the oldest.
    struct LogLine
    {
        std::string               Text;
        spdlog::level::level_enum Level = spdlog::level::info;
    };
    // Appends the lines numbered >= @p next to @p out; returns the number after the last one there is. No
    // lock when nothing is new.
    std::uint64_t TakeLogLines( std::uint64_t next, std::vector<LogLine>& out );

    namespace Detail
    {
        // The ring's size in lines (TakeLogLines): a few seconds of the busiest startup.
        inline constexpr std::size_t kLineRingCapacity = 16384;
        // The level gate's threshold as an int (Logger.cpp owns the one instance).
        [[nodiscard]] std::atomic<int>& LevelThreshold() noexcept;

        // One gated line: dropped below the threshold before anything is formatted; formatted into a stack
        // buffer otherwise and handed to the logger as a view, so no std::string is built per line.
        template <typename... Args>
        void Write( spdlog::level::level_enum level, fmt::format_string<Args...> message, Args&&... args )
        {
            if ( static_cast<int>( level ) < LevelThreshold().load( std::memory_order_relaxed ) )
                return;
            spdlog::logger* const logger = spdlog::default_logger_raw();
            if ( logger == nullptr )
                return;
            fmt::memory_buffer text;
            fmt::format_to( std::back_inserter( text ), message, std::forward<Args>( args )... );
            logger->log( spdlog::source_loc{}, level, spdlog::string_view_t( text.data(), text.size() ) );
        }
        // Holds the startup lines until the log file has a directory; generous, since a startup that
        // logs more than this before finding its own directory is itself the defect.
        inline constexpr std::size_t kEarlyLineCapacity = 16384;
        // The most memory a writer's queued lines take (each line's buffer object, logger name and text) before a
        // caller waits for it (MakeAsyncWriterSink).
        inline constexpr std::size_t kWriterQueueBytes = std::size_t{ 64 } << 20;
    } // namespace Detail

    inline void LogInit()
    {
        auto console_sink = MakeAsyncWriterSink( std::make_shared<spdlog::sinks::stdout_color_sink_mt>() );
        auto early_sink   = std::make_shared<spdlog::sinks::ringbuffer_sink_mt>( Detail::kEarlyLineCapacity );

        spdlog::set_default_logger(
             std::make_shared<spdlog::logger>( "desert", spdlog::sinks_init_list{ console_sink, early_sink } ) );
        // Millisecond timestamps (%e): startup-phase costs — a shader compile, an atlas bake — are
        // tens-to-hundreds of ms each, and a 1-second clock cannot attribute them to anything.
        spdlog::set_pattern( "%^[%T.%e][%l][Desert]: %v%$" );
        spdlog::set_level( spdlog::level::trace );
        spdlog::flush_on( spdlog::level::trace );
        AddPlatformDebuggerSink();

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
        Detail::Write( spdlog::level::debug, inMessage, std::forward<Args>( InArgs )... );
    }

    template <typename... Args>
    void LogInfo( fmt::format_string<Args...> inMessage, Args&&... InArgs )
    {
        Detail::Write( spdlog::level::info, inMessage, std::forward<Args>( InArgs )... );
    }

    template <typename... Args>
    void LogWarn( fmt::format_string<Args...> inMessage, Args&&... InArgs )
    {
        Detail::Write( spdlog::level::warn, inMessage, std::forward<Args>( InArgs )... );
    }

    template <typename... Args>
    void LogError( fmt::format_string<Args...> inMessage, Args&&... InArgs )
    {
        Detail::Write( spdlog::level::err, inMessage, std::forward<Args>( InArgs )... );
    }

    template <typename... Args>
    void LogCritical( fmt::format_string<Args...> inMessage, Args&&... InArgs )
    {
        Detail::Write( spdlog::level::critical, inMessage, std::forward<Args>( InArgs )... );
    }

    template <typename... Args>
    void LogTrace( fmt::format_string<Args...> inMessage, Args&&... InArgs )
    {
        Detail::Write( spdlog::level::trace, inMessage, std::forward<Args>( InArgs )... );
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
