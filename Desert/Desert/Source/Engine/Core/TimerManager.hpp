#pragma once

#include <CoreReflection/ReflectionMacros.hpp>
#include <Engine/Reflection/Value.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace Desert::Core
{
    /// WHICH TIMER — UE's FTimerHandle (Engine/TimerHandle.h): the slot a timer occupies and the serial number it
    /// was given there, so a handle kept after its timer finished or was cleared names nothing instead of the next
    /// timer to take the slot. Serial 0 is the handle of no timer (never set, or a refused set). Two 32-bit fields
    /// rather than UE's one packed uint64: a script number is a double, and it holds each of them exactly.
    struct TimerHandle
    {
        REFLECT( ScriptStruct )

        PROPERTY()
        uint32_t Index = 0;

        PROPERTY()
        uint32_t Serial = 0;
    };

    /// How a timer repeats — UE's FTimerManagerTimerParameters.
    struct TimerParameters
    {
        bool  Loop            = false; // calls every `rate` seconds until cleared
        bool  MaxOncePerFrame = false; // a looping timer that fell behind calls once per Tick, not once per period
        float FirstDelay      = -1.0f; // seconds to the first call; < 0: the rate
    };

    /// THE TIMERS OF A WORLD — a port of UE's FTimerManager (Engine/Private/TimerManager.cpp): a heap of active
    /// timers ordered by the game time they are due (ties in the order they were set), a pending set that joins
    /// the heap at the end of a Tick, a paused state that keeps the time remaining, and the one timer executing.
    /// A looping timer that fell behind by several periods is called once per period missed (catch-up) unless
    /// MaxOncePerFrame. A timer holds a Reflection::Callable, so the engine keeps and fires it without knowing
    /// which language made it; a callable whose owner is gone (a script reloaded, its entity destroyed) is a
    /// timer that no longer exists: never called again, dropped from the heap. Ticked by whoever advances game
    /// time (ScriptSystem in Play, after the scripts ran, so a timer set this frame counts from this Tick as in
    /// UE, where it is pending until the world's timers have ticked); reached by a library function through
    /// Core::WorldContext::Timers.
    class TimerManager
    {
    public:
        /// UE SetTimer: calls `callback` after FirstDelay (the rate when < 0), then every `rate` seconds when
        /// Loop. A rate <= 0 or a dead callback sets nothing and answers the invalid handle.
        TimerHandle SetTimer( Reflection::Callable callback, float rate, const TimerParameters& parameters = {} );
        /// UE SetTimerForNextTick: calls `callback` once, on the next Tick.
        TimerHandle SetTimerForNextTick( Reflection::Callable callback );

        /// UE ClearTimer: the timer is never called again (one executing finishes its current call).
        void ClearTimer( TimerHandle handle );
        /// UE PauseTimer / UnPauseTimer: the time remaining is kept while paused. A one-shot timer paused from its
        /// own call is over.
        void PauseTimer( TimerHandle handle );
        void UnPauseTimer( TimerHandle handle );

        [[nodiscard]] bool IsTimerActive( TimerHandle handle ) const; // exists and is not paused
        [[nodiscard]] bool IsTimerPaused( TimerHandle handle ) const;
        [[nodiscard]] bool IsTimerPending( TimerHandle handle ) const; // set, waiting for the end of a Tick
        [[nodiscard]] bool TimerExists( TimerHandle handle ) const;
        /// Seconds; -1 when the timer does not exist (UE).
        [[nodiscard]] float GetTimerRate( TimerHandle handle ) const;
        [[nodiscard]] float GetTimerElapsed( TimerHandle handle ) const;
        [[nodiscard]] float GetTimerRemaining( TimerHandle handle ) const;

        /// Advances game time by `dt` and calls the timers that came due, earliest first; then the pending ones
        /// join the heap.
        void Tick( float dt );

        void Clear();

        /// How many timers exist.
        [[nodiscard]] std::size_t Pending() const;

    private:
        enum class Status : std::uint8_t
        {
            Pending,
            Active,
            Paused,
            Executing,
            ActivePendingRemoval, // cleared while in the heap: removed when it reaches the top
        };

        struct TimerData
        {
            Reflection::Callable Callback;
            double               ExpireTime      = 0.0; // Active/Executing: game time due; Pending/Paused: seconds left
            float                Rate            = 0.0f;
            bool                 Loop            = false;
            bool                 MaxOncePerFrame = false;
            Status               State           = Status::Pending;
            uint32_t             Serial          = 0; // 0: the slot is free
            std::uint64_t        Order           = 0; // the order timers were set in: ties of ExpireTime fire in it
        };

        TimerHandle                    Add( TimerData data );
        void                           Remove( TimerHandle handle );
        [[nodiscard]] TimerData*       Find( TimerHandle handle );
        [[nodiscard]] const TimerData* Find( TimerHandle handle ) const;
        void                           PushActive( TimerHandle handle );
        void                           RemoveActive( TimerHandle handle );
        void                           RemovePending( TimerHandle handle );
        [[nodiscard]] bool             FiresLater( TimerHandle a, TimerHandle b ) const;
        [[nodiscard]] static bool      Same( TimerHandle a, TimerHandle b )
        {
            return a.Index == b.Index && a.Serial == b.Serial;
        }

        std::vector<TimerData>   m_Timers;
        std::vector<uint32_t>    m_Free;
        std::vector<TimerHandle> m_ActiveHeap; // std heap: the front fires first
        std::vector<TimerHandle> m_PendingSet;
        TimerHandle              m_Executing;
        double                   m_InternalTime = 0.0;
        uint32_t                 m_NextSerial   = 1;
        std::uint64_t            m_NextOrder    = 0;
    };
} // namespace Desert::Core
