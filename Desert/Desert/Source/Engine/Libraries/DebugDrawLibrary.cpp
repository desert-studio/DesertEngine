#include <Engine/Libraries/DebugDrawLibrary.hpp>

#include <Common/Core/Logger.hpp>
#include <Engine/Core/DebugDraw.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/Core/WorldContext.hpp>
#include <Engine/Libraries/ScriptWorld.hpp>

#include <glm/gtc/quaternion.hpp>

namespace Desert::Libraries
{
    namespace
    {
        Core::DebugDrawStyle Style( const glm::vec3& color, const float duration, const float thickness,
                                    const bool foreground )
        {
            return Core::DebugDrawStyle{ .Color     = glm::vec4( color, 1.0f ),
                                         .Duration  = duration,
                                         .Thickness = thickness,
                                         .Depth     = foreground ? Core::DebugDrawDepth::Foreground
                                                                 : Core::DebugDrawDepth::World };
        }

        glm::quat Rotation( const glm::vec3& degrees )
        {
            return glm::quat( glm::radians( degrees ) );
        }
    } // namespace

    void DebugDrawLibrary::Line( const glm::vec3& start, const glm::vec3& end, const glm::vec3& color,
                                 const float duration, const float thickness, const bool foreground )
    {
        if ( Core::Scene* scene = ScriptScene( "DebugDraw", "line" ) )
            Core::DebugDraw::Line( *scene, start, end, Style( color, duration, thickness, foreground ) );
    }

    void DebugDrawLibrary::Point( const glm::vec3& position, const float size, const glm::vec3& color,
                                  const float duration, const bool foreground )
    {
        if ( Core::Scene* scene = ScriptScene( "DebugDraw", "point" ) )
            Core::DebugDraw::Point( *scene, position, size, Style( color, duration, 0.0f, foreground ) );
    }

    void DebugDrawLibrary::Box( const glm::vec3& center, const glm::vec3& extent, const glm::vec3& color,
                                const float duration, const glm::vec3& rotationDegrees, const float thickness,
                                const bool foreground )
    {
        if ( Core::Scene* scene = ScriptScene( "DebugDraw", "box" ) )
            Core::DebugDraw::Box( *scene, center, extent, Rotation( rotationDegrees ),
                                  Style( color, duration, thickness, foreground ) );
    }

    void DebugDrawLibrary::Sphere( const glm::vec3& center, const float radius, const glm::vec3& color,
                                   const float duration, const int segments, const float thickness,
                                   const bool foreground )
    {
        if ( Core::Scene* scene = ScriptScene( "DebugDraw", "sphere" ) )
            Core::DebugDraw::Sphere( *scene, center, radius, segments,
                                     Style( color, duration, thickness, foreground ) );
    }

    void DebugDrawLibrary::Capsule( const glm::vec3& center, const float halfHeight, const float radius,
                                    const glm::vec3& color, const float duration, const glm::vec3& rotationDegrees,
                                    const float thickness, const bool foreground )
    {
        if ( Core::Scene* scene = ScriptScene( "DebugDraw", "capsule" ) )
            Core::DebugDraw::Capsule( *scene, center, halfHeight, radius, Rotation( rotationDegrees ),
                                      Style( color, duration, thickness, foreground ) );
    }

    void DebugDrawLibrary::Arrow( const glm::vec3& start, const glm::vec3& end, const float headSize,
                                  const glm::vec3& color, const float duration, const float thickness,
                                  const bool foreground )
    {
        if ( Core::Scene* scene = ScriptScene( "DebugDraw", "arrow" ) )
            Core::DebugDraw::Arrow( *scene, start, end, headSize,
                                    Style( color, duration, thickness, foreground ) );
    }

    void DebugDrawLibrary::Text( const glm::vec3& position, const std::string& text, const glm::vec3& color,
                                 const float duration, const float scale )
    {
        if ( Core::Scene* scene = ScriptScene( "DebugDraw", "text" ) )
            Core::DebugDraw::String( *scene, position, text, scale, Style( color, duration, 0.0f, false ) );
    }

    void DebugDrawLibrary::Flush()
    {
        if ( Core::Scene* scene = ScriptScene( "DebugDraw", "flush" ) )
        {
            Core::DebugDraw::FlushPersistent( *scene );
            Core::DebugDraw::FlushStrings( *scene );
        }
    }
} // namespace Desert::Libraries
