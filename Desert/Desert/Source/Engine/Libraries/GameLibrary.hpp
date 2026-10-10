#pragma once

#include <Engine/Reflection/ReflectionMacros.hpp>
#include <Engine/Reflection/Value.hpp>

#include <string>

namespace Desert::Libraries
{
    /// Logging into the engine's Logs panel (the panel filters by level).
    struct LogLibrary
    {
        REFLECT( ScriptName( "Log" ) )

        FUNCTION( ScriptCallable, ScriptName( "info" ) )
        static void Info( const std::string& message );

        FUNCTION( ScriptCallable, ScriptName( "warn" ) )
        static void Warn( const std::string& message );

        FUNCTION( ScriptCallable, ScriptName( "error" ) )
        static void Error( const std::string& message );
    };

    /// One-shot sounds through the audio engine.
    struct AudioLibrary
    {
        REFLECT( ScriptName( "Audio" ) )

        FUNCTION( ScriptCallable, ScriptName( "play" ),
                  Tooltip( "Plays a clip once at a volume (1 = as authored, the default)." ) )
        static void Play( const std::string& clip, float volume = 1.0f );

        FUNCTION( ScriptCallable, ScriptName( "stopAll" ) )
        static void StopAll();
    };

    /// The running project's identity.
    struct ProjectLibrary
    {
        REFLECT( ScriptName( "project" ) )

        FUNCTION( ScriptCallable, ScriptName( "name" ) )
        static std::string Name();

        FUNCTION( ScriptCallable, ScriptName( "company" ) )
        static std::string Company();
    };

    /// Level travel — UGameplayStatics::OpenLevel.
    struct LevelLibrary
    {
        REFLECT( ScriptName( "level" ) )

        FUNCTION( ScriptCallable, ScriptName( "open" ),
                  Tooltip( "Requests travel to a level; false (logged) when refused." ) )
        static bool Open( const std::string& level );
    };

    /// Game-time timers of the running script's world (Core::TimerManager through Core::WorldContext) — UE's
    /// SetTimer on the world's FTimerManager. A callback belongs to the script that scheduled it: a reload or
    /// the entity's destruction drops it unfired.
    struct TimerLibrary
    {
        REFLECT( ScriptName( "Timer" ) )

        FUNCTION( ScriptCallable, ScriptName( "after" ),
                  Tooltip( "Calls the function once after `seconds` of game time (Play only); it may re-arm itself." ) )
        static void After( float seconds, const Reflection::Callable& callback );
    };
} // namespace Desert::Libraries
