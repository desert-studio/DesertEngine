#pragma once

#include <Common/Core/ResultStr.hpp>
#include <CoreReflection/ReflectionMacros.hpp>
#include <Engine/Core/TimerManager.hpp>
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
                  Tooltip( "Requests travel to a level (none: the project's default map). Ok, or false and the "
                           "reason." ) )
        static Common::BoolResultStr Open( const std::string& level = "" );
    };

    /// Game-time timers of the running script's world (Core::TimerManager through Core::WorldContext) — UE's
    /// SetTimer / ClearTimer / PauseTimer and the timer queries of UKismetSystemLibrary, on the world's
    /// FTimerManager. A callback belongs to the script that set it: a reload or the entity's destruction ends its
    /// timers unfired. Every setter answers a TimerHandle the other functions take; the invalid handle (a refused
    /// set, or {}) names no timer.
    struct TimerLibrary
    {
        REFLECT( ScriptName( "Timer" ) )

        FUNCTION( ScriptCallable, ScriptName( "after" ),
                  Tooltip( "Calls the function once after `seconds` of game time (Play only); 0 or less: on the "
                           "next tick. Answers its TimerHandle." ) )
        static Core::TimerHandle After( float seconds, const Reflection::Callable& callback );

        FUNCTION( ScriptCallable, ScriptName( "every" ),
                  Tooltip( "Calls the function every `seconds` of game time until cleared; the first call after "
                           "firstDelay (< 0: `seconds`). A frame longer than several periods calls it once per "
                           "period missed, or once when maxOncePerFrame. Answers its TimerHandle; `seconds` <= 0 sets "
                           "nothing (the invalid handle)." ) )
        static Core::TimerHandle Every( float seconds, const Reflection::Callable& callback, float firstDelay = -1.0f,
                                        bool maxOncePerFrame = false );

        FUNCTION( ScriptCallable, ScriptName( "nextTick" ),
                  Tooltip( "Calls the function once on the next tick of the world's timers." ) )
        static Core::TimerHandle NextTick( const Reflection::Callable& callback );

        FUNCTION( ScriptCallable, ScriptName( "clear" ),
                  Tooltip( "The timer is never called again (one running finishes its current call)." ) )
        static void Clear( const Core::TimerHandle& handle );

        FUNCTION( ScriptCallable, ScriptName( "pause" ), Tooltip( "Stops the timer's clock; the time left is kept." ) )
        static void Pause( const Core::TimerHandle& handle );

        FUNCTION( ScriptCallable, ScriptName( "unpause" ), Tooltip( "Runs a paused timer's clock again." ) )
        static void UnPause( const Core::TimerHandle& handle );

        FUNCTION( ScriptCallable, ScriptName( "isActive" ), Tooltip( "The timer exists and is not paused." ) )
        static bool IsActive( const Core::TimerHandle& handle );

        FUNCTION( ScriptCallable, ScriptName( "isPaused" ) )
        static bool IsPaused( const Core::TimerHandle& handle );

        FUNCTION( ScriptCallable, ScriptName( "isPending" ),
                  Tooltip( "Set this frame: its clock starts when the world's timers tick." ) )
        static bool IsPending( const Core::TimerHandle& handle );

        FUNCTION( ScriptCallable, ScriptName( "exists" ),
                  Tooltip( "Set, and not yet cleared, finished or ended with its script." ) )
        static bool Exists( const Core::TimerHandle& handle );

        FUNCTION( ScriptCallable, ScriptName( "rate" ), Tooltip( "Seconds between calls; -1 when it does not exist." ) )
        static float Rate( const Core::TimerHandle& handle );

        FUNCTION( ScriptCallable, ScriptName( "elapsed" ),
                  Tooltip( "Seconds since the period began; -1 when it does not exist." ) )
        static float Elapsed( const Core::TimerHandle& handle );

        FUNCTION( ScriptCallable, ScriptName( "remaining" ),
                  Tooltip( "Seconds to the next call; -1 when it does not exist." ) )
        static float Remaining( const Core::TimerHandle& handle );
    };
} // namespace Desert::Libraries
