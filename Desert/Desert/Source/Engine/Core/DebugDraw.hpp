#pragma once
/**
 * @file DebugDraw.hpp
 * @brief Gameplay debug drawing - UE's DrawDebugHelpers: lines, boxes, spheres, capsules, arrows, points and
 *        world text, each for one frame, for a duration or until flushed.
 *
 * ── WHERE THE SHAPES LIVE ────────────────────────────────────────────────────────────────────────
 *
 * Every Scene owns one DebugDrawWorld (UE: the world's ULineBatchComponents). A shape is turned into world
 * line segments at the call (DrawDebugBox pushes twelve lines, as UE's does), so the renderer sees one list of
 * segments and one list of strings. The view's MeshRenderer draws the segments in its DebugLinesPass and the
 * view's renderer draws the strings over the picture at the projected point (UE DrawDebugString), so the
 * editor viewport and Play show the same thing.
 *
 * ── LIFETIME ─────────────────────────────────────────────────────────────────────────────────────
 *
 * Duration 0 and not persistent = one frame: the item is dropped at the start of the next scene update that
 * is not a pause (the frame a script drew it in is rendered, then the next frame's scripts draw again).
 * Duration > 0 counts down in the scene's time - the gameplay delta in Play (a pause freezes it, as UE's
 * paused world does), the editor's delta in Edit. Persistent stays until FlushPersistent.
 *
 * ── NOT IN A PLAYER'S BUILD ──────────────────────────────────────────────────────────────────────
 *
 * UE compiles DrawDebug* out of Shipping (ENABLE_DRAW_DEBUG); here the one question is DESERT_DEV_INSTRUMENTS
 * (Common/Core/DevInstruments.hpp). In a player build the calls below are empty inline functions, so a
 * gameplay script that draws keeps running and nothing is stored, drawn or paid for.
 */

#include <Common/Core/DevInstruments.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::Core
{
    class Scene;

    /// UE ESceneDepthPriorityGroup's two values a debug shape uses: tested against the scene's depth, or drawn
    /// over everything.
    enum class DebugDrawDepth : uint8_t
    {
        World,
        Foreground
    };

    /// How one debug item is drawn and how long it lives. Thickness is in world units (cm); 0 is a one-pixel
    /// line (UE's Thickness 0).
    struct DebugDrawStyle
    {
        glm::vec4      Color      = glm::vec4( 1.0f );
        float          Duration   = 0.0f; // seconds of scene time; 0 = one frame
        bool           Persistent = false;
        float          Thickness  = 0.0f;
        DebugDrawDepth Depth      = DebugDrawDepth::World;
    };

    struct DebugDrawLine
    {
        glm::vec3      Start;
        glm::vec3      End;
        glm::vec4      Color;
        float          Thickness;
        float          RemainingSeconds; // only read when !OneFrame && !Persistent
        bool           OneFrame;
        bool           Persistent;
        DebugDrawDepth Depth;
    };

    struct DebugDrawString
    {
        glm::vec3   Position;
        std::string Text;
        glm::vec4   Color;
        float       Scale; // the text's height in screen pixels is the engine font size times this
        float       RemainingSeconds;
        bool        OneFrame;
        bool        Persistent;
    };

    /// One coloured piece of a canvas line (UE's {color} markup in FGameplayDebuggerCanvasContext::Print, as
    /// data).
    struct DebugDrawTextRun
    {
        std::string Text;
        glm::vec4   Color = glm::vec4( 1.0f );
    };

    /// One line of a screen panel: its runs side by side.
    struct DebugDrawCanvasLine
    {
        std::vector<DebugDrawTextRun> Runs;
    };

    /// How a screen panel is drawn and how long it lives (the lifetime rule of DebugDrawStyle).
    struct DebugDrawPanelStyle
    {
        glm::vec4 Background = glm::vec4( 0.0f, 0.0f, 0.0f, 0.5f ); // alpha 0 = no background
        float     Scale      = 1.0f;
        float     Duration   = 0.0f; // seconds of scene time; 0 = one frame
        bool      Persistent = false;
    };

    /// Text anchored to the VIEW, not the world (UE's debug canvas / HUD text): lines from @p Origin pixels below
    /// and right of the view's top-left corner, over a background as wide as the widest line.
    struct DebugDrawScreenPanel
    {
        glm::vec2                        Origin;
        std::vector<DebugDrawCanvasLine> Lines;
        glm::vec4                        Background;
        float                            Scale;
        float                            RemainingSeconds;
        bool                             OneFrame;
        bool                             Persistent;
    };

    /// The scene's debug items. Written on the game thread (scripts, systems), read by the scene's views
    /// while they build their frames.
    class DebugDrawWorld
    {
    public:
        void AddLine( const glm::vec3& start, const glm::vec3& end, const DebugDrawStyle& style );
        void AddString( const glm::vec3& position, std::string text, float scale, const DebugDrawStyle& style );
        void AddScreenPanel( const glm::vec2& origin, std::vector<DebugDrawCanvasLine> lines,
                             const DebugDrawPanelStyle& style );

        /// The start of a scene update. @p advancesFrame false (a pause) keeps the one-frame items of the last
        /// frame; @p deltaSeconds ages the timed ones and drops those whose time ran out.
        void Tick( float deltaSeconds, bool advancesFrame );

        /// UE FlushPersistentDebugLines: every persistent and timed line goes; one-frame lines stay.
        void FlushPersistentLines();
        /// UE FlushDebugStrings: every string goes, the screen panels with them.
        void FlushStrings();

        [[nodiscard]] const std::vector<DebugDrawLine>& GetLines() const
        {
            return m_Lines;
        }
        [[nodiscard]] const std::vector<DebugDrawString>& GetStrings() const
        {
            return m_Strings;
        }
        [[nodiscard]] const std::vector<DebugDrawScreenPanel>& GetScreenPanels() const
        {
            return m_Panels;
        }
        [[nodiscard]] bool IsEmpty() const
        {
            return m_Lines.empty() && m_Strings.empty() && m_Panels.empty();
        }

    private:
        std::vector<DebugDrawLine>        m_Lines;
        std::vector<DebugDrawString>      m_Strings;
        std::vector<DebugDrawScreenPanel> m_Panels;
    };

    /// The calls. Each turns its shape into line segments in @p scene's DebugDrawWorld. Rotations are world
    /// rotations of the shape's local axes; a capsule stands on its local Y (the engine's up), as UE's on Z.
    namespace DebugDraw
    {
#if DESERT_DEV_INSTRUMENTS
        void Line( Scene& scene, const glm::vec3& start, const glm::vec3& end, const DebugDrawStyle& style = {} );
        /// A three-axis cross @p size cm across (UE draws a screen-facing square; a cross reads in depth too).
        void Point( Scene& scene, const glm::vec3& position, float size, const DebugDrawStyle& style = {} );
        void Box( Scene& scene, const glm::vec3& center, const glm::vec3& extent,
                  const glm::quat&      rotation = glm::quat( 1.0f, 0.0f, 0.0f, 0.0f ),
                  const DebugDrawStyle& style    = {} );
        /// Three great circles plus @p segments meridians and parallels, as UE DrawDebugSphere.
        void Sphere( Scene& scene, const glm::vec3& center, float radius, int segments = 12,
                     const DebugDrawStyle& style = {} );
        void Capsule( Scene& scene, const glm::vec3& center, float halfHeight, float radius,
                      const glm::quat&      rotation = glm::quat( 1.0f, 0.0f, 0.0f, 0.0f ),
                      const DebugDrawStyle& style    = {} );
        /// A line with a head of @p headSize cm at @p end (UE DrawDebugDirectionalArrow).
        void Arrow( Scene& scene, const glm::vec3& start, const glm::vec3& end, float headSize,
                    const DebugDrawStyle& style = {} );
        /// Text at a world point, drawn over the picture where the point projects (UE DrawDebugString).
        void String( Scene& scene, const glm::vec3& position, std::string_view text, float scale = 1.0f,
                     const DebugDrawStyle& style = {} );
        /// Lines of text at a fixed place of every view of @p scene (UE's debug canvas).
        void ScreenPanel( Scene& scene, const glm::vec2& origin, std::vector<DebugDrawCanvasLine> lines,
                          const DebugDrawPanelStyle& style = {} );
        void FlushPersistent( Scene& scene );
        void FlushStrings( Scene& scene );
#else
        inline void Line( Scene&, const glm::vec3&, const glm::vec3&, const DebugDrawStyle& = {} )
        {
        }
        inline void Point( Scene&, const glm::vec3&, float, const DebugDrawStyle& = {} )
        {
        }
        inline void Box( Scene&, const glm::vec3&, const glm::vec3&,
                         const glm::quat& = glm::quat( 1.0f, 0.0f, 0.0f, 0.0f ), const DebugDrawStyle& = {} )
        {
        }
        inline void Sphere( Scene&, const glm::vec3&, float, int = 12, const DebugDrawStyle& = {} )
        {
        }
        inline void Capsule( Scene&, const glm::vec3&, float, float,
                             const glm::quat& = glm::quat( 1.0f, 0.0f, 0.0f, 0.0f ), const DebugDrawStyle& = {} )
        {
        }
        inline void Arrow( Scene&, const glm::vec3&, const glm::vec3&, float, const DebugDrawStyle& = {} )
        {
        }
        inline void String( Scene&, const glm::vec3&, std::string_view, float = 1.0f, const DebugDrawStyle& = {} )
        {
        }
        inline void ScreenPanel( Scene&, const glm::vec2&, std::vector<DebugDrawCanvasLine>,
                                 const DebugDrawPanelStyle& = {} )
        {
        }
        inline void FlushPersistent( Scene& )
        {
        }
        inline void FlushStrings( Scene& )
        {
        }
#endif // DESERT_DEV_INSTRUMENTS
    } // namespace DebugDraw
} // namespace Desert::Core
