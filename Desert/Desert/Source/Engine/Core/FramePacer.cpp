#include "FramePacer.hpp"

#include <thread>

namespace Desert::Engine
{
    void FramePacer::SetLimit( uint32_t framesPerSecond )
    {
        m_Limit = framesPerSecond;
        m_Next  = {};
    }

    void FramePacer::WaitForNextFrame()
    {
        if ( m_Limit == 0 )
            return;
        const auto period = std::chrono::duration_cast<Clock::duration>( std::chrono::duration<double>( 1.0 / m_Limit ) );
        const auto now    = Clock::now();
        if ( m_Next == Clock::time_point{} || now > m_Next + period )
            m_Next = now; // the first frame, or one that ran late: the schedule starts over here
        else if ( now < m_Next )
            std::this_thread::sleep_until( m_Next );
        m_Next += period;
    }
} // namespace Desert::Engine
