#pragma once

#include <Engine/Reflection/ReflectionMacros.hpp>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <optional>
#include <string>

namespace Desert::Libraries
{
    /// Keyboard/mouse state and the local player's Enhanced Input for gameplay code — UE's input functions of APlayerController (IsInputKeyDown,
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

        // ---- Enhanced Input (GP1b): the local player's actions by their file names (IA_Jump), as UE's
        // FInputActionValue / ETriggerEvent (UEnhancedInputLocalPlayerSubsystem). An action no added context maps
        // fails the call naming it: a typo must not read as "not pressed" forever.

        FUNCTION( ScriptCallable, ScriptName( "actionValue" ),
                  Tooltip( "The action's value this frame (x; x, y for Axis2D; x, y, z for Axis3D)." ) )
        static glm::vec3 ActionValue( const std::string& action );

        FUNCTION( ScriptCallable, ScriptName( "actionTriggered" ), Tooltip( "ETriggerEvent::Triggered this frame." ) )
        static bool ActionTriggered( const std::string& action );

        FUNCTION( ScriptCallable, ScriptName( "actionStarted" ), Tooltip( "ETriggerEvent::Started this frame." ) )
        static bool ActionStarted( const std::string& action );

        FUNCTION( ScriptCallable, ScriptName( "actionOngoing" ), Tooltip( "ETriggerEvent::Ongoing this frame." ) )
        static bool ActionOngoing( const std::string& action );

        FUNCTION( ScriptCallable, ScriptName( "actionCompleted" ), Tooltip( "ETriggerEvent::Completed this frame." ) )
        static bool ActionCompleted( const std::string& action );

        FUNCTION( ScriptCallable, ScriptName( "actionCanceled" ), Tooltip( "ETriggerEvent::Canceled this frame." ) )
        static bool ActionCanceled( const std::string& action );

        FUNCTION( ScriptCallable, ScriptName( "actionSeconds" ),
                  Tooltip( "How long the action has been triggering (UE GetElapsedTime), seconds." ) )
        static float ActionSeconds( const std::string& action );

        FUNCTION( ScriptCallable, ScriptName( "addContext" ),
                  Tooltip( "UE AddMappingContext: the context by file name (IMC_Vehicle) or content path." ) )
        static void AddContext( const std::string& context, int priority );

        FUNCTION( ScriptCallable, ScriptName( "removeContext" ),
                  Tooltip( "UE RemoveMappingContext; false when no active context has that name." ) )
        static bool RemoveContext( const std::string& context );

        FUNCTION( ScriptCallable, ScriptName( "rebindKey" ),
                  Tooltip( "UE MapPlayerKey: key in place of defaultKey for the action, saved to this user's "
                           "input.json." ) )
        static void RebindKey( const std::string& context, const std::string& action, const std::string& defaultKey,
                               const std::string& key );

        /// Advances the frame: key edges for WasPressed, and the mouse delta the host computed (capture aware).
        static void NewFrame( glm::vec2 mouseDelta );

        /// nullopt when no gameplay code touched the cursor since the last call; true = lock, false = show.
        static std::optional<bool> ConsumeCursorLockRequest();
    };
} // namespace Desert::Libraries
