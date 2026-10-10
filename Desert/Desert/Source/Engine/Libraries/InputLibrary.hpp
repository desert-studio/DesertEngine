#pragma once

#include <Engine/Reflection/ReflectionMacros.hpp>

#include <glm/vec2.hpp>

#include <optional>
#include <string>

namespace Desert::Libraries
{
    /// Keyboard/mouse state for gameplay code — UE's input functions of APlayerController (IsInputKeyDown,
    /// WasInputKeyJustPressed, GetInputMouseDelta). Keys are named ("W", "5", "Space", "Shift", "Left", ...).
    /// The frame state (edges, mouse delta, the cursor request) is advanced by the game loop: NewFrame before
    /// gameplay runs, ConsumeCursorLockRequest after.
    struct InputLibrary
    {
        REFLECT( ScriptName( "Input" ) )

        FUNCTION( ScriptCallable, ScriptName( "isKeyDown" ), Tooltip( "Whether the named key is held." ) )
        static bool IsKeyDown( const std::string& key );

        FUNCTION( ScriptCallable, ScriptName( "wasPressed" ),
                  Tooltip( "Whether the named key went down this frame." ) )
        static bool WasPressed( const std::string& key );

        FUNCTION( ScriptCallable, ScriptName( "mouseDelta" ), Tooltip( "This frame's mouse movement (x, y)." ) )
        static glm::vec2 MouseDelta();

        FUNCTION( ScriptCallable, ScriptName( "lockCursor" ), Tooltip( "Captures the cursor (gameplay look)." ) )
        static void LockCursor();

        FUNCTION( ScriptCallable, ScriptName( "showCursor" ), Tooltip( "Frees the cursor (click UI)." ) )
        static void ShowCursor();

        FUNCTION( ScriptCallable, ScriptName( "isMouseDown" ),
                  Tooltip( "Whether \"left\", \"right\" or \"middle\" is held." ) )
        static bool IsMouseDown( const std::string& button = "left" );

        /// Advances the frame: key edges for WasPressed, and the mouse delta the host computed (capture aware).
        static void NewFrame( glm::vec2 mouseDelta );

        /// nullopt when no gameplay code touched the cursor since the last call; true = lock, false = show.
        static std::optional<bool> ConsumeCursorLockRequest();
    };
} // namespace Desert::Libraries
