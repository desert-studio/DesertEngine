#include <UI/Widgets/UIWidgets.hpp>

#include <UI/UICanvasRenderer2D.hpp>
#include <UI/UIOverlay.hpp>
#include <Common/Core/AssetHandle.hpp>
#include <Render2DCore/Text/BakedFont.hpp>
#include <UI/UIStyleResolver.hpp>
#include <Render2DCore/Text/Utf8.hpp>
#include <UI/UICanvasLayout.hpp>
#include <UI/UIDataStore.hpp>
#include <UI/UIPathGeometry.hpp>
#include <UI/UIRichText.hpp>
#include <UI/UIListSelection.hpp>

#include <Common/Core/Logger.hpp>

#include <algorithm>
#include <span>
#include <unordered_set>
#include <array>
#include <optional>
#include <cstdint>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <vector>
#include <format>

namespace Desert::UI::Walk
{
    namespace
    {
        // Is @p hot (last frame's pointer winner) @p e or inside it? A list's rows are its descendants, so a
        // pointer over a row's text is still a pointer over the list.
        bool HotWithin( const IUITree& tree, NodeId hot, NodeId e )
        {
            for ( NodeId n = hot; n != NodeId::Null && tree.Valid( n ); n = tree.Parent( n ) )
                if ( n == e )
                    return true;
            return false;
        }

        // Selected and hovered rows are painted under the row's own content (STableRow's selector brush).
        void DrawListRowBackdrop( ElementFrame& frame, const UIListShape& shape, const UIListViewState& state,
                                  int i, const Rect& row, int hoverRow )
        {
            const auto& lv = *frame.Tree.Get<UIListViewData>( frame.E );
            if ( lv.SelectionMode == UISelectionMode::None && shape.Tree == nullptr )
                return;
            const UIListItemKey key  = shape.KeyAt( frame.Tree, frame.E, i );
            const glm::vec2     mn   = { row.X, row.Y };
            const glm::vec2     mx   = { row.X + row.W, row.Y + row.H };
            if ( state.Selection.Has( key ) )
                frame.Dl.AddRectFilled( mn, mx, Tinted( frame.Ctx, glm::vec4( lv.SelectionColor, 1.0f ) ) );
            else if ( i == hoverRow && lv.SelectionMode != UISelectionMode::None )
                frame.Dl.AddRectFilled( mn, mx, Tinted( frame.Ctx, glm::vec4( lv.HoverColor, 1.0f ) ) );
        }

        // ...and outlined OVER it, so an entry template that paints its whole row opaque (an inventory slot)
        // still shows that it is selected; the keyboard cursor of a focused list gets a brighter edge. The
        // tree's expander sits in the gutter the template is not drawn in.
        void DrawListRowFront( ElementFrame& frame, const UIListShape& shape, const UIListViewState& state, int i,
                               const Rect& row )
        {
            const auto& lv = *frame.Tree.Get<UIListViewData>( frame.E );
            if ( lv.SelectionMode == UISelectionMode::None && shape.Tree == nullptr )
                return;
            const UIListItemKey key    = shape.KeyAt( frame.Tree, frame.E, i );
            const glm::vec2     mn     = { row.X, row.Y };
            const glm::vec2     mx     = { row.X + row.W, row.Y + row.H };
            const bool          cell   = frame.Focused != nullptr && *frame.Focused == frame.E;
            const float         border = std::max( 1.0f, 2.0f * frame.Scale );
            if ( cell && key == state.Selection.Cursor )
                frame.Dl.AddRect( mn, mx, glm::vec4( glm::min( lv.SelectionColor * 1.8f, glm::vec3( 1.0f ) ), 1.0f ),
                                  border );
            else if ( state.Selection.Has( key ) )
                frame.Dl.AddRect( mn, mx, Tinted( frame.Ctx, glm::vec4( lv.SelectionColor, 1.0f ) ), border );
            if ( shape.Tree == nullptr )
                return;
            // The expander: a triangle in the gutter, right when collapsed and down when expanded.
            const UITreeRow& tr = shape.Rows()[static_cast<std::size_t>( i )];
            if ( !tr.HasChildren )
                return;
            const auto&     tv = *frame.Tree.Get<UITreeViewData>( frame.E );
            const Rect      g  = TreeExpanderRect( row, tr.Depth, tv.Indent * frame.Scale );
            const float     cx = g.X + g.W * 0.5f, cy = g.Y + g.H * 0.5f;
            const float     a  = std::min( g.W, g.H ) * 0.22f;
            const glm::vec4 c( tv.ExpanderColor, 1.0f );
            if ( tr.Expanded )
                frame.Dl.AddTriangleFilled( { cx - a, cy - a * 0.6f }, { cx + a, cy - a * 0.6f }, { cx, cy + a * 0.6f },
                                            c );
            else
                frame.Dl.AddTriangleFilled( { cx - a * 0.6f, cy - a }, { cx - a * 0.6f, cy + a }, { cx + a * 0.6f, cy },
                                            c );
        }

        void SendSelectionChanged( ElementFrame& frame, const UIListViewData& lv, int row )
        {
            if ( !lv.OnSelectionChangedMessage.empty() )
                frame.Ctx.View.WalkMessages.push_back( std::format( "{}|{}", lv.OnSelectionChangedMessage, row ) );
        }

        // Keep row @p index on screen (STableViewBase::RequestScrollIntoView), in design px of ScrollY.
        void ScrollRowIntoView( UIListViewData& lv, const UIListShape& shape, int index, float viewportH,
                                float scale )
        {
            if ( index < 0 || scale <= 0.0f )
                return;
            const float pitch = std::max( 1.0f, lv.ItemHeight ) + std::max( 0.0f, lv.Spacing );
            const float top   = static_cast<float>( index / std::max( 1, shape.Columns ) ) * pitch;
            const float view  = viewportH / scale;
            if ( top < lv.ScrollY )
                lv.ScrollY = top;
            else if ( top + std::max( 1.0f, lv.ItemHeight ) > lv.ScrollY + view )
                lv.ScrollY = top + std::max( 1.0f, lv.ItemHeight ) - view;
        }

        void SetExpanded( UIListViewState& state, UIListItemKey key, bool expanded )
        {
            if ( expanded )
                state.Expanded.insert( key );
            else
                state.Expanded.erase( key );
            state.TreeStale = true;
        }

    // The pointer and the keyboard of a list, tile or tree view. Returns true when the rows or the scroll
    // offset changed (the caller resolves and solves again). @p hoverRow receives the row under the pointer.
    bool HandleListInput( ElementFrame& frame, UIListViewData& lv, const UIListShape& shape,
                          const ListWindow& window, UIListViewState& state, int& hoverRow )
    {
        auto&       ctx    = frame.Ctx;
        auto&       tree   = frame.Tree;
        const auto  e      = frame.E;
        const auto* input  = frame.Input;
        const auto& rect   = frame.ElementRect;
        const float scale  = frame.Scale;
        const bool  tree_  = shape.Tree != nullptr;
        const bool  select = lv.SelectionMode != UISelectionMode::None;
        if ( input == nullptr || !frame.Interactive || ( !select && !tree_ ) )
            return false;

        const auto keyAt = [&]( int i ) { return shape.KeyAt( tree, e, i ); };
        bool       rowsChanged = false;

        // --- Pointer ------------------------------------------------------------------------------------
        const glm::vec2 p        = frame.PointerPx;
        const bool      overBar  = lv.ShowScrollbar && window.ScrollMaxPx > 0.0f &&
                             p.x >= rect.X + rect.W - 6.0f * scale;
        const NodeId    hot      = ctx.View.Hot;
        // A control inside a row (a button, a toggle) takes its own click, as a UE button in an entry does —
        // also when the pointer landed on its label or icon: the click bubbles to that control
        // (UIViewContext::PointerTarget), never to the row under it.
        const NodeId    ctl       = ctx.View.PointerTarget;
        const bool      onControl = ( hot != e && tree.Valid( hot ) && IsFocusable( tree, hot ) ) ||
                               ( ctl != e && ctl != NodeId::Null && HotWithin( tree, ctl, e ) );
        if ( HotWithin( tree, hot, e ) && !overBar && PointIn( rect, p ) && ctx.View.ThumbDrag.Owner != e )
            for ( int i = window.First; i <= window.Last; ++i )
                if ( PointIn( ListRowRect( rect, window, i ), p ) )
                {
                    hoverRow = i;
                    break;
                }
        const bool press = input->MouseDown && !ctx.View.PrevDown;
        if ( press && hoverRow >= 0 && !onControl )
        {
            if ( select && frame.Focused != nullptr )
                *frame.Focused = e; // focus on press, as STableViewBase takes keyboard focus on a row click
            const UITreeRow* tr =
                 tree_ ? &shape.Rows()[static_cast<std::size_t>( hoverRow )] : nullptr;
            if ( tr != nullptr && tr->HasChildren &&
                 PointIn( TreeExpanderRect( ListRowRect( rect, window, hoverRow ), tr->Depth, shape.Tree->Indent * scale ),
                          p ) )
            {
                SetExpanded( state, tr->Key, !tr->Expanded );
                rowsChanged = true;
            }
            else if ( ClickRow( state.Selection, lv.SelectionMode, shape.Count, keyAt, hoverRow,
                                HasMod( input->Mods, UIKeyMods::Shift ),
                                HasMod( input->Mods, UIKeyMods::Ctrl ) || HasMod( input->Mods, UIKeyMods::Super ) ) )
                SendSelectionChanged( frame, lv, hoverRow );
        }

        // --- Keyboard (focused list only) ----------------------------------------------------------------
        if ( !select || frame.Focused == nullptr || *frame.Focused != e )
            return rowsChanged;
        using Common::KeyCode;
        const float pitchPx   = ( std::max( 1.0f, lv.ItemHeight ) + std::max( 0.0f, lv.Spacing ) ) * scale;
        const int   pageItems = std::max( 1, static_cast<int>( rect.H / std::max( 1.0f, pitchPx ) ) ) * shape.Columns;
        for ( const UIKeyEvent& k : input->Keys )
        {
            // Rows change under a tree's expand/collapse; later keys of the same frame see the new rows only
            // after the caller re-resolves, so a frame stops at the first structural change.
            if ( rowsChanged && tree_ )
                break;
            const bool shift  = HasMod( k.Mods, UIKeyMods::Shift );
            const bool ctrl   = HasMod( k.Mods, UIKeyMods::Ctrl ) || HasMod( k.Mods, UIKeyMods::Super );
            const int  cursor = IndexOfKey( shape.Count, keyAt, state.Selection.Cursor );
            if ( k.Key == KeyCode::A && ctrl )
            {
                if ( SelectAllRows( state.Selection, lv.SelectionMode, shape.Count, keyAt ) )
                    SendSelectionChanged( frame, lv, std::max( 0, cursor ) );
                ctx.View.ConsumeKey( k.Key );
                continue;
            }
            if ( k.Key == KeyCode::Space || k.Key == KeyCode::Enter )
            {
                if ( cursor >= 0 && ToggleCursorRow( state.Selection, lv.SelectionMode, shape.Count, keyAt ) )
                    SendSelectionChanged( frame, lv, cursor );
                ctx.View.ConsumeKey( k.Key );
                continue;
            }
            // A tree's Left/Right (STreeView::OnKeyDown): Right expands, then steps into the first child; Left
            // collapses, then steps out to the parent.
            if ( tree_ && cursor >= 0 && ( k.Key == KeyCode::Left || k.Key == KeyCode::Right ) )
            {
                const UITreeRow& tr     = shape.Rows()[static_cast<std::size_t>( cursor )];
                int              target = -1;
                if ( k.Key == KeyCode::Right )
                {
                    if ( tr.HasChildren && !tr.Expanded )
                    {
                        SetExpanded( state, tr.Key, true );
                        rowsChanged = true;
                    }
                    else if ( tr.Expanded )
                        target = cursor + 1;
                }
                else if ( tr.Expanded )
                {
                    SetExpanded( state, tr.Key, false );
                    rowsChanged = true;
                }
                else
                    target = tr.Parent;
                if ( target >= 0 && target < shape.Count )
                {
                    if ( NavigateRow( state.Selection, lv.SelectionMode, shape.Count, keyAt, target, shift, ctrl ) )
                        SendSelectionChanged( frame, lv, target );
                    ScrollRowIntoView( lv, shape, target, rect.H, scale );
                    rowsChanged = true;
                }
                ctx.View.ConsumeKey( k.Key );
                continue;
            }
            const int target = NavigateTarget( k.Key, cursor, shape.Count, shape.Columns, pageItems );
            // Not a key of this list, or one that cannot move (Up on the first row): left to focus navigation,
            // so the arrow that would go nowhere inside the list moves focus out of it.
            if ( target < 0 || ( target == cursor && cursor >= 0 ) )
                continue;
            if ( NavigateRow( state.Selection, lv.SelectionMode, shape.Count, keyAt, target, shift, ctrl ) )
                SendSelectionChanged( frame, lv, target );
            ScrollRowIntoView( lv, shape, target, rect.H, scale );
            rowsChanged = true;
            ctx.View.ConsumeKey( k.Key );
        }
        return rowsChanged;
    }
    } // namespace

    void DrawChildren( ElementFrame& frame )
    {
        auto& ctx         = frame.Ctx;
        auto& tree        = frame.Tree;
        auto& e           = frame.E;
        auto& scale       = frame.Scale;
        auto& dl          = frame.Dl;
        auto& input       = frame.Input;
        auto& outClicked  = frame.OutClicked;
        auto& focused     = frame.Focused;
        auto& popups      = frame.Popups;
        auto& focusables  = frame.Focusables;
        auto& clipRegion  = frame.ClipRegion;
        auto& st          = frame.St;
        auto& rect        = frame.ElementRect;
        auto& interactive = frame.Interactive;
        auto& childScope  = frame.ChildScope;

        Rect childParent = rect;
        // Clip Contents (RectMask2D) OR a scroll view both scissor children to this element's rect.
        bool clip = tree.Has<UILayoutData>( e ) && tree.Get<UILayoutData>( e )->ClipContents;

        // The scrolling containers' shared state, stated once so the scrollbar below is written
        // once. >0 in ScrollMaxPx is also "the content overflows", i.e. "draw a scrollbar".
        float      contentPx     = 0.0f;
        float      scrollPx      = 0.0f;
        float      scrollMaxPx   = 0.0f;
        bool       showScrollbar = false;
        glm::vec3  scrollbarColor( 0.0f );
        const bool wheelOver = input != nullptr && interactive && e == ctx.View.WheelTarget;
        if ( tree.Has<UIScrollViewData>( e ) )
        {
            auto& sv = *tree.GetState<UIScrollViewData>( e );
            dl.AddRectFilled( { rect.X, rect.Y }, { rect.X + rect.W, rect.Y + rect.H },
                              glm::vec4( st.Color( StyleSlot::ScrollViewBackground, sv.Background ), 1.0f ) );

            contentPx   = sv.ContentHeight * scale;
            scrollMaxPx = std::max( 0.0f, contentPx - rect.H );
            if ( wheelOver && input->ScrollDelta != 0.0f )
            {
                sv.ScrollY -= input->ScrollDelta * 30.0f; // 30 design px per wheel notch
            }
            const float maxScrollDesign = scale > 0.0f ? scrollMaxPx / scale : 0.0f;
            sv.ScrollY                  = std::clamp( sv.ScrollY, 0.0f, maxScrollDesign );

            scrollPx       = sv.ScrollY * scale;
            showScrollbar  = sv.ShowScrollbar;
            scrollbarColor = st.Color( StyleSlot::ScrollViewScrollbar, sv.ScrollbarColor );
            clip           = true;
            childParent.Y -= scrollPx; // shift children up by the scroll offset
        }

        // THE VIRTUALIZED LIST (Ю17). Everything above is the same container seen from one step
        // back — a background, a wheel, a clip and a scrollbar — and the one difference is below,
        // in the child loop: this one walks a WINDOW of its children and the scroll view walks all
        // of them. It reads the same ROW GEOMETRY the enumeration reads, out of one function, so
        // the row that draws here and the row that is picked in the editor cannot be two rows.
        const bool       isList = tree.Has<UIListViewData>( e );
        ListWindow       window;
        UIListShape      shape;
        UIListViewState* listState = nullptr;
        int              hoverRow  = -1;
        if ( isList )
        {
            auto&       lv = *tree.GetState<UIListViewData>( e );
            const auto* tv = tree.Get<UITreeViewData>( e );
            listState      = &ctx.Canvas.Lists[e];
            if ( tv != nullptr && lv.Collection.empty() && ctx.Canvas.WarnedListTemplates.insert( e ).second )
                LOG_WARN( "[UI] list {} has a UITreeView but no Collection; a tree's rows are records and their "
                          "children collections, so it draws as a plain list",
                          static_cast<std::uint32_t>( e ) );
            // THE ROWS, resolved by the one function the enumeration also asks (UIListSelection.hpp). A tree
            // first brings its cached linearisation up to date — a lookup per expanded row while nothing
            // changed, a rebuild of the expanded part when something did.
            const auto resolveRows = [&]
            {
                if ( tv != nullptr && !lv.Collection.empty() )
                    RefreshTreeRows( *listState, UIDataStore::Get(), lv.Collection, tv->ChildrenField );
                shape = ResolveListShape( tree, e, rect.W, scale, listState );
            };
            resolveRows();
            // Bound (UIL1): the row count is the collection's, and a list whose collection has not
            // been written yet is an empty list — the same answer a binding to an unwritten key
            // gives, the authored state until gameplay says otherwise.
            const UICollection* boundRows = shape.Bound;
            if ( boundRows != nullptr )
            {
                // Follow the records, not the indices: see AnchorListScroll. Done BEFORE the wheel
                // and the solve, so this frame's window is already the anchored one.
                auto& seen = ctx.Canvas.ListBindings[e];
                if ( seen.Serial != boundRows->Serial() || seen.Generation != boundRows->Generation() )
                {
                    std::vector<UICollection::Change> changes;
                    const bool                        complete =
                         seen.Serial == boundRows->Serial() && boundRows->ChangesSince( seen.Generation, changes );
                    // A tile line holds Columns records, so one record moves the content a Columns-th of a line.
                    const float pitch = ( std::max( 1.0f, lv.ItemHeight ) + std::max( 0.0f, lv.Spacing ) ) /
                                        static_cast<float>( shape.Columns );
                    if ( seen.Serial != 0 )
                        lv.ScrollY =
                             AnchorListScroll( lv.ScrollY, pitch, lv.FollowEnd, seen.AtEnd, complete, changes );
                    seen.Serial     = boundRows->Serial();
                    seen.Generation = boundRows->Generation();
                    // Records that left the collection leave the selection (UE: items no longer in the source
                    // are dropped from the selection on refresh). Only on a change, so it is O(records) per
                    // mutation and never per frame.
                    if ( !listState->Selection.Selected.empty() || listState->Selection.Cursor != 0 )
                    {
                        std::unordered_set<UIListItemKey> alive;
                        for ( int i = 0; i < boundRows->Size(); ++i )
                            alive.insert( RecordItemKey( *boundRows, i ) );
                        (void)PruneSelection( listState->Selection,
                                              [&alive]( UIListItemKey k ) { return alive.contains( k ); } );
                    }
                }
            }
            else if ( lv.Collection.empty() && !listState->Selection.Selected.empty() )
            {
                // Child rows: a selected child that was destroyed or moved out of this list is not selected.
                (void)PruneSelection( listState->Selection,
                                      [&tree, e]( UIListItemKey k )
                                      {
                                          const auto n = static_cast<NodeId>( static_cast<std::uint32_t>( k - 1 ) );
                                          return tree.Valid( n ) && tree.Parent( n ) == e;
                                      } );
            }
            dl.AddRectFilled( { rect.X, rect.Y }, { rect.X + rect.W, rect.Y + rect.H },
                              glm::vec4( st.Color( StyleSlot::ScrollViewBackground, lv.Background ), 1.0f ) );

            if ( wheelOver && input->ScrollDelta != 0.0f )
            {
                lv.ScrollY -= input->ScrollDelta * 30.0f; // the scroll view's notch, exactly
            }

            const auto solve = [&]
            {
                window = SolveListWindow( shape.Count, lv.ItemHeight, lv.Spacing, lv.Overscan, lv.ScrollY, rect.H,
                                          scale, shape.Columns, shape.ItemWidth );
            };
            solve();

            // SELECTION, EXPANSION AND THE LIST'S KEYBOARD (UI-LIST+). After the solve, because a click is
            // resolved against the rows where they are drawn this frame; anything that changes the rows or
            // the offset solves again, so the picture below is the post-input one.
            if ( HandleListInput( frame, lv, shape, window, *listState, hoverRow ) )
            {
                resolveRows();
                solve();
            }

            // Written back CLAMPED, from the one place that knows the content height — which here
            // is derived from the child count and not authored, so it cannot be stale.
            lv.ScrollY = scale > 0.0f ? window.ScrollPx / scale : 0.0f;
            if ( boundRows != nullptr )
                ctx.Canvas.ListBindings[e].AtEnd = window.ScrollPx >= window.ScrollMaxPx - 0.5f;

            contentPx      = window.ContentPx;
            scrollPx       = window.ScrollPx;
            scrollMaxPx    = window.ScrollMaxPx;
            showScrollbar  = lv.ShowScrollbar;
            scrollbarColor = st.Color( StyleSlot::ScrollViewScrollbar, lv.ScrollbarColor );
            clip           = true;
        }

        // The visible box of a scrolling container, for scroll-to-focus (UIFocus.hpp ScrollIntoView): screen
        // pixels per design pixel of ScrollY is the canvas scale times whatever the transform does to height.
        if ( tree.Has<UIScrollViewData>( e ) || isList )
        {
            const Rect screen = ScreenBoundsOf( dl, rect );
            ctx.View.ScrollPorts.push_back( { e, screen, rect.H > 0.0f ? scale * ( screen.H / rect.H ) : scale } );
        }

        if ( clip )
            dl.PushClipRect( { rect.X, rect.Y }, { rect.X + rect.W, rect.Y + rect.H } );

        // Children inherit the clip for hit testing too, so what is scrolled out of view can't be
        // clicked through its viewport. Narrowed by THE SAME FUNCTION DrawList2D::PushClipRect just
        // called, with the same matrix and the same rect — the pointer is refused exactly where the
        // geometry was cut, because there is one implementation of "where" and not two that agree.
        // The return value is ignored on purpose: the draw list has already logged an inexact
        // region, and both halves get the same superset either way.
        Graphic::Render2D::ClipRegion2D childClip = clipRegion;
        if ( clip )
            (void)Graphic::Render2D::IntersectClipRegion( childClip, dl.GetTransform(), { rect.X, rect.Y },
                                                          { rect.X + rect.W, rect.Y + rect.H } );

        const ChildRange children = ChildrenOf( tree, e );
        if ( isList )
        {
            // THE WHOLE POINT, AND IT IS FOUR LINES. Everything outside [First, Last] is never
            // reached, so it costs no rect, no style, no tween, no hit test and no vertex — and,
            // because asking is also the demand, a UIRenderTexture row that left the window has
            // its capture destroyed and its renderer slot returned with no code at this site.
            const bool bound = !tree.Get<UIListViewData>( e )->Collection.empty();
            if ( bound && children.size() != 1 && ctx.Canvas.WarnedListTemplates.insert( e ).second )
            {
                LOG_WARN( "[UI] list {} is bound to collection '{}' and has {} children; a bound list "
                          "draws its ONE child as the entry template, so it draws no rows",
                          static_cast<std::uint32_t>( e ), tree.Get<UIListViewData>( e )->Collection,
                          children.size() );
            }
            if ( bound )
            {
                // ONE ENTITY, MANY ROWS: the template is walked once per record in the window with
                // that record answering its bindings, so a record costs a row only while it is on
                // screen. The previous record is restored rather than cleared, for a bound list
                // nested inside another one's row.
                const NodeId entry = children.size() == 1 ? children.front() : NodeId::Null;
                if ( shape.Count > 0 && tree.Valid( entry ) )
                {
                    const UIDataStore* outer = ctx.Canvas.RowRecord;
                    for ( int i = window.First; i <= window.Last; ++i )
                    {
                        ctx.Canvas.RowRecord = shape.RowRecord( i );
                        Rect rowRect         = ListRowRect( rect, window, i );
                        DrawListRowBackdrop( frame, shape, *listState, i, rowRect, hoverRow );
                        if ( shape.Tree != nullptr )
                            rowRect = TreeContentRect( rowRect, shape.Rows()[static_cast<std::size_t>( i )].Depth,
                                                       shape.Tree->Indent * scale );
                        DrawElement( ctx, tree, entry, rect, scale, dl, input, outClicked, focused, popups,
                                     focusables, childClip, childScope, &rowRect );
                        DrawListRowFront( frame, shape, *listState, i, ListRowRect( rect, window, i ) );
                    }
                    ctx.Canvas.RowRecord = outer;
                }
            }
            for ( int i = window.First; !bound && i <= window.Last; ++i )
            {
                const NodeId c = children[static_cast<std::size_t>( i )];
                if ( !tree.Valid( c ) )
                {
                    continue;
                }
                const Rect rowRect = ListRowRect( rect, window, i );
                DrawListRowBackdrop( frame, shape, *listState, i, rowRect, hoverRow );
                DrawElement( ctx, tree, c, rect, scale, dl, input, outClicked, focused, popups, focusables,
                             childClip, childScope, &rowRect );
                DrawListRowFront( frame, shape, *listState, i, rowRect );
            }
        }
        else if ( tree.Has<UILayoutGroupData>( e ) )
        {
            // Auto-layout: the group positions + sizes its children (overriding their anchors). Each
            // child's preferred size = CustomMinimumSize, else its authored offset size (design px).
            // THE LAYOUT AXIS lives in GatherLayoutSlots: a Collapsed child is not given a slot, so every
            // sibling after it moves up by that slot's size plus the spacing. A Hidden one is kept there and
            // stopped at the top of DrawElement, which is what leaves its hole open. The same gather and
            // arrange serve the pick (UICanvasLayout), so a child is clicked where it is drawn.
            const auto&         g = *tree.Get<UILayoutGroupData>( e );
            std::vector<NodeId> kids;
            const auto          slots    = GatherLayoutSlots( tree, e, scale, &kids );
            const auto          arranged = ArrangeLayoutGroup( childParent, GroupParams( g, st, scale ), slots );
            // A ScaleBox hands its child a layout scale: text, padding and grandchildren scale with it.
            for ( std::size_t i = 0; i < kids.size(); ++i )
                DrawElement( ctx, tree, kids[i], childParent, scale * arranged[i].Scale, dl, input, outClicked,
                             focused, popups, focusables, childClip, childScope, &arranged[i].R );
        }
        else
        {
            for ( auto c : children )
                if ( tree.Valid( c ) )
                    DrawElement( ctx, tree, c, childParent, scale, dl, input, outClicked, focused, popups,
                                 focusables, childClip, childScope );
        }
        if ( clip )
            dl.PopClipRect();

        // Scroll thumb on the right edge (outside the clip), shown only when the content overflows.
        // ONE BLOCK FOR BOTH CONTAINERS: the thumb is a function of (content, viewport, offset) and
        // nothing else, and a second copy of it for the list would be a second thing to keep in
        // step with the first.
        if ( showScrollbar && scrollMaxPx > 0.0f && contentPx > 0.0f )
        {
            const float barW   = 6.0f * scale;
            const float trackX = rect.X + rect.W - barW;
            const float thumbH = std::max( barW * 2.0f, rect.H * ( rect.H / contentPx ) );
            const float t      = scrollPx / scrollMaxPx;
            const float thumbY = rect.Y + t * ( rect.H - thumbH );
            dl.AddRectFilled( { trackX, thumbY }, { rect.X + rect.W, thumbY + thumbH },
                              glm::vec4( scrollbarColor, 1.0f ), barW * 0.5f );

            // THE THUMB IS DRAGGED (SScrollBar): a press on the thumb grabs it where it was pressed; a press
            // on the track elsewhere centres the thumb under the pointer and grabs it there. While the button
            // is held the drag owns the pointer, wherever it goes — the capture a slider-by-hover would lose
            // the moment the pointer slid off the bar. The offset lands next frame, as every hot-driven
            // control's does.
            auto&      drag    = ctx.View.ThumbDrag;
            const auto pointer = frame.PointerPx;
            const bool press   = input != nullptr && input->MouseDown && !ctx.View.PrevDown;
            if ( press && interactive && HotWithin( tree, ctx.View.Hot, e ) && pointer.x >= trackX &&
                 pointer.x <= rect.X + rect.W && pointer.y >= rect.Y && pointer.y <= rect.Y + rect.H )
            {
                drag.Owner  = e;
                drag.GrabPx = ( pointer.y >= thumbY && pointer.y <= thumbY + thumbH ) ? pointer.y - thumbY
                                                                                        : thumbH * 0.5f;
            }
            if ( drag.Owner == e )
            {
                if ( input == nullptr || !input->MouseDown )
                    drag.Owner = NodeId::Null;
                else
                {
                    const float travel = std::max( 1.0f, rect.H - thumbH );
                    const float nt     = std::clamp( ( pointer.y - drag.GrabPx - rect.Y ) / travel, 0.0f, 1.0f );
                    const float design = scale > 0.0f ? nt * scrollMaxPx / scale : 0.0f;
                    if ( auto* sv = tree.GetState<UIScrollViewData>( e ) )
                        sv->ScrollY = design;
                    else if ( auto* lv = tree.GetState<UIListViewData>( e ) )
                        lv->ScrollY = design;
                }
            }
        }
        else if ( ctx.View.ThumbDrag.Owner == e )
            ctx.View.ThumbDrag.Owner = NodeId::Null; // the content stopped overflowing under the drag
    }
} // namespace Desert::UI::Walk
