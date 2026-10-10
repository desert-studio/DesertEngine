#include <UI/UIFocus.hpp>

#include <UI/UICanvasRenderer2D.hpp>

#include <algorithm>
#include <cstddef>
#include <string_view>

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

    UINavigation NavigationOf( const UIInput& input, bool textEntry, std::span<const Common::KeyCode> consumed )
    {
        UINavigation nav = UINavigation::None;
        for ( const UIKeyEvent& k : input.Keys )
        {
            if ( std::find( consumed.begin(), consumed.end(), k.Key ) != consumed.end() )
                continue;
            if ( textEntry && ( k.Key == Common::KeyCode::W || k.Key == Common::KeyCode::S ) )
                continue; // typed into the field, not a step
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

    namespace
    {
        const FocusEntry* EntryOf( const std::vector<FocusEntry>& entries, NodeId n )
        {
            for ( const FocusEntry& e : entries )
                if ( e.Node == n )
                    return &e;
            return nullptr;
        }

        struct DirectionRule
        {
            UINavigationRule Rule = UINavigationRule::Escape;
            std::string_view Target;
        };

        DirectionRule RuleOf( const UINavigationData& d, UINavigation dir )
        {
            switch ( dir )
            {
                case UINavigation::Up:
                    return { d.Up, d.UpTarget };
                case UINavigation::Down:
                    return { d.Down, d.DownTarget };
                case UINavigation::Left:
                    return { d.Left, d.LeftTarget };
                case UINavigation::Right:
                    return { d.Right, d.RightTarget };
                default:
                    return {};
            }
        }
    } // namespace

    NodeId ResolveNavigation( const IUITree& tree, const std::vector<FocusEntry>& entries,
                              const std::vector<NavigationBox>& boxes, NodeId from, UINavigation dir,
                              const Rect& viewport )
    {
        const bool spatial = dir == UINavigation::Left || dir == UINavigation::Right || dir == UINavigation::Up ||
                             dir == UINavigation::Down;
        if ( spatial && EntryOf( entries, from ) != nullptr )
        {
            for ( NodeId n = from; n != NodeId::Null; n = tree.Parent( n ) )
            {
                const UINavigationData* d = tree.Get<UINavigationData>( n );
                if ( d == nullptr )
                    continue;
                const DirectionRule r = RuleOf( *d, dir );
                if ( r.Rule == UINavigationRule::Escape )
                    continue;
                if ( r.Rule == UINavigationRule::Explicit )
                {
                    if ( r.Target.empty() )
                        return NodeId::Null; // an Explicit rule naming nothing goes nowhere
                    for ( const FocusEntry& e : entries )
                        if ( e.Node != from && tree.Name( e.Node ) == r.Target )
                            return e.Node;
                    return NodeId::Null;
                }
                // Stop / Wrap inside this element's box; an element the walk did not record a box for this
                // frame (not drawn) cannot bound anything, so the view does.
                const FocusEntry* box = EntryOf( boxes, n );
                return FindNextFocusable( entries, from, dir, box != nullptr ? box->Bounds : viewport, r.Rule );
            }
        }
        return FindNextFocusable( entries, from, dir, viewport, UINavigationRule::Escape );
    }

    void ScrollIntoView( IUITree& tree, const std::vector<FocusEntry>& entries,
                         const std::vector<ScrollPort>& ports, NodeId node )
    {
        const FocusEntry* entry = EntryOf( entries, node );
        if ( entry == nullptr )
            return;
        float top    = entry->Bounds.Y;
        float bottom = entry->Bounds.Y + entry->Bounds.H;
        for ( NodeId a = tree.Parent( node ); a != NodeId::Null; a = tree.Parent( a ) )
        {
            const auto port =
                 std::find_if( ports.begin(), ports.end(), [a]( const ScrollPort& p ) { return p.Node == a; } );
            if ( port == ports.end() || port->PxPerDesign <= 0.0f )
                continue;
            const float portTop    = port->Screen.Y;
            const float portBottom = port->Screen.Y + port->Screen.H;
            float       deltaPx    = 0.0f;
            if ( top < portTop )
                deltaPx = top - portTop;
            else if ( bottom > portBottom )
                deltaPx = std::min( bottom - portBottom, top - portTop ); // taller than the port: its top wins
            if ( deltaPx == 0.0f )
                continue;
            if ( UIScrollViewData* sv = tree.GetState<UIScrollViewData>( a ) )
                sv->ScrollY += deltaPx / port->PxPerDesign;
            else if ( UIListViewData* lv = tree.GetState<UIListViewData>( a ) )
                lv->ScrollY += deltaPx / port->PxPerDesign;
            // The control moves with the content it sits in, so an outer port sees where it will be.
            top -= deltaPx;
            bottom -= deltaPx;
        }
    }

    NodeId FocusScopeOf( const IUITree& tree, NodeId n )
    {
        NodeId root = n;
        for ( NodeId a = n; a != NodeId::Null; a = tree.Parent( a ) )
        {
            if ( tree.Has<UIScreenData>( a ) || tree.Has<UIOverlayData>( a ) )
                return a;
            root = a;
        }
        return root;
    }

    NodeId UpdateFocusScopes( const IUITree& tree, const std::vector<FocusEntry>& entries, NodeId focused,
                              FocusMemory& memory )
    {
        std::vector<NodeId> scopes;
        std::vector<NodeId> scopeOfEntry;
        scopeOfEntry.reserve( entries.size() );
        for ( const FocusEntry& e : entries )
        {
            const NodeId s = FocusScopeOf( tree, e.Node );
            scopeOfEntry.push_back( s );
            if ( std::find( scopes.begin(), scopes.end(), s ) == scopes.end() )
                scopes.push_back( s );
        }

        // What a scope hands focus to: the control it remembers, else its InitialFocus control.
        const auto pick = [&]( NodeId scope ) -> NodeId
        {
            if ( const auto it = memory.Restore.find( scope ); it != memory.Restore.end() )
                if ( EntryOf( entries, it->second ) != nullptr )
                    return it->second;
            for ( std::size_t i = 0; i < entries.size(); ++i )
                if ( scopeOfEntry[i] == scope )
                    if ( const UINavigationData* d = tree.Get<UINavigationData>( entries[i].Node );
                         d != nullptr && d->InitialFocus )
                        return entries[i].Node;
            return NodeId::Null;
        };

        std::size_t focusedScope = scopes.size(); // "not present"
        for ( std::size_t i = 0; i < entries.size(); ++i )
            if ( entries[i].Node == focused )
                focusedScope = static_cast<std::size_t>(
                     std::find( scopes.begin(), scopes.end(), scopeOfEntry[i] ) - scopes.begin() );
        const bool present = focusedScope != scopes.size();

        NodeId result = focused;
        bool   moved  = false;
        // A scope that appeared this frame, above the focused control's, is activated: topmost first.
        for ( std::size_t i = scopes.size(); i-- > 0; )
        {
            if ( present && i <= focusedScope )
                break;
            if ( std::find( memory.Scopes.begin(), memory.Scopes.end(), scopes[i] ) != memory.Scopes.end() )
                continue;
            if ( const NodeId t = pick( scopes[i] ); t != NodeId::Null )
            {
                result = t;
                moved  = true;
                break;
            }
        }
        // The focused control is gone: the topmost scope that has something to give takes over.
        if ( !moved && !present && focused != NodeId::Null )
            for ( std::size_t i = scopes.size(); i-- > 0; )
                if ( const NodeId t = pick( scopes[i] ); t != NodeId::Null )
                {
                    result = t;
                    break;
                }

        for ( std::size_t i = 0; i < entries.size(); ++i )
            if ( entries[i].Node == result )
                memory.Restore[scopeOfEntry[i]] = result;
        std::erase_if( memory.Restore, [&tree]( const auto& kv ) { return !tree.Valid( kv.first ); } );
        memory.Scopes = std::move( scopes );
        return result;
    }
} // namespace Desert::UI
