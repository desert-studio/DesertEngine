#include <Engine/UI/UIFocus.hpp>

#include <Engine/UI/UICanvasRenderer2D.hpp>

#include <cstddef>

namespace Desert::UI
{
    namespace
    {
        struct Box
        {
            float L, T, R, B;
        };

        Box BoxOf( const Rect& r )
        {
            return { r.X, r.Y, r.X + r.W, r.Y + r.H };
        }

        // FSlateRect::DoRectanglesIntersect: touching edges count.
        bool Intersect( const Box& a, const Box& b )
        {
            return !( a.R < b.L || a.L > b.R || a.B < b.T || a.T > b.B );
        }

        float DistSq( const Box& a, const Box& b )
        {
            const float dx = ( a.L + a.R ) * 0.5f - ( b.L + b.R ) * 0.5f;
            const float dy = ( a.T + a.B ) * 0.5f - ( b.T + b.B ) * 0.5f;
            return dx * dx + dy * dy;
        }

        // The per-direction functions of FHittestGrid::FindNextFocusableWidgetDefault (HittestGrid.cpp:575):
        // which side of the source the search starts from, which side of a candidate faces it, and the
        // "is further along the axis" comparison with Slate's 0.1 px tolerance.
        struct Axis
        {
            UINavigation Dir;
            bool         Horizontal() const
            {
                return Dir == UINavigation::Left || Dir == UINavigation::Right;
            }
            bool Forward() const
            {
                return Dir == UINavigation::Right || Dir == UINavigation::Down;
            }
            float Source( const Box& b ) const
            {
                switch ( Dir )
                {
                    case UINavigation::Left:
                        return b.L;
                    case UINavigation::Right:
                        return b.R;
                    case UINavigation::Up:
                        return b.T;
                    default:
                        return b.B;
                }
            }
            float Dest( const Box& b ) const
            {
                switch ( Dir )
                {
                    case UINavigation::Left:
                        return b.R;
                    case UINavigation::Right:
                        return b.L;
                    case UINavigation::Up:
                        return b.B;
                    default:
                        return b.T;
                }
            }
            bool Compare( float a, float b ) const
            {
                return Forward() ? a + 0.1f > b : a - 0.1f < b;
            }
        };

        // FHittestGrid::FindFocusableWidget (HittestGrid.cpp:366) over a flat list.
        NodeId FindSpatial( const std::vector<FocusEntry>& entries, NodeId from, const Box& src, const Axis& ax,
                            float sourceSide, const Box& boundary )
        {
            // The swept band: the source's extent across the axis (narrowed by half a pixel so a neighbour
            // that merely touches is not in it), the whole boundary along it.
            Box swept = src;
            if ( ax.Horizontal() )
            {
                swept.L = boundary.L;
                swept.R = boundary.R;
                swept.T += 0.5f;
                swept.B -= 0.5f;
            }
            else
            {
                swept.T = boundary.T;
                swept.B = boundary.B;
                swept.L += 0.5f;
                swept.R -= 0.5f;
            }

            const FocusEntry* best = nullptr;
            Box               bestBox{};
            for ( const FocusEntry& e : entries )
            {
                if ( e.Node == from )
                    continue;
                const Box test = BoxOf( e.Bounds );
                if ( !( ax.Compare( ax.Dest( test ), sourceSide ) && Intersect( swept, test ) ) )
                    continue;
                if ( best != nullptr )
                {
                    const float bestDest = ax.Dest( bestBox );
                    const float testDest = ax.Dest( test );
                    if ( !ax.Compare( bestDest, testDest ) ) // further along the axis: keep the best
                        continue;
                    if ( ax.Compare( testDest, bestDest ) && DistSq( src, test ) >= DistSq( src, bestBox ) )
                        continue; // equidistant on the axis: the nearer centre wins
                }
                best    = &e;
                bestBox = test;
            }
            return best != nullptr ? best->Node : NodeId::Null;
        }
    } // namespace

    UINavigation NavigationOf( const UIInput& input )
    {
        UINavigation nav = UINavigation::None;
        for ( const UIKeyEvent& k : input.Keys )
        {
            switch ( k.Key )
            {
                case Common::KeyCode::Tab:
                    nav = HasMod( k.Mods, UIKeyMods::Shift ) ? UINavigation::Previous : UINavigation::Next;
                    break;
                case Common::KeyCode::Left:
                    nav = UINavigation::Left;
                    break;
                case Common::KeyCode::Right:
                    nav = UINavigation::Right;
                    break;
                case Common::KeyCode::Up:
                case Common::KeyCode::W:
                    nav = UINavigation::Up;
                    break;
                case Common::KeyCode::Down:
                case Common::KeyCode::S:
                    nav = UINavigation::Down;
                    break;
                default:
                    break;
            }
        }
        return nav;
    }

    NodeId FindNextFocusable( const std::vector<FocusEntry>& entries, NodeId from, UINavigation dir,
                              const Rect& boundary, UINavigationRule rule )
    {
        if ( entries.empty() || dir == UINavigation::None )
            return NodeId::Null;

        std::size_t at = entries.size();
        for ( std::size_t i = 0; i < entries.size(); ++i )
            if ( entries[i].Node == from )
            {
                at = i;
                break;
            }
        if ( at == entries.size() )
            return entries.front().Node;

        const std::size_t n = entries.size();
        if ( dir == UINavigation::Next )
            return entries[( at + 1 ) % n].Node;
        if ( dir == UINavigation::Previous )
            return entries[( at + n - 1 ) % n].Node;

        const Axis   ax{ dir };
        const Box    src   = BoxOf( entries[at].Bounds );
        const Box    bound = BoxOf( boundary );
        const NodeId hit   = FindSpatial( entries, from, src, ax, ax.Source( src ), bound );
        if ( hit != NodeId::Null || rule != UINavigationRule::Wrap )
            return hit;

        // EUINavigationRule::Wrap: restart from the boundary's far side (HittestGrid.cpp:516), so the
        // candidate nearest the opposite edge of the same band is taken.
        const Axis opposite{ dir == UINavigation::Left    ? UINavigation::Right
                             : dir == UINavigation::Right ? UINavigation::Left
                             : dir == UINavigation::Up    ? UINavigation::Down
                                                          : UINavigation::Up };
        return FindSpatial( entries, NodeId::Null, src, ax, opposite.Source( bound ), bound );
    }
} // namespace Desert::UI
