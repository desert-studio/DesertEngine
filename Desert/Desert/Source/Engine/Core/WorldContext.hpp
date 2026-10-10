#pragma once

namespace Desert::Assets
{
    class AssetManager;
}

namespace Desert::Core
{
    class Scene;
    class TimerManager;

    /// THE WORLD A SCRIPT CALL RUNS IN — UE's WorldContextObject, supplied by the host instead of spelled by
    /// every caller. A script host opens a Scope around each entry into script code (top level, OnStart,
    /// OnUpdate, a timer, an event), so a reflected library function that needs the world (World.find,
    /// World.spawn) reads Current() and no language type crosses into it.
    struct WorldContext
    {
        Scene*                World  = nullptr;
        Assets::AssetManager* Assets = nullptr;
        TimerManager*         Timers = nullptr; // the world's timers (Timer.after)

        /// The innermost open scope's context, or nullptr outside any script call.
        static const WorldContext* Current();

        class Scope
        {
        public:
            explicit Scope( const WorldContext& context );
            ~Scope();
            Scope( const Scope& )            = delete;
            Scope& operator=( const Scope& ) = delete;

        private:
            const WorldContext* m_Previous;
        };
    };
} // namespace Desert::Core
