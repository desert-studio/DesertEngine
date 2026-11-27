#include "Editor/Core/StartupRecord.hpp"

#include "Editor/Core/ProjectContext.hpp"

#if defined( DESERT_PLATFORM_WINDOWS )
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#elif defined( DESERT_PLATFORM_MACOS )
#include <sys/sysctl.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace Desert::Editor
{
    namespace
    {
        // The process's age in seconds on the wall clock, from the kernel's record of its creation.
        Common::ResultStr<double> ProcessAgeSeconds()
        {
#if defined( DESERT_PLATFORM_WINDOWS )
            FILETIME created, exited, kernel, user, now;
            if ( !GetProcessTimes( GetCurrentProcess(), &created, &exited, &kernel, &user ) )
                return Common::MakeFormattedError<double>( "GetProcessTimes failed (error {})", GetLastError() );
            GetSystemTimeAsFileTime( &now );
            const auto ticks = []( const FILETIME& t )
            { return ( static_cast<unsigned long long>( t.dwHighDateTime ) << 32 ) | t.dwLowDateTime; };
            // FILETIME counts 100 ns.
            return Common::MakeSuccess( static_cast<double>( ticks( now ) - ticks( created ) ) / 1.0e7 );
#elif defined( DESERT_PLATFORM_MACOS )
            kinfo_proc info{};
            size_t     size  = sizeof( info );
            int        mib[] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid() };
            struct timeval now
            {
            };
            gettimeofday( &now, nullptr );
            if ( sysctl( mib, 4, &info, &size, nullptr, 0 ) != 0 )
                return Common::MakeError<double>( "sysctl(KERN_PROC_PID) did not answer" );
            const struct timeval start = info.kp_proc.p_starttime;
            return Common::MakeSuccess( static_cast<double>( now.tv_sec - start.tv_sec ) +
                                        static_cast<double>( now.tv_usec - start.tv_usec ) / 1.0e6 );
#else
            return Common::MakeError<double>( "this platform has no process start query in the editor" );
#endif
        }
    } // namespace

    Common::ResultStr<std::chrono::steady_clock::time_point> ProcessStartTime()
    {
        const auto steadyNow = std::chrono::steady_clock::now();
        auto       age       = ProcessAgeSeconds();
        if ( !age )
            return Common::MakeError<std::chrono::steady_clock::time_point>( age.GetError() );
        return Common::MakeSuccess( steadyNow - std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                                     std::chrono::duration<double>( age.ExtractValue() ) ) );
    }

    std::filesystem::path StartupHistoryFile()
    {
        return std::filesystem::path( ProjectContext::ConfigDirectory() ) / "startup_timings.json";
    }
} // namespace Desert::Editor
