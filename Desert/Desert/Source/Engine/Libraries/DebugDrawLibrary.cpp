#include <Engine/Libraries/DebugDrawLibrary.hpp>

#include <Common/Core/Logger.hpp>
#include <Engine/Core/DebugDraw.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/Core/WorldContext.hpp>

#include <glm/gtc/quaternion.hpp>

namespace Desert::Libraries
{
    namespace
    {
        // The world of the running script; nullptr (logged) outside a script call.
        Core::Scene* World( const char* function )
        {
            const Core::WorldContext* context = Core::WorldContext::Current();
            if ( context == nullptr || context->World == nullptr )
            {
                LOG_ERROR( "[Script] DebugDraw.{}: no world — called outside a script call", function );
                return nullptr;
            }
            return context->World;
        }

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
        if ( Core::Scene* scene = World( "line" ) )
            Core::DebugDraw::Line( *scene, start, end, Style( color, duration, thickness, foreground ) );
    }

    void DebugDrawLibrary::Point( const glm::vec3& position, const float size, const glm::vec3& color,
                                  const float duration, const bool foreground )
    {
        if ( Core::Scene* scene = World( "point" ) )
            Core::DebugDraw::Point( *scene, position, size, Style( color, duration, 0.0f, foreground ) );
    }

    void DebugDrawLibrary::Box( const glm::vec3& center, const glm::vec3& extent, const glm::vec3& color,
                                const float duration, const glm::vec3& rotationDegrees, const float thickness,
                                const bool foreground )
    {
        if ( Core::Scene* scene = World( "box" ) )
            Core::DebugDraw::Box( *scene, center, extent, Rotation( rotationDegrees ),
                                  Style( color, duration, thickness, foreground ) );
    }

    void DebugDrawLibrary::Sphere( const glm::vec3& center, const float radius, const glm::vec3& color,
                                   const float duration, const int segments, const float thickness,
                                   const bool foreground )
    {
        if ( Core::Scene* scene = World( "sphere" ) )
            Core::DebugDraw::Sphere( *scene, center, radius, segments,
                                     Style( color, duration, thickness, foreground ) );
    }

    void DebugDrawLibrary::Capsule( const glm::vec3& center, const float halfHeight, const float radius,
                                    const glm::vec3& color, const float duration, const glm::vec3& rotationDegrees,
                                    const float thickness, const bool foreground )
    {
        if ( Core::Scene* scene = World( "capsule" ) )
            Core::DebugDraw::Capsule( *scene, center, halfHeight, radius, Rotation( rotationDegrees ),
                                      Style( color, duration, thickness, foreground ) );
    }

    void DebugDrawLibrary::Arrow( const glm::vec3& start, const glm::vec3& end, const float headSize,
                                  const glm::vec3& color, const float duration, const float thickness,
                                  const bool foreground )
    {
        if ( Core::Scene* scene = World( "arrow" ) )
            Core::DebugDraw::Arrow( *scene, start, end, headSize,
                                    Style( color, duration, thickness, foreground ) );
    }

    void DebugDrawLibrary::Text( const glm::vec3& position, const std::string& text, const glm::vec3& color,
                                 const float duration, const float scale )
    {
        if ( Core::Scene* scene = World( "text" ) )
            Core::DebugDraw::String( *scene, position, text, scale, Style( color, duration, 0.0f, false ) );
    }

    void DebugDrawLibrary::Flush()
    {
        if ( Core::Scene* scene = World( "flush" ) )
        {
            Core::DebugDraw::FlushPersistent( *scene );
            Core::DebugDraw::FlushStrings( *scene );
        }
    }
} // namespace Desert::Libraries
