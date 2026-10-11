#include "UIOverlay.hpp"

#include <Engine/UI/UICanvasLayout.hpp>

#include <Common/Core/Logger.hpp>

#include <algorithm>
#include <cstdint>
#include <utility>

namespace Desert::UI
{
    namespace
    {
        // Is @p maybeAncestor @p e itself, or one of its ancestors? The relationship stores only the parent
        // link, so this is the one direction a tree can be walked, and the loop is bounded by the number of
        // entities so a corrupted parent cycle cannot hang the frame.
        bool IsSelfOrAncestor( IUITree& tree, NodeId maybeAncestor, NodeId e )
        {
            if ( maybeAncestor == NodeId::Null )
                return false;
            std::size_t guard = 0;
            for ( NodeId t = e; t != NodeId::Null && tree.Valid( t ) && guard++ <= tree.NodeBound(); )
            {
                if ( t == maybeAncestor )
                    return true;
                t = tree.Parent( t );
            }
            return false;
        }

        // The nearest trigger at or above @p e that answers to @p on. Filtered by the EVENT and not merely
        // by the component, because a button may carry a right-click trigger while the panel above it
        // carries a hover one, and the two are different questions asked of the same chain.
        NodeId TriggerAtOrAbove( IUITree& tree, NodeId e, UIOverlayTriggerEvent on )
        {
            std::size_t guard = 0;
            for ( NodeId t = e; t != NodeId::Null && tree.Valid( t ) && guard++ <= tree.NodeBound(); )
            {
                if ( tree.Has<UIOverlayTriggerData>( t ) && tree.Get<UIOverlayTriggerData>( t )->On == on &&
                     !tree.Get<UIOverlayTriggerData>( t )->Overlay.empty() )
                    return t;
                t = tree.Parent( t );
            }
            return NodeId::Null;
        }

        // What opened this overlay, as an event. entt::null (opened by name, not by a trigger) answers
        // LeftClick, which is the conservative reading: it is not a hover overlay, so it does not close
        // itself when the pointer wanders off.
        UIOverlayTriggerEvent OpenerEvent( IUITree& tree, NodeId opener )
        {
            return ( opener != NodeId::Null && tree.Valid( opener ) && tree.Has<UIOverlayTriggerData>( opener ) )
                        ? tree.Get<UIOverlayTriggerData>( opener )->On
                        : UIOverlayTriggerEvent::LeftClick;
        }

        // The axis-aligned screen box of one element, resolved through the SAME walk the renderer uses and
        // with this view's own context — so an item of an already-placed submenu reports where it really
        // is and not where it was authored. Returns false when the element is not drawn at all, which is a
        // meaningful answer: there is nothing to place a submenu beside.
        bool ElementScreenBox( IUITree& tree, UIViewContext& view, NodeId e, Rect& out )
        {
            const NodeId canvas = CanvasOf( tree, e );
            if ( canvas == NodeId::Null )
                return false;
            std::vector<UIElementNode> nodes;
            if ( const auto walked =
                      EnumerateCanvas( tree, canvas, view.ViewportPx, nodes, &view.CanvasState( canvas ) );
                 !walked )
                return false;
            for ( const UIElementNode& n : nodes )
                if ( n.Entity == e && n.Drawn && n.OwnRect )
                {
                    out = n.ScreenPx;
                    return true;
                }
            return false;
        }

        // The box the overlay's own content occupies with NO placement applied — the thing PlaceOverlay
        // then moves. Measured through the real walk rather than from the canvas rect, because a canvas
        // rect is the whole viewport under Stretch and placing that would move nothing.
        //
        // It is measured on a COPY of the cell with the placement removed and the overlay forced open:
        // measuring through the live cell would fold last frame's shift into this frame's, and measuring a
        // closed overlay would return nothing on the very frame it opens, which is the frame that needs it.
        bool OverlayContentBox( IUITree& tree, UIViewContext& view, NodeId canvas, Rect& out )
        {
            UICanvasContext probe = view.CanvasState( canvas );
            probe.OverlayOpen     = true;
            probe.OverlayShift    = glm::vec2( 0.0f );

            std::vector<UIElementNode> nodes;
            if ( const auto walked = EnumerateCanvas( tree, canvas, view.ViewportPx, nodes, &probe ); !walked )
                return false;

            bool  any  = false;
            float minX = 0.0f;
            float minY = 0.0f;
            float maxX = 0.0f;
            float maxY = 0.0f;
            for ( const UIElementNode& n : nodes )
            {
                if ( !n.Drawn || !n.OwnRect )
                    continue;
                const float l = n.ScreenPx.X;
                const float t = n.ScreenPx.Y;
                const float r = n.ScreenPx.X + n.ScreenPx.W;
                const float b = n.ScreenPx.Y + n.ScreenPx.H;
                minX          = any ? std::min( minX, l ) : l;
                minY          = any ? std::min( minY, t ) : t;
                maxX          = any ? std::max( maxX, r ) : r;
                maxY          = any ? std::max( maxY, b ) : b;
                any           = true;
            }
            if ( !any )
                return false;
            out = Rect{ minX, minY, maxX - minX, maxY - minY };
            return true;
        }

        // Where this overlay's canvas root has to move so its content lands beside what opened it, without
        // leaving the view. Modal and Toast have no origin and are placed by their own anchors — see
        // UILayout.hpp for why fabricating one for them would be a mechanism that moves nothing.
        void PlaceOverlayCanvas( IUITree& tree, UIViewContext& view, NodeId canvas, const UIInput& input )
        {
            UICanvasContext&          cell = view.CanvasState( canvas );
            const UIOverlayData*      d    = OverlayDataOf( tree, canvas );
            if ( d == nullptr || d->Kind == UIOverlayKind::Modal || d->Kind == UIOverlayKind::Toast )
            {
                cell.OverlayShift = glm::vec2( 0.0f );
                return;
            }

            Rect content;
            if ( !OverlayContentBox( tree, view, canvas, content ) )
            {
                cell.OverlayShift = glm::vec2( 0.0f );
                return;
            }

            // A pointer origin is a zero-size rect at the cursor; an element origin is the element's own
            // box. One parameter, because "flip about the thing I am attached to" is one sentence.
            const bool byHover      = OpenerEvent( tree, cell.OverlayOpenedBy ) == UIOverlayTriggerEvent::Hover;
            const bool pinToElement = ( d->Kind == UIOverlayKind::Tooltip ) ? !d->FollowPointer : byHover;

            Rect        origin{ input.MousePx.x, input.MousePx.y, 0.0f, 0.0f };
            OverlayAxis axis = OverlayAxis::Vertical;
            if ( pinToElement )
            {
                Rect box;
                if ( ElementScreenBox( tree, view, cell.OverlayOpenedBy, box ) )
                {
                    origin = box;
                    // A submenu gets clear of its parent item SIDEWAYS; a pinned tooltip still drops below
                    // the element it describes, because covering that element is the one thing it must not
                    // do and stepping aside is not enough to avoid it.
                    if ( d->Kind == UIOverlayKind::ContextMenu )
                        axis = OverlayAxis::Horizontal;
                }
            }

            const auto  scale = CanvasScale( tree, canvas, view.ViewportPx );
            const float k     = scale ? scale.GetValue() : 1.0f;
            const Rect  placed =
                 PlaceOverlay( { content.W, content.H }, origin, d->Gap * k, view.ViewportPx, axis );
            cell.OverlayShift = glm::vec2( placed.X - content.X, placed.Y - content.Y );
        }

        // Pop stack entries from the top down to and including @p index, clearing each one's open state.
        //
        // @p dismissed marks this as a DELIBERATE dismissal (Escape, a press outside) rather than the
        // pointer simply wandering off. The difference matters for a hover-opened submenu: dismissing one
        // while the pointer is still on the item that opens it has to stick, or the hover clock reopens it
        // on the next frame and Escape appears to do nothing.
        void CloseStackDownTo( UIViewContext& view, IUITree& tree, std::size_t index, bool dismissed = false )
        {
            while ( view.OverlayStack.size() > index )
            {
                const NodeId c = view.OverlayStack.back();
                view.OverlayStack.pop_back();
                if ( tree.Valid( c ) )
                {
                    if ( dismissed && OpenerEvent( tree, view.CanvasState( c ).OverlayOpenedBy ) ==
                                           UIOverlayTriggerEvent::Hover )
                        view.OverlayHoverSuppressed = view.CanvasState( c ).OverlayOpenedBy;
                    UICanvasContext& cell = view.CanvasState( c );
                    cell.OverlayOpen      = false;
                    cell.OverlayOpenedBy  = NodeId::Null;
                    cell.OverlayShift     = glm::vec2( 0.0f );
                    cell.Locals.Erase( kOverlayTextKey );
                }
            }
        }

        void CloseTooltip( UIViewContext& view, IUITree& tree )
        {
            if ( view.OverlayTooltip == NodeId::Null )
                return;
            if ( tree.Valid( view.OverlayTooltip ) )
            {
                UICanvasContext& cell = view.CanvasState( view.OverlayTooltip );
                cell.OverlayOpen      = false;
                cell.OverlayOpenedBy  = NodeId::Null;
                cell.OverlayShift     = glm::vec2( 0.0f );
                cell.Locals.Erase( kOverlayTextKey );
            }
            view.OverlayTooltip = NodeId::Null;
        }

        // Index of the open stack entry whose OWN CONTENT the pointer is on, or -1.
        //
        // `hot == stack[i]` is the modal's scrim — the canvas entity elects itself for every point its
        // dialog does not cover — and that is OUTSIDE, not inside. Reading it as inside is what would turn
        // "click the dim to dismiss" into "the dim can never be clicked".
        int StackEntryUnderPointer( IUITree& tree, const UIViewContext& view, NodeId hot )
        {
            if ( hot == NodeId::Null || !tree.Valid( hot ) )
                return -1;
            const NodeId       hotCanvas = CanvasOf( tree, hot );
            int                found     = -1;
            for ( std::size_t i = 0; i < view.OverlayStack.size(); ++i )
                if ( hotCanvas == view.OverlayStack[i] && hot != view.OverlayStack[i] )
                    found = static_cast<int>( i );
            return found;
        }

        void OpenOverlay( IUITree& tree, UIViewContext& view, NodeId canvas, NodeId trigger,
                          const std::string& text, const UIInput& input )
        {
            const UIOverlayData* d = OverlayDataOf( tree, canvas );
            if ( d == nullptr )
                return;

            UICanvasContext& cell = view.CanvasState( canvas );
            cell.OverlayOpenedBy  = trigger;
            cell.OverlayOpen      = true;
            if ( text.empty() )
                cell.Locals.Erase( kOverlayTextKey );
            else
                cell.Locals.Set( kOverlayTextKey, text );

            if ( d->Kind == UIOverlayKind::Tooltip )
            {
                if ( view.OverlayTooltip != canvas )
                    CloseTooltip( view, tree );
                view.OverlayTooltip = canvas;
            }
            else
            {
                // A menu opened from inside another menu STACKS on it; one opened from anywhere else
                // replaces whatever was open. That single rule is all nesting needs: a submenu is a
                // ContextMenu overlay whose trigger happens to live in the menu below it.
                const NodeId triggerCanvas = trigger != NodeId::Null ? CanvasOf( tree, trigger ) : NodeId::Null;
                int                owner         = -1;
                for ( std::size_t i = 0; i < view.OverlayStack.size(); ++i )
                    if ( view.OverlayStack[i] == triggerCanvas )
                        owner = static_cast<int>( i );
                // "everything above the entry that owns the trigger", and -1 (it owns none) means all of
                // it. Spelled out rather than as owner+1 on an int, because the -1 then reaches an unsigned
                // conversion and only arrives at 0 by wrapping.
                const std::size_t keepBelow = owner < 0 ? 0U : static_cast<std::size_t>( owner ) + 1U;
                CloseStackDownTo( view, tree, keepBelow );

                cell.OverlayOpen     = true; // CloseStackDownTo may have cleared it if it was already up
                cell.OverlayOpenedBy = trigger;
                if ( !text.empty() )
                    cell.Locals.Set( kOverlayTextKey, text );
                view.OverlayStack.push_back( canvas );
            }

            PlaceOverlayCanvas( tree, view, canvas, input );
        }

        void RaiseToastOn( IUITree& tree, UIViewContext& view, NodeId canvas, std::string text )
        {
            const UIOverlayData* d = OverlayDataOf( tree, canvas );
            if ( d == nullptr )
                return;
            UICanvasContext& cell  = view.CanvasState( canvas );
            const int        slots = std::clamp( d->ToastSlots, 1, 8 );

            // THE QUEUE IS BOUNDED, AND WHAT IT DROPS IS THE OLDEST WAITING LINE. Three policies were on
            // the table. Growing without limit is a leak whose symptom is a notification appearing minutes
            // after the event it describes. Refusing the newest keeps the stalest, which is backwards for
            // something whose whole value is recency. Evicting a VISIBLE toast to make room makes a burst
            // unreadable — every line flashes past. So the visible stack is held for its lifetime, the
            // waiting room is four deep per slot, and when it is full the line that has waited longest is
            // the one nobody will read anyway.
            const std::size_t cap = static_cast<std::size_t>( slots ) * 4u;
            if ( cell.OverlayToastPending.size() >= cap )
            {
                cell.OverlayToastPending.pop_front();
                if ( !cell.OverlayToastOverflowReported )
                {
                    cell.OverlayToastOverflowReported = true;
                    LOG_WARN( "[UI] overlay '{}' has {} notifications waiting behind {} slots; the oldest "
                              "waiting line is being dropped. Further drops in this burst are not repeated.",
                              d->Name, cap, slots );
                }
            }
            cell.OverlayToastPending.push_back( std::move( text ) );
        }

        void UpdateToasts( IUITree& tree, UIViewContext& view )
        {
            // Notifications raised from outside the UI reach exactly one view — the one that owns the
            // scene's shared clocks. See UIOverlay.hpp.
            if ( view.DrivesSceneAnimation )
            {
                for ( UIOverlayRequests::Request& r : UIOverlayRequests::Get().Drain() )
                {
                    const auto canvas = OverlayByName( tree, r.Overlay );
                    if ( !canvas )
                    {
                        LOG_ERROR( "[UI] a notification was raised for overlay '{}': {}", r.Overlay,
                                   canvas.GetError() );
                        continue;
                    }
                    const UIOverlayData* d = OverlayDataOf( tree, canvas.GetValue() );
                    if ( d == nullptr || d->Kind != UIOverlayKind::Toast )
                    {
                        LOG_ERROR( "[UI] a notification was raised for overlay '{}', which is not a Toast; "
                                   "notifications are queued only by Toast overlays",
                                   r.Overlay );
                        continue;
                    }
                    RaiseToastOn( tree, view, canvas.GetValue(), std::move( r.Text ) );
                }
            }

            for ( const NodeId canvas : RootsOf( tree, ArgKind::Overlay ) )
            {
                const auto& d = *tree.Get<UIOverlayData>( canvas );
                if ( d.Kind != UIOverlayKind::Toast )
                    continue;
                UICanvasContext& cell  = view.CanvasState( canvas );
                const int        slots = std::clamp( d.ToastSlots, 1, 8 );

                for ( UICanvasContext::ToastItem& t : cell.OverlayToastVisible )
                    t.Remaining -= view.FrameDt;
                cell.OverlayToastVisible.erase(
                     std::remove_if( cell.OverlayToastVisible.begin(), cell.OverlayToastVisible.end(),
                                     []( const UICanvasContext::ToastItem& t ) { return t.Remaining <= 0.0f; } ),
                     cell.OverlayToastVisible.end() );

                while ( static_cast<int>( cell.OverlayToastVisible.size() ) < slots &&
                        !cell.OverlayToastPending.empty() )
                {
                    cell.OverlayToastVisible.push_back(
                         { cell.OverlayToastPending.front(), std::max( 0.01f, d.ToastLifetime ) } );
                    cell.OverlayToastPending.pop_front();
                }
                if ( cell.OverlayToastPending.empty() )
                    cell.OverlayToastOverflowReported = false;

                // The slot keys are published EVERY frame, including the empty ones, because a slot whose
                // `visible` key is simply absent falls back to what the author typed — which is a filled
                // toast nobody raised.
                for ( int i = 0; i < slots; ++i )
                {
                    const bool filled = i < static_cast<int>( cell.OverlayToastVisible.size() );
                    cell.Locals.Set( OverlayToastTextKey( i ),
                                     filled ? cell.OverlayToastVisible[static_cast<std::size_t>( i )].Text
                                            : std::string{} );
                    cell.Locals.Set( OverlayToastVisibleKey( i ), filled );
                }
                cell.OverlayOpen = !cell.OverlayToastVisible.empty();
            }
        }

        // Drop stack entries and the tooltip whose canvas has stopped being an overlay (or stopped
        // existing). The cells themselves are retired by UIViewContext::RetireDeadCanvases; this is the
        // view-level index into them, and an index that outlives what it points at is the same defect one
        // level up.
        void PruneOverlays( IUITree& tree, UIViewContext& view )
        {
            const auto gone = [&]( NodeId c ) { return !tree.Valid( c ) || !tree.Has<UIOverlayData>( c ); };
            view.OverlayStack.erase( std::remove_if( view.OverlayStack.begin(), view.OverlayStack.end(), gone ),
                                     view.OverlayStack.end() );
            if ( view.OverlayTooltip != NodeId::Null && gone( view.OverlayTooltip ) )
                view.OverlayTooltip = NodeId::Null;
            if ( view.OverlayHoverTrigger != NodeId::Null && !tree.Valid( view.OverlayHoverTrigger ) )
            {
                view.OverlayHoverTrigger = NodeId::Null;
                view.OverlayHoverHeld    = 0.0f;
            }
            if ( view.OverlayHoverSuppressed != NodeId::Null && !tree.Valid( view.OverlayHoverSuppressed ) )
                view.OverlayHoverSuppressed = NodeId::Null;
        }
    } // namespace

    const UIOverlayData* OverlayDataOf( IUITree& tree, NodeId canvas )
    {
        if ( canvas == NodeId::Null || !tree.Valid( canvas ) || !tree.Has<UIOverlayData>( canvas ) )
            return nullptr;
        return tree.Get<UIOverlayData>( canvas );
    }

    Common::ResultStr<NodeId> OverlayByName( IUITree& tree, const std::string& name )
    {
        if ( name.empty() )
            return Common::MakeError<NodeId>( "[UI] an overlay was asked for by an empty name" );

        std::vector<NodeId> matches;
        for ( const NodeId e : RootsOf( tree, ArgKind::Overlay ) )
            if ( tree.Get<UIOverlayData>( e )->Name == name )
                matches.push_back( e );

        if ( matches.empty() )
            return Common::MakeFormattedError<NodeId>( "[UI] no overlay canvas is named '{}' in this scene",
                                                       name );
        if ( matches.size() > 1 )
            return Common::MakeFormattedError<NodeId>(
                 "[UI] {} overlay canvases are named '{}'; a name must identify one overlay, so none is "
                 "opened rather than one of them being guessed",
                 matches.size(), name );
        return Common::MakeSuccess( matches.front() );
    }

    UIOverlayRequests& UIOverlayRequests::Get()
    {
        static UIOverlayRequests requests;
        return requests;
    }

    void UIOverlayRequests::Raise( std::string overlay, std::string text )
    {
        if ( overlay.empty() )
        {
            LOG_ERROR( "[UI] a notification was raised with no overlay name and is dropped; name the Toast "
                       "overlay it belongs to" );
            return;
        }
        m_Pending.push_back( { std::move( overlay ), std::move( text ) } );
    }

    std::vector<UIOverlayRequests::Request> UIOverlayRequests::Drain()
    {
        std::vector<Request> out;
        out.swap( m_Pending );
        return out;
    }

    void UIOverlayRequests::Clear()
    {
        m_Pending.clear();
    }

    std::string OverlayToastTextKey( int slot )
    {
        return "overlay.toast." + std::to_string( slot ) + ".text";
    }

    std::string OverlayToastVisibleKey( int slot )
    {
        return "overlay.toast." + std::to_string( slot ) + ".visible";
    }

    void CloseAllOverlays( UIViewContext& view, IUITree& tree )
    {
        CloseStackDownTo( view, tree, 0 );
        CloseTooltip( view, tree );
        view.OverlayHoverTrigger    = NodeId::Null;
        view.OverlayHoverHeld       = 0.0f;
        view.OverlayHoverSuppressed = NodeId::Null;
    }

    void UpdateOverlays( UIViewContext& view, IUITree& tree, const UIInput& input )
    {
        PruneOverlays( tree, view );
        UpdateToasts( tree, view );

        const NodeId       hot          = view.HotNext; // THIS frame's election, before the hand-over
        const bool         pressedLeft  = input.MouseDown && !view.PrevDown;
        const bool         pressedRight = input.MouseRightDown && !view.PrevRightDown;
        view.PrevRightDown              = input.MouseRightDown;

        // --- Escape closes the innermost capturing overlay that allows it ------------------------------
        if ( input.Pressed( Common::KeyCode::Escape ) && !view.KeyConsumed( Common::KeyCode::Escape ) &&
             !view.OverlayStack.empty() )
        {
            const std::size_t         top = view.OverlayStack.size() - 1;
            const UIOverlayData*      d   = OverlayDataOf( tree, view.OverlayStack[top] );
            if ( d != nullptr && d->CloseOnEscape )
                CloseStackDownTo( view, tree, top, /*dismissed=*/true );
        }

        // --- A press the open stack did not take closes back to whoever owns it ------------------------
        if ( pressedLeft || pressedRight )
        {
            const int         owner     = StackEntryUnderPointer( tree, view, hot );
            const std::size_t keepBelow = owner < 0 ? 0U : static_cast<std::size_t>( owner ) + 1U;
            for ( std::size_t i = view.OverlayStack.size(); i-- > keepBelow; )
            {
                const UIOverlayData* d = OverlayDataOf( tree, view.OverlayStack[i] );
                if ( d == nullptr || !d->CloseOnClickOutside )
                    break;
                CloseStackDownTo( view, tree, i, /*dismissed=*/true );
            }
        }

        // --- A hover-opened menu closes when the pointer is neither on it nor on what opened it --------
        {
            const int under = StackEntryUnderPointer( tree, view, hot );
            for ( std::size_t i = view.OverlayStack.size(); i-- > 0; )
            {
                const NodeId           canvas = view.OverlayStack[i];
                const UICanvasContext& cell   = view.CanvasState( canvas );
                if ( OpenerEvent( tree, cell.OverlayOpenedBy ) != UIOverlayTriggerEvent::Hover )
                    continue;
                const bool onOpener = IsSelfOrAncestor( tree, cell.OverlayOpenedBy, hot );
                const bool inside   = under >= static_cast<int>( i );
                if ( !onOpener && !inside )
                    CloseStackDownTo( view, tree, i );
            }
        }

        // --- The hover clock, and what it opens --------------------------------------------------------
        const NodeId hoverTrigger = TriggerAtOrAbove( tree, hot, UIOverlayTriggerEvent::Hover );
        if ( hoverTrigger != view.OverlayHoverTrigger )
        {
            // A different trigger (or none): the delay is a delay, not an accumulator over everything the
            // pointer has ever touched.
            view.OverlayHoverTrigger = hoverTrigger;
            view.OverlayHoverHeld    = 0.0f;
        }
        else if ( hoverTrigger != NodeId::Null )
        {
            view.OverlayHoverHeld += view.FrameDt;
        }

        // The pointer moved off the trigger whose overlay was dismissed, so the dismissal is spent. Kept
        // AFTER the clock above and not between its two arms: putting it there silently re-parented the
        // `else` onto this condition and the hover clock never advanced again — the tooltip delay could
        // then never elapse. The suite caught it; nothing about the code reads wrong.
        if ( hoverTrigger != view.OverlayHoverSuppressed )
            view.OverlayHoverSuppressed = NodeId::Null;

        // A tooltip belongs to the trigger it was opened from and to no other. The pointer moving to a
        // different trigger — or off every trigger — retires it rather than leaving it hanging over
        // something it no longer describes.
        if ( view.OverlayTooltip != NodeId::Null &&
             view.CanvasState( view.OverlayTooltip ).OverlayOpenedBy != hoverTrigger )
            CloseTooltip( view, tree );

        if ( hoverTrigger != NodeId::Null )
        {
            const auto& t      = *tree.Get<UIOverlayTriggerData>( hoverTrigger );
            const auto  canvas = OverlayByName( tree, t.Overlay );
            if ( !canvas )
            {
                // Once per trigger per frame would be a flood; the refusal is the author's typo and it
                // names the overlay, so one line the moment the pointer first rests on it is enough.
                if ( view.OverlayHoverHeld == 0.0f )
                    LOG_ERROR( "[UI] hover trigger on entity {}: {}", static_cast<std::uint32_t>( hoverTrigger ),
                               canvas.GetError() );
            }
            else
            {
                const UIOverlayData*      d    = OverlayDataOf( tree, canvas.GetValue() );
                const bool                open = view.CanvasState( canvas.GetValue() ).OverlayOpen;
                if ( d != nullptr && !open && view.OverlayHoverSuppressed != hoverTrigger &&
                     view.OverlayHoverHeld >= std::max( 0.0f, d->OpenDelay ) )
                    OpenOverlay( tree, view, canvas.GetValue(), hoverTrigger, t.Text, input );
            }
        }

        // --- The click triggers ------------------------------------------------------------------------
        const auto fireClick = [&]( UIOverlayTriggerEvent on )
        {
            const NodeId trigger = TriggerAtOrAbove( tree, hot, on );
            if ( trigger == NodeId::Null )
                return;
            const auto& t      = *tree.Get<UIOverlayTriggerData>( trigger );
            const auto  canvas = OverlayByName( tree, t.Overlay );
            if ( !canvas )
            {
                LOG_ERROR( "[UI] click trigger on entity {}: {}", static_cast<std::uint32_t>( trigger ),
                           canvas.GetError() );
                return;
            }
            const UIOverlayData* d = OverlayDataOf( tree, canvas.GetValue() );
            if ( d == nullptr )
                return;
            // A trigger aimed at a Toast RAISES one. It does not "open" it: a toast canvas is open exactly
            // while it has something to show, and that is decided by the queue and the clock.
            if ( d->Kind == UIOverlayKind::Toast )
                RaiseToastOn( tree, view, canvas.GetValue(), t.Text );
            else
                OpenOverlay( tree, view, canvas.GetValue(), trigger, t.Text, input );
        };
        if ( pressedLeft )
            fireClick( UIOverlayTriggerEvent::LeftClick );
        if ( pressedRight )
            fireClick( UIOverlayTriggerEvent::RightClick );

        // --- A following tooltip is re-placed every frame ----------------------------------------------
        if ( view.OverlayTooltip != NodeId::Null )
        {
            const UIOverlayData* d = OverlayDataOf( tree, view.OverlayTooltip );
            if ( d != nullptr && d->FollowPointer )
                PlaceOverlayCanvas( tree, view, view.OverlayTooltip, input );
        }
    }
} // namespace Desert::UI
