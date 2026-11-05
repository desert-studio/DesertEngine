#include "FramePacer.hpp"

#include <Common/Core/Logger.hpp>

#include <cstdint>
#include <thread>

#if defined( DESERT_PLATFORM_WINDOWS )
#include <Windows.h>
#endif

namespace Desert::Engine
{
    FramePacer::~FramePacer()
    {
#if defined( DESERT_PLATFORM_WINDOWS )
        if ( m_Timer != nullptr )
            ::CloseHandle( static_cast<HANDLE>( m_Timer ) );
#endif
    }

    void FramePacer::SleepUntil( const Clock::time_point until )
    {
        constexpr auto kSpinTail = std::chrono::microseconds( 500 );
        const auto     coarse    = until - kSpinTail;
        const auto     now       = Clock::now();
        if ( coarse > now && !SleepOnTimer( coarse - now ) )
            std::this_thread::sleep_until( coarse ); // the plain wait: Windows' 15.6 ms tick, precise elsewhere
        // The tail: at most 0.5 ms of one core, waiting on the clock alone (no thread is waited for).
        while ( Clock::now() < until )
            std::this_thread::yield();
    }

    bool FramePacer::SleepOnTimer( const Clock::duration span ) noexcept
    {
#if defined( DESERT_PLATFORM_WINDOWS )
        if ( m_TimerRefused )
            return false;
        const char* refused = nullptr; // the call that failed, if one did
        if ( m_Timer == nullptr )
        {
            m_Timer = ::CreateWaitableTimerExW( nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                                TIMER_ALL_ACCESS );
            if ( m_Timer == nullptr )
                refused = "CreateWaitableTimerExW"; // older than Windows 10 1803: no high-resolution timer
        }
        if ( refused == nullptr )
        {
            const auto    ticks = std::chrono::duration_cast<std::chrono::nanoseconds>( span ).count() / 100;
            LARGE_INTEGER due{};
            due.QuadPart       = -static_cast<LONGLONG>( ticks ); // relative, in 100 ns units
            const HANDLE timer = static_cast<HANDLE>( m_Timer );
            if ( ::SetWaitableTimerEx( timer, &due, 0, nullptr, nullptr, nullptr, 0 ) == 0 )
                refused = "SetWaitableTimerEx";
            else if ( ::WaitForSingleObject( timer, INFINITE ) != WAIT_OBJECT_0 )
                refused = "WaitForSingleObject";
        }
        if ( refused == nullptr )
            return true;
        // REFUSED ONCE IS REFUSED FOR GOOD: said here, once, and every later wait is the plain one. A refusal
        // retried each frame would cost a failed call per frame and a log line per frame.
        const unsigned long error = ::GetLastError();
        if ( m_Timer != nullptr )
            ::CloseHandle( static_cast<HANDLE>( m_Timer ) );
        m_Timer        = nullptr;
        m_TimerRefused = true;
        LOG_WARN(
             "[FramePacer] the high-resolution timer was refused ({} failed, error {}): the cap sleeps on the "
             "15.6 ms tick from here on",
             refused, error );
        return false;
#else
        (void)span;
        return false; // nanosleep is already precise to well under the 0.5 ms tail
#endif
    }

    void FramePacer::SetLimit( uint32_t framesPerSecond )
    {
        m_Limit = framesPerSecond;
        m_Next  = {};
    }

    void FramePacer::WaitForNextFrame()
    {
        if ( m_Limit == 0 )
            return;
        const auto period =
             std::chrono::duration_cast<Clock::duration>( std::chrono::duration<double>( 1.0 / m_Limit ) );
        const auto now = Clock::now();
        if ( m_Next == Clock::time_point{} || now > m_Next + period )
            m_Next = now; // the first frame, or one that ran late: the schedule starts over here
        else if ( now < m_Next )
            SleepUntil( m_Next );
        m_Next += period;
    }
} // namespace Desert::Engine
