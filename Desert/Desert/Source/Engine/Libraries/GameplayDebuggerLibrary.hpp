#pragma once

#include <CoreReflection/ReflectionMacros.hpp>

#include <string>

namespace Desert::Libraries
{
    /// The running script's world's gameplay debugger (GameplayDebugger/GameplayDebugger.hpp) switched from a
    /// script - what the ` key and the digits do, for a test level or a scripted repro:
    ///
    ///   GameplayDebugger.setActive( true )             -- show the overlay
    ///   GameplayDebugger.setCategory( "Basic", false ) -- hide one category
    ///
    /// In a player's build there is no debugger (DESERT_DEV_INSTRUMENTS): the calls do nothing and isActive is
    /// false.
    struct GameplayDebuggerLibrary
    {
        REFLECT( ScriptName( "GameplayDebugger" ) )

        FUNCTION( ScriptCallable, ScriptName( "setActive" ), Tooltip( "Shows or hides the gameplay debugger." ) )
        static void SetActive( bool active );

        FUNCTION( ScriptCallable, ScriptName( "isActive" ), Tooltip( "True while the gameplay debugger shows." ) )
        static bool IsActive();

        FUNCTION( ScriptCallable, ScriptName( "setCategory" ),
                  Tooltip( "Turns the category of that name on or off; false when there is none." ) )
        static bool SetCategory( const std::string& name, bool enabled );
    };
} // namespace Desert::Libraries
