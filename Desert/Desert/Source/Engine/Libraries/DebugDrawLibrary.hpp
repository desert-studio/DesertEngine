#pragma once

#include <CoreReflection/ReflectionMacros.hpp>

#include <glm/vec3.hpp>

#include <string>

namespace Desert::Libraries
{
    /// Gameplay debug drawing from a script - UE's DrawDebug* Blueprint nodes over Core/DebugDraw.hpp, into the
    /// running script's world. Positions and sizes in cm, colours as rgb vectors, durations in seconds of the
    /// world's time (0 = this frame only; a pause freezes the count). `foreground` draws over everything instead
    /// of being hidden by the scene.
    ///
    ///   DebugDraw.line( a, b, vector( 1, 0, 0 ), 2.0 )        -- a red line for two seconds
    ///   DebugDraw.sphere( p, 50, vector( 0, 1, 0 ) )          -- a green sphere this frame
    ///   DebugDraw.text( p, "target", vector( 1, 1, 0 ), 2.0 ) -- a label at p
    ///
    /// Named DebugDraw, not `debug`: `debug` is Luau's own library (debug.traceback), and a global of that name
    /// would hide it from every script. In a player's build the calls draw nothing (DESERT_DEV_INSTRUMENTS).
    struct DebugDrawLibrary
    {
        REFLECT( ScriptName( "DebugDraw" ) )

        FUNCTION( ScriptCallable, ScriptName( "line" ),
                  Tooltip( "A line from start to end; thickness in cm (0 = one pixel)." ) )
        static void Line( const glm::vec3& start, const glm::vec3& end, const glm::vec3& color = glm::vec3( 1.0f ),
                          float duration = 0.0f, float thickness = 0.0f, bool foreground = false );

        FUNCTION( ScriptCallable, ScriptName( "point" ), Tooltip( "A three-axis cross size cm across." ) )
        static void Point( const glm::vec3& position, float size = 10.0f,
                           const glm::vec3& color = glm::vec3( 1.0f ), float duration = 0.0f,
                           bool foreground = false );

        FUNCTION(
             ScriptCallable, ScriptName( "box" ),
             Tooltip( "An axis-aligned box: center and half extent; rotationDegrees turns it (pitch, yaw, roll "
                      "about X, Y, Z)." ) )
        static void Box( const glm::vec3& center, const glm::vec3& extent,
                         const glm::vec3& color = glm::vec3( 1.0f ), float duration = 0.0f,
                         const glm::vec3& rotationDegrees = glm::vec3( 0.0f ), float thickness = 0.0f,
                         bool foreground = false );

        FUNCTION( ScriptCallable, ScriptName( "sphere" ), Tooltip( "A wire sphere of segments meridians." ) )
        static void Sphere( const glm::vec3& center, float radius, const glm::vec3& color = glm::vec3( 1.0f ),
                            float duration = 0.0f, int segments = 12, float thickness = 0.0f,
                            bool foreground = false );

        FUNCTION( ScriptCallable, ScriptName( "capsule" ),
                  Tooltip( "A capsule standing on Y: halfHeight from the center to a tip." ) )
        static void Capsule( const glm::vec3& center, float halfHeight, float radius,
                             const glm::vec3& color = glm::vec3( 1.0f ), float duration = 0.0f,
                             const glm::vec3& rotationDegrees = glm::vec3( 0.0f ), float thickness = 0.0f,
                             bool foreground = false );

        FUNCTION( ScriptCallable, ScriptName( "arrow" ), Tooltip( "A line with a head of headSize cm at end." ) )
        static void Arrow( const glm::vec3& start, const glm::vec3& end, float headSize = 20.0f,
                           const glm::vec3& color = glm::vec3( 1.0f ), float duration = 0.0f,
                           float thickness = 0.0f, bool foreground = false );

        FUNCTION(
             ScriptCallable, ScriptName( "text" ),
             Tooltip( "Text drawn over the picture where position projects; scale 1 is the debug font size." ) )
        static void Text( const glm::vec3& position, const std::string& text,
                          const glm::vec3& color = glm::vec3( 1.0f ), float duration = 0.0f, float scale = 1.0f );

        FUNCTION( ScriptCallable, ScriptName( "flush" ),
                  Tooltip( "Removes every timed and persistent line and every string of the world." ) )
        static void Flush();
    };
} // namespace Desert::Libraries
