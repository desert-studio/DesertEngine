#include <Engine/Core/TimerManager.hpp>

#include <Common/Core/Logger.hpp>

#include <algorithm>
#include <limits>
#include <utility>

namespace Desert::Core
{
    TimerHandle TimerManager::SetTimer( Reflection::Callable callback, float rate, const TimerParameters& parameters )
    {
        if ( rate <= 0.0f || !callback.Alive() )
            return TimerHandle();
        TimerData data;
        data.Callback        = std::move( callback );
        data.Rate            = rate;
        data.Loop            = parameters.Loop;
        data.MaxOncePerFrame = parameters.MaxOncePerFrame;
        data.ExpireTime      = parameters.FirstDelay >= 0.0f ? parameters.FirstDelay : rate; // remaining, pending
        data.State           = Status::Pending;
        const TimerHandle handle = Add( std::move( data ) );
        m_PendingSet.push_back( handle );
        return handle;
    }

    TimerHandle TimerManager::SetTimerForNextTick( Reflection::Callable callback )
    {
        if ( !callback.Alive() )
            return TimerHandle();
        TimerData data;
        data.Callback   = std::move( callback );
        data.ExpireTime = m_InternalTime; // due the moment game time moves on
        data.State      = Status::Active;
        const TimerHandle handle = Add( std::move( data ) );
        PushActive( handle );
        return handle;
    }

    void TimerManager::ClearTimer( TimerHandle handle )
    {
        TimerData* data = Find( handle );
        if ( data == nullptr )
            return;
        switch ( data->State )
        {
            case Status::Pending:
                RemovePending( handle );
                Remove( handle );
                break;
            case Status::Active:
                data->State    = Status::ActivePendingRemoval;
                data->Callback = Reflection::Callable(); // the function is released now, the slot at the top
                break;
            case Status::Paused:
                Remove( handle );
                break;
            case Status::Executing:
                // Cleared by its own call: no further catch-up call, no re-arm.
                m_Executing = TimerHandle();
                Remove( handle );
                break;
            case Status::ActivePendingRemoval:
                break;
        }
    }

    void TimerManager::PauseTimer( TimerHandle handle )
    {
        TimerData* data = Find( handle );
        if ( data == nullptr || data->State == Status::Paused )
            return;
        const Status previous = data->State;
        switch ( previous )
        {
            case Status::Active:
                RemoveActive( handle );
                break;
            case Status::Pending:
                RemovePending( handle );
                break;
            case Status::Executing:
                m_Executing = TimerHandle();
                break;
            case Status::Paused:
            case Status::ActivePendingRemoval:
                break;
        }
        if ( previous == Status::Executing && !data->Loop )
        {
            Remove( handle );
            return;
        }
        data->State = Status::Paused;
        if ( previous != Status::Pending ) // a pending timer already holds the time remaining
            data->ExpireTime -= m_InternalTime;
    }

    void TimerManager::UnPauseTimer( TimerHandle handle )
    {
        TimerData* data = Find( handle );
        if ( data == nullptr || data->State != Status::Paused )
            return;
        // The time remaining stays in ExpireTime; it becomes a due time when the pending set joins the heap.
        data->State = Status::Pending;
        m_PendingSet.push_back( handle );
    }

    bool TimerManager::IsTimerActive( TimerHandle handle ) const
    {
        const TimerData* data = Find( handle );
        return data != nullptr && data->State != Status::Paused;
    }

    bool TimerManager::IsTimerPaused( TimerHandle handle ) const
    {
        const TimerData* data = Find( handle );
        return data != nullptr && data->State == Status::Paused;
    }

    bool TimerManager::IsTimerPending( TimerHandle handle ) const
    {
        const TimerData* data = Find( handle );
        return data != nullptr && data->State == Status::Pending;
    }

    bool TimerManager::TimerExists( TimerHandle handle ) const
    {
        return Find( handle ) != nullptr;
    }

    float TimerManager::GetTimerRate( TimerHandle handle ) const
    {
        const TimerData* data = Find( handle );
        return data != nullptr ? data->Rate : -1.0f;
    }

    float TimerManager::GetTimerElapsed( TimerHandle handle ) const
    {
        const TimerData* data = Find( handle );
        if ( data == nullptr )
            return -1.0f;
        if ( data->State == Status::Active || data->State == Status::Executing )
            return static_cast<float>( static_cast<double>( data->Rate ) - ( data->ExpireTime - m_InternalTime ) );
        return static_cast<float>( static_cast<double>( data->Rate ) - data->ExpireTime );
    }

    float TimerManager::GetTimerRemaining( TimerHandle handle ) const
    {
        const TimerData* data = Find( handle );
        if ( data == nullptr )
            return -1.0f;
        switch ( data->State )
        {
            case Status::Active:
                return static_cast<float>( data->ExpireTime - m_InternalTime );
            case Status::Executing:
                return 0.0f;
            case Status::Pending:
            case Status::Paused:
            case Status::ActivePendingRemoval:
                break;
        }
        return static_cast<float>( data->ExpireTime );
    }

    void TimerManager::Tick( float dt )
    {
        m_InternalTime += static_cast<double>( dt );
        const auto later = [this]( TimerHandle a, TimerHandle b ) { return FiresLater( a, b ); };
        while ( !m_ActiveHeap.empty() )
        {
            const TimerHandle top  = m_ActiveHeap.front();
            TimerData&        head = m_Timers[top.Index];
            const bool        gone = head.State == Status::ActivePendingRemoval || !head.Callback.Alive();
            if ( !gone && m_InternalTime <= head.ExpireTime )
                break; // the earliest is not due: neither is any other
            std::pop_heap( m_ActiveHeap.begin(), m_ActiveHeap.end(), later );
            m_ActiveHeap.pop_back();
            if ( gone )
            {
                Remove( top );
                continue;
            }

            head.State  = Status::Executing;
            m_Executing = top;
            // A looping timer is called once per period it fell behind by (a long frame, a short rate).
            const int calls =
                 head.Loop && !head.MaxOncePerFrame
                      ? static_cast<int>( ( m_InternalTime - head.ExpireTime ) / static_cast<double>( head.Rate ) ) + 1
                      : 1;
            // A copy: a call may set timers (m_Timers grows, `head` moves) or clear this one (its slot is freed).
            const Reflection::Callable callback = head.Callback;
            for ( int call = 0; call < calls && Same( m_Executing, top ) && callback.Alive(); ++call )
                if ( Common::BoolResultStr called = callback.Call(); !called.IsSuccess() )
                    LOG_ERROR( "[Timer] callback error: {}", called.GetError() );
            if ( !Same( m_Executing, top ) )
                continue; // cleared or paused by its own call
            m_Executing     = TimerHandle();
            TimerData& done = m_Timers[top.Index];
            if ( done.Loop && done.Callback.Alive() )
            {
                done.ExpireTime += static_cast<double>( calls ) * static_cast<double>( done.Rate );
                done.State = Status::Active;
                PushActive( top );
            }
            else
                Remove( top );
        }

        // Timers set or unpaused since the last Tick (and by this Tick's calls) count from now.
        std::vector<TimerHandle> pending;
        pending.swap( m_PendingSet );
        for ( const TimerHandle handle : pending )
        {
            TimerData& data = m_Timers[handle.Index];
            data.ExpireTime += m_InternalTime;
            data.State = Status::Active;
            PushActive( handle );
        }

        // A paused timer never reaches the heap's top: one whose owner is gone is dropped here.
        for ( uint32_t index = 0; index < m_Timers.size(); ++index )
        {
            const TimerData& data = m_Timers[index];
            if ( data.Serial != 0 && data.State == Status::Paused && !data.Callback.Alive() )
            {
                TimerHandle handle;
                handle.Index  = index;
                handle.Serial = data.Serial;
                Remove( handle );
            }
        }
    }

    void TimerManager::Clear()
    {
        m_Timers.clear();
        m_Free.clear();
        m_ActiveHeap.clear();
        m_PendingSet.clear();
        m_Executing = TimerHandle();
    }

    std::size_t TimerManager::Pending() const
    {
        std::size_t count = 0;
        for ( uint32_t index = 0; index < m_Timers.size(); ++index )
        {
            TimerHandle handle;
            handle.Index  = index;
            handle.Serial = m_Timers[index].Serial;
            if ( Find( handle ) != nullptr )
                ++count;
        }
        return count;
    }

    TimerHandle TimerManager::Add( TimerData data )
    {
        data.Serial  = m_NextSerial;
        m_NextSerial = m_NextSerial == std::numeric_limits<uint32_t>::max() ? 1u : m_NextSerial + 1u;
        data.Order   = m_NextOrder++;
        TimerHandle handle;
        handle.Serial = data.Serial;
        if ( !m_Free.empty() )
        {
            handle.Index = m_Free.back();
            m_Free.pop_back();
            m_Timers[handle.Index] = std::move( data );
        }
        else
        {
            handle.Index = static_cast<uint32_t>( m_Timers.size() );
            m_Timers.push_back( std::move( data ) );
        }
        return handle;
    }

    void TimerManager::Remove( TimerHandle handle )
    {
        m_Timers[handle.Index] = TimerData(); // Serial 0: free; the callable releases its function
        m_Free.push_back( handle.Index );
    }

    TimerManager::TimerData* TimerManager::Find( TimerHandle handle )
    {
        return const_cast<TimerData*>( static_cast<const TimerManager&>( *this ).Find( handle ) );
    }

    const TimerManager::TimerData* TimerManager::Find( TimerHandle handle ) const
    {
        if ( handle.Serial == 0 || handle.Index >= m_Timers.size() )
            return nullptr;
        const TimerData& data = m_Timers[handle.Index];
        if ( data.Serial != handle.Serial || data.State == Status::ActivePendingRemoval || !data.Callback.Alive() )
            return nullptr;
        return &data;
    }

    void TimerManager::PushActive( TimerHandle handle )
    {
        m_ActiveHeap.push_back( handle );
        std::push_heap( m_ActiveHeap.begin(), m_ActiveHeap.end(),
                        [this]( TimerHandle a, TimerHandle b ) { return FiresLater( a, b ); } );
    }

    void TimerManager::RemoveActive( TimerHandle handle )
    {
        std::erase_if( m_ActiveHeap, [handle]( TimerHandle h ) { return Same( h, handle ); } );
        std::make_heap( m_ActiveHeap.begin(), m_ActiveHeap.end(),
                        [this]( TimerHandle a, TimerHandle b ) { return FiresLater( a, b ); } );
    }

    void TimerManager::RemovePending( TimerHandle handle )
    {
        std::erase_if( m_PendingSet, [handle]( TimerHandle h ) { return Same( h, handle ); } );
    }

    bool TimerManager::FiresLater( TimerHandle a, TimerHandle b ) const
    {
        const TimerData& x = m_Timers[a.Index];
        const TimerData& y = m_Timers[b.Index];
        if ( x.ExpireTime != y.ExpireTime )
            return x.ExpireTime > y.ExpireTime;
        return x.Order > y.Order;
    }
} // namespace Desert::Core
