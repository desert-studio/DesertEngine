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
#if defined( DESERT_PLATFORM_WINDOWS )
        if ( m_Timer == nullptr && !m_TimerRefused )
        {
            m_Timer = ::CreateWaitableTimerExW( nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                                TIMER_ALL_ACCESS );
            if ( m_Timer == nullptr )
            {
                m_TimerRefused            = true;
                const unsigned long error = ::GetLastError();
                LOG_WARN( "[FramePacer] no high-resolution timer (error {}): the cap sleeps on the 15.6 ms tick",
                          error );
            }
        }
        const auto now = Clock::now();
        if ( m_Timer != nullptr && coarse > now )
        {
            const auto    wait = std::chrono::duration_cast<std::chrono::nanoseconds>( coarse - now ).count();
            LARGE_INTEGER due{};
            due.QuadPart = -static_cast<LONGLONG>( wait / 100 ); // relative, in 100 ns units
            const HANDLE timer = static_cast<HANDLE>( m_Timer );
            if ( ::SetWaitableTimerEx( timer, &due, 0, nullptr, nullptr, nullptr, 0 ) != 0 )
                ::WaitForSingleObject( timer, INFINITE );
        }
        else if ( m_Timer == nullptr )
            std::this_thread::sleep_until( coarse );
#else
        std::this_thread::sleep_until( coarse );
#endif
        // The tail: at most 0.5 ms of one core, waiting on the clock alone (no thread is waited for).
        while ( Clock::now() < until )
            std::this_thread::yield();
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
