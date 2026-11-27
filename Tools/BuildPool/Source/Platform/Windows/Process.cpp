// BuildPool platform, Windows: ChildJob on a Job Object and SpawnDetached.
#include "BuildPool/Platform.hpp"

#include "Win.hpp"

#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cwchar>
#include <iterator>
#include <map>

#pragma comment( lib, "psapi.lib" )

namespace buildpool
{
    namespace
    {
        DWORD PriorityClassOf( OsPriority priority ) noexcept
        {
            switch ( priority )
            {
                case OsPriority::BelowNormal:
                    return BELOW_NORMAL_PRIORITY_CLASS;
                case OsPriority::Idle:
                    return IDLE_PRIORITY_CLASS;
                case OsPriority::Normal:
                    break;
            }
            return NORMAL_PRIORITY_CLASS;
        }

        struct CaseInsensitiveLess
        {
            bool operator()( const std::wstring& a, const std::wstring& b ) const noexcept
            {
                return CompareStringOrdinal( a.c_str(), static_cast<int>( a.size() ), b.c_str(),
                                             static_cast<int>( b.size() ), TRUE ) == CSTR_LESS_THAN;
            }
        };

        // The inherited environment with `overrides` applied, as a sorted CREATE_UNICODE_ENVIRONMENT block.
        // Variables whose name starts with '=' (the per-drive current directories) are kept as they are.
        std::wstring EnvironmentBlock( const std::vector<std::pair<std::string, std::string>>& overrides )
        {
            std::map<std::wstring, std::wstring, CaseInsensitiveLess> variables;
            std::vector<std::wstring>                                 driveEntries;
            if ( wchar_t* block = GetEnvironmentStringsW() )
            {
                for ( const wchar_t* entry = block; *entry != L'\0'; entry += wcslen( entry ) + 1 )
                {
                    const std::wstring_view text( entry );
                    if ( text.front() == L'=' )
                    {
                        driveEntries.emplace_back( text );
                        continue;
                    }
                    const size_t equals = text.find( L'=' );
                    if ( equals == std::wstring_view::npos )
                    {
                        continue;
                    }
                    variables[std::wstring( text.substr( 0, equals ) )] =
                         std::wstring( text.substr( equals + 1 ) );
                }
                FreeEnvironmentStringsW( block );
            }
            for ( const auto& [name, value] : overrides )
            {
                const std::wstring wideName = win::Widen( name );
                if ( value.empty() )
                {
                    variables.erase( wideName );
                }
                else
                {
                    variables[wideName] = win::Widen( value );
                }
            }
            std::wstring result;
            for ( const std::wstring& entry : driveEntries )
            {
                result += entry;
                result.push_back( L'\0' );
            }
            for ( const auto& [name, value] : variables )
            {
                result += name;
                result.push_back( L'=' );
                result += value;
                result.push_back( L'\0' );
            }
            result.push_back( L'\0' );
            return result;
        }

        // The std handles a child inherits, made inheritable and listed once each
        // (PROC_THREAD_ATTRIBUTE_HANDLE_LIST rejects duplicates and non-inheritable handles). Only these handles
        // reach the child.
        void AddInheritable( std::vector<HANDLE>& list, HANDLE handle ) noexcept
        {
            if ( handle == nullptr || handle == INVALID_HANDLE_VALUE )
            {
                return;
            }
            if ( std::find( list.begin(), list.end(), handle ) != list.end() )
            {
                return;
            }
            DWORD flags = 0;
            if ( !GetHandleInformation( handle, &flags ) )
            {
                return;
            }
            if ( ( flags & HANDLE_FLAG_INHERIT ) == 0 &&
                 !SetHandleInformation( handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT ) )
            {
                return;
            }
            list.push_back( handle );
        }

        // CreateProcessW with exactly `inherit` inherited and the std handles given.
        struct Launch
        {
            std::wstring commandLine;
            std::wstring environment; // empty: inherit as it is
            DWORD        flags  = 0;
            HANDLE       input  = nullptr;
            HANDLE       output = nullptr;
            HANDLE       error  = nullptr;
        };

        // `createError` receives CreateProcessW's error code when that call is what failed (0 otherwise).
        Result<PROCESS_INFORMATION> CreateWithHandles( Launch& launch, DWORD& createError )
        {
            createError = 0;
            std::vector<HANDLE> inherit;
            AddInheritable( inherit, launch.input );
            AddInheritable( inherit, launch.output );
            AddInheritable( inherit, launch.error );

            SIZE_T attributeBytes = 0;
            InitializeProcThreadAttributeList( nullptr, 1, 0, &attributeBytes );
            std::vector<std::byte> attributeStorage( attributeBytes );
            auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>( attributeStorage.data() );
            if ( !InitializeProcThreadAttributeList( attributes, 1, 0, &attributeBytes ) )
            {
                return Fail( win::LastErrorText( "InitializeProcThreadAttributeList" ) );
            }
            struct AttributeListGuard
            {
                LPPROC_THREAD_ATTRIBUTE_LIST list;
                ~AttributeListGuard()
                {
                    DeleteProcThreadAttributeList( list );
                }
            } guard{ attributes };
            if ( !inherit.empty() &&
                 !UpdateProcThreadAttribute( attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit.data(),
                                             inherit.size() * sizeof( HANDLE ), nullptr, nullptr ) )
            {
                return Fail( win::LastErrorText( "UpdateProcThreadAttribute(HANDLE_LIST)" ) );
            }

            STARTUPINFOEXW startup{};
            startup.StartupInfo.cb         = sizeof( startup );
            startup.StartupInfo.dwFlags    = STARTF_USESTDHANDLES;
            startup.StartupInfo.hStdInput  = launch.input;
            startup.StartupInfo.hStdOutput = launch.output;
            startup.StartupInfo.hStdError  = launch.error;
            startup.lpAttributeList        = attributes;

            DWORD flags       = launch.flags | EXTENDED_STARTUPINFO_PRESENT;
            void* environment = nullptr;
            if ( !launch.environment.empty() )
            {
                flags |= CREATE_UNICODE_ENVIRONMENT;
                environment = launch.environment.data();
            }
            if ( launch.commandLine.size() >= 32767 )
            {
                return Fail( "command line of " + std::to_string( launch.commandLine.size() ) +
                             " characters exceeds the Windows limit of 32767" );
            }
            PROCESS_INFORMATION info{};
            if ( !CreateProcessW( nullptr, launch.commandLine.data(), nullptr, nullptr,
                                  inherit.empty() ? FALSE : TRUE, flags, environment, nullptr,
                                  &startup.StartupInfo, &info ) )
            {
                createError = GetLastError();
                return Fail( win::ErrorText(
                     "CreateProcessW " + win::Narrow( launch.commandLine.substr( 0, 200 ) ), createError ) );
            }
            return info;
        }

        // A uniquely named inbound pipe: the read end is overlapped (ReadStdout waits with a timeout), the write
        // end is the child's stdout.
        Status CreateStdoutPipe( win::Handle& readEnd, win::Handle& writeEnd, const void* unique )
        {
            const std::wstring name = L"\\\\.\\pipe\\buildpool-out-" + std::to_wstring( GetCurrentProcessId() ) +
                                      L"-" + std::to_wstring( reinterpret_cast<uintptr_t>( unique ) ) + L"-" +
                                      std::to_wstring( GetTickCount64() );
            readEnd.Reset( CreateNamedPipeW(
                 name.c_str(), PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                 PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 0, 1 << 16, 0,
                 nullptr ) );
            if ( !readEnd.Valid() )
            {
                return Fail( win::LastErrorText( "CreateNamedPipeW (stdout)" ) );
            }
            SECURITY_ATTRIBUTES inheritable{ sizeof( SECURITY_ATTRIBUTES ), nullptr, TRUE };
            writeEnd.Reset( CreateFileW( name.c_str(), GENERIC_WRITE, 0, &inheritable, OPEN_EXISTING,
                                         FILE_ATTRIBUTE_NORMAL, nullptr ) );
            if ( !writeEnd.Valid() )
            {
                return Fail( win::LastErrorText( "open stdout pipe" ) );
            }
            return Ok();
        }

        // ---- The MSVC PDB server
        // mspdbsrv serves every cl/link of the user. Started by a tool inside a ChildJob it would die with that
        // job (Terminate, or KILL_ON_JOB_CLOSE when the client dies) under every other build, and its memory would
        // count as that TU's peak. So the pool starts it itself, outside every job, before the child runs.

        // The mspdbsrv.exe beside the first command-line word that names an executable with one next to it: the
        // compiler or linker, also behind a wrapper (ccache cl ..., cmd /c cl ...). Empty: no MSVC tool runs.
        std::wstring PdbServerFor( const std::vector<std::string>& argv )
        {
            for ( const std::string& arg : argv )
            {
                if ( arg.empty() || arg.front() == '/' || arg.front() == '-' || arg.front() == '@' )
                {
                    continue; // an option or a response file, not a program
                }
                const std::wstring name = win::Widen( arg );
                wchar_t            found[MAX_PATH * 2]{};
                wchar_t*           filePart = nullptr;
                const DWORD        length   = SearchPathW( nullptr, name.c_str(), L".exe",
                                                           static_cast<DWORD>( std::size( found ) ), found, &filePart );
                if ( length == 0 || length >= std::size( found ) || filePart == nullptr )
                {
                    continue;
                }
                std::wstring server( found, static_cast<size_t>( filePart - found ) );
                server += L"mspdbsrv.exe";
                const DWORD attributes = GetFileAttributesW( server.c_str() );
                if ( attributes != INVALID_FILE_ATTRIBUTES && ( attributes & FILE_ATTRIBUTE_DIRECTORY ) == 0 )
                {
                    return server;
                }
            }
            return {};
        }

        bool SamePath( const std::wstring& a, const std::wstring& b ) noexcept
        {
            return CompareStringOrdinal( a.c_str(), static_cast<int>( a.size() ), b.c_str(),
                                         static_cast<int>( b.size() ), TRUE ) == CSTR_EQUAL;
        }

        // Whether a process of exactly this image is running.
        Result<bool> PdbServerRunning( const std::wstring& server )
        {
            win::Handle snapshot( CreateToolhelp32Snapshot( TH32CS_SNAPPROCESS, 0 ) );
            if ( !snapshot.Valid() )
            {
                return Fail( win::LastErrorText( "CreateToolhelp32Snapshot" ) );
            }
            PROCESSENTRY32W entry{};
            entry.dwSize = sizeof( entry );
            for ( BOOL more = Process32FirstW( snapshot.Get(), &entry ); more;
                  more      = Process32NextW( snapshot.Get(), &entry ) )
            {
                const std::wstring image( entry.szExeFile );
                if ( !SamePath( image, L"mspdbsrv.exe" ) )
                {
                    continue;
                }
                win::Handle process(
                     OpenProcess( PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID ) );
                if ( !process.Valid() )
                {
                    continue; // exited since the snapshot, or another user's
                }
                wchar_t path[MAX_PATH * 2]{};
                DWORD   pathLength = static_cast<DWORD>( std::size( path ) );
                if ( QueryFullProcessImageNameW( process.Get(), 0, path, &pathLength ) &&
                     SamePath( std::wstring( path, pathLength ), server ) )
                {
                    return true;
                }
            }
            return false;
        }

        // Starts `server` detached and outside every job, with the environment the child gets (the endpoint
        // variable _MSPDBSRV_ENDPOINT_ must match). A job that forbids breakaway is an error: the server would
        // otherwise die with one compile under every other build.
        Status EnsurePdbServer( const ChildSpec& spec )
        {
            const std::wstring server = PdbServerFor( spec.argv );
            if ( server.empty() )
            {
                return Ok();
            }
            Result<bool> running = PdbServerRunning( server );
            if ( !running )
            {
                return running.error();
            }
            if ( running.value() )
            {
                return Ok();
            }

            SECURITY_ATTRIBUTES inheritable{ sizeof( SECURITY_ATTRIBUTES ), nullptr, TRUE };
            win::Handle nul( CreateFileW( L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                          &inheritable, OPEN_EXISTING, 0, nullptr ) );
            if ( !nul.Valid() )
            {
                return Fail( win::LastErrorText( "open NUL" ) );
            }
            Launch launch;
            launch.commandLine = win::QuoteCommandLine( { win::Narrow( server ), "-start", "-spawn" } );
            if ( !spec.environment.empty() )
            {
                launch.environment = EnvironmentBlock( spec.environment );
            }
            launch.flags  = DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP | CREATE_BREAKAWAY_FROM_JOB;
            launch.input  = nul.Get();
            launch.output = nul.Get();
            launch.error  = nul.Get();
            DWORD                       createError = 0;
            Result<PROCESS_INFORMATION> created     = CreateWithHandles( launch, createError );
            if ( !created )
            {
                if ( createError == ERROR_ACCESS_DENIED )
                {
                    return Fail( "mspdbsrv cannot leave this client's job (the job forbids breakaway); started "
                                 "inside it, it would die with one compile under every other build" );
                }
                return created.error();
            }
            // Two clients racing here both start one; the second fails to claim the endpoint and exits.
            CloseHandle( created.value().hThread );
            CloseHandle( created.value().hProcess );
            return Ok();
        }
    } // namespace

    struct ChildJob::Impl
    {
        win::Handle      job;
        win::Handle      process;
        win::Handle      stdoutRead;
        win::Handle      readEvent;
        bool             exited     = false;
        int              exitCode   = 0;
        bool             terminated = false;
        mutable uint64_t peakSeen   = 0;
    };

    ChildJob::ChildJob( std::unique_ptr<Impl> impl ) noexcept : m_impl( std::move( impl ) )
    {
    }
    ChildJob::ChildJob( ChildJob&& ) noexcept = default;

    ChildJob& ChildJob::operator=( ChildJob&& other ) noexcept
    {
        if ( this != &other )
        {
            {
                ChildJob retired( std::move( *this ) ); // the destructor's rules apply to the job being replaced
            }
            m_impl = std::move( other.m_impl );
        }
        return *this;
    }

    ChildJob::~ChildJob()
    {
        if ( !m_impl )
        {
            return;
        }
        if ( !m_impl->exited && !m_impl->terminated )
        {
            // Still running: the RAII contract kills the whole tree.
            Terminate();
            return;
        }
        // The direct child exited by itself. A process it left behind on purpose (a server it started for other
        // tools) must outlive this job: KILL_ON_JOB_CLOSE is withdrawn before the job handle closes.
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        if ( QueryInformationJobObject( m_impl->job.Get(), JobObjectExtendedLimitInformation, &limits,
                                        sizeof( limits ), nullptr ) )
        {
            limits.BasicLimitInformation.LimitFlags &= ~static_cast<DWORD>( JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE );
            SetInformationJobObject( m_impl->job.Get(), JobObjectExtendedLimitInformation, &limits,
                                     sizeof( limits ) );
        }
    }

    Result<ChildJob> ChildJob::Spawn( const ChildSpec& spec ) noexcept
    {
        if ( spec.argv.empty() )
        {
            return Fail( "ChildJob::Spawn: empty argv" );
        }
        // The MSVC PDB server is shared by every build of the user: it is started outside any job before the
        // child, so the child connects to it instead of starting one inside this job.
        if ( Status server = EnsurePdbServer( spec ); !server )
        {
            return server.error();
        }
        auto impl = std::make_unique<Impl>();

        // A job nested inside whatever job this client runs in (Windows 8+ nests jobs).
        impl->job.Reset( CreateJobObjectW( nullptr, nullptr ) );
        if ( !impl->job.Valid() )
        {
            return Fail( win::LastErrorText( "CreateJobObjectW" ) );
        }
        // BREAKAWAY_OK: a nested client inside this job may start the daemon and mspdbsrv outside every job.
        // Only a process created with an explicit CREATE_BREAKAWAY_FROM_JOB leaves, so the measurement holds.
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags =
             JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_PRIORITY_CLASS | JOB_OBJECT_LIMIT_BREAKAWAY_OK;
        limits.BasicLimitInformation.PriorityClass = PriorityClassOf( spec.priority );
        if ( !SetInformationJobObject( impl->job.Get(), JobObjectExtendedLimitInformation, &limits,
                                       sizeof( limits ) ) )
        {
            return Fail( win::LastErrorText( "SetInformationJobObject(KILL_ON_JOB_CLOSE)" ) );
        }

        win::Handle stdoutWrite;
        if ( spec.captureStdout )
        {
            if ( Status piped = CreateStdoutPipe( impl->stdoutRead, stdoutWrite, impl.get() ); !piped )
            {
                return piped.error();
            }
            impl->readEvent.Reset( CreateEventW( nullptr, TRUE, FALSE, nullptr ) );
            if ( !impl->readEvent.Valid() )
            {
                return Fail( win::LastErrorText( "CreateEventW" ) );
            }
        }

        Launch launch;
        launch.commandLine = win::QuoteCommandLine( spec.argv );
        if ( !spec.environment.empty() )
        {
            launch.environment = EnvironmentBlock( spec.environment );
        }
        // Suspended until it is inside the job, so nothing it starts escapes the measurement.
        launch.flags  = CREATE_SUSPENDED | PriorityClassOf( spec.priority );
        launch.input  = GetStdHandle( STD_INPUT_HANDLE );
        launch.output = spec.captureStdout ? stdoutWrite.Get() : GetStdHandle( STD_OUTPUT_HANDLE );
        launch.error  = GetStdHandle( STD_ERROR_HANDLE );

        DWORD                       createError = 0;
        Result<PROCESS_INFORMATION> created     = CreateWithHandles( launch, createError );
        stdoutWrite.Reset(); // the child holds the only write end now: its exit ends the stream
        if ( !created )
        {
            return created.error();
        }
        PROCESS_INFORMATION info = created.value();
        impl->process.Reset( info.hProcess );
        win::Handle thread( info.hThread );
        if ( !AssignProcessToJobObject( impl->job.Get(), impl->process.Get() ) )
        {
            const std::string reason = win::LastErrorText( "AssignProcessToJobObject" );
            TerminateProcess( impl->process.Get(), 1 );
            return Fail( reason );
        }
        if ( ResumeThread( thread.Get() ) == static_cast<DWORD>( -1 ) )
        {
            const std::string reason = win::LastErrorText( "ResumeThread" );
            TerminateJobObject( impl->job.Get(), 1 );
            return Fail( reason );
        }
        return ChildJob( std::move( impl ) );
    }

    Result<std::optional<int>> ChildJob::Wait( uint32_t timeoutMs ) noexcept
    {
        if ( m_impl->exited )
        {
            return std::optional<int>( m_impl->exitCode );
        }
        const DWORD waited = WaitForSingleObject( m_impl->process.Get(), timeoutMs );
        if ( waited == WAIT_TIMEOUT )
        {
            return std::optional<int>();
        }
        if ( waited != WAIT_OBJECT_0 )
        {
            return Fail( win::LastErrorText( "WaitForSingleObject(child)" ) );
        }
        DWORD code = 0;
        if ( !GetExitCodeProcess( m_impl->process.Get(), &code ) )
        {
            return Fail( win::LastErrorText( "GetExitCodeProcess" ) );
        }
        m_impl->exited   = true;
        m_impl->exitCode = static_cast<int>( code );
        return std::optional<int>( m_impl->exitCode );
    }

    Result<std::optional<size_t>> ChildJob::ReadStdout( std::span<std::byte> into, uint32_t timeoutMs ) noexcept
    {
        if ( !m_impl->stdoutRead.Valid() )
        {
            return Fail( "ChildJob::ReadStdout: stdout was not captured" );
        }
        if ( into.empty() )
        {
            return std::optional<size_t>( 0 );
        }
        HANDLE     pipe = m_impl->stdoutRead.Get();
        OVERLAPPED overlapped{};
        overlapped.hEvent = m_impl->readEvent.Get();
        ResetEvent( overlapped.hEvent );
        const DWORD want = static_cast<DWORD>( std::min<size_t>( into.size(), 1u << 20 ) );
        DWORD       got  = 0;
        if ( ReadFile( pipe, into.data(), want, &got, &overlapped ) )
        {
            return std::optional<size_t>( got );
        }
        DWORD code = GetLastError();
        if ( code == ERROR_BROKEN_PIPE )
        {
            return std::optional<size_t>( 0 );
        }
        if ( code != ERROR_IO_PENDING )
        {
            return Fail( win::ErrorText( "read child stdout", code ) );
        }

        // Pending: the pipe is empty. Wait for data or for the child's exit.
        const HANDLE waits[2]  = { overlapped.hEvent, m_impl->process.Get() };
        DWORD        waited    = WaitForMultipleObjects( 2, waits, FALSE, timeoutMs );
        bool         childGone = waited == WAIT_OBJECT_0 + 1;
        if ( childGone )
        {
            // What the child wrote before exiting is already in the pipe; a short grace covers the last write.
            waited = WaitForSingleObject( overlapped.hEvent, 50 );
        }
        if ( waited != WAIT_OBJECT_0 )
        {
            CancelIoEx( pipe, &overlapped );
        }
        if ( GetOverlappedResult( pipe, &overlapped, &got, TRUE ) )
        {
            return std::optional<size_t>( got );
        }
        code = GetLastError();
        if ( code == ERROR_BROKEN_PIPE )
        {
            return std::optional<size_t>( 0 );
        }
        if ( code != ERROR_OPERATION_ABORTED )
        {
            return Fail( win::ErrorText( "read child stdout", code ) );
        }
        if ( childGone )
        {
            // The child exited and its stream is drained; a process it left behind (mspdbsrv) may still hold the
            // write end, which would otherwise keep the stream open forever.
            return std::optional<size_t>( 0 );
        }
        return std::optional<size_t>();
    }

    JobUsage ChildJob::Usage() const noexcept
    {
        JobUsage usage;
        if ( !m_impl )
        {
            return usage;
        }
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        if ( QueryInformationJobObject( m_impl->job.Get(), JobObjectExtendedLimitInformation, &limits,
                                        sizeof( limits ), nullptr ) )
        {
            usage.peakBytes = limits.PeakJobMemoryUsed;
        }
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
        if ( QueryInformationJobObject( m_impl->job.Get(), JobObjectBasicAccountingInformation, &accounting,
                                        sizeof( accounting ), nullptr ) )
        {
            const uint64_t ticks = static_cast<uint64_t>( accounting.TotalUserTime.QuadPart ) +
                                   static_cast<uint64_t>( accounting.TotalKernelTime.QuadPart );
            usage.cpuMs = ticks / 10000; // 100 ns units
        }

        // The job's live commit: the private bytes of every process in it.
        struct
        {
            JOBOBJECT_BASIC_PROCESS_ID_LIST header;
            ULONG_PTR                       more[255];
        } ids{};
        if ( QueryInformationJobObject( m_impl->job.Get(), JobObjectBasicProcessIdList, &ids, sizeof( ids ),
                                        nullptr ) ||
             GetLastError() == ERROR_MORE_DATA )
        {
            const DWORD listed = std::min<DWORD>( ids.header.NumberOfProcessIdsInList, 256 );
            for ( DWORD i = 0; i < listed; ++i )
            {
                const DWORD pid = static_cast<DWORD>( ids.header.ProcessIdList[i] );
                win::Handle process( OpenProcess( PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid ) );
                if ( !process.Valid() )
                {
                    continue; // exited between the listing and the open
                }
                PROCESS_MEMORY_COUNTERS_EX counters{};
                if ( GetProcessMemoryInfo( process.Get(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>( &counters ),
                                           sizeof( counters ) ) )
                {
                    usage.currentBytes += counters.PrivateUsage;
                }
            }
        }
        m_impl->peakSeen = std::max( { m_impl->peakSeen, usage.peakBytes, usage.currentBytes } );
        usage.peakBytes  = m_impl->peakSeen;
        return usage;
    }

    void ChildJob::Terminate() noexcept
    {
        if ( !m_impl || m_impl->terminated )
        {
            return;
        }
        m_impl->terminated = true;
        TerminateJobObject( m_impl->job.Get(), 1 );
        if ( !m_impl->exited )
        {
            WaitForSingleObject( m_impl->process.Get(), INFINITE );
            DWORD code = 1;
            GetExitCodeProcess( m_impl->process.Get(), &code );
            m_impl->exited   = true;
            m_impl->exitCode = static_cast<int>( code );
        }
    }

    // ---- SpawnDetached
    // ---------------------------------------------------------------------------------------------
    Status SpawnDetached( const std::vector<std::string>& argv, const std::filesystem::path& logFile ) noexcept
    {
        if ( argv.empty() )
        {
            return Fail( "SpawnDetached: empty argv" );
        }
        std::error_code ec;
        std::filesystem::create_directories( logFile.parent_path(), ec );
        SECURITY_ATTRIBUTES inheritable{ sizeof( SECURITY_ATTRIBUTES ), nullptr, TRUE };
        win::Handle         log( CreateFileW( logFile.c_str(), FILE_APPEND_DATA,
                                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &inheritable,
                                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr ) );
        if ( !log.Valid() )
        {
            return Fail( win::LastErrorText( "open " + logFile.string() ) );
        }
        win::Handle nul( CreateFileW( L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable,
                                      OPEN_EXISTING, 0, nullptr ) );
        if ( !nul.Valid() )
        {
            return Fail( win::LastErrorText( "open NUL" ) );
        }

        Launch launch;
        launch.commandLine = win::QuoteCommandLine( argv );
        launch.input       = nul.Get();
        launch.output      = log.Get();
        launch.error       = log.Get();
        // Out of the caller's console, process group and job: the daemon outlives the build that started it.
        launch.flags = DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP | CREATE_BREAKAWAY_FROM_JOB;
        DWORD                       createError = 0;
        Result<PROCESS_INFORMATION> created     = CreateWithHandles( launch, createError );
        if ( !created && createError == ERROR_ACCESS_DENIED )
        {
            // The caller's job forbids breakaway. The daemon then lives inside that job; the log says so.
            const char note[] =
                 "buildpool: the starting process's job forbids breakaway; the daemon lives in it\r\n";
            DWORD written = 0;
            WriteFile( log.Get(), note, sizeof( note ) - 1, &written, nullptr );
            launch.flags &= ~static_cast<DWORD>( CREATE_BREAKAWAY_FROM_JOB );
            created = CreateWithHandles( launch, createError );
        }
        if ( !created )
        {
            return created.error();
        }
        CloseHandle( created.value().hThread );
        CloseHandle( created.value().hProcess );
        return Ok();
    }
} // namespace buildpool
