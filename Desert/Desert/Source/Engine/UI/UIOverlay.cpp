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
        bool IsSelfOrAncestor( entt::registry& reg, entt::entity maybeAncestor, entt::entity e )
        {
            if ( maybeAncestor == entt::null )
                return false;
            std::size_t guard = 0;
            for ( entt::entity t = e; t != entt::null && reg.valid( t ) && guard++ <= reg.size(); )
            {
                if ( t == maybeAncestor )
                    return true;
                t = reg.has<ECS::RelationshipComponent>( t ) ? reg.get<ECS::RelationshipComponent>( t ).Parent
                                                             : entt::null;
            }
            return false;
        }

        // The nearest trigger at or above @p e that answers to @p on. Filtered by the EVENT and not merely
        // by the component, because a button may carry a right-click trigger while the panel above it
        // carries a hover one, and the two are different questions asked of the same chain.
        entt::entity TriggerAtOrAbove( entt::registry& reg, entt::entity e, ECS::UIOverlayTriggerEvent on )
        {
            std::size_t guard = 0;
            for ( entt::entity t = e; t != entt::null && reg.valid( t ) && guard++ <= reg.size(); )
            {
                if ( reg.has<ECS::UIOverlayTriggerComponent>( t ) &&
                     reg.get<ECS::UIOverlayTriggerComponent>( t ).Data.On == on &&
                     !reg.get<ECS::UIOverlayTriggerComponent>( t ).Data.Overlay.empty() )
                    return t;
                t = reg.has<ECS::RelationshipComponent>( t ) ? reg.get<ECS::RelationshipComponent>( t ).Parent
                                                             : entt::null;
            }
            return entt::null;
        }

        // What opened this overlay, as an event. entt::null (opened by name, not by a trigger) answers
        // LeftClick, which is the conservative reading: it is not a hover overlay, so it does not close
        // itself when the pointer wanders off.
        ECS::UIOverlayTriggerEvent OpenerEvent( entt::registry& reg, entt::entity opener )
        {
            return ( opener != entt::null && reg.valid( opener ) &&
                     reg.has<ECS::UIOverlayTriggerComponent>( opener ) )
                        ? reg.get<ECS::UIOverlayTriggerComponent>( opener ).Data.On
                        : ECS::UIOverlayTriggerEvent::LeftClick;
        }

        // The axis-aligned screen box of one element, resolved through the SAME walk the renderer uses and
        // with this view's own context — so an item of an already-placed submenu reports where it really
        // is and not where it was authored. Returns false when the element is not drawn at all, which is a
        // meaningful answer: there is nothing to place a submenu beside.
        bool ElementScreenBox( entt::registry& reg, UIViewContext& view, entt::entity e, Rect& out )
        {
            const entt::entity canvas = CanvasOf( reg, e );
            if ( canvas == entt::null )
                return false;
            std::vector<UIElementNode> nodes;
            if ( const auto walked =
                      EnumerateCanvas( reg, canvas, view.ViewportPx, nodes, &view.CanvasState( canvas ) );
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
        bool OverlayContentBox( entt::registry& reg, UIViewContext& view, entt::entity canvas, Rect& out )
        {
            UICanvasContext probe = view.CanvasState( canvas );
            probe.OverlayOpen     = true;
            probe.OverlayShift    = glm::vec2( 0.0f );

            std::vector<UIElementNode> nodes;
            if ( const auto walked = EnumerateCanvas( reg, canvas, view.ViewportPx, nodes, &probe ); !walked )
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
        void PlaceOverlayCanvas( entt::registry& reg, UIViewContext& view, entt::entity canvas,
                                 const UIInput& input )
        {
            UICanvasContext&          cell = view.CanvasState( canvas );
            const ECS::UIOverlayData* d    = OverlayDataOf( reg, canvas );
            if ( d == nullptr || d->Kind == ECS::UIOverlayKind::Modal || d->Kind == ECS::UIOverlayKind::Toast )
            {
                cell.OverlayShift = glm::vec2( 0.0f );
                return;
            }

            Rect content;
            if ( !OverlayContentBox( reg, view, canvas, content ) )
            {
                cell.OverlayShift = glm::vec2( 0.0f );
                return;
            }

            // A pointer origin is a zero-size rect at the cursor; an element origin is the element's own
            // box. One parameter, because "flip about the thing I am attached to" is one sentence.
            const bool byHover = OpenerEvent( reg, cell.OverlayOpenedBy ) == ECS::UIOverlayTriggerEvent::Hover;
            const bool pinToElement = ( d->Kind == ECS::UIOverlayKind::Tooltip ) ? !d->FollowPointer : byHover;

            Rect        origin{ input.MousePx.x, input.MousePx.y, 0.0f, 0.0f };
            OverlayAxis axis = OverlayAxis::Vertical;
            if ( pinToElement )
            {
                Rect box;
                if ( ElementScreenBox( reg, view, cell.OverlayOpenedBy, box ) )
                {
                    origin = box;
                    // A submenu gets clear of its parent item SIDEWAYS; a pinned tooltip still drops below
                    // the element it describes, because covering that element is the one thing it must not
                    // do and stepping aside is not enough to avoid it.
                    if ( d->Kind == ECS::UIOverlayKind::ContextMenu )
                        axis = OverlayAxis::Horizontal;
                }
            }

            const auto  scale = CanvasScale( reg, canvas, view.ViewportPx );
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
        void CloseStackDownTo( UIViewContext& view, entt::registry& reg, std::size_t index,
                               bool dismissed = false )
        {
            while ( view.OverlayStack.size() > index )
            {
                const entt::entity c = view.OverlayStack.back();
                view.OverlayStack.pop_back();
                if ( reg.valid( c ) )
                {
                    if ( dismissed && OpenerEvent( reg, view.CanvasState( c ).OverlayOpenedBy ) ==
                                           ECS::UIOverlayTriggerEvent::Hover )
                        view.OverlayHoverSuppressed = view.CanvasState( c ).OverlayOpenedBy;
                    UICanvasContext& cell = view.CanvasState( c );
                    cell.OverlayOpen      = false;
                    cell.OverlayOpenedBy  = entt::null;
                    cell.OverlayShift     = glm::vec2( 0.0f );
                    cell.Locals.Erase( kOverlayTextKey );
                }
            }
        }

        void CloseTooltip( UIViewContext& view, entt::registry& reg )
        {
            if ( view.OverlayTooltip == entt::null )
                return;
            if ( reg.valid( view.OverlayTooltip ) )
            {
                UICanvasContext& cell = view.CanvasState( view.OverlayTooltip );
                cell.OverlayOpen      = false;
                cell.OverlayOpenedBy  = entt::null;
                cell.OverlayShift     = glm::vec2( 0.0f );
                cell.Locals.Erase( kOverlayTextKey );
            }
            view.OverlayTooltip = entt::null;
        }

        // Index of the open stack entry whose OWN CONTENT the pointer is on, or -1.
        //
        // `hot == stack[i]` is the modal's scrim — the canvas entity elects itself for every point its
        // dialog does not cover — and that is OUTSIDE, not inside. Reading it as inside is what would turn
        // "click the dim to dismiss" into "the dim can never be clicked".
        int StackEntryUnderPointer( entt::registry& reg, const UIViewContext& view, entt::entity hot )
        {
            if ( hot == entt::null || !reg.valid( hot ) )
                return -1;
            const entt::entity hotCanvas = CanvasOf( reg, hot );
            int                found     = -1;
            for ( std::size_t i = 0; i < view.OverlayStack.size(); ++i )
                if ( hotCanvas == view.OverlayStack[i] && hot != view.OverlayStack[i] )
                    found = static_cast<int>( i );
            return found;
        }

        void OpenOverlay( entt::registry& reg, UIViewContext& view, entt::entity canvas, entt::entity trigger,
                          const std::string& text, const UIInput& input )
        {
            const ECS::UIOverlayData* d = OverlayDataOf( reg, canvas );
            if ( d == nullptr )
                return;

            UICanvasContext& cell = view.CanvasState( canvas );
            cell.OverlayOpenedBy  = trigger;
            cell.OverlayOpen      = true;
            if ( text.empty() )
                cell.Locals.Erase( kOverlayTextKey );
            else
                cell.Locals.Set( kOverlayTextKey, text );

            if ( d->Kind == ECS::UIOverlayKind::Tooltip )
            {
                if ( view.OverlayTooltip != canvas )
                    CloseTooltip( view, reg );
                view.OverlayTooltip = canvas;
            }
            else
            {
                // A menu opened from inside another menu STACKS on it; one opened from anywhere else
                // replaces whatever was open. That single rule is all nesting needs: a submenu is a
                // ContextMenu overlay whose trigger happens to live in the menu below it.
                const entt::entity triggerCanvas = trigger != entt::null ? CanvasOf( reg, trigger ) : entt::null;
                int                owner         = -1;
                for ( std::size_t i = 0; i < view.OverlayStack.size(); ++i )
                    if ( view.OverlayStack[i] == triggerCanvas )
                        owner = static_cast<int>( i );
                // "everything above the entry that owns the trigger", and -1 (it owns none) means all of
                // it. Spelled out rather than as owner+1 on an int, because the -1 then reaches an unsigned
                // conversion and only arrives at 0 by wrapping.
                const std::size_t keepBelow = owner < 0 ? 0U : static_cast<std::size_t>( owner ) + 1U;
                CloseStackDownTo( view, reg, keepBelow );

                cell.OverlayOpen     = true; // CloseStackDownTo may have cleared it if it was already up
                cell.OverlayOpenedBy = trigger;
                if ( !text.empty() )
                    cell.Locals.Set( kOverlayTextKey, text );
                view.OverlayStack.push_back( canvas );
            }

            PlaceOverlayCanvas( reg, view, canvas, input );
        }

        void RaiseToastOn( entt::registry& reg, UIViewContext& view, entt::entity canvas, std::string text )
        {
            const ECS::UIOverlayData* d = OverlayDataOf( reg, canvas );
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

        void UpdateToasts( entt::registry& reg, UIViewContext& view )
        {
            // Notifications raised from outside the UI reach exactly one view — the one that owns the
            // scene's shared clocks. See UIOverlay.hpp.
            if ( view.DrivesSceneAnimation )
            {
                for ( UIOverlayRequests::Request& r : UIOverlayRequests::Get().Drain() )
                {
                    const auto canvas = OverlayByName( reg, r.Overlay );
                    if ( !canvas )
                    {
                        LOG_ERROR( "[UI] a notification was raised for overlay '{}': {}", r.Overlay,
                                   canvas.GetError() );
                        continue;
                    }
                    const ECS::UIOverlayData* d = OverlayDataOf( reg, canvas.GetValue() );
                    if ( d == nullptr || d->Kind != ECS::UIOverlayKind::Toast )
                    {
                        LOG_ERROR( "[UI] a notification was raised for overlay '{}', which is not a Toast; "
                                   "notifications are queued only by Toast overlays",
                                   r.Overlay );
                        continue;
                    }
                    RaiseToastOn( reg, view, canvas.GetValue(), std::move( r.Text ) );
                }
            }

            for ( const entt::entity canvas : reg.view<ECS::UIOverlayComponent>() )
            {
                const auto& d = reg.get<ECS::UIOverlayComponent>( canvas ).Data;
                if ( d.Kind != ECS::UIOverlayKind::Toast )
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
        void PruneOverlays( entt::registry& reg, UIViewContext& view )
        {
            const auto gone = [&]( entt::entity c )
            { return !reg.valid( c ) || !reg.has<ECS::UIOverlayComponent>( c ); };
            view.OverlayStack.erase( std::remove_if( view.OverlayStack.begin(), view.OverlayStack.end(), gone ),
                                     view.OverlayStack.end() );
            if ( view.OverlayTooltip != entt::null && gone( view.OverlayTooltip ) )
                view.OverlayTooltip = entt::null;
            if ( view.OverlayHoverTrigger != entt::null && !reg.valid( view.OverlayHoverTrigger ) )
            {
                view.OverlayHoverTrigger = entt::null;
                view.OverlayHoverHeld    = 0.0f;
            }
            if ( view.OverlayHoverSuppressed != entt::null && !reg.valid( view.OverlayHoverSuppressed ) )
                view.OverlayHoverSuppressed = entt::null;
        }
    } // namespace

    const ECS::UIOverlayData* OverlayDataOf( entt::registry& reg, entt::entity canvas )
    {
        if ( canvas == entt::null || !reg.valid( canvas ) || !reg.has<ECS::UIOverlayComponent>( canvas ) )
            return nullptr;
        return &reg.get<ECS::UIOverlayComponent>( canvas ).Data;
    }

    Common::ResultStr<entt::entity> OverlayByName( entt::registry& reg, const std::string& name )
    {
        if ( name.empty() )
            return Common::MakeError<entt::entity>( "[UI] an overlay was asked for by an empty name" );

        std::vector<entt::entity> matches;
        for ( const entt::entity e : reg.view<ECS::UIOverlayComponent>() )
            if ( reg.get<ECS::UIOverlayComponent>( e ).Data.Name == name )
                matches.push_back( e );

        if ( matches.empty() )
            return Common::MakeFormattedError<entt::entity>( "[UI] no overlay canvas is named '{}' in this scene",
                                                             name );
        if ( matches.size() > 1 )
            return Common::MakeFormattedError<entt::entity>(
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

    void CloseAllOverlays( UIViewContext& view, entt::registry& reg )
    {
        CloseStackDownTo( view, reg, 0 );
        CloseTooltip( view, reg );
        view.OverlayHoverTrigger    = entt::null;
        view.OverlayHoverHeld       = 0.0f;
        view.OverlayHoverSuppressed = entt::null;
    }

    void UpdateOverlays( UIViewContext& view, entt::registry& reg, const UIInput& input )
    {
        PruneOverlays( reg, view );
        UpdateToasts( reg, view );

        const entt::entity hot          = view.HotNext; // THIS frame's election, before the hand-over
        const bool         pressedLeft  = input.MouseDown && !view.PrevDown;
        const bool         pressedRight = input.MouseRightDown && !view.PrevRightDown;
        view.PrevRightDown              = input.MouseRightDown;

        // --- Escape closes the innermost capturing overlay that allows it ------------------------------
        if ( input.Escape && !view.OverlayStack.empty() )
        {
            const std::size_t         top = view.OverlayStack.size() - 1;
            const ECS::UIOverlayData* d   = OverlayDataOf( reg, view.OverlayStack[top] );
            if ( d != nullptr && d->CloseOnEscape )
                CloseStackDownTo( view, reg, top, /*dismissed=*/true );
        }

        // --- A press the open stack did not take closes back to whoever owns it ------------------------
        if ( pressedLeft || pressedRight )
        {
            const int         owner     = StackEntryUnderPointer( reg, view, hot );
            const std::size_t keepBelow = owner < 0 ? 0U : static_cast<std::size_t>( owner ) + 1U;
            for ( std::size_t i = view.OverlayStack.size(); i-- > keepBelow; )
            {
                const ECS::UIOverlayData* d = OverlayDataOf( reg, view.OverlayStack[i] );
                if ( d == nullptr || !d->CloseOnClickOutside )
                    break;
                CloseStackDownTo( view, reg, i, /*dismissed=*/true );
            }
        }

        // --- A hover-opened menu closes when the pointer is neither on it nor on what opened it --------
        {
            const int under = StackEntryUnderPointer( reg, view, hot );
            for ( std::size_t i = view.OverlayStack.size(); i-- > 0; )
            {
                const entt::entity     canvas = view.OverlayStack[i];
                const UICanvasContext& cell   = view.CanvasState( canvas );
                if ( OpenerEvent( reg, cell.OverlayOpenedBy ) != ECS::UIOverlayTriggerEvent::Hover )
                    continue;
                const bool onOpener = IsSelfOrAncestor( reg, cell.OverlayOpenedBy, hot );
                const bool inside   = under >= static_cast<int>( i );
                if ( !onOpener && !inside )
                    CloseStackDownTo( view, reg, i );
            }
        }

        // --- The hover clock, and what it opens --------------------------------------------------------
        const entt::entity hoverTrigger = TriggerAtOrAbove( reg, hot, ECS::UIOverlayTriggerEvent::Hover );
        if ( hoverTrigger != view.OverlayHoverTrigger )
        {
            // A different trigger (or none): the delay is a delay, not an accumulator over everything the
            // pointer has ever touched.
            view.OverlayHoverTrigger = hoverTrigger;
            view.OverlayHoverHeld    = 0.0f;
        }
        else if ( hoverTrigger != entt::null )
        {
            view.OverlayHoverHeld += view.FrameDt;
        }

        // The pointer moved off the trigger whose overlay was dismissed, so the dismissal is spent. Kept
        // AFTER the clock above and not between its two arms: putting it there silently re-parented the
        // `else` onto this condition and the hover clock never advanced again — the tooltip delay could
        // then never elapse. The suite caught it; nothing about the code reads wrong.
        if ( hoverTrigger != view.OverlayHoverSuppressed )
            view.OverlayHoverSuppressed = entt::null;

        // A tooltip belongs to the trigger it was opened from and to no other. The pointer moving to a
        // different trigger — or off every trigger — retires it rather than leaving it hanging over
        // something it no longer describes.
        if ( view.OverlayTooltip != entt::null &&
             view.CanvasState( view.OverlayTooltip ).OverlayOpenedBy != hoverTrigger )
            CloseTooltip( view, reg );

        if ( hoverTrigger != entt::null )
        {
            const auto& t      = reg.get<ECS::UIOverlayTriggerComponent>( hoverTrigger ).Data;
            const auto  canvas = OverlayByName( reg, t.Overlay );
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
                const ECS::UIOverlayData* d    = OverlayDataOf( reg, canvas.GetValue() );
                const bool                open = view.CanvasState( canvas.GetValue() ).OverlayOpen;
                if ( d != nullptr && !open && view.OverlayHoverSuppressed != hoverTrigger &&
                     view.OverlayHoverHeld >= std::max( 0.0f, d->OpenDelay ) )
                    OpenOverlay( reg, view, canvas.GetValue(), hoverTrigger, t.Text, input );
            }
        }

        // --- The click triggers ------------------------------------------------------------------------
        const auto fireClick = [&]( ECS::UIOverlayTriggerEvent on )
        {
            const entt::entity trigger = TriggerAtOrAbove( reg, hot, on );
            if ( trigger == entt::null )
                return;
            const auto& t      = reg.get<ECS::UIOverlayTriggerComponent>( trigger ).Data;
            const auto  canvas = OverlayByName( reg, t.Overlay );
            if ( !canvas )
            {
                LOG_ERROR( "[UI] click trigger on entity {}: {}", static_cast<std::uint32_t>( trigger ),
                           canvas.GetError() );
                return;
            }
            const ECS::UIOverlayData* d = OverlayDataOf( reg, canvas.GetValue() );
            if ( d == nullptr )
                return;
            // A trigger aimed at a Toast RAISES one. It does not "open" it: a toast canvas is open exactly
            // while it has something to show, and that is decided by the queue and the clock.
            if ( d->Kind == ECS::UIOverlayKind::Toast )
                RaiseToastOn( reg, view, canvas.GetValue(), t.Text );
            else
                OpenOverlay( reg, view, canvas.GetValue(), trigger, t.Text, input );
        };
        if ( pressedLeft )
            fireClick( ECS::UIOverlayTriggerEvent::LeftClick );
        if ( pressedRight )
            fireClick( ECS::UIOverlayTriggerEvent::RightClick );

        // --- A following tooltip is re-placed every frame ----------------------------------------------
        if ( view.OverlayTooltip != entt::null )
        {
            const ECS::UIOverlayData* d = OverlayDataOf( reg, view.OverlayTooltip );
            if ( d != nullptr && d->FollowPointer )
                PlaceOverlayCanvas( reg, view, view.OverlayTooltip, input );
        }
    }
} // namespace Desert::UI
