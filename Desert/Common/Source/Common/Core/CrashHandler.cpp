// Ported from UE 5.8 Runtime/Core/Private/Windows/WindowsPlatformCrashContext.cpp (the
// SetUnhandledExceptionFilter + _set_purecall_handler + _set_invalid_parameter_handler triple, the
// minidump written from the faulting EXCEPTION_POINTERS, the "capture a CONTEXT and synthesise an
// EXCEPTION_RECORD" trick for the non-SEH entry points) and
// Runtime/Core/Private/GenericPlatform/GenericPlatformCrashContext.cpp (the context assembled
// before the fault and the one-directory-per-crash layout).
// Adapted: no FString/FPlatformMisc/UObject and no CrashReportClient XML — the context is the flat
// key=value file documented below; every buffer here is a fixed C array filled before the fault, so
// the POSIX arm can run the SAME writer inside a signal handler with nothing but write(2).

#include "CrashHandler.hpp"

#include <Common/Core/Core.hpp>
#include <Common/Core/Logger.hpp>
#include <Common/Core/Version.hpp>
#include <Common/Settings/EngineUserDirectory.hpp>

#include <spdlog/sinks/base_sink.h>

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>

#if defined( DESERT_PLATFORM_WINDOWS )
#include <Windows.h>
// DbgHelp must follow Windows.h; it is what supplies MiniDumpWriteDump and StackWalk64.
#include <DbgHelp.h>
#include <intrin.h>
#else
#include <dlfcn.h>
#include <execinfo.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>
#if defined( __APPLE__ )
#include <sys/ucontext.h>
#else
#include <ucontext.h>
#endif
extern char** environ;
#endif

// ─────────────────────────────────────────────────────────────────────────────────────────────────
// THE crash.txt FORMAT — THE ONE SOURCE OF TRUTH. CR2's reporter parses this; nothing else does.
//
// The file is ASCII, LF-terminated, and strictly line-oriented. Three line shapes and no others:
//
//   DESERTCRASH <n>      the very first line; <n> is the format version, currently 1
//   [<section>]          opens a section; sections appear in the fixed order listed below
//   <key>=<value>        a field of the open section; <value> runs to the end of the line and may
//                        be empty. A value never contains CR or LF (the writer replaces both with a
//                        space) so a parser may split on the first '=' of each line and stop at LF.
//
// A reader must ignore an unknown section and an unknown key: fields are only ever appended, and a
// truncated file (the process died mid-write) is a legal input that ends early.
//
// Sections, in order, with every key this writer can emit:
//
//   [report]    host=<Editor|Runtime|…>  pid=<dec>  tid=<dec>  started=<YYYYMMDD-HHMMSS, local>
//               crash_epoch=<dec seconds since the epoch, UTC>
//   [exception] kind=<exception|signal|purecall|invalid_parameter>
//               code=<0x… SEH code, or the decimal signal number for kind=signal>
//               codename=<EXCEPTION_ACCESS_VIOLATION|SIGSEGV|…>
//               address=<0x… faulting instruction, 0x0 when unknown>
//               synthesized=<0|1>   1 ⇒ the context was captured inside the handler, so frame 0 is
//                                   this module and `function` was chosen by the skip rule below
//               module=<file name of the module holding `address`>
//               module_offset=<0x… offset of `address` within that module>
//               function=<the faulting function, or "unknown" when no symbol resolved>
//               fault_frame=<dec index into the [stack] frames that `function` came from>
//   [build]     version=…  sha=…  branch=…  dirty=<0|1>
//   [context]   machine=<host name>  scene=<path or "none">  os=…  gpu=…
//   [stack]     frame=<dec index>|<0x… address>|<module>|<function>|<file:line or "">
//               Repeated, innermost first. Unresolved parts are empty between the pipes; the field
//               count is always five so a parser can split on '|' unconditionally.
//   [log]       log=<one captured log line, oldest first>   (the ring sink, capacity kLogRingLines)
//   [end]       written=<complete>    ABSENT if the process died before finishing — its presence is
//                                     how a reader tells a whole report from a truncated one.
//
// `function` for a real fault (synthesized=0) is frame 0, because the SEH filter runs with the
// faulting thread's own CONTEXT. For synthesized=1 the innermost frames belong to this file and to
// the CRT, so `function` is the innermost frame that is BOTH outside `Common::Crash::Detail` AND
// inside the main executable module. Every frame is still in [stack] either way.
// On POSIX every report is synthesized=1: the frames are walked from inside the signal handler (frame
// 0 is the interrupted PC from the ucontext, then the handler, the trampoline and the callers), and the
// function names stay Itanium-MANGLED, because demangling allocates.
// ─────────────────────────────────────────────────────────────────────────────────────────────────

namespace Common::Crash
{
    namespace Detail
    {
        // Capacities. Fixed, because every one of these buffers is read from a signal handler where
        // an allocation is undefined behaviour. Chosen to cover the real values with room to spare;
        // anything longer is truncated, which still identifies a crash.
        constexpr std::size_t kSmallField     = 128;
        constexpr std::size_t kPathField      = 1024;
        constexpr std::size_t kLogRingLines   = 64;
        constexpr std::size_t kLogLineChars   = 256;
        constexpr std::size_t kMaxStackFrames = 64;
        constexpr std::size_t kWriteBuffer    = 8192;

        // The report context, formatted BEFORE the fault. See the header: nothing here is produced
        // inside a handler.
        char g_Host[kSmallField]    = "unknown";
        char g_Version[kSmallField] = "unknown";
        char g_Sha[kSmallField]     = "unknown";
        char g_Branch[kSmallField]  = "unknown";
        bool g_Dirty                = false;
        char g_Machine[kSmallField] = "unknown";
        char g_Os[kPathField]       = "unknown";
        char g_Gpu[kPathField]      = "unknown";
        char g_Scene[kPathField]    = "none";
        char g_Started[kSmallField] = "unknown";

        // The per-crash paths, all built at Install() time. The handler does no path arithmetic:
        // building one needs concatenation, and concatenation in a handler needs a scratch buffer
        // that a smashed stack may no longer have. The directory is named for the process START, not
        // for the crash — a process crashes once, so the two identify the same report, and the start
        // time is knowable while the program is still healthy.
        char g_DirUtf8[kPathField] = "";
        char g_TxtUtf8[kPathField] = "";
#if defined( DESERT_PLATFORM_WINDOWS )
        wchar_t g_DirNative[kPathField]               = L"";
        wchar_t g_TxtNative[kPathField]               = L"";
        wchar_t g_DmpNative[kPathField]               = L"";
        wchar_t g_ReporterNative[kPathField]          = L"";
        wchar_t g_ReporterCommandLine[kPathField * 2] = L"";
#else
        char  g_DmpUtf8[kPathField]      = "";
        char  g_ReporterUtf8[kPathField] = "";
        char* g_ReporterArgv[3]          = { nullptr, nullptr, nullptr };
#endif
        bool g_HasReporter = false;

        std::atomic<bool> g_Installed{ false };
        // Re-entry guard: a fault inside the handler must kill the process rather than recurse until
        // the stack is gone. `exchange` and not a bool, because a second thread can fault while the
        // first is still writing.
        std::atomic<bool> g_InHandler{ false };

        std::filesystem::path g_ReportRoot;

        // ── The log ring ────────────────────────────────────────────────────────────────────────
        // A plain array of fixed rows. The handler reads it with nothing but memcpy, so no sink
        // mutex is taken and no allocator is entered while the process is dying.
        char                       g_LogRing[kLogRingLines][kLogLineChars] = {};
        std::atomic<std::uint32_t> g_LogWritten{ 0 };

        void StoreLogLine( const char* inText, std::size_t inLength )
        {
            const std::uint32_t slot = g_LogWritten.load( std::memory_order_relaxed ) % kLogRingLines;
            char*               row  = g_LogRing[slot];
            std::size_t         copy = inLength < ( kLogLineChars - 1 ) ? inLength : ( kLogLineChars - 1 );
            std::memcpy( row, inText, copy );
            // Newlines would break the one-field-per-line contract documented above.
            for ( std::size_t i = 0; i < copy; ++i )
            {
                if ( row[i] == '\n' || row[i] == '\r' )
                {
                    row[i] = ' ';
                }
            }
            row[copy] = '\0';
            g_LogWritten.fetch_add( 1, std::memory_order_release );
        }

        class RingSink final : public spdlog::sinks::base_sink<std::mutex>
        {
        protected:
            void sink_it_( const spdlog::details::log_msg& inMessage ) override
            {
                spdlog::memory_buf_t formatted;
                base_sink<std::mutex>::formatter_->format( inMessage, formatted );
                StoreLogLine( formatted.data(), formatted.size() );
            }

            void flush_() override
            {
            }
        };

        void CopyIntoFixed( char* outBuffer, std::size_t inCapacity, std::string_view inValue )
        {
            const std::size_t copy = inValue.size() < ( inCapacity - 1 ) ? inValue.size() : ( inCapacity - 1 );
            std::memcpy( outBuffer, inValue.data(), copy );
            for ( std::size_t i = 0; i < copy; ++i )
            {
                if ( outBuffer[i] == '\n' || outBuffer[i] == '\r' )
                {
                    outBuffer[i] = ' ';
                }
            }
            outBuffer[copy] = '\0';
        }

        // ── The raw writer ──────────────────────────────────────────────────────────────────────
        // Everything below formats by hand into one stack buffer and empties it with a single raw
        // write syscall. No printf (not async-signal-safe, and it allocates for %s on some libcs),
        // no std::string, no iostream.
#if defined( DESERT_PLATFORM_WINDOWS )
        using RawHandle = HANDLE;
        // Not constexpr: INVALID_HANDLE_VALUE is a cast of -1 to a pointer, which is not a constant
        // expression in C++ even though the macro reads like one.
        const RawHandle kInvalidRawHandle = INVALID_HANDLE_VALUE;
#else
        using RawHandle                       = int;
        constexpr RawHandle kInvalidRawHandle = -1;
#endif

        struct RawWriter
        {
            RawHandle   handle               = kInvalidRawHandle;
            char        buffer[kWriteBuffer] = {};
            std::size_t used                 = 0;

            void Flush()
            {
                if ( used == 0 || handle == kInvalidRawHandle )
                {
                    used = 0;
                    return;
                }
#if defined( DESERT_PLATFORM_WINDOWS )
                DWORD written = 0;
                ::WriteFile( handle, buffer, static_cast<DWORD>( used ), &written, nullptr );
#else
                std::size_t offset = 0;
                while ( offset < used )
                {
                    const ssize_t n = ::write( handle, buffer + offset, used - offset );
                    if ( n <= 0 )
                    {
                        break;
                    }
                    offset += static_cast<std::size_t>( n );
                }
#endif
                used = 0;
            }

            void Raw( const char* inText, std::size_t inLength )
            {
                while ( inLength > 0 )
                {
                    if ( used == kWriteBuffer )
                    {
                        Flush();
                    }
                    const std::size_t room = kWriteBuffer - used;
                    const std::size_t copy = inLength < room ? inLength : room;
                    std::memcpy( buffer + used, inText, copy );
                    used += copy;
                    inText += copy;
                    inLength -= copy;
                }
            }

            void Str( const char* inText )
            {
                if ( inText != nullptr )
                {
                    Raw( inText, std::strlen( inText ) );
                }
            }

            void Char( char inCharacter )
            {
                Raw( &inCharacter, 1 );
            }

            void Dec( std::uint64_t inValue )
            {
                char        digits[24];
                std::size_t count = 0;
                do
                {
                    digits[count++] = static_cast<char>( '0' + ( inValue % 10 ) );
                    inValue /= 10;
                } while ( inValue != 0 && count < sizeof( digits ) );
                while ( count > 0 )
                {
                    Char( digits[--count] );
                }
            }

            void Hex( std::uint64_t inValue )
            {
                static const char kDigits[] = "0123456789ABCDEF";
                Str( "0x" );
                char        digits[16];
                std::size_t count = 0;
                do
                {
                    digits[count++] = kDigits[inValue & 0xFu];
                    inValue >>= 4u;
                } while ( inValue != 0 && count < sizeof( digits ) );
                while ( count > 0 )
                {
                    Char( digits[--count] );
                }
            }

            void Field( const char* inKey, const char* inValue )
            {
                Str( inKey );
                Char( '=' );
                Str( inValue );
                Char( '\n' );
            }
        };

        // One resolved stack frame. Filled by the platform walker, printed by the shared writer.
        struct ResolvedFrame
        {
            std::uint64_t address              = 0;
            char          module[kSmallField]  = "";
            char          function[kPathField] = "";
            char          source[kPathField]   = "";
            bool          inMainModule         = false;
        };

        ResolvedFrame g_Frames[kMaxStackFrames];
        std::size_t   g_FrameCount = 0;

        // ── Platform layer ──────────────────────────────────────────────────────────────────────
#if defined( DESERT_PLATFORM_WINDOWS )
        RawHandle RawCreateFile( const wchar_t* inPath )
        {
            return ::CreateFileW( inPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr );
        }

        void RawCloseFile( RawHandle inHandle )
        {
            if ( inHandle != kInvalidRawHandle )
            {
                ::CloseHandle( inHandle );
            }
        }

        void RawCreateDirectory()
        {
            ::CreateDirectoryW( g_DirNative, nullptr );
        }
#else
        RawHandle RawCreateFile( const char* inPath )
        {
            return ::open( inPath, O_WRONLY | O_CREAT | O_TRUNC, 0644 );
        }

        void RawCloseFile( RawHandle inHandle )
        {
            if ( inHandle != kInvalidRawHandle )
            {
                ::close( inHandle );
            }
        }

        void RawCreateDirectory()
        {
            ::mkdir( g_DirUtf8, 0755 );
        }
#endif

        void WriteStderr( const char* inText )
        {
#if defined( DESERT_PLATFORM_WINDOWS )
            DWORD written = 0;
            ::WriteFile( ::GetStdHandle( STD_ERROR_HANDLE ), inText, static_cast<DWORD>( std::strlen( inText ) ),
                         &written, nullptr );
#else
            const ssize_t ignored = ::write( STDERR_FILENO, inText, std::strlen( inText ) );
            (void)ignored;
#endif
        }
    } // namespace Detail
} // namespace Common::Crash

namespace Common::Crash::Detail
{
    // ── The shared crash.txt writer ─────────────────────────────────────────────────────────────
    struct FaultDescription
    {
        const char*   kind        = "exception";
        std::uint64_t code        = 0;
        bool          codeIsHex   = true;
        const char*   codeName    = "unknown";
        std::uint64_t address     = 0;
        bool          synthesized = false;
    };

    void WriteCrashText( const FaultDescription& inFault, std::uint64_t inThreadId )
    {
        RawHandle file = RawCreateFile(
#if defined( DESERT_PLATFORM_WINDOWS )
             g_TxtNative
#else
             g_TxtUtf8
#endif
        );
        if ( file == kInvalidRawHandle )
        {
            WriteStderr( "[Crash] could not open crash.txt in the report directory\n" );
            return;
        }

        RawWriter writer;
        writer.handle = file;

        writer.Str( "DESERTCRASH 1\n" );

        writer.Str( "[report]\n" );
        writer.Field( "host", g_Host );
        writer.Str( "pid=" );
#if defined( DESERT_PLATFORM_WINDOWS )
        writer.Dec( ::GetCurrentProcessId() );
#else
        writer.Dec( static_cast<std::uint64_t>( ::getpid() ) );
#endif
        writer.Char( '\n' );
        writer.Str( "tid=" );
        writer.Dec( inThreadId );
        writer.Char( '\n' );
        writer.Field( "started", g_Started );
        writer.Str( "crash_epoch=" );
        // `time` is on POSIX's async-signal-safe list, which is why the crash instant is an integer
        // here and the human-readable stamp was formatted before the fault.
        writer.Dec( static_cast<std::uint64_t>( std::time( nullptr ) ) );
        writer.Char( '\n' );

        writer.Str( "[exception]\n" );
        writer.Field( "kind", inFault.kind );
        writer.Str( "code=" );
        if ( inFault.codeIsHex )
        {
            writer.Hex( inFault.code );
        }
        else
        {
            writer.Dec( inFault.code );
        }
        writer.Char( '\n' );
        writer.Field( "codename", inFault.codeName );
        writer.Str( "address=" );
        writer.Hex( inFault.address );
        writer.Char( '\n' );
        writer.Field( "synthesized", inFault.synthesized ? "1" : "0" );

        // The faulting frame, chosen by the rule documented at the top of this file.
        std::size_t faultIndex = 0;
        if ( inFault.synthesized )
        {
            for ( std::size_t i = 0; i < g_FrameCount; ++i )
            {
                // The second spelling is the Itanium-mangled one: POSIX names stay mangled (no allocation).
                const bool isHandlerInternal =
                     std::strstr( g_Frames[i].function, "Common::Crash::Detail" ) != nullptr ||
                     std::strstr( g_Frames[i].function, "N6Common5Crash6Detail" ) != nullptr;
                if ( !isHandlerInternal && g_Frames[i].inMainModule )
                {
                    faultIndex = i;
                    break;
                }
            }
        }
        if ( faultIndex < g_FrameCount )
        {
            writer.Field( "module", g_Frames[faultIndex].module );
            writer.Str( "module_offset=" );
            writer.Hex( g_Frames[faultIndex].address );
            writer.Char( '\n' );
            writer.Field( "function",
                          g_Frames[faultIndex].function[0] != '\0' ? g_Frames[faultIndex].function : "unknown" );
        }
        else
        {
            writer.Field( "module", "unknown" );
            writer.Field( "module_offset", "0x0" );
            writer.Field( "function", "unknown" );
        }
        writer.Str( "fault_frame=" );
        writer.Dec( faultIndex );
        writer.Char( '\n' );

        writer.Str( "[build]\n" );
        writer.Field( "version", g_Version );
        writer.Field( "sha", g_Sha );
        writer.Field( "branch", g_Branch );
        writer.Field( "dirty", g_Dirty ? "1" : "0" );

        writer.Str( "[context]\n" );
        writer.Field( "machine", g_Machine );
        writer.Field( "scene", g_Scene );
        writer.Field( "os", g_Os );
        writer.Field( "gpu", g_Gpu );

        writer.Str( "[stack]\n" );
        for ( std::size_t i = 0; i < g_FrameCount; ++i )
        {
            writer.Str( "frame=" );
            writer.Dec( i );
            writer.Char( '|' );
            writer.Hex( g_Frames[i].address );
            writer.Char( '|' );
            writer.Str( g_Frames[i].module );
            writer.Char( '|' );
            writer.Str( g_Frames[i].function );
            writer.Char( '|' );
            writer.Str( g_Frames[i].source );
            writer.Char( '\n' );
        }

        writer.Str( "[log]\n" );
        const std::uint32_t written = g_LogWritten.load( std::memory_order_acquire );
        const std::uint32_t count   = written < static_cast<std::uint32_t>( kLogRingLines )
                                           ? written
                                           : static_cast<std::uint32_t>( kLogRingLines );
        for ( std::uint32_t i = 0; i < count; ++i )
        {
            const std::uint32_t slot = ( written - count + i ) % static_cast<std::uint32_t>( kLogRingLines );
            writer.Str( "log=" );
            writer.Str( g_LogRing[slot] );
            writer.Char( '\n' );
        }

        // Last, and only once everything above is in the buffer: its presence is the reader's
        // "this report is whole" signal.
        writer.Str( "[end]\nwritten=complete\n" );
        writer.Flush();
        RawCloseFile( file );
    }

    // THE ONE PERMITTED "NOT THERE YET" BRANCH IN THIS MODULE, AND IT IS LOUD.
    // CR2 builds Tools/CrashReporter. Until it lands there is no reporter binary to start, and a
    // crash must still leave a usable artefact — so the absent-reporter case prints the report
    // directory to stderr and says why. It is a named branch with output, not a fallback: the user
    // is told where the report is and that no reporter ran.
    void LaunchReporter()
    {
        if ( !g_HasReporter )
        {
            WriteStderr( "[Crash] report written to: " );
            WriteStderr( g_DirUtf8 );
            WriteStderr(
                 "\n[Crash] no crash reporter executable beside this binary - the report was not sent.\n" );
            return;
        }

#if defined( DESERT_PLATFORM_WINDOWS )
        STARTUPINFOW startup        = {};
        startup.cb                  = sizeof( startup );
        PROCESS_INFORMATION process = {};
        if ( ::CreateProcessW( g_ReporterNative, g_ReporterCommandLine, nullptr, nullptr, FALSE, 0, nullptr,
                               nullptr, &startup, &process ) != 0 )
        {
            ::CloseHandle( process.hThread );
            ::CloseHandle( process.hProcess );
        }
        else
        {
            WriteStderr( "[Crash] could not start the crash reporter; report written to: " );
            WriteStderr( g_DirUtf8 );
            WriteStderr( "\n" );
        }
#else
        pid_t child = 0;
        if ( ::posix_spawn( &child, g_ReporterUtf8, nullptr, nullptr, g_ReporterArgv, environ ) != 0 )
        {
            WriteStderr( "[Crash] could not start the crash reporter; report written to: " );
            WriteStderr( g_DirUtf8 );
            WriteStderr( "\n" );
        }
#endif
    }

    [[noreturn]] void TerminateAfterReport()
    {
#if defined( DESERT_PLATFORM_WINDOWS )
        // TerminateProcess and not exit(): the CRT's atexit chain runs destructors over state the
        // fault has already corrupted, and a second fault there replaces the report with a hang.
        ::TerminateProcess( ::GetCurrentProcess(), 3 );
#else
        ::_exit( 3 );
#endif
        // Unreachable; present so the compiler accepts [[noreturn]] on every path.
        for ( ;; )
        {
        }
    }
} // namespace Common::Crash::Detail

// ═════════════════════════════════════════════════════════════════════════════════════════════════
// Windows
// ═════════════════════════════════════════════════════════════════════════════════════════════════
#if defined( DESERT_PLATFORM_WINDOWS )
namespace Common::Crash::Detail
{
    HMODULE g_MainModule = nullptr;

    const char* ExceptionCodeName( DWORD inCode )
    {
        switch ( inCode )
        {
            case EXCEPTION_ACCESS_VIOLATION:
                return "EXCEPTION_ACCESS_VIOLATION";
            case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
                return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
            case EXCEPTION_DATATYPE_MISALIGNMENT:
                return "EXCEPTION_DATATYPE_MISALIGNMENT";
            case EXCEPTION_FLT_DIVIDE_BY_ZERO:
                return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
            case EXCEPTION_ILLEGAL_INSTRUCTION:
                return "EXCEPTION_ILLEGAL_INSTRUCTION";
            case EXCEPTION_INT_DIVIDE_BY_ZERO:
                return "EXCEPTION_INT_DIVIDE_BY_ZERO";
            case EXCEPTION_PRIV_INSTRUCTION:
                return "EXCEPTION_PRIV_INSTRUCTION";
            case EXCEPTION_STACK_OVERFLOW:
                return "EXCEPTION_STACK_OVERFLOW";
            case EXCEPTION_IN_PAGE_ERROR:
                return "EXCEPTION_IN_PAGE_ERROR";
            default:
                return "EXCEPTION_UNKNOWN";
        }
    }

    void ResolveFrame( HANDLE inProcess, DWORD64 inAddress, ResolvedFrame& outFrame )
    {
        outFrame.address = inAddress;

        HMODULE owner = nullptr;
        if ( ::GetModuleHandleExW( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCWSTR>( inAddress ), &owner ) != 0 &&
             owner != nullptr )
        {
            wchar_t wide[MAX_PATH] = L"";
            ::GetModuleFileNameW( owner, wide, MAX_PATH );
            const wchar_t* leaf = std::wcsrchr( wide, L'\\' );
            leaf                = ( leaf != nullptr ) ? leaf + 1 : wide;
            ::WideCharToMultiByte( CP_UTF8, 0, leaf, -1, outFrame.module, static_cast<int>( kSmallField ), nullptr,
                                   nullptr );
            outFrame.address      = inAddress - reinterpret_cast<DWORD64>( owner );
            outFrame.inMainModule = ( owner == g_MainModule );
        }

        // SYMBOL_INFO is a variable-length struct; the name lives past its end, which is why the
        // storage is a raw byte array rather than a plain SYMBOL_INFO.
        alignas( SYMBOL_INFO ) char symbolStorage[sizeof( SYMBOL_INFO ) + kPathField] = {};
        SYMBOL_INFO*                symbol = reinterpret_cast<SYMBOL_INFO*>( symbolStorage );
        symbol->SizeOfStruct               = sizeof( SYMBOL_INFO );
        symbol->MaxNameLen                 = static_cast<ULONG>( kPathField - 1 );
        DWORD64 displacement               = 0;
        if ( ::SymFromAddr( inProcess, inAddress, &displacement, symbol ) != 0 )
        {
            CopyIntoFixed( outFrame.function, kPathField, symbol->Name );
        }

        IMAGEHLP_LINE64 line   = {};
        line.SizeOfStruct      = sizeof( IMAGEHLP_LINE64 );
        DWORD lineDisplacement = 0;
        if ( ::SymGetLineFromAddr64( inProcess, inAddress, &lineDisplacement, &line ) != 0 &&
             line.FileName != nullptr )
        {
            const char* leaf = std::strrchr( line.FileName, '\\' );
            leaf             = ( leaf != nullptr ) ? leaf + 1 : line.FileName;
            RawWriter scratch;
            CopyIntoFixed( outFrame.source, kPathField, leaf );
            const std::size_t used = std::strlen( outFrame.source );
            if ( used + 12 < kPathField )
            {
                outFrame.source[used] = ':';
                std::size_t   cursor  = used + 1;
                std::uint32_t value   = line.LineNumber;
                char          digits[12];
                std::size_t   count = 0;
                do
                {
                    digits[count++] = static_cast<char>( '0' + ( value % 10 ) );
                    value /= 10;
                } while ( value != 0 && count < sizeof( digits ) );
                while ( count > 0 )
                {
                    outFrame.source[cursor++] = digits[--count];
                }
                outFrame.source[cursor] = '\0';
            }
            (void)scratch;
        }
    }

    void WalkStack( const CONTEXT& inContext )
    {
        const HANDLE process = ::GetCurrentProcess();
        const HANDLE thread  = ::GetCurrentThread();

        // StackWalk64 WRITES to the context it is given, so it gets a copy: the same CONTEXT is
        // handed to MiniDumpWriteDump afterwards and a walked-over one describes the wrong thread.
        CONTEXT walkContext = inContext;

        STACKFRAME64 frame     = {};
        frame.AddrPC.Offset    = walkContext.Rip;
        frame.AddrPC.Mode      = AddrModeFlat;
        frame.AddrFrame.Offset = walkContext.Rbp;
        frame.AddrFrame.Mode   = AddrModeFlat;
        frame.AddrStack.Offset = walkContext.Rsp;
        frame.AddrStack.Mode   = AddrModeFlat;

        g_FrameCount = 0;
        while ( g_FrameCount < kMaxStackFrames )
        {
            if ( ::StackWalk64( IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &walkContext, nullptr,
                                ::SymFunctionTableAccess64, ::SymGetModuleBase64, nullptr ) == 0 )
            {
                break;
            }
            if ( frame.AddrPC.Offset == 0 )
            {
                break;
            }
            ResolveFrame( process, frame.AddrPC.Offset, g_Frames[g_FrameCount] );
            ++g_FrameCount;
        }
    }

    void WriteMiniDump( EXCEPTION_POINTERS* inPointers )
    {
        const HANDLE file = ::CreateFileW( g_DmpNative, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                           FILE_ATTRIBUTE_NORMAL, nullptr );
        if ( file == INVALID_HANDLE_VALUE )
        {
            WriteStderr( "[Crash] could not create crash.dmp in the report directory\n" );
            return;
        }

        MINIDUMP_EXCEPTION_INFORMATION information = {};
        information.ThreadId                       = ::GetCurrentThreadId();
        information.ExceptionPointers              = inPointers;
        information.ClientPointers                 = FALSE;

        // WithIndirectlyReferencedMemory is what makes the locals in the faulting frame readable in
        // the debugger; WithThreadInfo carries the other threads' stacks, which is where a deadlock
        // that presents as a crash actually lives. Both are what UE asks for in its default dump.
        const MINIDUMP_TYPE type =
             static_cast<MINIDUMP_TYPE>( MiniDumpWithDataSegs | MiniDumpWithHandleData | MiniDumpWithThreadInfo |
                                         MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithUnloadedModules );
        if ( ::MiniDumpWriteDump( ::GetCurrentProcess(), ::GetCurrentProcessId(), file, type, &information,
                                  nullptr, nullptr ) == FALSE )
        {
            WriteStderr( "[Crash] MiniDumpWriteDump failed\n" );
        }
        ::CloseHandle( file );
    }

    [[noreturn]] void HandleWindowsFault( EXCEPTION_POINTERS* inPointers, const char* inKind, bool inSynthesized,
                                          std::uint64_t inCodeOverride, const char* inCodeNameOverride )
    {
        if ( g_InHandler.exchange( true ) )
        {
            TerminateAfterReport();
        }

        RawCreateDirectory();

        // Symbols are initialised HERE and not at Install(): SymInitialize loads every module's PDB,
        // which costs hundreds of milliseconds and tens of megabytes on a debug build, and paying
        // that at every startup to serve a crash that usually never happens is the wrong trade.
        const HANDLE process = ::GetCurrentProcess();
        ::SymSetOptions( SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_UNDNAME );
        ::SymInitialize( process, nullptr, TRUE );

        WalkStack( *inPointers->ContextRecord );
        WriteMiniDump( inPointers );

        FaultDescription fault;
        fault.kind        = inKind;
        fault.synthesized = inSynthesized;
        fault.code = inCodeNameOverride != nullptr ? inCodeOverride : inPointers->ExceptionRecord->ExceptionCode;
        fault.codeName = inCodeNameOverride != nullptr
                              ? inCodeNameOverride
                              : ExceptionCodeName( inPointers->ExceptionRecord->ExceptionCode );
        fault.address  = reinterpret_cast<std::uint64_t>( inPointers->ExceptionRecord->ExceptionAddress );
        WriteCrashText( fault, ::GetCurrentThreadId() );

        LaunchReporter();
        TerminateAfterReport();
    }

    // The non-SEH entry points have no EXCEPTION_POINTERS of their own, so they build one the way
    // UE's WindowsPlatformCrashContext does: capture the live CONTEXT and describe the fault with a
    // synthetic record whose address is this call's return address.
    [[noreturn]] void HandleSynthesizedFault( const char* inKind, std::uint64_t inCode, const char* inCodeName,
                                              void* inAddress )
    {
        CONTEXT context = {};
        ::RtlCaptureContext( &context );

        EXCEPTION_RECORD record = {};
        record.ExceptionCode    = static_cast<DWORD>( inCode );
        record.ExceptionAddress = inAddress;

        EXCEPTION_POINTERS pointers = {};
        pointers.ExceptionRecord    = &record;
        pointers.ContextRecord      = &context;

        HandleWindowsFault( &pointers, inKind, true, inCode, inCodeName );
    }

    LONG WINAPI UnhandledFilter( EXCEPTION_POINTERS* inPointers )
    {
        HandleWindowsFault( inPointers, "exception", false, 0, nullptr );
    }

    void PureCallHandler()
    {
        HandleSynthesizedFault( "purecall", 0xC0000025u, "PURE_VIRTUAL_CALL", _ReturnAddress() );
    }

    void InvalidParameterHandler( const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t )
    {
        HandleSynthesizedFault( "invalid_parameter", 0xC000000Du, "INVALID_CRT_PARAMETER", _ReturnAddress() );
    }

    void AbortSignalHandler( int )
    {
        HandleSynthesizedFault( "signal", SIGABRT, "SIGABRT", _ReturnAddress() );
    }

    void InstallPlatformHandlers()
    {
        g_MainModule = ::GetModuleHandleW( nullptr );
        ::SetUnhandledExceptionFilter( &UnhandledFilter );
        ::_set_purecall_handler( &PureCallHandler );
        ::_set_invalid_parameter_handler( &InvalidParameterHandler );
        // WITHOUT THIS THE ABORT DIALOG EATS THE CRASH. The debug CRT's abort() pops a message box
        // and returns a non-zero exit code without ever reaching the signal handler when a console
        // is absent, so an editor crash produced no report at all.
        ::_set_abort_behavior( 0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT );
        std::signal( SIGABRT, &AbortSignalHandler );
    }

    void FormatOsDescription()
    {
        // RtlGetVersion and not GetVersionEx: the documented API lies to a process without a
        // manifest entry for the running Windows, and a crash report that says "Windows 8" on
        // Windows 11 sends the reader after the wrong thing.
        using RtlGetVersionFn          = LONG( WINAPI* )( PRTL_OSVERSIONINFOW );
        char          text[kPathField] = "Windows (version unavailable)";
        const HMODULE ntdll            = ::GetModuleHandleW( L"ntdll.dll" );
        if ( ntdll != nullptr )
        {
            const auto getVersion = reinterpret_cast<RtlGetVersionFn>(
                 reinterpret_cast<void*>( ::GetProcAddress( ntdll, "RtlGetVersion" ) ) );
            RTL_OSVERSIONINFOW version  = {};
            version.dwOSVersionInfoSize = sizeof( version );
            if ( getVersion != nullptr && getVersion( &version ) == 0 )
            {
                SYSTEM_INFO systemInfo = {};
                ::GetNativeSystemInfo( &systemInfo );
                std::snprintf( text, sizeof( text ), "Windows %lu.%lu build %lu, %u logical cores",
                               version.dwMajorVersion, version.dwMinorVersion, version.dwBuildNumber,
                               systemInfo.dwNumberOfProcessors );
            }
        }
        CopyIntoFixed( g_Os, kPathField, text );

        wchar_t wideName[MAX_COMPUTERNAME_LENGTH + 1] = L"";
        DWORD   nameLength                            = MAX_COMPUTERNAME_LENGTH + 1;
        if ( ::GetComputerNameW( wideName, &nameLength ) != 0 )
        {
            char narrow[kSmallField] = "";
            ::WideCharToMultiByte( CP_UTF8, 0, wideName, -1, narrow, static_cast<int>( kSmallField ), nullptr,
                                   nullptr );
            CopyIntoFixed( g_Machine, kSmallField, narrow );
        }
    }

    void FreezeNativePaths( const std::filesystem::path& inDirectory, const std::filesystem::path& inReporter )
    {
        const std::wstring directory = inDirectory.wstring();
        const std::wstring text      = ( inDirectory / "crash.txt" ).wstring();
        const std::wstring dump      = ( inDirectory / "crash.dmp" ).wstring();
        std::wcsncpy( g_DirNative, directory.c_str(), kPathField - 1 );
        std::wcsncpy( g_TxtNative, text.c_str(), kPathField - 1 );
        std::wcsncpy( g_DmpNative, dump.c_str(), kPathField - 1 );

        if ( !inReporter.empty() )
        {
            const std::wstring reporter = inReporter.wstring();
            std::wcsncpy( g_ReporterNative, reporter.c_str(), kPathField - 1 );
            // CreateProcessW wants a MUTABLE command line whose argv[0] is the program itself; the
            // report DIRECTORY is the single argument, quoted because it contains spaces on every
            // machine whose user name has one.
            const std::wstring command = L"\"" + reporter + L"\" \"" + directory + L"\"";
            std::wcsncpy( g_ReporterCommandLine, command.c_str(), ( kPathField * 2 ) - 1 );
            g_HasReporter = true;
        }
    }
} // namespace Common::Crash::Detail

// ═════════════════════════════════════════════════════════════════════════════════════════════════
// POSIX
// ═════════════════════════════════════════════════════════════════════════════════════════════════
#else
namespace Common::Crash::Detail
{
    // The alternate stack: SIGSEGV from a stack overflow cannot be handled on the stack that
    // overflowed, so the handler is given its own pages. This is the whole reason sigaltstack exists
    // and the reason a naive signal() install reports nothing for the most common editor crash.
    constexpr std::size_t kAltStackSize = 1 << 18;
    char                  g_AltStack[kAltStackSize];

    const char* SignalName( int inSignal )
    {
        switch ( inSignal )
        {
            case SIGSEGV:
                return "SIGSEGV";
            case SIGBUS:
                return "SIGBUS";
            case SIGILL:
                return "SIGILL";
            case SIGFPE:
                return "SIGFPE";
            case SIGABRT:
                return "SIGABRT";
            default:
                return "SIGNAL_UNKNOWN";
        }
    }

    // The image this file is linked into (Common is static, so: the host executable), taken at Install.
    // A frame is "ours" when dladdr names the same image base; the skip rule needs it.
    const void* g_MainBase = nullptr;

    // The interrupted instruction, from the context the kernel handed the handler. backtrace() walks
    // return addresses from the HANDLER, so the faulting function's own PC is not in it: a leaf that
    // dereferences null shows up only as its caller.
    std::uint64_t InterruptedPc( const void* inContext )
    {
        if ( inContext == nullptr )
        {
            return 0;
        }
        const auto* context = static_cast<const ucontext_t*>( inContext );
#if defined( __APPLE__ ) && defined( __aarch64__ )
        return static_cast<std::uint64_t>( context->uc_mcontext->__ss.__pc );
#elif defined( __APPLE__ ) && defined( __x86_64__ )
        return static_cast<std::uint64_t>( context->uc_mcontext->__ss.__rip );
#elif defined( __linux__ ) && defined( __x86_64__ )
        return static_cast<std::uint64_t>( context->uc_mcontext.gregs[REG_RIP] );
#elif defined( __linux__ ) && defined( __aarch64__ )
        return static_cast<std::uint64_t>( context->uc_mcontext.pc );
#else
        return 0;
#endif
    }

    // `inIsReturnAddress`: a backtrace entry is the instruction AFTER a call. The call of a
    // [[noreturn]] function (abort) is the last instruction of its caller, so that address already
    // belongs to the NEXT function in the image; the lookup uses the byte before it, as every
    // unwinder does. The interrupted PC is the faulting instruction itself and is looked up as is.
    void ResolveFrame( const std::uint64_t inAddress, const bool inIsReturnAddress, ResolvedFrame& outFrame )
    {
        outFrame              = {};
        outFrame.address      = inAddress;
        outFrame.inMainModule = false;
        Dl_info info          = {};
        // dladdr is what backtrace_symbols_fd itself calls on both macOS and glibc; it reads the loader's
        // image list without allocating. The name stays MANGLED: __cxa_demangle mallocs.
        const std::uint64_t lookup = inIsReturnAddress && inAddress != 0 ? inAddress - 1 : inAddress;
        if ( ::dladdr( reinterpret_cast<const void*>( lookup ), &info ) == 0 )
        {
            return;
        }
        outFrame.address      = inAddress - reinterpret_cast<std::uint64_t>( info.dli_fbase );
        outFrame.inMainModule = info.dli_fbase == g_MainBase;
        if ( info.dli_fname != nullptr )
        {
            const char* base = std::strrchr( info.dli_fname, '/' );
            CopyIntoFixed( outFrame.module, kSmallField, base != nullptr ? base + 1 : info.dli_fname );
        }
        if ( info.dli_sname != nullptr )
        {
            CopyIntoFixed( outFrame.function, kPathField, info.dli_sname );
        }
    }

    // Frame 0 is the interrupted PC, then the handler's own backtrace (handler, trampoline, and the
    // callers of the faulting function). Every POSIX report is therefore `synthesized`: the skip rule
    // picks the innermost frame outside Common::Crash::Detail in the host image.
    void CaptureFrames( const void* inContext )
    {
        g_FrameCount = 0;
        if ( const std::uint64_t pc = InterruptedPc( inContext ); pc != 0 )
        {
            ResolveFrame( pc, false, g_Frames[g_FrameCount++] );
        }
        void*     addresses[kMaxStackFrames];
        const int count = ::backtrace( addresses, static_cast<int>( kMaxStackFrames ) );
        for ( int i = 0; i < count && g_FrameCount < kMaxStackFrames; ++i )
        {
            ResolveFrame( reinterpret_cast<std::uint64_t>( addresses[i] ), true, g_Frames[g_FrameCount++] );
        }
    }

    void SignalHandler( int inSignal, siginfo_t* inInfo, void* inContext )
    {
        if ( g_InHandler.exchange( true ) )
        {
            TerminateAfterReport();
        }

        RawCreateDirectory();

        // NOTHING BELOW MAY ALLOCATE: backtrace() fills a caller-supplied array, dladdr reads the
        // loader's list, and the frames land in the fixed g_Frames, so the shared writer emits [stack].
        CaptureFrames( inContext );

        FaultDescription fault;
        fault.kind        = "signal";
        fault.code        = static_cast<std::uint64_t>( inSignal );
        fault.codeIsHex   = false;
        fault.codeName    = SignalName( inSignal );
        fault.address     = reinterpret_cast<std::uint64_t>( inInfo != nullptr ? inInfo->si_addr : nullptr );
        fault.synthesized = true;
        WriteCrashText( fault, static_cast<std::uint64_t>( ::getpid() ) );

        LaunchReporter();
        TerminateAfterReport();
    }

    void InstallPlatformHandlers()
    {
        Dl_info self = {};
        if ( ::dladdr( reinterpret_cast<const void*>( &InstallPlatformHandlers ), &self ) != 0 )
        {
            g_MainBase = self.dli_fbase;
        }

        stack_t alternate  = {};
        alternate.ss_sp    = g_AltStack;
        alternate.ss_size  = kAltStackSize;
        alternate.ss_flags = 0;
        ::sigaltstack( &alternate, nullptr );

        struct sigaction action = {};
        action.sa_sigaction     = &SignalHandler;
        action.sa_flags         = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
        // Unqualified on purpose: macOS <signal.h> defines sigemptyset as a MACRO, and `::sigemptyset(`
        // does not compile there (the first POSIX build of this file, PKG1).
        sigemptyset( &action.sa_mask );

        const int signals[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT };
        for ( const int number : signals )
        {
            ::sigaction( number, &action, nullptr );
        }
    }

    void FormatOsDescription()
    {
        utsname system           = {};
        char    text[kPathField] = "unknown";
        if ( ::uname( &system ) == 0 )
        {
            std::snprintf( text, sizeof( text ), "%s %s %s", system.sysname, system.release, system.machine );
            CopyIntoFixed( g_Machine, kSmallField, system.nodename );
        }
        CopyIntoFixed( g_Os, kPathField, text );
    }

    void FreezeNativePaths( const std::filesystem::path& inDirectory, const std::filesystem::path& inReporter )
    {
        CopyIntoFixed( g_DmpUtf8, kPathField, ( inDirectory / "crash.dmp" ).string() );
        if ( !inReporter.empty() )
        {
            CopyIntoFixed( g_ReporterUtf8, kPathField, inReporter.string() );
            g_ReporterArgv[0] = g_ReporterUtf8;
            g_ReporterArgv[1] = g_DirUtf8;
            g_ReporterArgv[2] = nullptr;
            g_HasReporter     = true;
        }
    }
} // namespace Common::Crash::Detail
#endif

// ═════════════════════════════════════════════════════════════════════════════════════════════════
// The public surface
// ═════════════════════════════════════════════════════════════════════════════════════════════════
namespace Common::Crash
{
    namespace
    {
        std::filesystem::path ResolveReportRoot( const InstallOptions& inOptions )
        {
            if ( !inOptions.reportRootOverride.empty() )
            {
                return inOptions.reportRootOverride;
            }
            if ( !inOptions.gameUserDirectory.empty() )
            {
                return inOptions.gameUserDirectory / "Crashes";
            }
            if ( !inOptions.projectRoot.empty() )
            {
                return inOptions.projectRoot / "Saved" / "Crashes";
            }
#if defined( DESERT_PLATFORM_WINDOWS )
            const char* localAppData = std::getenv( "LOCALAPPDATA" );
            if ( localAppData != nullptr && localAppData[0] != '\0' )
            {
                return std::filesystem::path( localAppData ) / "DesertEngine" / "Crashes";
            }
            // No LOCALAPPDATA means a service account or a stripped environment. Naming the reason
            // matters more than the location, so this is reported rather than guessed around.
            return {};
#else
            const char* home = std::getenv( "HOME" );
            if ( home != nullptr && home[0] != '\0' )
            {
                return Common::Settings::EngineUserDirectoryUnder( home ) / "Crashes";
            }
            return {};
#endif
        }

        std::filesystem::path ExecutableDirectory()
        {
#if defined( DESERT_PLATFORM_WINDOWS )
            wchar_t     path[MAX_PATH] = L"";
            const DWORD length         = ::GetModuleFileNameW( nullptr, path, MAX_PATH );
            if ( length == 0 )
            {
                return {};
            }
            return std::filesystem::path( std::wstring( path, length ) ).parent_path();
#else
            std::error_code             error;
            const std::filesystem::path self = std::filesystem::read_symlink( "/proc/self/exe", error );
            if ( !error )
            {
                return self.parent_path();
            }
            return {};
#endif
        }

        std::string FormatStartStamp()
        {
            const std::time_t now   = std::time( nullptr );
            std::tm           local = {};
#if defined( DESERT_PLATFORM_WINDOWS )
            ::localtime_s( &local, &now );
#else
            ::localtime_r( &now, &local );
#endif
            char text[32] = "";
            std::strftime( text, sizeof( text ), "%Y%m%d-%H%M%S", &local );
            return text;
        }
    } // namespace

    BoolResultStr Install( const InstallOptions& inOptions )
    {
        using namespace Detail;

        if ( g_Installed.load( std::memory_order_acquire ) )
        {
            return Common::MakeFormattedError(
                 "Common::Crash::Install was called twice (already installed for host '{}')", g_Host );
        }

        const std::filesystem::path root = ResolveReportRoot( inOptions );
        if ( root.empty() )
        {
            return Common::MakeFormattedError(
                 "no crash report location: project root is empty and the per-user base directory "
                 "could not be resolved from the environment (host '{}')",
                 inOptions.hostName );
        }

        const std::string stamp = FormatStartStamp();
#if defined( DESERT_PLATFORM_WINDOWS )
        const std::uint64_t pid = ::GetCurrentProcessId();
#else
        const std::uint64_t pid = static_cast<std::uint64_t>( ::getpid() );
#endif
        const std::filesystem::path directory = root / ( stamp + "-" + std::to_string( pid ) );

        // The ROOT is created now, while the process is healthy and can report a failure; the
        // per-crash directory is created inside the handler, so a run that never crashes leaves no
        // empty folders behind.
        std::error_code error;
        std::filesystem::create_directories( root, error );
        if ( error )
        {
            return Common::MakeFormattedError( "could not create the crash report directory {}: {} ({})",
                                               root.string(), error.message(), error.value() );
        }

        CopyIntoFixed( g_Host, kSmallField, inOptions.hostName.empty() ? "unknown" : inOptions.hostName );
        CopyIntoFixed( g_Version, kSmallField, Common::Version::Full() );
        CopyIntoFixed( g_Sha, kSmallField, Common::Version::Hash() );
        CopyIntoFixed( g_Branch, kSmallField, Common::Version::Branch() );
        g_Dirty = Common::Version::Dirty();
        CopyIntoFixed( g_Started, kSmallField, stamp );
        CopyIntoFixed( g_DirUtf8, kPathField, directory.string() );
        CopyIntoFixed( g_TxtUtf8, kPathField, ( directory / "crash.txt" ).string() );
        FormatOsDescription();

        std::filesystem::path reporter = inOptions.reporterExecutable;
        if ( reporter.empty() )
        {
            const std::filesystem::path beside = ExecutableDirectory();
            if ( !beside.empty() )
            {
#if defined( DESERT_PLATFORM_WINDOWS )
                reporter = beside / "DesertCrashReporter.exe";
#else
                reporter = beside / "DesertCrashReporter";
#endif
            }
        }
        if ( !reporter.empty() && !std::filesystem::exists( reporter ) )
        {
            reporter.clear();
        }
        FreezeNativePaths( directory, reporter );

        // The ring sink joins the logger that LogInit() already created, so every line the host has
        // logged from this point on is in the report. Appending to the existing default logger and
        // not replacing it: the console, the file and the debugger sinks must keep working.
        if ( const std::shared_ptr<spdlog::logger> logger = spdlog::default_logger(); logger != nullptr )
        {
            logger->sinks().push_back( std::make_shared<RingSink>() );
        }

        InstallPlatformHandlers();
        g_ReportRoot = root;
        g_Installed.store( true, std::memory_order_release );

        LOG_INFO( "[Crash] handler installed for '{}'; reports go to {}", inOptions.hostName, directory.string() );
        if ( !g_HasReporter )
        {
            LOG_INFO( "[Crash] no reporter executable found - a crash will print its report path to stderr "
                      "instead of launching one" );
        }
        return Common::MakeSuccess( true );
    }

    bool IsInstalled()
    {
        return Detail::g_Installed.load( std::memory_order_acquire );
    }

    const std::filesystem::path& ReportRootDirectory()
    {
        return Detail::g_ReportRoot;
    }

    void SetScenePath( std::string_view inScenePath )
    {
        Detail::CopyIntoFixed( Detail::g_Scene, Detail::kPathField, inScenePath.empty() ? "none" : inScenePath );
    }

    void SetGpuDescription( std::string_view inGpuDescription )
    {
        Detail::CopyIntoFixed( Detail::g_Gpu, Detail::kPathField,
                               inGpuDescription.empty() ? "unknown" : inGpuDescription );
    }

    std::optional<TestKind> ParseTestKind( std::string_view inWord )
    {
        if ( inWord == "segv" )
        {
            return TestKind::Segv;
        }
        if ( inWord == "abort" )
        {
            return TestKind::Abort;
        }
        if ( inWord == "purecall" )
        {
            return TestKind::PureCall;
        }
        return std::nullopt;
    }

    const char* TestKindName( TestKind inKind )
    {
        switch ( inKind )
        {
            case TestKind::Segv:
                return "segv";
            case TestKind::Abort:
                return "abort";
            case TestKind::PureCall:
                return "purecall";
        }
        return "unknown";
    }

    namespace
    {
        // These three names are the contract the CR1 suite asserts on: each must appear verbatim in
        // crash.txt when the matching --crash-test runs. They are deliberately outside
        // Common::Crash::Detail so the synthesized-fault skip rule keeps them.
        volatile int* g_NullTarget = nullptr;

        [[noreturn]] void CrashTestSegv()
        {
            // `volatile` so the store is not elided: MSVC and clang both delete a dead null store and
            // then the "crash test" quietly does nothing, which is the exact failure this whole
            // module exists to make impossible.
            *g_NullTarget = 1;
            std::abort();
        }

        [[noreturn]] void CrashTestAbort()
        {
            std::abort();
        }

        struct PureCallBase
        {
            PureCallBase()
            {
                Finish();
            }
            virtual ~PureCallBase() = default;
            virtual void Detonate() = 0;
            void         Finish()
            {
                // Calling a pure virtual from the base constructor: the vtable is still the base's,
                // so the CRT's purecall handler runs. This is the canonical reproduction and the one
                // UE's own crash-test menu uses.
                Detonate();
            }
        };

        struct PureCallDerived final : PureCallBase
        {
            void Detonate() override
            {
            }
        };

        [[noreturn]] void CrashTestPureCall()
        {
            PureCallDerived derived;
            (void)derived;
            // If the purecall handler returned instead of terminating, say so rather than exiting
            // clean and letting the test read a missing report as a passing one.
            std::abort();
        }
    } // namespace

    void TriggerTestCrash( TestKind inKind )
    {
        switch ( inKind )
        {
            case TestKind::Segv:
                CrashTestSegv();
            case TestKind::Abort:
                CrashTestAbort();
            case TestKind::PureCall:
                CrashTestPureCall();
        }
        std::abort();
    }
} // namespace Common::Crash
