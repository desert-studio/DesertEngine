#include <Common/Core/Logger.hpp>

#include <spdlog/details/log_msg_buffer.h>

#include <chrono>
#include <condition_variable>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <spdlog/sinks/msvc_sink.h>
#endif

namespace Common::Logger
{
    namespace
    {
        constexpr const char* kLogFileName = "engine_log.txt";

        // The file's line format without its colour marks and end of line: what TakeLogLines hands out is the
        // line exactly as the file holds it.
        constexpr const char* kRingPattern = "[%T.%e][%l][Desert]: %v";

        // THE PARSED LINE RING (TakeLogLines). Written by the file writer's thread, read by the Logs panel;
        // s_RingCount is published after the slots, so a reader that sees no new count takes no lock.
        std::mutex                 s_RingMutex;
        std::vector<LogLine>       s_Ring;
        std::atomic<std::uint64_t> s_RingCount{ 0 };

        // Under s_RingMutex. A message of several lines becomes several ring lines: the first carries the
        // message's level, the rest are plain (info), exactly as a reader of the file used to parse them.
        void PushRingText( std::string_view text, spdlog::level::level_enum level )
        {
            if ( s_Ring.empty() )
                s_Ring.resize( Detail::kLineRingCapacity );
            std::uint64_t count = s_RingCount.load( std::memory_order_relaxed );
            std::size_t   begin = 0;
            while ( begin <= text.size() )
            {
                std::size_t end = text.find( '\n', begin );
                if ( end == std::string_view::npos )
                    end = text.size();
                std::string_view line = text.substr( begin, end - begin );
                if ( !line.empty() && line.back() == '\r' )
                    line.remove_suffix( 1 );
                if ( !line.empty() )
                {
                    LogLine& slot = s_Ring[count % Detail::kLineRingCapacity];
                    slot.Text.assign( line.data(), line.size() );
                    slot.Level = level;
                    ++count;
                }
                level = spdlog::level::info;
                begin = end + 1;
            }
            s_RingCount.store( count, std::memory_order_release );
        }

        // The level a formatted line's own "[time][level]" field names (the early lines carried into the file
        // at its first placement), info when it names none.
        spdlog::level::level_enum LevelOfFormatted( std::string_view line )
        {
            const std::size_t open = line.find( "][" );
            if ( line.empty() || line.front() != '[' || open == std::string_view::npos )
                return spdlog::level::info;
            const std::size_t close = line.find( ']', open + 2 );
            if ( close == std::string_view::npos )
                return spdlog::level::info;
            const std::string_view                         name  = line.substr( open + 2, close - open - 2 );
            const std::optional<spdlog::level::level_enum> level = LevelFromName( name );
            return level.value_or( spdlog::level::info );
        }

        // THE LOG FILE, the file writer's target. spdlog's basic_file_sink throws spdlog_ex on a failed
        // open, write or flush; this one returns each failure as a value: Open gives null and the reason,
        // a failed write or flush is counted and said on stderr at most once a second (the file is what
        // failed), and the writer's line count still advances, so a caller waiting for an error line is
        // never held. Never throws but for an allocation, which ends the process at its site.
        class LogFileSink final : public spdlog::sinks::sink
        {
        public:
            // The file at @p path, truncated; null with @p error set when it cannot be opened.
            static std::shared_ptr<LogFileSink> Open( const std::filesystem::path& path, std::string& error )
            {
                // The wide name on Windows: path::string() converts to the ANSI code page, which throws or
                // mangles a character the page lacks.
#ifdef _WIN32
                std::FILE* const file = _wfopen( path.c_str(), L"wb" );
#else
                std::FILE* const file = std::fopen( path.c_str(), "wb" );
#endif
                if ( file == nullptr )
                {
                    error = std::error_code( errno, std::generic_category() ).message();
                    return nullptr;
                }
                return std::make_shared<LogFileSink>( file );
            }

            explicit LogFileSink( std::FILE* file ) noexcept : m_File( file )
            {
            }
            ~LogFileSink() override
            {
                (void)std::fclose( m_File );
            }
            LogFileSink( const LogFileSink& )            = delete;
            LogFileSink& operator=( const LogFileSink& ) = delete;

            void log( const spdlog::details::log_msg& msg ) override
            {
                const std::lock_guard lock( m_Mutex );
                m_Line.clear();
                m_Formatter->format( msg, m_Line );
                if ( std::fwrite( m_Line.data(), 1, m_Line.size(), m_File ) != m_Line.size() )
                    Failed( "write" );
            }

            void flush() override
            {
                const std::lock_guard lock( m_Mutex );
                if ( std::fflush( m_File ) != 0 )
                    Failed( "flush" );
            }

            void set_pattern( const std::string& pattern ) override
            {
                set_formatter( std::make_unique<spdlog::pattern_formatter>( pattern ) );
            }

            void set_formatter( std::unique_ptr<spdlog::formatter> formatter ) override
            {
                const std::lock_guard lock( m_Mutex );
                m_Formatter = std::move( formatter );
            }

        private:
            // Under m_Mutex.
            void Failed( const char* what ) noexcept
            {
                const int error = errno;
                ++m_Failures;
                const auto now = std::chrono::steady_clock::now();
                if ( m_Failures > 1 && now - m_LastReport < std::chrono::seconds( 1 ) )
                    return;
                m_LastReport = now;
                std::fprintf( stderr,
                              "[*** LOG ERROR ***] [log file] %s failed (errno %d); %llu failure(s) so far\n",
                              what, error, static_cast<unsigned long long>( m_Failures ) );
                std::fflush( stderr );
            }

            std::mutex                            m_Mutex;
            std::FILE*                            m_File = nullptr;
            std::unique_ptr<spdlog::formatter>    m_Formatter = std::make_unique<spdlog::pattern_formatter>();
            spdlog::memory_buf_t                  m_Line;
            std::uint64_t                         m_Failures = 0;
            std::chrono::steady_clock::time_point m_LastReport;
        };

        // MakeAsyncWriterSink's sink (Logger.hpp): the caller copies its line into the queue; the writer thread
        // writes the queue to the target in order and flushes it after every batch.
        class AsyncWriterSink final : public spdlog::sinks::sink
        {
        public:
            // @p feedsRing: the log file's writer, which also hands each line it writes to the line ring.
            explicit AsyncWriterSink( std::shared_ptr<spdlog::sinks::sink> target, bool feedsRing = false )
                 : m_Target( std::move( target ) ),
                   m_RingFormatter( feedsRing ? std::make_unique<spdlog::pattern_formatter>(
                                                     kRingPattern, spdlog::pattern_time_type::local, "" )
                                              : nullptr ),
                   m_Writer( [this]() { Run(); } )
            {
            }

            ~AsyncWriterSink() override
            {
                {
                    const std::lock_guard lock( m_Mutex );
                    m_Stop = true;
                }
                m_Wake.notify_all();
                m_Writer.join(); // the writer writes what is still queued first
            }

            void log( const spdlog::details::log_msg& msg ) override
            {
                std::unique_lock lock( m_Mutex );
                m_Written.wait( lock, [this]() { return m_Stop || m_QueuedBytes < Detail::kWriterQueueBytes; } );
                m_Queue.emplace_back( msg );
                m_QueuedBytes += QueuedSize( msg );
                const std::uint64_t mine = ++m_Queued;
                m_Wake.notify_one();
                if ( msg.level >= spdlog::level::err )
                    m_Written.wait( lock, [&]() { return m_Done >= mine; } );
            }

            // Never waits: the writer flushes the target after every batch it writes (flush_on(trace) holds).
            void flush() override
            {
            }

            void set_pattern( const std::string& pattern ) override
            {
                m_Target->set_pattern( pattern );
            }

            void set_formatter( std::unique_ptr<spdlog::formatter> formatter ) override
            {
                m_Target->set_formatter( std::move( formatter ) );
            }

            void Drain()
            {
                std::unique_lock lock( m_Mutex );
                const std::uint64_t upTo = m_Queued;
                m_Written.wait( lock, [&]() { return m_Done >= upTo; } );
            }

            const std::shared_ptr<spdlog::sinks::sink>& Target() const
            {
                return m_Target;
            }

        private:
            // What one queued line holds: the log_msg_buffer itself (its fmt buffer keeps 250 bytes inline, so a
            // short line costs far more than its text) and the logger name and text it copies. Counting the text
            // alone let a queue of short lines grow several times past kWriterQueueBytes.
            static std::size_t QueuedSize( const spdlog::details::log_msg& line )
            {
                return sizeof( spdlog::details::log_msg_buffer ) + line.logger_name.size() + line.payload.size();
            }

            // noexcept and catching nothing: a target reports its failures as values (LogFileSink; the console
            // sinks write with fwrite / WriteConsole), so what is left to throw is a bug or an allocation, and
            // that ends the process here, at its site.
            void Run() noexcept
            {
                std::vector<spdlog::details::log_msg_buffer> batch;
                std::unique_lock                             lock( m_Mutex );
                for ( ;; )
                {
                    m_Wake.wait( lock, [this]() { return m_Stop || !m_Queue.empty(); } );
                    if ( m_Queue.empty() )
                        return; // stopped, nothing left
                    batch.swap( m_Queue );
                    lock.unlock();
                    for ( const spdlog::details::log_msg_buffer& line : batch )
                        m_Target->log( line );
                    m_Target->flush();
                    if ( m_RingFormatter )
                        FeedRing( batch );
                    lock.lock();
                    for ( const spdlog::details::log_msg_buffer& line : batch )
                        m_QueuedBytes -= QueuedSize( line );
                    m_Done += batch.size();
                    batch.clear();
                    // A stall's backlog (up to kWriterQueueBytes) is not kept as the two vectors' capacity once it
                    // is written: a frame's lines are tens, a backlog after a held disk is hundreds of thousands.
                    if ( batch.capacity() > kKeptBatchLines )
                        std::vector<spdlog::details::log_msg_buffer>().swap( batch );
                    m_Written.notify_all();
                }
            }

            // The writer thread, after a batch is in the file: the same lines, formatted as the file holds
            // them, into the line ring under one lock for the batch.
            void FeedRing( const std::vector<spdlog::details::log_msg_buffer>& batch )
            {
                const std::lock_guard lock( s_RingMutex );
                for ( const spdlog::details::log_msg_buffer& line : batch )
                {
                    m_RingText.clear();
                    m_RingFormatter->format( line, m_RingText );
                    PushRingText( std::string_view( m_RingText.data(), m_RingText.size() ), line.level );
                }
            }

            static constexpr std::size_t kKeptBatchLines = 4096; // Run: the batch capacity kept between batches

            std::shared_ptr<spdlog::sinks::sink>         m_Target;
            std::unique_ptr<spdlog::formatter>           m_RingFormatter; // set on the file writer only
            spdlog::memory_buf_t                         m_RingText;      // the writer thread's (FeedRing)
            std::mutex                                   m_Mutex;
            std::condition_variable                      m_Wake;    // the writer: lines queued, or stop
            std::condition_variable                      m_Written; // callers: a batch is written
            std::vector<spdlog::details::log_msg_buffer> m_Queue;
            std::size_t                                  m_QueuedBytes = 0;
            std::uint64_t                                m_Queued      = 0; // lines ever queued
            std::uint64_t                                m_Done        = 0; // lines ever written and flushed
            bool                                         m_Stop        = false;
            std::thread                                  m_Writer; // last: started once the members above exist
        };

        // The writer whose target is the log file, or null.
        std::shared_ptr<AsyncWriterSink> FileWriterOf( const std::shared_ptr<spdlog::sinks::sink>& sink )
        {
            auto writer = std::dynamic_pointer_cast<AsyncWriterSink>( sink );
            if ( writer && std::dynamic_pointer_cast<LogFileSink>( writer->Target() ) )
                return writer;
            return nullptr;
        }
    } // namespace

    std::shared_ptr<spdlog::sinks::sink> MakeAsyncWriterSink( std::shared_ptr<spdlog::sinks::sink> target )
    {
        return std::make_shared<AsyncWriterSink>( std::move( target ) );
    }

    namespace Detail
    {
        std::atomic<int>& LevelThreshold() noexcept
        {
#if defined( DESERT_CONFIG_DEBUG )
            static std::atomic<int> s_Threshold{ static_cast<int>( spdlog::level::trace ) };
#else
            static std::atomic<int> s_Threshold{ static_cast<int>( spdlog::level::info ) };
#endif
            return s_Threshold;
        }
    } // namespace Detail

    void SetLevel( const spdlog::level::level_enum level )
    {
        Detail::LevelThreshold().store( static_cast<int>( level ), std::memory_order_relaxed );
    }

    spdlog::level::level_enum Level()
    {
        const int threshold = Detail::LevelThreshold().load( std::memory_order_relaxed );
        return static_cast<spdlog::level::level_enum>( threshold );
    }

    std::optional<spdlog::level::level_enum> LevelFromName( const std::string_view name )
    {
        if ( name == "warn" )
            return spdlog::level::warn;
        // Through `off` inclusive (to_string_view(off) is "off"): `--log-level off` is a documented name.
        for ( int level = spdlog::level::trace; level <= spdlog::level::off; ++level )
        {
            const spdlog::string_view_t text = spdlog::level::to_string_view(
                 static_cast<spdlog::level::level_enum>( level ) );
            if ( name == std::string_view( text.data(), text.size() ) )
                return static_cast<spdlog::level::level_enum>( level );
        }
        return std::nullopt;
    }

    std::uint64_t TakeLogLines( const std::uint64_t next, std::vector<LogLine>& out )
    {
        if ( s_RingCount.load( std::memory_order_acquire ) == next )
            return next;
        const std::lock_guard lock( s_RingMutex );
        const std::uint64_t   count  = s_RingCount.load( std::memory_order_relaxed );
        const std::uint64_t   oldest = count > Detail::kLineRingCapacity ? count - Detail::kLineRingCapacity : 0;
        std::uint64_t         at     = next < oldest ? oldest : next;
        if ( at > count )
            at = oldest; // a number past the end belongs to no ring this process wrote: start from what is kept
        for ( ; at < count; ++at )
            out.push_back( s_Ring[at % Detail::kLineRingCapacity] );
        return count;
    }

    void DrainLog()
    {
        if ( auto logger = spdlog::default_logger() )
        {
            for ( const std::shared_ptr<spdlog::sinks::sink>& sink : logger->sinks() )
            {
                if ( auto writer = std::dynamic_pointer_cast<AsyncWriterSink>( sink ) )
                    writer->Drain();
            }
        }
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
        const std::lock_guard lock( s_PathMutex );
        return s_LogFilePath;
    }

    void RelocateLogFile( const std::filesystem::path& directory )
    {
        auto logger = spdlog::default_logger();
        if ( !logger )
            return;

        const std::filesystem::path to = ( directory / kLogFileName ).lexically_normal();
        {
            const std::lock_guard lock( s_PathMutex );
            if ( s_LogFilePath == to )
                return;
        }
        logger->flush();
        DrainLog(); // the file being moved holds every line logged so far

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
            if ( FileWriterOf( *it ) )
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
        std::string                        openError;
        const std::shared_ptr<LogFileSink> placed = LogFileSink::Open( to, openError );
        if ( !placed )
        {
            // Said and kept where it was (the early ring, or the previous file): a value, never a throw. The
            // name in UTF-8, not the ANSI code page (see LogFileSink::Open).
            const std::u8string name = to.u8string();
            Detail::Write( spdlog::level::err,
                           "[Log] the log file '{}' could not be opened ({}); the log stays where it was.",
                           std::string_view( reinterpret_cast<const char*>( name.data() ), name.size() ),
                           openError );
            return;
        }
        placed->set_formatter( std::make_unique<spdlog::pattern_formatter>( "%v" ) );
        std::string carried;
        for ( const std::string& line : earlier )
            carried += line;
        while ( !carried.empty() && carried.back() == '\n' )
            carried.pop_back();
        if ( !carried.empty() )
            placed->log( spdlog::details::log_msg( "desert", spdlog::level::info, carried ) );
        placed->set_formatter( std::make_unique<spdlog::pattern_formatter>( "%^[%T.%e][%l][Desert]: %v%$" ) );
        // Replacing the slot destroys the previous writer (it joins after the drain above), which closes the
        // previous file, so removing it works on Windows too.
        // The first placement seeds the line ring with the startup lines it carried into the file; a later
        // one moves a file whose lines the ring already holds.
        if ( previous.empty() )
        {
            const std::lock_guard lock( s_RingMutex );
            std::string_view text( carried );
            for ( std::size_t begin = 0; begin < text.size(); )
            {
                std::size_t end = text.find( '\n', begin );
                if ( end == std::string_view::npos )
                    end = text.size();
                const std::string_view line = text.substr( begin, end - begin );
                PushRingText( line, LevelOfFormatted( line ) );
                begin = end + 1;
            }
        }
        *slot = std::make_shared<AsyncWriterSink>( placed, true );
        if ( !previous.empty() )
            std::filesystem::remove( previous, ec ); // the old copy would be a second, stale log

        const std::lock_guard lock( s_PathMutex );
        s_LogFilePath = to;
    }
} // namespace Common::Logger
