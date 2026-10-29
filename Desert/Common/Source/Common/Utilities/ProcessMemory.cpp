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
// declaring that in `Desert/Common/CMakeLists.txt` would put a Windows-only link line in a shared build
// file for the benefit of one function — and every test suite that links Common would depend on somebody
// having remembered to propagate it. The pragma travels with the translation unit that needs it, which
// is the only place that can be wrong about it.
#if defined( _MSC_VER )
#pragma comment( lib, "psapi.lib" )
#endif
#elif defined( __APPLE__ )
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/task.h>
#include <mach/vm_statistics.h>
#include <malloc/malloc.h>
#elif defined( __linux__ )
#include <cstdio>
#endif

#include <algorithm>
#include <format>
#include <map>

namespace Common::Utils
{
    std::string ProcessFootprint::Describe() const
    {
        if ( !Known )
        {
            return "process=unknown (the platform query failed)";
        }
        return std::format( "process resident={} peak={}", Resident, Peak );
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
        PROCESS_MEMORY_COUNTERS_EX extended{};
        extended.cb = sizeof( extended );
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) - the _EX struct is the API's own extension
        if ( ::GetProcessMemoryInfo( ::GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>( &extended ),
                                     sizeof( extended ) ) )
            out.Physical = static_cast<uint64_t>( extended.PrivateUsage );
#elif defined( __APPLE__ )
        // MACH_TASK_BASIC_INFO and not TASK_BASIC_INFO: the older flavour's `resident_size` is a 32-bit
        // `vm_size_t` on some ABIs and its `resident_size_max` does not exist at all, so the peak — the
        // one field a growth claim can rest on — would have had to be tracked by hand and would reset
        // with every process that forgot to.
        mach_task_basic_info_data_t info{};
        mach_msg_type_number_t      count = MACH_TASK_BASIC_INFO_COUNT;
        // `task_info` takes a `task_info_t` (an `integer_t*`) and the flavour argument says which struct
        // is really there. That is the kernel interface's own type erasure: there is no cast-free
        // spelling, and `static_cast` between unrelated pointer types does not compile.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        if ( ::task_info( ::mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>( &info ),
                          &count ) == KERN_SUCCESS )
        {
            out.Known    = true;
            out.Resident = static_cast<uint64_t>( info.resident_size );
            out.Peak     = static_cast<uint64_t>( info.resident_size_max );
        }
        // phys_footprint lives in TASK_VM_INFO only; it is what the kernel's memory pressure (and jetsam)
        // charges, Metal's GPU memory included.
        task_vm_info_data_t    vm{};
        mach_msg_type_number_t vmCount = TASK_VM_INFO_COUNT;
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) - see task_info above
        if ( ::task_info( ::mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>( &vm ), &vmCount ) ==
             KERN_SUCCESS )
            out.Physical = static_cast<uint64_t>( vm.phys_footprint );
#elif defined( __linux__ )
        // VmHWM before VmRSS in /proc/self/status, and both in kB. Parsed rather than taken from
        // /proc/self/statm because statm carries no high-water mark.
        if ( std::FILE* status = std::fopen( "/proc/self/status", "r" ) )
        {
            char      line[256];
            long long rssKb = -1;
            long long hwmKb = -1;
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
                out.Physical = out.Resident;
            }
        }
#endif

        return out;
    }

    std::string FootprintBreakdown::Describe( const uint64_t footprint ) const
    {
        if ( !Known )
            return "footprint by region kind: unknown (no region walk on this platform)";
        constexpr double MB  = 1024.0 * 1024.0;
        std::string      out = std::format( "footprint {:.1f} MB by region kind (dirty + compressed):",
                                            static_cast<double>( footprint ) / MB );
        for ( const FootprintKind& kind : Kinds )
            out += std::format( "\n  {:<34} x{:<6} {:9.1f} MB", kind.Name, kind.Regions,
                                static_cast<double>( kind.Bytes ) / MB );
        out += std::format( "\n  {:<34} {:>7} {:9.1f} MB", "mapped nowhere (GPU private storage)", "",
                            footprint > Walked ? static_cast<double>( footprint - Walked ) / MB : 0.0 );
        out += std::format( "\n  malloc zones: {:.1f} MB in use of {:.1f} MB reserved",
                            static_cast<double>( HeapInUse ) / MB, static_cast<double>( HeapAllocated ) / MB );
        return out;
    }

#if defined( __APPLE__ )
    namespace
    {
        // The kernel's VM user tags (mach/vm_statistics.h), grouped the way vmmap's summary groups them.
        [[nodiscard]] std::string KindName( const unsigned tag, const unsigned char shareMode )
        {
            switch ( tag )
            {
                case VM_MEMORY_MALLOC:
                case VM_MEMORY_MALLOC_HUGE:
                case VM_MEMORY_REALLOC:
                    return "malloc (other)";
                case VM_MEMORY_MALLOC_SMALL:
                    return "malloc small";
                case VM_MEMORY_MALLOC_LARGE:
                case VM_MEMORY_MALLOC_LARGE_REUSABLE:
                case VM_MEMORY_MALLOC_LARGE_REUSED:
                    return "malloc large";
                case VM_MEMORY_MALLOC_TINY:
                    return "malloc tiny";
                case VM_MEMORY_MALLOC_NANO:
                    return "malloc nano";
                case VM_MEMORY_MALLOC_MEDIUM:
                    return "malloc medium";
                case VM_MEMORY_IOACCELERATOR:
                    return "IOAccelerator (GPU driver)";
                case VM_MEMORY_IOSURFACE:
                    return "IOSurface";
                case VM_MEMORY_STACK:
                    return "stacks";
                case VM_MEMORY_DYLD:
                case VM_MEMORY_DYLD_MALLOC:
                    return "dyld";
                case VM_MEMORY_OS_ALLOC_ONCE:
                    return "os_alloc_once";
                case VM_MEMORY_COREGRAPHICS:
                case VM_MEMORY_COREGRAPHICS_DATA:
                case VM_MEMORY_COREGRAPHICS_SHARED:
                case VM_MEMORY_COREGRAPHICS_FRAMEBUFFERS:
                case VM_MEMORY_COREGRAPHICS_BACKINGSTORES:
                    return "CoreGraphics";
                case VM_MEMORY_FOUNDATION:
                case VM_MEMORY_CORESERVICES:
                    return "Foundation/CoreServices";
                case 0:
                    return shareMode == SM_PRIVATE || shareMode == SM_COW ? "untagged (images' data, mmap)"
                                                                          : "untagged shared";
                default:
                    return std::format( "VM tag {}", tag );
            }
        }
    } // namespace
#endif

    FootprintBreakdown ReadFootprintBreakdown()
    {
        FootprintBreakdown out;
#if defined( __APPLE__ )
        std::map<std::string, FootprintKind> kinds;
        const uint64_t                       page    = static_cast<uint64_t>( vm_page_size );
        mach_vm_address_t                    address = 0;
        for ( ;; )
        {
            mach_vm_size_t                 size = 0;
            vm_region_extended_info_data_t info{};
            mach_msg_type_number_t         count  = VM_REGION_EXTENDED_INFO_COUNT;
            mach_port_t                    object = MACH_PORT_NULL;
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) - see task_info above
            if ( ::mach_vm_region( ::mach_task_self(), &address, &size, VM_REGION_EXTENDED_INFO,
                                   reinterpret_cast<vm_region_info_t>( &info ), &count, &object ) != KERN_SUCCESS )
                break;
            const uint64_t bytes = ( static_cast<uint64_t>( info.pages_dirtied ) +
                                     static_cast<uint64_t>( info.pages_swapped_out ) ) *
                                   page;
            if ( bytes > 0 && info.share_mode != SM_EMPTY )
            {
                const std::string name = KindName( info.user_tag, info.share_mode );
                FootprintKind&    kind = kinds[name];
                kind.Name              = name;
                kind.Bytes += bytes;
                ++kind.Regions;
                out.Walked += bytes;
            }
            address += size;
        }
        for ( auto& [name, kind] : kinds )
            out.Kinds.push_back( std::move( kind ) );
        std::ranges::sort( out.Kinds,
                           []( const FootprintKind& a, const FootprintKind& b ) { return a.Bytes > b.Bytes; } );

        vm_address_t* zones     = nullptr;
        unsigned      zoneCount = 0;
        if ( ::malloc_get_all_zones( ::mach_task_self(), nullptr, &zones, &zoneCount ) == KERN_SUCCESS )
            for ( unsigned i = 0; i < zoneCount; ++i )
            {
                malloc_statistics_t stats{};
                // NOLINTNEXTLINE(performance-no-int-to-ptr) - the zone list is addresses in this very task
                ::malloc_zone_statistics( reinterpret_cast<malloc_zone_t*>( zones[i] ), &stats );
                out.HeapInUse += stats.size_in_use;
                out.HeapAllocated += stats.size_allocated;
            }
        out.Known = true;
#endif
        return out;
    }

} // namespace Common::Utils
