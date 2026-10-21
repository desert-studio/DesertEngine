#pragma once
/**
 * @file GameplayDebuggerCategory.hpp
 * @brief One page of the gameplay debugger (UE FGameplayDebuggerCategory): what it collects about the target and
 *        how the collected data is drawn.
 *
 * ── COLLECT, THEN DRAW, WITH PLAIN DATA BETWEEN ──────────────────────────────────────────────────────────────
 *
 * UE splits a category in two: CollectData runs where the world's authority is (the server) every
 * CollectDataInterval, and DrawData runs on the viewing client from what was replicated. Here the same seam is
 * kept even though there is one machine today: CollectData fills a GameplayDebuggerCategoryData - text lines and
 * shapes, values only, no Scene, entity or component pointers - and DrawData reads nothing else. That struct is
 * what the network slice (GDT-NET) sends; nothing a category draws may come from anywhere but it.
 */

#include <Common/Core/DevInstruments.hpp>

#if DESERT_DEV_INSTRUMENTS
#include <Engine/Core/DebugDraw.hpp>

#include <Engine/ECS/Entity.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace Desert::Core
{
    class Scene;
}

namespace Desert::Debug
{
    /// A shape a category shows in the world (UE FGameplayDebuggerShape), as values. Drawn depth-tested, for the
    /// frame, with Description written at its first point.
    struct GameplayDebuggerShape
    {
        enum class Kind : uint8_t
        {
            Point,   // A, Radius = the cross's half size
            Segment, // A -> B
            Arrow,   // A -> B, head Radius cm
            Box,     // centre A, half extent Extent, Rotation
            Sphere,  // centre A, Radius
            Capsule  // centre A, HalfHeight, Radius, Rotation (stands on its local Y)
        };

        Kind        Type = Kind::Point;
        glm::vec3   A    = glm::vec3( 0.0f );
        glm::vec3   B    = glm::vec3( 0.0f );
        glm::vec3   Extent{ 0.0f };
        glm::quat   Rotation   = glm::quat( 1.0f, 0.0f, 0.0f, 0.0f );
        float       Radius     = 0.0f;
        float       HalfHeight = 0.0f;
        glm::vec4   Color      = glm::vec4( 1.0f );
        std::string Description;
    };

    /// Everything one category collected about the target in one collection: what DrawData draws and what the
    /// network slice will replicate.
    struct GameplayDebuggerCategoryData
    {
        std::vector<Core::DebugDrawCanvasLine> Lines;
        std::vector<GameplayDebuggerShape>     Shapes;

        /// UE AddTextLine, as runs instead of {color} markup.
        void AddTextLine( std::vector<Core::DebugDrawTextRun> runs )
        {
            Lines.push_back( Core::DebugDrawCanvasLine{ std::move( runs ) } );
        }
        /// UE AddShape.
        void AddShape( GameplayDebuggerShape shape )
        {
            Shapes.push_back( std::move( shape ) );
        }
        void Reset()
        {
            Lines.clear();
            Shapes.clear();
        }
    };

    class IGameplayDebuggerCategory
    {
    public:
        virtual ~IGameplayDebuggerCategory() = default;

        /// The name in the debugger's header and the one a script toggles it by.
        [[nodiscard]] virtual std::string_view GetName() const = 0;

        /// UE CollectDataInterval: seconds of scene time between two collections; 0 = every frame. A new target
        /// is always collected at once.
        [[nodiscard]] virtual float GetCollectInterval() const
        {
            return 0.0f;
        }

        /// UE's category default state: on when the debugger first shows.
        [[nodiscard]] virtual bool IsEnabledByDefault() const
        {
            return true;
        }

        /// UE CollectData: what to show about @p target (nullptr when nothing is targeted) into @p out, which
        /// arrives empty. Reads the scene; never keeps a pointer into it.
        virtual void CollectData( const Core::Scene& scene, const ECS::Entity* target,
                                  GameplayDebuggerCategoryData& out ) = 0;

        /// UE DrawData: shows @p data - the text into @p panel (the debugger's screen panel, below this category's
        /// title), the shapes into @p scene's debug drawing for this frame. Override only to draw the same data
        /// differently; whatever is drawn must come from @p data.
        virtual void DrawData( Core::Scene& scene, const GameplayDebuggerCategoryData& data,
                               std::vector<Core::DebugDrawCanvasLine>& panel ) const;
    };
} // namespace Desert::Debug
#endif // DESERT_DEV_INSTRUMENTS
