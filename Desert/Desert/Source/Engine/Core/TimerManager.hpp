#pragma once

#include <Engine/Reflection/Value.hpp>

#include <vector>

namespace Desert::Core
{
    /// ONE-SHOT TIMERS OF A WORLD — UE's FTimerManager (UWorld::GetTimerManager), reduced to what gameplay asks of
    /// it today: call this after N seconds of game time. A timer holds a Reflection::Callable, so the engine keeps
    /// and fires it without knowing which language made it; a callable whose owner is gone (a script reloaded,
    /// its entity destroyed) is dropped instead of fired. Ticked by whoever advances game time (ScriptSystem in
    /// Play); reached by a library function through Core::WorldContext::Timers.
    class TimerManager
    {
    public:
        /// Calls `callback` once after `seconds` (clamped to 0) of ticked time.
        void After( float seconds, Reflection::Callable callback );

        /// Advances every timer by `dt`; fires, in scheduling order, the ones that came due and are still alive
        /// (a callback may schedule again — that timer waits for the next Tick). Dead timers are dropped.
        void Tick( float dt );

        void Clear();

        [[nodiscard]] std::size_t Pending() const
        {
            return m_Timers.size();
        }

    private:
        struct Timer
        {
            float                Remaining = 0.0f;
            Reflection::Callable Callback;
        };
        std::vector<Timer> m_Timers;
    };
} // namespace Desert::Core
