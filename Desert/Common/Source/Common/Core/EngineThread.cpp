#include <Common/Core/EngineThread.hpp>

#include <algorithm>
#include <format>
#include <iterator>
#include <mutex>
#include <vector>

#if defined( DESERT_PLATFORM_WINDOWS )
#include <Windows.h>
// TlHelp32 must follow Windows.h; it is what supplies the process's thread snapshot for the census.
#include <TlHelp32.h>
#else
#include <pthread.h>
#if defined( __APPLE__ )
#include <pthread/qos.h>
#endif
#endif

namespace Common
{
    namespace
    {
        struct NamedThread
        {
            std::thread::id Id;
            std::string     Name;
            ThreadPriority  Priority        = ThreadPriority::Normal;
            bool            PriorityApplied = false;
        };

        // NEVER DESTROYED, and on purpose: the job pool's workers leave the census while the pool's own static
        // destructor joins them, and a census constructed after the pool (by the first worker) would be destroyed
        // before it. A heap object nobody deletes outlives every thread of the process.
        struct ThreadCensus
        {
            std::mutex               Mutex;
            std::vector<NamedThread> Threads;
        };
        ThreadCensus& Census() noexcept
        {
            static ThreadCensus* const s_Census = new ThreadCensus();
            return *s_Census;
        }

        // The thread leaves the census when it ends: a thread_local's destructor runs on the thread's exit.
        struct CensusLeave
        {
            bool Listed = false;
            ~CensusLeave()
            {
                if ( !Listed )
                    return;
                ThreadCensus&                     census = Census();
                const std::thread::id             self   = std::this_thread::get_id();
                const std::lock_guard<std::mutex> lock( census.Mutex );
                std::erase_if( census.Threads, [self]( const NamedThread& thread ) { return thread.Id == self; } );
            }
        };

        const char* PriorityName( const ThreadPriority priority ) noexcept
        {
            switch ( priority )
            {
                case ThreadPriority::AboveNormal:
                    return "AboveNormal";
                case ThreadPriority::Normal:
                    break;
            }
            return "Normal";
        }

#if defined( DESERT_PLATFORM_WINDOWS )
        // Resolved from kernel32 as UE's FWindowsPlatformProcess::SetThreadName does: the declaration depends on
        // the SDK's target version, the export on the running Windows (10 1607 and later).
        using SetThreadDescriptionFn = HRESULT( WINAPI* )( HANDLE, PCWSTR );

        void SetOsThreadName( const std::string_view name ) noexcept
        {
            static const SetThreadDescriptionFn s_SetDescription =
                 reinterpret_cast<SetThreadDescriptionFn>( reinterpret_cast<void*>(
                      ::GetProcAddress( ::GetModuleHandleW( L"kernel32.dll" ), "SetThreadDescription" ) ) );
            if ( s_SetDescription == nullptr )
                return; // a Windows older than 10 1607: debuggers show the id, the census still shows the name
            wchar_t   wide[128] = {};
            const int length    = ::MultiByteToWideChar( CP_UTF8, 0, name.data(), static_cast<int>( name.size() ),
                                                         wide, static_cast<int>( std::size( wide ) ) - 1 );
            if ( length <= 0 )
                return;
            wide[length] = L'\0';
            (void)s_SetDescription( ::GetCurrentThread(), wide );
        }

        bool SetOsThreadPriority( const ThreadPriority priority ) noexcept
        {
            int value = THREAD_PRIORITY_NORMAL;
            if ( priority == ThreadPriority::AboveNormal )
                value = THREAD_PRIORITY_ABOVE_NORMAL;
            return ::SetThreadPriority( ::GetCurrentThread(), value ) != 0;
        }

        // Threads of this process now, whoever started them; 0 when the snapshot is refused.
        size_t CountProcessThreads() noexcept
        {
            const HANDLE snapshot = ::CreateToolhelp32Snapshot( TH32CS_SNAPTHREAD, 0 );
            if ( snapshot == INVALID_HANDLE_VALUE )
                return 0;
            const DWORD   self  = ::GetCurrentProcessId();
            size_t        count = 0;
            THREADENTRY32 entry{};
            entry.dwSize = sizeof( entry );
            for ( BOOL more = ::Thread32First( snapshot, &entry ); more != FALSE;
                  more      = ::Thread32Next( snapshot, &entry ) )
            {
                if ( entry.th32OwnerProcessID == self )
                    ++count;
            }
            ::CloseHandle( snapshot );
            return count;
        }
#elif defined( __APPLE__ )
        void SetOsThreadName( const std::string_view name ) noexcept
        {
            char         text[64] = {}; // MAXTHREADNAMESIZE
            const size_t length   = std::min( name.size(), std::size( text ) - 1 );
            std::copy_n( name.data(), length, text );
            (void)pthread_setname_np( text );
        }

        bool SetOsThreadPriority( const ThreadPriority priority ) noexcept
        {
            if ( priority == ThreadPriority::Normal )
                return true; // the QoS the thread was created with
            return pthread_set_qos_class_self_np( QOS_CLASS_USER_INTERACTIVE, 0 ) == 0;
        }
#else
        void SetOsThreadName( const std::string_view name ) noexcept
        {
            char         text[16] = {}; // the kernel's TASK_COMM_LEN
            const size_t length   = std::min( name.size(), std::size( text ) - 1 );
            std::copy_n( name.data(), length, text );
            (void)pthread_setname_np( pthread_self(), text );
        }

        bool SetOsThreadPriority( const ThreadPriority priority ) noexcept
        {
            // Raising a thread above the default needs CAP_SYS_NICE; the census reports the role as not applied.
            return priority == ThreadPriority::Normal;
        }
#endif
    } // namespace

    void NameCurrentThread( const std::string_view name, const ThreadPriority priority ) noexcept
    {
        SetOsThreadName( name );
        const bool applied = SetOsThreadPriority( priority );

        static thread_local CensusLeave t_Leave;
        t_Leave.Listed = true;

        ThreadCensus&                     census = Census();
        const std::thread::id             self   = std::this_thread::get_id();
        const std::lock_guard<std::mutex> lock( census.Mutex );
        const auto isSelf = [self]( const NamedThread& thread ) { return thread.Id == self; };
        const auto listed = std::find_if( census.Threads.begin(), census.Threads.end(), isSelf );
        if ( listed != census.Threads.end() )
        {
            listed->Name            = std::string( name ); // renamed: one row per thread
            listed->Priority        = priority;
            listed->PriorityApplied = applied;
            return;
        }
        census.Threads.push_back( NamedThread{ self, std::string( name ), priority, applied } );
    }

    std::string DescribeEngineThreads()
    {
        struct Row
        {
            std::string    Role;
            ThreadPriority Priority = ThreadPriority::Normal;
            bool           Applied  = false;
            size_t         Count    = 0;
        };
        std::vector<Row> rows;
        size_t           engine = 0;
        {
            ThreadCensus&                     census = Census();
            const std::lock_guard<std::mutex> lock( census.Mutex );
            for ( const NamedThread& thread : census.Threads )
            {
                // "Job Worker #3" counts as the role "Job Worker".
                const std::string role = thread.Name.substr( 0, thread.Name.find( " #" ) );
                auto              row  = std::find_if( rows.begin(), rows.end(),
                                                       [&]( const Row& r ) {
                                             return r.Role == role && r.Priority == thread.Priority &&
                                                    r.Applied == thread.PriorityApplied;
                                         } );
                if ( row == rows.end() )
                    row = rows.insert( rows.end(), Row{ role, thread.Priority, thread.PriorityApplied, 0 } );
                ++row->Count;
                ++engine;
            }
        }

        std::string text = std::format( "{} engine threads:", engine );
        for ( const Row& row : rows )
        {
            const char* applied = row.Applied ? "" : " (not applied)";
            std::format_to( std::back_inserter( text ), " {} {}{} x{};", row.Role, PriorityName( row.Priority ),
                            applied, row.Count );
        }
#if defined( DESERT_PLATFORM_WINDOWS )
        const size_t process = CountProcessThreads();
        if ( process == 0 )
            text += " the process's thread snapshot was refused";
        else
        {
            const size_t foreign = process > engine ? process - engine : 0;
            std::format_to( std::back_inserter( text ), " {} of the process's {} threads are not the engine's",
                            foreign, process );
        }
#endif
        return text;
    }

    BoolResultStr OptOutOfPowerThrottling() noexcept
    {
#if defined( DESERT_PLATFORM_WINDOWS )
        PROCESS_POWER_THROTTLING_STATE state{};
        state.Version     = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
        state.StateMask   = 0; // controlled and not set: EXECUTION_SPEED throttling is OFF, focus or not
        if ( ::SetProcessInformation( ::GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof( state ) ) ==
             0 )
        {
            const unsigned long error = ::GetLastError();
            return MakeFormattedError<bool>( "SetProcessInformation(ProcessPowerThrottling) failed: error {}",
                                             error );
        }
#endif
        return MakeSuccess( true );
    }
} // namespace Common
