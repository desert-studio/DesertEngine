#include <Common/Platform/ThreadCpuClock.hpp>

#if defined( _WIN32 )
#include <Windows.h>
#else
#include <ctime>
#endif

namespace Common::Platform
{
    ThreadCpuClock::time_point ThreadCpuClock::now() noexcept
    {
#if defined( _WIN32 )
        FILETIME creation{};
        FILETIME exit{};
        FILETIME kernel{};
        FILETIME user{};
        if ( ::GetThreadTimes( ::GetCurrentThread(), &creation, &exit, &kernel, &user ) == 0 )
            return time_point{}; // the pseudo-handle of the calling thread always has THREAD_QUERY_INFORMATION
        const auto ticks = []( const FILETIME& time )
        { return ( static_cast<unsigned long long>( time.dwHighDateTime ) << 32U ) | time.dwLowDateTime; };
        return time_point{ duration{ static_cast<rep>( ( ticks( kernel ) + ticks( user ) ) * 100ULL ) } };
#else
        timespec spec{};
        ::clock_gettime( CLOCK_THREAD_CPUTIME_ID, &spec );
        return time_point{ std::chrono::seconds{ spec.tv_sec } + std::chrono::nanoseconds{ spec.tv_nsec } };
#endif
    }
} // namespace Common::Platform
