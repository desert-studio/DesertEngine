#pragma once

#include <Engine/Reflection/ReflectionMacros.hpp>

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

        FUNCTION( ScriptCallable, ScriptName( "play" ), Tooltip( "Plays a clip once at a volume (1 = as authored)." ) )
        static void Play( const std::string& clip, float volume );

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

        FUNCTION( ScriptCallable, ScriptName( "open" ), Tooltip( "Requests travel to a level; false (logged) when refused." ) )
        static bool Open( const std::string& level );
    };
} // namespace Desert::Libraries
