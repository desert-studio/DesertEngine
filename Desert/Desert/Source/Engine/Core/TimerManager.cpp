#include <Engine/Core/TimerManager.hpp>

#include <Common/Core/Logger.hpp>

#include <algorithm>
#include <utility>

namespace Desert::Core
{
    void TimerManager::After( float seconds, Reflection::Callable callback )
    {
        m_Timers.push_back( Timer{ std::max( seconds, 0.0f ), std::move( callback ) } );
    }

    void TimerManager::Tick( float dt )
    {
        std::vector<Reflection::Callable> due;
        std::erase_if( m_Timers,
                       [&]( Timer& timer )
                       {
                           if ( !timer.Callback.Alive() )
                               return true;
                           timer.Remaining -= dt;
                           if ( timer.Remaining > 0.0f )
                               return false;
                           due.push_back( std::move( timer.Callback ) );
                           return true;
                       } );
        for ( const Reflection::Callable& callback : due )
            if ( callback.Alive() )
                if ( Common::BoolResultStr called = callback.Call(); !called.IsSuccess() )
                    LOG_ERROR( "[Timer] callback error: {}", called.GetError() );
    }

    void TimerManager::Clear()
    {
        m_Timers.clear();
    }
} // namespace Desert::Core
