#include "CompilerProcess.hpp"

#include <Windows.h>

#include <algorithm>
#include <chrono>

namespace Desert::Editor::LiveCoding
{
    namespace
    {
        std::wstring Widen( const std::string& text )
        {
            if ( text.empty() )
            {
                return {};
            }
            const int    size = MultiByteToWideChar( CP_UTF8, 0, text.data(), int( text.size() ), nullptr, 0 );
            std::wstring out( std::size_t( size ), L'\0' );
            MultiByteToWideChar( CP_UTF8, 0, text.data(), int( text.size() ), out.data(), size );
            return out;
        }

        /// One cl.exe the runner started: its process, the read end of its output pipe, its slot in the results.
        struct Child
        {
            HANDLE                                Process = nullptr;
            HANDLE                                Output  = nullptr;
            std::size_t                           Job     = 0;
            std::chrono::steady_clock::time_point Started;
        };

        void Drain( HANDLE pipe, std::string& into )
        {
            char  buffer[4096];
            DWORD available = 0;
            while ( PeekNamedPipe( pipe, nullptr, 0, nullptr, &available, nullptr ) && available > 0 )
            {
                DWORD read = 0;
                if ( !ReadFile( pipe, buffer, std::min<DWORD>( available, sizeof( buffer ) ), &read, nullptr ) ||
                     read == 0 )
                {
                    return;
                }
                into.append( buffer, read );
            }
        }

        void Close( Child& child )
        {
            CloseHandle( child.Process );
            CloseHandle( child.Output );
            child.Process = nullptr;
            child.Output  = nullptr;
        }

        /// Starts one job with its stdout and stderr on a pipe only it inherits
        /// (PROC_THREAD_ATTRIBUTE_HANDLE_LIST: a sibling cl.exe started meanwhile must not hold this pipe's write
        /// end).
        Common::ResultStr<Child> Start( const CompileJob& job, std::size_t index )
        {
            SECURITY_ATTRIBUTES inherit{ sizeof( SECURITY_ATTRIBUTES ), nullptr, TRUE };
            HANDLE              read  = nullptr;
            HANDLE              write = nullptr;
            if ( !CreatePipe( &read, &write, &inherit, 1 << 16 ) )
            {
                return Common::MakeFormattedError<Child>( "CreatePipe failed ({})", GetLastError() );
            }
            SetHandleInformation( read, HANDLE_FLAG_INHERIT, 0 );

            SIZE_T attributeSize = 0;
            InitializeProcThreadAttributeList( nullptr, 1, 0, &attributeSize );
            std::vector<std::byte> attributeStorage( attributeSize );
            auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>( attributeStorage.data() );
            if ( !InitializeProcThreadAttributeList( attributes, 1, 0, &attributeSize ) ||
                 !UpdateProcThreadAttribute( attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &write,
                                             sizeof( HANDLE ), nullptr, nullptr ) )
            {
                const DWORD error = GetLastError();
                CloseHandle( read );
                CloseHandle( write );
                return Common::MakeFormattedError<Child>( "the process attribute list failed ({})", error );
            }

            STARTUPINFOEXW startup{};
            startup.StartupInfo.cb         = sizeof( STARTUPINFOEXW );
            startup.StartupInfo.dwFlags    = STARTF_USESTDHANDLES;
            startup.StartupInfo.hStdInput  = nullptr;
            startup.StartupInfo.hStdOutput = write;
            startup.StartupInfo.hStdError  = write;
            startup.lpAttributeList        = attributes;

            std::wstring        command   = Widen( job.Command );
            const std::wstring  directory = job.Directory.wstring();
            PROCESS_INFORMATION process{};
            const BOOL          started = CreateProcessW(
                 nullptr, command.data(), nullptr, nullptr, TRUE,
                 CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS | EXTENDED_STARTUPINFO_PRESENT, nullptr,
                 directory.empty() ? nullptr : directory.c_str(), &startup.StartupInfo, &process );
            const DWORD error = GetLastError();
            DeleteProcThreadAttributeList( attributes );
            CloseHandle( write ); // the child holds its own copy; EOF follows its exit
            if ( !started )
            {
                CloseHandle( read );
                return Common::MakeFormattedError<Child>( "cannot start the compiler for {} ({}): {}",
                                                          job.Object.generic_string(), error, job.Command );
            }
            CloseHandle( process.hThread );
            return Common::MakeSuccess( Child{ process.hProcess, read, index, std::chrono::steady_clock::now() } );
        }
    } // namespace

    Common::ResultStr<std::vector<CompileResult>>
    RunCompilers( std::span<const CompileJob> jobs, unsigned parallel, const std::atomic<bool>& cancel ) noexcept
    {
        std::vector<CompileResult> results( jobs.size() );
        std::vector<Child>         running;
        const std::size_t          width = std::clamp<std::size_t>( parallel, 1, MAXIMUM_WAIT_OBJECTS );
        running.reserve( width );
        std::size_t next = 0;

        const auto stopAll = [&]( const std::string& reason )
        {
            for ( Child& child : running )
            {
                TerminateProcess( child.Process, 1 ); // our own child, by its handle
                WaitForSingleObject( child.Process, INFINITE );
                Close( child );
            }
            running.clear();
            return Common::MakeError<std::vector<CompileResult>>( reason );
        };

        while ( next < jobs.size() || !running.empty() )
        {
            if ( cancel.load( std::memory_order_relaxed ) )
            {
                return stopAll( "cancelled: the editor is closing" );
            }
            while ( next < jobs.size() && running.size() < width )
            {
                Common::ResultStr<Child> child = Start( jobs[next], next );
                if ( !child )
                {
                    return stopAll( child.GetError() );
                }
                running.push_back( child.ExtractValue() );
                ++next;
            }

            std::vector<HANDLE> handles;
            handles.reserve( running.size() );
            for ( const Child& child : running )
            {
                handles.push_back( child.Process );
            }
            // Short waits: the pipes are drained between them (a full pipe would block cl.exe), and cancel is
            // seen.
            const DWORD wait = WaitForMultipleObjects( DWORD( handles.size() ), handles.data(), FALSE, 50 );
            if ( wait == WAIT_FAILED )
            {
                return stopAll( "WaitForMultipleObjects failed (" + std::to_string( GetLastError() ) + ")" );
            }
            for ( std::size_t i = 0; i < running.size(); )
            {
                Child& child = running[i];
                Drain( child.Output, results[child.Job].Output );
                if ( WaitForSingleObject( child.Process, 0 ) != WAIT_OBJECT_0 )
                {
                    ++i;
                    continue;
                }
                Drain( child.Output, results[child.Job].Output );
                DWORD exitCode = 0;
                GetExitCodeProcess( child.Process, &exitCode );
                results[child.Job].ExitCode = int( exitCode );
                results[child.Job].Seconds =
                     std::chrono::duration<double>( std::chrono::steady_clock::now() - child.Started ).count();
                Close( child );
                running.erase( running.begin() + std::ptrdiff_t( i ) );
            }
        }
        return Common::MakeSuccess( std::move( results ) );
    }
} // namespace Desert::Editor::LiveCoding
