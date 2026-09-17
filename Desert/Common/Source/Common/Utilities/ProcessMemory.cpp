#include "ProcessMemory.hpp"

// THE PLATFORM IS SELECTED BY THE COMPILER'S OWN MACROS, NOT BY `DESERT_PLATFORM_*`, and `Core.hpp`
// records why in the comment above DESERT_DEBUG_BREAK: "every project file in the tree carries its own
// filter system:* defines { DESERT_PLATFORM_* } block, and any target that forgets one — every test
// suite does, and test suites compile engine sources". A footprint reader keyed on the engine's own
// platform define would therefore compile to "unknown" inside every suite that links it, and the suite
// asserting that it answers at all would be the thing that went quiet.
#if defined( _WIN32 )
// clang-format off
#include <windows.h>
#include <psapi.h>
// clang-format on
// LINKED FROM HERE RATHER THAN FROM THE BUILD SCRIPT. `GetProcessMemoryInfo` lives in psapi.lib, and
// declaring that in `Desert/Common/premake5.lua` would put a Windows-only link line in a shared build
// file for the benefit of one function — and every test suite that links Common would depend on somebody
// having remembered to propagate it. The pragma travels with the translation unit that needs it, which
// is the only place that can be wrong about it.
#if defined( _MSC_VER )
#pragma comment( lib, "psapi.lib" )
#endif
#elif defined( __APPLE__ )
#include <mach/mach.h>
#include <mach/task.h>
#elif defined( __linux__ )
#include <cstdio>
#endif

namespace Common::Utils
{
    std::string ProcessFootprint::Describe() const
    {
        if ( !Known )
        {
            return "process=unknown (the platform query failed)";
        }
        return "process resident=" + std::to_string( Resident ) + " peak=" + std::to_string( Peak );
    }

    ProcessFootprint ReadProcessFootprint()
    {
        ProcessFootprint out;

#if defined( _WIN32 )
        PROCESS_MEMORY_COUNTERS counters{};
        counters.cb = sizeof( counters );
        if ( ::GetProcessMemoryInfo( ::GetCurrentProcess(), &counters, sizeof( counters ) ) )
        {
            out.Known    = true;
            out.Resident = static_cast<uint64_t>( counters.WorkingSetSize );
            out.Peak     = static_cast<uint64_t>( counters.PeakWorkingSetSize );
        }
#elif defined( __APPLE__ )
        // MACH_TASK_BASIC_INFO and not TASK_BASIC_INFO: the older flavour's `resident_size` is a 32-bit
        // `vm_size_t` on some ABIs and its `resident_size_max` does not exist at all, so the peak — the
        // one field a growth claim can rest on — would have had to be tracked by hand and would reset
        // with every process that forgot to.
        mach_task_basic_info_data_t info{};
        mach_msg_type_number_t      count = MACH_TASK_BASIC_INFO_COUNT;
        if ( ::task_info( ::mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>( &info ),
                          &count ) == KERN_SUCCESS )
        {
            out.Known    = true;
            out.Resident = static_cast<uint64_t>( info.resident_size );
            out.Peak     = static_cast<uint64_t>( info.resident_size_max );
        }
#elif defined( __linux__ )
        // VmHWM before VmRSS in /proc/self/status, and both in kB. Parsed rather than taken from
        // /proc/self/statm because statm carries no high-water mark.
        if ( std::FILE* status = std::fopen( "/proc/self/status", "r" ) )
        {
            char        line[256];
            long long   rssKb = -1;
            long long   hwmKb = -1;
            while ( std::fgets( line, sizeof( line ), status ) != nullptr )
            {
                long long value = 0;
                if ( std::sscanf( line, "VmRSS: %lld kB", &value ) == 1 )
                    rssKb = value;
                else if ( std::sscanf( line, "VmHWM: %lld kB", &value ) == 1 )
                    hwmKb = value;
            }
            std::fclose( status );
            if ( rssKb >= 0 && hwmKb >= 0 )
            {
                out.Known    = true;
                out.Resident = static_cast<uint64_t>( rssKb ) * 1024u;
                out.Peak     = static_cast<uint64_t>( hwmKb ) * 1024u;
            }
        }
#endif

        return out;
    }

} // namespace Common::Utils
