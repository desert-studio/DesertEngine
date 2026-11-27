// BuildPool platform, macOS: the machine's memory (host_statistics64, the kernel pressure level) and what one
// process group uses (proc_pid_rusage phys_footprint, the number Activity Monitor shows as "Memory").
#include "BuildPool/Platform.hpp"

#include "../Posix/Posix.hpp"

#include <libproc.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <sys/proc_info.h>
#include <sys/sysctl.h>

#include <algorithm>
#include <cerrno>
#include <vector>

namespace buildpool
{
    namespace
    {
        // kern.memorystatus_vm_pressure_level: 1 normal, 2 warning, 4 critical.
        constexpr int kPressureWarning = 2;

        uint64_t TicksToMs( uint64_t ticks ) noexcept
        {
            static const mach_timebase_info_data_t timebase = []
            {
                mach_timebase_info_data_t info{};
                mach_timebase_info( &info );
                return info;
            }();
            // ri_user_time / ri_system_time are in mach ticks; numer/denom turn them into nanoseconds.
            return ticks / 1000000u * timebase.numer / timebase.denom +
                   ( ticks % 1000000u ) * timebase.numer / timebase.denom / 1000000u;
        }
    } // namespace

    Result<MemoryState> QueryMemory() noexcept
    {
        MemoryState state;

        uint64_t total  = 0;
        size_t   length = sizeof( total );
        if ( ::sysctlbyname( "hw.memsize", &total, &length, nullptr, 0 ) != 0 )
        {
            return Fail( posix::ErrnoText( "sysctl hw.memsize", errno ) );
        }
        state.physicalTotal = total;

        vm_size_t pageSize = 0;
        if ( host_page_size( mach_host_self(), &pageSize ) != KERN_SUCCESS )
        {
            return Fail( "host_page_size failed" );
        }
        vm_statistics64_data_t stats{};
        mach_msg_type_number_t count  = HOST_VM_INFO64_COUNT;
        const kern_return_t    result = host_statistics64( mach_host_self(), HOST_VM_INFO64,
                                                           reinterpret_cast<host_info64_t>( &stats ), &count );
        if ( result != KERN_SUCCESS )
        {
            return Fail( "host_statistics64 failed (kern_return " + std::to_string( result ) + ")" );
        }
        const uint64_t pages = static_cast<uint64_t>( stats.free_count ) + stats.inactive_count +
                               stats.speculative_count + stats.purgeable_count;
        state.physicalAvailable = std::min<uint64_t>( pages * pageSize, total );

        int    level       = 0;
        size_t levelLength = sizeof( level );
        if ( ::sysctlbyname( "kern.memorystatus_vm_pressure_level", &level, &levelLength, nullptr, 0 ) != 0 )
        {
            return Fail( posix::ErrnoText( "sysctl kern.memorystatus_vm_pressure_level", errno ) );
        }
        state.osPressure = level >= kPressureWarning;
        // commitFree stays empty: the macOS kernel has no commit limit.
        return state;
    }

    namespace posix
    {
        GroupSample SampleGroup( pid_t group ) noexcept
        {
            GroupSample        sample;
            std::vector<pid_t> pids( 64 );
            for ( ;; )
            {
                const int bytes =
                     proc_listpgrppids( group, pids.data(), static_cast<int>( pids.size() * sizeof( pid_t ) ) );
                if ( bytes <= 0 )
                {
                    return sample; // no member left (or the group is gone)
                }
                const size_t found = static_cast<size_t>( bytes ) / sizeof( pid_t );
                if ( found < pids.size() )
                {
                    pids.resize( found );
                    break;
                }
                pids.resize( pids.size() * 2 ); // the buffer was full: the list may be cut short
            }
            for ( const pid_t pid : pids )
            {
                rusage_info_v4 info{};
                if ( proc_pid_rusage( pid, RUSAGE_INFO_V4, reinterpret_cast<rusage_info_t*>( &info ) ) != 0 )
                {
                    continue; // exited between the listing and the probe
                }
                sample.currentBytes += info.ri_phys_footprint;
                sample.peakBytes = std::max<uint64_t>( sample.peakBytes, info.ri_lifetime_max_phys_footprint );
                sample.cpuMs += TicksToMs( info.ri_user_time + info.ri_system_time );
                ++sample.members;
            }
            sample.peakBytes = std::max( sample.peakBytes, sample.currentBytes );
            return sample;
        }

        uint64_t MaxRssBytes( const struct rusage& usage ) noexcept
        {
            return static_cast<uint64_t>( usage.ru_maxrss ); // bytes on macOS
        }
    } // namespace posix
} // namespace buildpool
