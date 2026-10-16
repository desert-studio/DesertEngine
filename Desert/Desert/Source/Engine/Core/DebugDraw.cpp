#include <Engine/Core/DebugDraw.hpp>

#include <Engine/Core/Scene.hpp>

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <array>
#include <utility>

namespace Desert::Core
{
    namespace
    {
        bool IsOneFrame( const DebugDrawStyle& style )
        {
            return !style.Persistent && style.Duration <= 0.0f;
        }

        // Ages @p items by @p deltaSeconds and drops the expired ones; one-frame items go only when the frame
        // advances.
        template <class T>
        void AgeItems( std::vector<T>& items, const float deltaSeconds, const bool advancesFrame )
        {
            std::erase_if( items,
                           [&]( T& item )
                           {
                               if ( item.Persistent )
                                   return false;
                               if ( item.OneFrame )
                                   return advancesFrame;
                               item.RemainingSeconds -= deltaSeconds;
                               return item.RemainingSeconds <= 0.0f;
                           } );
        }
    } // namespace

    void DebugDrawWorld::AddLine( const glm::vec3& start, const glm::vec3& end, const DebugDrawStyle& style )
    {
        m_Lines.push_back( DebugDrawLine{ .Start            = start,
                                          .End              = end,
                                          .Color            = style.Color,
                                          .Thickness        = std::max( style.Thickness, 0.0f ),
                                          .RemainingSeconds = style.Duration,
                                          .OneFrame         = IsOneFrame( style ),
                                          .Persistent       = style.Persistent,
                                          .Depth            = style.Depth } );
    }

    void DebugDrawWorld::AddString( const glm::vec3& position, std::string text, const float scale,
                                    const DebugDrawStyle& style )
    {
        m_Strings.push_back( DebugDrawString{ .Position         = position,
                                              .Text             = std::move( text ),
                                              .Color            = style.Color,
                                              .Scale            = scale > 0.0f ? scale : 1.0f,
                                              .RemainingSeconds = style.Duration,
                                              .OneFrame         = IsOneFrame( style ),
                                              .Persistent       = style.Persistent } );
    }

    void DebugDrawWorld::AddScreenPanel( const glm::vec2& origin, std::vector<DebugDrawCanvasLine> lines,
                                         const DebugDrawPanelStyle& style )
    {
        const bool oneFrame = !style.Persistent && style.Duration <= 0.0f;
        m_Panels.push_back( DebugDrawScreenPanel{ .Origin           = origin,
                                                  .Lines            = std::move( lines ),
                                                  .Background       = style.Background,
                                                  .Scale            = style.Scale > 0.0f ? style.Scale : 1.0f,
                                                  .RemainingSeconds = style.Duration,
                                                  .OneFrame         = oneFrame,
                                                  .Persistent       = style.Persistent } );
    }

    void DebugDrawWorld::Tick( const float deltaSeconds, const bool advancesFrame )
    {
        AgeItems( m_Lines, deltaSeconds, advancesFrame );
        AgeItems( m_Strings, deltaSeconds, advancesFrame );
        AgeItems( m_Panels, deltaSeconds, advancesFrame );
    }

    void DebugDrawWorld::FlushPersistentLines()
    {
        std::erase_if( m_Lines, []( const DebugDrawLine& line ) { return !line.OneFrame; } );
    }

    void DebugDrawWorld::FlushStrings()
    {
        m_Strings.clear();
        m_Panels.clear();
    }

#if DESERT_DEV_INSTRUMENTS
    namespace DebugDraw
    {
        namespace
        {
            // A circle of @p segments chords around @p center in the plane spanned by @p axisX and @p axisY.
            void Circle( DebugDrawWorld& world, const glm::vec3& center, const glm::vec3& axisX,
                         const glm::vec3& axisY, const float radius, const int segments,
                         const DebugDrawStyle& style )
            {
                const float step = glm::two_pi<float>() / static_cast<float>( segments );
                glm::vec3   prev = center + axisX * radius;
                for ( int i = 1; i <= segments; ++i )
                {
                    const float     a    = step * static_cast<float>( i );
                    const glm::vec3 next = center + ( axisX * std::cos( a ) + axisY * std::sin( a ) ) * radius;
                    world.AddLine( prev, next, style );
                    prev = next;
                }
            }

            // Half a circle from +axisX through +axisY to -axisX (a capsule's cap profile).
            void HalfCircle( DebugDrawWorld& world, const glm::vec3& center, const glm::vec3& axisX,
                             const glm::vec3& axisY, const float radius, const int segments,
                             const DebugDrawStyle& style )
            {
                const float step = glm::pi<float>() / static_cast<float>( segments );
                glm::vec3   prev = center + axisX * radius;
                for ( int i = 1; i <= segments; ++i )
                {
                    const float     a    = step * static_cast<float>( i );
                    const glm::vec3 next = center + ( axisX * std::cos( a ) + axisY * std::sin( a ) ) * radius;
                    world.AddLine( prev, next, style );
                    prev = next;
                }
            }

            // Any unit vector perpendicular to @p dir.
            glm::vec3 Perpendicular( const glm::vec3& dir )
            {
                const glm::vec3 helper =
                     std::abs( dir.y ) < 0.99f ? glm::vec3( 0.0f, 1.0f, 0.0f ) : glm::vec3( 1.0f, 0.0f, 0.0f );
                return glm::normalize( glm::cross( dir, helper ) );
            }
        } // namespace

        void Line( Scene& scene, const glm::vec3& start, const glm::vec3& end, const DebugDrawStyle& style )
        {
            scene.GetDebugDraw().AddLine( start, end, style );
        }

        void Point( Scene& scene, const glm::vec3& position, const float size, const DebugDrawStyle& style )
        {
            DebugDrawWorld& world = scene.GetDebugDraw();
            const float     h     = std::max( size, 0.0f ) * 0.5f;
            for ( int axis = 0; axis < 3; ++axis )
            {
                glm::vec3 d( 0.0f );
                d[axis] = h;
                world.AddLine( position - d, position + d, style );
            }
        }

        void Box( Scene& scene, const glm::vec3& center, const glm::vec3& extent, const glm::quat& rotation,
                  const DebugDrawStyle& style )
        {
            DebugDrawWorld& world = scene.GetDebugDraw();
            // Corner index bits = x | y << 1 | z << 2; the twelve edges join corners one bit apart.
            std::array<glm::vec3, 8> corners{};
            for ( int i = 0; i < 8; ++i )
            {
                const glm::vec3 local( ( i & 1 ) != 0 ? extent.x : -extent.x,
                                       ( i & 2 ) != 0 ? extent.y : -extent.y,
                                       ( i & 4 ) != 0 ? extent.z : -extent.z );
                corners[i] = center + rotation * local;
            }
            for ( int i = 0; i < 8; ++i )
                for ( int bit = 1; bit < 8; bit <<= 1 )
                    if ( ( i & bit ) == 0 )
                        world.AddLine( corners[i], corners[i | bit], style );
        }

        void Sphere( Scene& scene, const glm::vec3& center, const float radius, const int segments,
                     const DebugDrawStyle& style )
        {
            DebugDrawWorld& world = scene.GetDebugDraw();
            const int       sides = std::max( segments, 4 );
            const int       rings = std::max( sides / 2, 2 );
            // Parallels: rings - 1 circles between the poles, around Y.
            for ( int j = 1; j < rings; ++j )
            {
                const float theta = glm::pi<float>() * static_cast<float>( j ) / static_cast<float>( rings );
                Circle( world, center + glm::vec3( 0.0f, radius * std::cos( theta ), 0.0f ), glm::vec3( 1, 0, 0 ),
                        glm::vec3( 0, 0, 1 ), radius * std::sin( theta ), sides, style );
            }
            // Meridians: half of `sides` great circles through both poles.
            for ( int k = 0; k < rings; ++k )
            {
                const float     phi = glm::pi<float>() * static_cast<float>( k ) / static_cast<float>( rings );
                const glm::vec3 axis( std::cos( phi ), 0.0f, std::sin( phi ) );
                Circle( world, center, axis, glm::vec3( 0, 1, 0 ), radius, sides, style );
            }
        }

        void Capsule( Scene& scene, const glm::vec3& center, const float halfHeight, const float radius,
                      const glm::quat& rotation, const DebugDrawStyle& style )
        {
            DebugDrawWorld& world  = scene.GetDebugDraw();
            constexpr int   kSides = 16;
            const glm::vec3 x      = rotation * glm::vec3( 1, 0, 0 );
            const glm::vec3 y      = rotation * glm::vec3( 0, 1, 0 );
            const glm::vec3 z      = rotation * glm::vec3( 0, 0, 1 );
            // UE's HalfHeight runs to the tip; the cylinder part is what the caps leave.
            const float     cylinder = std::max( halfHeight - radius, 0.0f );
            const glm::vec3 top      = center + y * cylinder;
            const glm::vec3 bottom   = center - y * cylinder;
            Circle( world, top, x, z, radius, kSides, style );
            Circle( world, bottom, x, z, radius, kSides, style );
            HalfCircle( world, top, x, y, radius, kSides / 2, style );
            HalfCircle( world, top, z, y, radius, kSides / 2, style );
            HalfCircle( world, bottom, x, -y, radius, kSides / 2, style );
            HalfCircle( world, bottom, z, -y, radius, kSides / 2, style );
            for ( const glm::vec3& side : { x, -x, z, -z } )
                world.AddLine( top + side * radius, bottom + side * radius, style );
        }

        void Arrow( Scene& scene, const glm::vec3& start, const glm::vec3& end, const float headSize,
                    const DebugDrawStyle& style )
        {
            DebugDrawWorld& world = scene.GetDebugDraw();
            world.AddLine( start, end, style );
            const glm::vec3 shaft  = end - start;
            const float     length = glm::length( shaft );
            if ( length <= 0.0f || headSize <= 0.0f )
                return;
            // UE DrawDebugDirectionalArrow: four barbs back along the shaft, at 45 degrees.
            const glm::vec3 dir  = shaft / length;
            const glm::vec3 side = Perpendicular( dir );
            const glm::vec3 up   = glm::cross( dir, side );
            for ( const glm::vec3& barb : { side, -side, up, -up } )
                world.AddLine( end, end + ( -dir + barb ) * ( headSize * glm::one_over_root_two<float>() ),
                               style );
        }

        void String( Scene& scene, const glm::vec3& position, const std::string_view text, const float scale,
                     const DebugDrawStyle& style )
        {
            scene.GetDebugDraw().AddString( position, std::string( text ), scale, style );
        }

        void ScreenPanel( Scene& scene, const glm::vec2& origin, std::vector<DebugDrawCanvasLine> lines,
                          const DebugDrawPanelStyle& style )
        {
            scene.GetDebugDraw().AddScreenPanel( origin, std::move( lines ), style );
        }

        void FlushPersistent( Scene& scene )
        {
            scene.GetDebugDraw().FlushPersistentLines();
        }

        void FlushStrings( Scene& scene )
        {
            scene.GetDebugDraw().FlushStrings();
        }
    } // namespace DebugDraw
#endif // DESERT_DEV_INSTRUMENTS
} // namespace Desert::Core
