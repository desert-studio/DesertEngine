#pragma once

namespace Desert::Assets
{
    class AssetManager;
}

namespace Desert::Input
{
    class LocalPlayerInput;
}

namespace Desert::Core
{
    class Scene;
    class TimerManager;

    /// What a library function asks of the script host itself. The host is a plugin over Engine (Scripting), so
    /// Engine code reaches it through this and never by its type.
    class ScriptHost
    {
    public:
        virtual ~ScriptHost() = default;

        /// A SaveGame load wrote the SaveGame properties into the ScriptComponents: the running scripts get
        /// the loaded values now (UGameplayStatics::LoadGameFromSlot's caller reads them on its next line).
        virtual void ApplySaveGameProperties() = 0;
    };

    /// THE WORLD A SCRIPT CALL RUNS IN — UE's WorldContextObject, supplied by the host instead of spelled by
    /// every caller. A script host opens a Scope around each entry into script code (top level, OnStart,
    /// OnUpdate, a timer, an event), so a reflected library function that needs the world (World.find,
    /// World.spawn) reads Current() and no language type crosses into it.
    struct WorldContext
    {
        Scene*                   World       = nullptr;
        Assets::AssetManager*    Assets      = nullptr;
        TimerManager*            Timers      = nullptr; // the world's timers (Timer.after)
        Input::LocalPlayerInput* PlayerInput = nullptr; // the local player's Enhanced Input (Input.action*)
        ScriptHost*              Host        = nullptr; // the running script host (SaveGame.load)

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
