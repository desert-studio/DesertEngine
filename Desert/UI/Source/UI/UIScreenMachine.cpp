#include <UI/UIScreenMachine.hpp>

#include <UI/UICanvasContext.hpp>
#include <UI/UICanvasLayout.hpp>

#include <algorithm>
#include <cstddef>

namespace Desert::UI
{
    namespace
    {
        // Is @p ancestor @p e itself or above it? Bounded: a Parent cycle is authorable (see IUITree).
        bool IsSelfOrAncestor( const IUITree& tree, NodeId ancestor, NodeId e )
        {
            const std::size_t limit = tree.NodeBound() + 1;
            std::size_t       steps = 0;
            for ( NodeId cur = e; cur != NodeId::Null && tree.Valid( cur ) && steps < limit; ++steps )
            {
                if ( cur == ancestor )
                    return true;
                cur = tree.Parent( cur );
            }
            return false;
        }

        // How many ancestors @p e has — the "deepest" in BackScreenTarget.
        std::size_t DepthOf( const IUITree& tree, NodeId e )
        {
            const std::size_t limit = tree.NodeBound() + 1;
            std::size_t       depth = 0;
            for ( NodeId cur = tree.Parent( e ); cur != NodeId::Null && tree.Valid( cur ) && depth < limit;
                  cur        = tree.Parent( cur ) )
                ++depth;
            return depth;
        }

        // The first named screen @p owner owns, in DRAW order: a sub-tree walk that does not descend into a
        // nested stack (its screens are that stack's). Runs only on a (re-)seed, never per frame.
        bool FirstOwnedScreen( const IUITree& tree, NodeId owner, NodeId e, std::string& out )
        {
            for ( NodeId c : ChildrenOf( tree, e ) )
            {
                if ( !tree.Valid( c ) )
                    continue;
                if ( tree.Has<UIScreenData>( c ) && !tree.Get<UIScreenData>( c )->Name.empty() )
                {
                    out = tree.Get<UIScreenData>( c )->Name;
                    return true;
                }
                if ( tree.Has<UIScreenStackData>( c ) || tree.Has<UICanvasData>( c ) )
                    continue; // another machine's screens
                if ( FirstOwnedScreen( tree, owner, c, out ) )
                    return true;
            }
            return false;
        }

        // Every screen above @p owner (and @p owner itself, when it is a screen) is current: the machine is
        // on screen, so a Back aimed at it is visible.
        bool IsActive( const UICanvasContext& cell, const IUITree& tree, NodeId owner )
        {
            const std::size_t limit = tree.NodeBound() + 1;
            std::size_t       steps = 0;
            for ( NodeId cur = owner; cur != NodeId::Null && tree.Valid( cur ) && steps < limit; ++steps )
            {
                if ( tree.Has<UIScreenData>( cur ) && !ScreenPresenceOf( cell, tree, cur ).Current )
                    return false;
                if ( tree.Has<UICanvasData>( cur ) )
                    return true;
                cur = tree.Parent( cur );
            }
            return true;
        }

        // The deepest active machine of @p cell that has somewhere to go back to.
        NodeId DeepestBackTarget( const UICanvasContext& cell, const IUITree& tree )
        {
            NodeId      best      = NodeId::Null;
            std::size_t bestDepth = 0;
            for ( const auto& [owner, m] : cell.Screens )
            {
                if ( m.History.empty() || !tree.Valid( owner ) || !IsActive( cell, tree, owner ) )
                    continue;
                const std::size_t d = DepthOf( tree, owner );
                if ( best == NodeId::Null || d > bestDepth )
                {
                    best      = owner;
                    bestDepth = d;
                }
            }
            return best;
        }

        void GoBack( UIScreenMachine& m )
        {
            if ( m.History.empty() ) // at the bottom of the history Back is simply ignored
                return;
            m.From   = m.Screen;
            m.Screen = m.History.back();
            m.History.pop_back();
            m.T    = 0.0f;
            m.Back = true;
        }
    } // namespace

    NodeId ScreenOwnerOf( const IUITree& tree, NodeId screen )
    {
        if ( !tree.Valid( screen ) )
            return NodeId::Null;
        const std::size_t limit = tree.NodeBound() + 1;
        std::size_t       steps = 0;
        for ( NodeId cur = tree.Parent( screen ); cur != NodeId::Null && tree.Valid( cur ) && steps < limit;
              ++steps, cur = tree.Parent( cur ) )
        {
            if ( tree.Has<UIScreenStackData>( cur ) || tree.Has<UICanvasData>( cur ) )
                return cur;
        }
        return NodeId::Null;
    }

    UIScreenPresence ScreenPresenceOf( const UICanvasContext& cell, const IUITree& tree, NodeId screen )
    {
        UIScreenPresence p;
        if ( !tree.Has<UIScreenData>( screen ) )
            return p;
        const auto it = cell.Screens.find( ScreenOwnerOf( tree, screen ) );
        if ( it == cell.Screens.end() )
            return p;
        const std::string& name = tree.Get<UIScreenData>( screen )->Name;
        p.Machine               = &it->second;
        p.Current               = !name.empty() && name == it->second.Screen;
        p.Leaving               = !name.empty() && name == it->second.From && it->second.T < 1.0f;
        return p;
    }

    void UpdateScreenMachines( UICanvasContext& cell, const IUITree& tree, NodeId canvas, float dt )
    {
        // THE INDEX IS ASKED OF THE SCREENS, NOT OF THE TREE: a handful of UIScreen nodes, each walking UP
        // to its owner, which is the depth of the tree and not its size. A per-frame sub-tree walk of the
        // canvas cost 8.6 ms of a Debug frame on a canvas holding a 20 000-row list.
        cell.ScreenIndex.clear();
        for ( const NodeId s : RootsOf( tree, ArgKind::Screen ) )
        {
            const std::string& n = tree.Get<UIScreenData>( s )->Name;
            if ( n.empty() || !IsSelfOrAncestor( tree, canvas, s ) )
                continue;
            const NodeId owner = ScreenOwnerOf( tree, s );
            if ( owner == NodeId::Null )
                continue;
            cell.ScreenIndex.push_back( { s, owner, n } );
        }

        // Machines that own nothing any more (a stack removed, a scene swapped) go with their owner.
        for ( auto it = cell.Screens.begin(); it != cell.Screens.end(); )
        {
            const NodeId owner = it->first;
            const bool   used  = std::any_of( cell.ScreenIndex.begin(), cell.ScreenIndex.end(),
                                              [owner]( const UIScreenEntry& x ) { return x.Owner == owner; } );
            it                 = used ? std::next( it ) : cell.Screens.erase( it );
        }

        for ( const UIScreenEntry& entry : cell.ScreenIndex )
        {
            const NodeId     owner = entry.Owner;
            UIScreenMachine& m     = cell.Screens[owner];
            if ( m.Screen.empty() && m.T >= 1.0f && m.From.empty() && tree.Has<UIScreenStackData>( owner ) )
                m.Screen = tree.Get<UIScreenStackData>( owner )->InitialScreen;
            if ( tree.Has<UIScreenStackData>( owner ) )
            {
                const auto& st = *tree.Get<UIScreenStackData>( owner );
                m.Time         = st.TransitionTime;
                m.SlidePx      = st.SlidePx;
                m.Easing       = st.Easing;
            }
        }

        for ( auto& [owner, m] : cell.Screens )
        {
            // Re-seed when the current name is not one of THIS machine's screens — otherwise a name left over
            // from another scene (or a mistyped Initial Screen) would hide every screen it owns.
            const bool exists =
                 std::any_of( cell.ScreenIndex.begin(), cell.ScreenIndex.end(),
                              [&]( const UIScreenEntry& x ) { return x.Owner == owner && x.Name == m.Screen; } );
            if ( !exists )
            {
                std::string first;
                if ( FirstOwnedScreen( tree, owner, owner, first ) )
                {
                    m.Screen = first;
                    m.From.clear();
                    m.History.clear();
                    m.T = 1.0f;
                }
            }
            if ( m.T < 1.0f )
            {
                m.T = m.Time > 0.0f ? std::min( 1.0f, m.T + dt / m.Time ) : 1.0f;
                if ( m.T >= 1.0f )
                    m.From.clear(); // hand-over finished; the outgoing screen stops drawing
            }
        }
    }

    NodeId ShowScreenTarget( const UICanvasContext& cell, const IUITree& tree, NodeId from,
                             const std::string& name )
    {
        if ( name.empty() )
            return NodeId::Null;
        const std::size_t limit = tree.NodeBound() + 1;
        std::size_t       steps = 0;
        for ( NodeId a = from; a != NodeId::Null && tree.Valid( a ) && steps < limit;
              ++steps, a = tree.Parent( a ) )
        {
            for ( const UIScreenEntry& x : cell.ScreenIndex )
                if ( x.Name == name && IsSelfOrAncestor( tree, a, x.Owner ) )
                    return x.Owner;
        }
        return NodeId::Null;
    }

    NodeId TabsOf( const IUITree& tree, NodeId button )
    {
        const std::size_t limit = tree.NodeBound() + 1;
        std::size_t       steps = 0;
        for ( NodeId a = button; a != NodeId::Null && tree.Valid( a ) && steps < limit;
              ++steps, a = tree.Parent( a ) )
        {
            if ( tree.Has<UITabsData>( a ) )
                return a;
            if ( tree.Has<UICanvasData>( a ) )
                break;
        }
        return NodeId::Null;
    }

    NodeId BackScreenTarget( const UICanvasContext& cell, const IUITree& tree, NodeId from )
    {
        const std::size_t limit = tree.NodeBound() + 1;
        std::size_t       steps = 0;
        for ( NodeId a = from; a != NodeId::Null && tree.Valid( a ) && steps < limit;
              ++steps, a = tree.Parent( a ) )
        {
            const auto it = cell.Screens.find( a );
            if ( it != cell.Screens.end() && !it->second.History.empty() )
                return a;
        }
        return DeepestBackTarget( cell, tree );
    }

    bool BackScreenOnEscape( UIViewContext& view, const IUITree& tree )
    {
        const std::vector<NodeId> canvases = CanvasesInDrawOrder( tree );
        for ( auto it = canvases.rbegin(); it != canvases.rend(); ++it ) // topmost canvas first
        {
            if ( view.FindCanvasState( *it ) == nullptr )
                continue; // not drawn by this view
            UICanvasContext& cell   = view.CanvasState( *it );
            const NodeId     target = DeepestBackTarget( cell, tree );
            if ( target == NodeId::Null )
                continue;
            GoBack( cell.Screens[target] );
            return true;
        }
        return false;
    }

    void ApplyScreenRequests( UICanvasContext& cell )
    {
        for ( const UIScreenRequest& r : cell.ScreenRequests )
        {
            const auto it = cell.Screens.find( r.Owner );
            if ( it == cell.Screens.end() )
                continue;
            UIScreenMachine& m = it->second;
            if ( r.Back )
            {
                GoBack( m );
            }
            else if ( r.Name != m.Screen )
            {
                if ( r.PushHistory )
                    m.History.push_back( m.Screen );
                m.From   = m.Screen;
                m.Screen = r.Name;
                m.T      = 0.0f;
                m.Back   = false;
            }
        }
        cell.ScreenRequests.clear();
    }
} // namespace Desert::UI
