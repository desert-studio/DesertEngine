#include <Engine/UI/Widgets/UIWidgets.hpp>

#include <Engine/UI/UICanvasRenderer2D.hpp>
#include <Engine/UI/UIOverlay.hpp>
#include <Engine/Assets/Common.hpp>
#include <Engine/Text/BakedFont.hpp>
#include <Engine/UI/UIStyleResolver.hpp>
#include <Engine/Text/Utf8.hpp>
#include <Engine/UI/UICanvasLayout.hpp>
#include <Engine/UI/UIDataStore.hpp>
#include <Engine/UI/UIPathGeometry.hpp>
#include <Engine/UI/UIRichText.hpp>

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

namespace Desert::UI::Walk
{
    void DrawChildren( ElementFrame& frame )
    {
        auto& ctx         = frame.Ctx;
        auto& tree         = frame.Tree;
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
        const bool wheelOver = input != nullptr && interactive && e == ctx.View.Hot;
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
        const bool          isList = tree.Has<UIListViewData>( e );
        ListWindow          window;
        const UICollection* boundRows = nullptr;
        if ( isList )
        {
            auto& lv = *tree.GetState<UIListViewData>( e );
            // Bound (UIL1): the row count is the collection's, and a list whose collection has not
            // been written yet is an empty list — the same answer a binding to an unwritten key
            // gives, the authored state until gameplay says otherwise.
            boundRows = lv.Collection.empty() ? nullptr : UIDataStore::Get().FindCollection( lv.Collection );
            std::size_t rowCount = tree.ChildCount( e );
            if ( !lv.Collection.empty() )
                rowCount = boundRows != nullptr ? static_cast<std::size_t>( boundRows->Size() ) : 0;
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
                    const float pitch = std::max( 1.0f, lv.ItemHeight ) + std::max( 0.0f, lv.Spacing );
                    if ( seen.Serial != 0 )
                        lv.ScrollY =
                             AnchorListScroll( lv.ScrollY, pitch, lv.FollowEnd, seen.AtEnd, complete, changes );
                    seen.Serial     = boundRows->Serial();
                    seen.Generation = boundRows->Generation();
                }
            }
            dl.AddRectFilled( { rect.X, rect.Y }, { rect.X + rect.W, rect.Y + rect.H },
                              glm::vec4( st.Color( StyleSlot::ScrollViewBackground, lv.Background ), 1.0f ) );

            if ( wheelOver && input->ScrollDelta != 0.0f )
            {
                lv.ScrollY -= input->ScrollDelta * 30.0f; // the scroll view's notch, exactly
            }

            window = SolveListWindow( static_cast<int>( rowCount ), lv.ItemHeight, lv.Spacing, lv.Overscan,
                                      lv.ScrollY, rect.H, scale );
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
                if ( boundRows != nullptr && tree.Valid( entry ) )
                {
                    const UIDataStore* outer = ctx.Canvas.RowRecord;
                    for ( int i = window.First; i <= window.Last; ++i )
                    {
                        ctx.Canvas.RowRecord = &boundRows->Record( i );
                        const Rect rowRect   = ListRowRect( rect, window, i );
                        DrawElement( ctx, tree, entry, rect, scale, dl, input, outClicked, focused, popups,
                                     focusables, childClip, childScope, &rowRect );
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
                DrawElement( ctx, tree, c, rect, scale, dl, input, outClicked, focused, popups, focusables,
                             childClip, childScope, &rowRect );
            }
        }
        else if ( tree.Has<UILayoutGroupData>( e ) )
        {
            // Auto-layout: the group positions + sizes its children (overriding their anchors). Each
            // child's preferred size = CustomMinimumSize, else its authored offset size (design px).
            const auto&               g = *tree.Get<UILayoutGroupData>( e );
            std::vector<NodeId> kids;
            std::vector<glm::vec2>    sizes;
            std::vector<float>        flex;
            for ( auto c : children )
            {
                // THE LAYOUT AXIS, and the only place it does anything: a Collapsed child is not
                // given a slot, so every sibling after it moves up by that slot's size plus the
                // spacing. A Hidden one is kept here and stopped at the top of DrawElement, which
                // is what leaves its hole open.
                if ( !tree.Valid( c ) || !TakesLayoutSpace( tree, c ) )
                    continue;
                glm::vec2 pref( 0.0f );
                float     fg = 0.0f;
                if ( tree.Has<UILayoutData>( c ) )
                {
                    const auto& L = *tree.Get<UILayoutData>( c );
                    pref          = glm::max( L.CustomMinimumSize, L.OffsetMax - L.OffsetMin );
                    fg            = L.FlexGrow;
                }
                kids.push_back( c );
                sizes.push_back( pref * scale );
                flex.push_back( fg );
            }

            LayoutGroupParams params = GroupParams( g, st, scale );
            params.StretchCross      = g.StretchCross;
            params.CellSize          = g.CellSize * scale;
            params.Columns           = g.Columns;

            const auto rects = SolveLayoutGroup( childParent, params, sizes, flex );
            for ( std::size_t i = 0; i < kids.size(); ++i )
                DrawElement( ctx, tree, kids[i], childParent, scale, dl, input, outClicked, focused, popups,
                             focusables, childClip, childScope, &rects[i] );
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
        }
    }
} // namespace Desert::UI::Walk
