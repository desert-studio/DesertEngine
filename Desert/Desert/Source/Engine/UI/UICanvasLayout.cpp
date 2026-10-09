#include "UICanvasLayout.hpp"

#include <Engine/Graphic/Render2D/ClipRegion2D.hpp>
#include <Engine/Graphic/Render2D/Transform2D.hpp>
#include <Engine/UI/UICanvasContext.hpp>
#include <Engine/UI/UIDataStore.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace Desert::UI
{
    NodeId CanvasOf( const IUITree& tree, NodeId e )
    {
        // Bounded by the node count rather than trusting the tree to be acyclic: a Parent cycle is
        // authorable (the hierarchy panel can reparent), and an unbounded walk here would hang the editor
        // instead of returning "not under a canvas".
        const std::size_t limit = tree.NodeBound() + 1;
        std::size_t       steps = 0;
        for ( NodeId cur = e; cur != NodeId::Null && tree.Valid( cur ) && steps < limit; ++steps )
        {
            if ( tree.Has<UICanvasData>( cur ) )
                return cur;
            cur = tree.Parent( cur );
        }
        return NodeId::Null;
    }

    std::size_t CanvasCount( const IUITree& tree )
    {
        std::vector<NodeId> canvases;
        tree.Roots( ArgKind::Canvas, canvases );
        return canvases.size();
    }

    std::vector<NodeId> CanvasesInDrawOrder( const IUITree& tree )
    {
        // Two canvases with the same authored Sort Order are decided by the order the tree authored them
        // (IUITree::Roots' contract) — for a level being loaded, the order the file lists them. The sort is
        // STABLE so that tie-break is kept rather than re-derived; the ECS adapter is what turns entt's
        // pool order (measured: the reverse of creation) into authored order, and
        // UICanvasContextPair.CanvasesAreOrderedByTheirAuthoredSortOrder holds both halves.
        std::vector<NodeId> out;
        tree.Roots( ArgKind::Canvas, out );
        std::stable_sort(
             out.begin(), out.end(), [&tree]( NodeId a, NodeId b )
             { return tree.Get<UICanvasData>( a )->SortOrder < tree.Get<UICanvasData>( b )->SortOrder; } );
        return out;
    }

    Common::ResultStr<NodeId> SoleCanvas( const IUITree& tree )
    {
        std::vector<NodeId> canvases;
        tree.Roots( ArgKind::Canvas, canvases );
        const std::size_t n = canvases.size();
        if ( n == 1 )
            return Common::MakeSuccess( canvases.front() );
        if ( n == 0 )
            return Common::MakeFormattedError<NodeId>(
                 "[UI] the scene has no UI canvas, so no host can be given one to draw or measure" );
        return Common::MakeFormattedError<NodeId>(
             "[UI] the scene has {} UI canvases and this host did not name one; it is NOT the first one's "
             "job to win by iteration order — name the canvas (UI::CanvasOf on an element of it, or the "
             "host's own document/subject)",
             n );
    }

    LayoutGroupParams LayoutParamsOf( const UILayoutGroupData& g, float scale )
    {
        LayoutGroupParams p;
        switch ( g.Type )
        {
        case UILayoutType::Vertical: p.Type = LayoutGroupType::Vertical; break;
        case UILayoutType::Horizontal: p.Type = LayoutGroupType::Horizontal; break;
        case UILayoutType::Grid: p.Type = LayoutGroupType::Grid; break;
        case UILayoutType::Wrap: p.Type = LayoutGroupType::Wrap; break;
        case UILayoutType::Overlay: p.Type = LayoutGroupType::Overlay; break;
        case UILayoutType::UniformGrid: p.Type = LayoutGroupType::UniformGrid; break;
        case UILayoutType::SizeBox: p.Type = LayoutGroupType::SizeBox; break;
        case UILayoutType::ScaleBox: p.Type = LayoutGroupType::ScaleBox; break;
        }
        p.PaddingL     = g.Padding.x * scale;
        p.PaddingT     = g.Padding.y * scale;
        p.PaddingR     = g.Padding.z * scale;
        p.PaddingB     = g.Padding.w * scale;
        p.Spacing      = g.Spacing * scale;
        p.StretchCross = g.StretchCross;
        p.CellSize     = g.CellSize * scale;
        p.Columns      = g.Columns;
        p.WrapSize     = g.WrapSize * scale;
        p.WrapVertical = g.WrapVertical;
        p.MinSlotSize  = g.MinSlotSize * scale;
        // An unset (negative) SizeBox bound stays negative after scaling, which is all "unset" needs.
        p.SizeMin          = g.SizeMin * scale;
        p.SizeMax          = g.SizeMax * scale;
        p.SizeOverride     = g.SizeOverride * scale;
        p.Stretch          = static_cast<LayoutScaleStretch>( g.Stretch );
        p.StretchDirection = static_cast<LayoutScaleDirection>( g.StretchDirection );
        p.UserScale        = g.UserScale;
        return p;
    }

    glm::vec2 SlotPreferredPx( const IUITree& tree, NodeId e, float scale )
    {
        glm::vec2 pref( 0.0f );
        if ( tree.Has<UILayoutData>( e ) )
        {
            const auto& L = *tree.Get<UILayoutData>( e );
            pref          = glm::max( L.CustomMinimumSize, L.OffsetMax - L.OffsetMin ) * scale;
        }
        if ( tree.Has<UILayoutGroupData>( e ) )
        {
            const auto& g = *tree.Get<UILayoutGroupData>( e );
            if ( g.Type == UILayoutType::SizeBox )
            {
                std::vector<glm::vec2> sizes;
                for ( const LayoutSlot& s : GatherLayoutSlots( tree, e, scale ) )
                    sizes.push_back( s.Pref );
                pref = MeasureLayoutGroup( LayoutParamsOf( g, scale ), sizes );
            }
        }
        return pref;
    }

    std::vector<LayoutSlot> GatherLayoutSlots( const IUITree& tree, NodeId e, float scale, std::vector<NodeId>* kids )
    {
        std::vector<LayoutSlot> slots;
        for ( std::size_t i = 0, count = tree.ChildCount( e ); i < count; ++i )
        {
            const NodeId c = tree.ChildAt( e, i );
            if ( !tree.Valid( c ) || !TakesLayoutSpace( tree, c ) )
                continue; // Collapsed: no slot, so no place in the group and no part of its content size
            LayoutSlot s;
            s.Pref = SlotPreferredPx( tree, c, scale );
            if ( tree.Has<UILayoutData>( c ) )
            {
                const auto& L = *tree.Get<UILayoutData>( c );
                s.Min         = L.CustomMinimumSize * scale;
                s.Grow        = L.FlexGrow;
                s.Shrink      = L.FlexShrink;
            }
            slots.push_back( s );
            if ( kids )
                kids->push_back( c );
        }
        return slots;
    }

    bool TakesLayoutSpace( const IUITree& tree, NodeId e )
    {
        const auto* L = tree.Valid( e ) ? tree.Get<UILayoutData>( e ) : nullptr;
        return L == nullptr || L->Visibility != UIVisibility::Collapsed;
    }

    bool IsElementVisible( const IUITree& tree, NodeId e )
    {
        const auto* L = tree.Valid( e ) ? tree.Get<UILayoutData>( e ) : nullptr;
        return L == nullptr || L->Visibility == UIVisibility::Visible;
    }

    namespace
    {
        // Maps the canvas to the viewport per its scale mode (see UICanvasScaleMode). Returns the canvas root
        // rect (screen px) + a uniform scale applied to every element's offsets/min-size/font — so Stretch is
        // 1:1 (anchors drive layout, no resize zoom), ScaleWithScreen scales the whole design from the
        // reference resolution, and Letterbox fits + centres it. Mirrors RenderCanvas2D's own ResolveCanvas.
        struct CanvasFit
        {
            Rect  Root;
            float Scale;
        };

        CanvasFit ResolveCanvas( const UICanvasData& d, const Rect& viewportPx )
        {
            switch ( d.ScaleMode )
            {
                case UICanvasScaleMode::ScaleWithScreen:
                {
                    const float sx = d.ReferenceWidth > 0.0f ? viewportPx.W / d.ReferenceWidth : 1.0f;
                    const float sy = d.ReferenceHeight > 0.0f ? viewportPx.H / d.ReferenceHeight : 1.0f;
                    const float m  = std::clamp( d.MatchWidthHeight, 0.0f, 1.0f );
                    return { viewportPx, sx * ( 1.0f - m ) + sy * m };
                }
                case UICanvasScaleMode::Letterbox:
                {
                    const Rect fit = CanvasRect( d.ReferenceWidth, d.ReferenceHeight, viewportPx.W, viewportPx.H );
                    const float scale = d.ReferenceWidth > 0.0f ? fit.W / d.ReferenceWidth : 1.0f;
                    return { Rect{ viewportPx.X + fit.X, viewportPx.Y + fit.Y, fit.W, fit.H }, scale };
                }
                case UICanvasScaleMode::Stretch:
                default:
                    return { viewportPx, 1.0f }; // canvas == viewport, 1:1 px
            }
        }

        // Resolves the root rect of the canvas the caller NAMED, exactly like the renderer. Shared by
        // PickElement / GetElementRect / CanvasScale so hit-testing matches drawing.
        //
        // The three refusals are distinct on purpose. "You named something that is not a canvas" is a caller
        // bug and reads nothing like "this canvas is switched off", and both used to arrive as the same
        // `false` — on top of a canvas nobody had named in the first place.
        Common::ResultStr<CanvasFit> ResolveNamedCanvas( const IUITree& tree, NodeId canvas,
                                                         const Rect& viewportPx )
        {
            if ( canvas == NodeId::Null || !tree.Valid( canvas ) )
                return Common::MakeFormattedError<CanvasFit>(
                     "[UI] no canvas was named for this layout query (entity {})",
                     static_cast<std::uint32_t>( canvas ) );
            if ( !tree.Has<UICanvasData>( canvas ) )
                return Common::MakeFormattedError<CanvasFit>(
                     "[UI] entity {} was named as a canvas but carries no UICanvasComponent",
                     static_cast<std::uint32_t>( canvas ) );

            const auto& canvasData = *tree.Get<UICanvasData>( canvas );
            if ( !canvasData.Visible )
                return Common::MakeFormattedError<CanvasFit>(
                     "[UI] canvas {} is not Visible, so it has no on-screen layout to report",
                     static_cast<std::uint32_t>( canvas ) );

            return Common::MakeSuccess( ResolveCanvas( canvasData, viewportPx ) );
        }

        // Content size (px) a layout-group container needs to hug its children — mirrors the renderer's
        // GroupContentPx so the Content Size Fitter picks with the same rect it draws.
        glm::vec2 GroupContentPx( const IUITree& tree, NodeId e, float scale )
        {
            if ( !tree.Has<UILayoutGroupData>( e ) )
                return { 0.0f, 0.0f };
            std::vector<glm::vec2> sizes;
            for ( const LayoutSlot& s : GatherLayoutSlots( tree, e, scale ) )
                sizes.push_back( s.Pref );
            return MeasureLayoutGroup( LayoutParamsOf( *tree.Get<UILayoutGroupData>( e ), scale ), sizes );
        }

        // If `e` is an auto-layout container, solve its children's rects exactly like the renderer's
        // DrawElement does — so hit-testing / handles match the drawn positions (children of a group are
        // placed by the group, NOT their own anchors). Fills kids + one arranged slot each (rect + the layout
        // scale a ScaleBox hands its child); empty when not a group.
        void SolveGroupChildren( const IUITree& tree, NodeId e, const Rect& container, float scale,
                                 std::vector<NodeId>& kids, std::vector<ArrangedSlot>& arranged )
        {
            if ( !tree.Has<UILayoutGroupData>( e ) )
                return;
            const auto slots = GatherLayoutSlots( tree, e, scale, &kids );
            arranged = ArrangeLayoutGroup( container, LayoutParamsOf( *tree.Get<UILayoutGroupData>( e ), scale ),
                                           slots );
        }

        // The element's accumulated transform: its parent's, with its own composed inside it. THE SAME
        // COMPOSITION ORDER THE RENDERER USES (DrawList2D::PushTransform), because a pick that composed
        // the other way round would be right for one level and wrong for two.
        glm::mat3 AccumulateTransform( const IUITree& tree, NodeId e, const Rect& rect,
                                       const glm::mat3& parentXform )
        {
            if ( !tree.Has<UILayoutData>( e ) )
                return parentXform;
            const auto& L = *tree.Get<UILayoutData>( e );
            if ( L.Rotation == 0.0f && L.Scale == glm::vec2( 1.0f, 1.0f ) )
                return parentXform;
            const glm::vec2 pivotPx( rect.X + L.Pivot.x * rect.W, rect.Y + L.Pivot.y * rect.H );
            return parentXform * Graphic::Render2D::MakeTransform2D( pivotPx, L.Rotation, L.Scale );
        }

        // The pointer brought into @p xform's space — the inverse of what the geometry went through, so
        // "is the cursor inside this element" is asked about the rect the element actually has.
        glm::vec2 UndoTransform( const glm::mat3& xform, const glm::vec2& p )
        {
            return Graphic::Render2D::IsIdentity2D( xform )
                        ? p
                        : Graphic::Render2D::TransformPoint2D( Graphic::Render2D::InverseTransform2D( xform ), p );
        }

        // Two axis-aligned boxes, intersected. Used for the BOX half of a clip region — what the scissor
        // cuts — and for nothing else: the oblique half is IntersectClipRegion's and there is no second
        // implementation of it here.
        Rect IntersectRectPx( const Rect& a, const Rect& b )
        {
            const float x0 = std::max( a.X, b.X );
            const float y0 = std::max( a.Y, b.Y );
            const float x1 = std::min( a.X + a.W, b.X + b.W );
            const float y1 = std::min( a.Y + a.H, b.Y + b.H );
            return Rect{ x0, y0, std::max( 0.0f, x1 - x0 ), std::max( 0.0f, y1 - y0 ) };
        }

        // The axis-aligned box @p r occupies on screen once @p xform has acted on it — the same box
        // DrawList2D's clip region keeps for a rotated clipper, because that is the half of the clip the
        // hardware scissor can express.
        Rect ScreenBoundsOf( const glm::mat3& xform, const Rect& r )
        {
            if ( Graphic::Render2D::IsIdentity2D( xform ) )
                return r;
            glm::vec2 mn, mx;
            Graphic::Render2D::TransformedAABB2D( xform, { r.X, r.Y }, { r.X + r.W, r.Y + r.H }, mn, mx );
            return Rect{ mn.x, mn.y, mx.x - mn.x, mx.y - mn.y };
        }

        // The four screen-space corners of @p r under @p xform, in order — the shape a rotated clipper
        // actually has to be asked about, as opposed to the box around it.
        void ScreenQuadOf( const glm::mat3& xform, const Rect& r, glm::vec2 ( &out )[4] )
        {
            out[0] = Graphic::Render2D::TransformPoint2D( xform, { r.X, r.Y } );
            out[1] = Graphic::Render2D::TransformPoint2D( xform, { r.X + r.W, r.Y } );
            out[2] = Graphic::Render2D::TransformPoint2D( xform, { r.X + r.W, r.Y + r.H } );
            out[3] = Graphic::Render2D::TransformPoint2D( xform, { r.X, r.Y + r.H } );
        }

        // Everything one level of the walk inherits from the level above it.
        struct EnumScope
        {
            Rect Parent{}; // the box children lay out in (already scrolled, for a list)

            // The inherited clip, as the REGION the draw list cuts by — box plus the oblique half-planes a
            // scissor cannot express. Narrowed only through IntersectClipRegion, which is the same call
            // UICanvasRenderer2D makes beside DrawList2D::PushClipRect: one implementation of "where the
            // clip is", read forward by the geometry and pointwise by the pointer.
            Graphic::Render2D::ClipRegion2D Clip{};

            glm::mat3    Xform{ 1.0f };    // ancestors' composed render transform
            int          Depth        = 0; // 0 for a direct child of the canvas
            NodeId       ParentEntity = NodeId::Null;
            bool         Elect        = true;       // may the pointer stop anywhere in here
            NodeId       SkippedBy    = NodeId::Null; // the ancestor that stopped the walk, if one did

            // Set by a UIListView on the rows its window does not cover. It is the ROW's own reason and
            // not an inherited one, so it lives on the scope the parent builds per child and is never
            // copied further down: a grandchild of a windowed-out row is AncestorSkipped, which is what
            // every other self-cause already does one level up.
            bool OutsideWindow = false;

            // Inside a bound list's row (UIL1): the record answering this subtree's bindings and its
            // index. Inherited all the way down, unlike OutsideWindow — every element of the entry
            // template belongs to the row.
            const UIDataStore* Row      = nullptr;
            int                RowIndex = -1;
        };

        // A CANVAS IS A TREE AND THIS WALK IS ITS TRAVERSAL; the depth is the authored nesting, and the
        // one unbounded case — a Parent cycle — is refused by the step-limited walks that answer "which
        // canvas is this" rather than by this one.
        // NOLINTNEXTLINE(misc-no-recursion)
        void EnumRecurse( const IUITree& tree, NodeId e, const EnumScope& scope, float scale,
                          const Rect& viewportPx, const UICanvasContext* ctx, std::vector<UIElementNode>& out,
                          int& order, const Rect* forcedRect )
        {
            const bool hasLayout = tree.Has<UILayoutData>( e );

            UIElementNode node;
            node.Entity    = e;
            node.ListRow   = scope.RowIndex;
            node.Parent    = scope.ParentEntity;
            node.Depth     = scope.Depth;
            node.TakesSlot = TakesLayoutSpace( tree, e );
            node.OwnRect   = ( forcedRect != nullptr ) || hasLayout;
            node.RectValid = true;

            // WHY THIS ELEMENT IS NOT DRAWN, in the order the walk asks it. An ancestor that already
            // stopped wins over anything this element says about itself: the walk never reached it, so
            // its own Visibility was never read and reporting it would invent a reason.
            const UIHitTest hitTest = hasLayout ? tree.Get<UILayoutData>( e )->HitTest : UIHitTest::All;
            node.HitTest = hitTest;
            node.ElectsSelf = scope.Elect && ( hitTest == UIHitTest::All || hitTest == UIHitTest::Blocking );

            UISkipCause self = UISkipCause::None;
            if ( hasLayout )
            {
                switch ( tree.Get<UILayoutData>( e )->Visibility )
                {
                    case UIVisibility::Hidden:
                        self = UISkipCause::SelfHidden;
                        break;
                    case UIVisibility::Collapsed:
                        self = UISkipCause::SelfCollapsed;
                        break;
                    default:
                        break;
                }
            }
            // The list's window, asked BEFORE the element's own visibility, because it is the container
            // deciding not to walk this row at all — the same precedence an ancestor already has over a
            // child's own Visibility, one level down.
            if ( self == UISkipCause::None && scope.OutsideWindow )
            {
                self = UISkipCause::OutsideWindow;
            }

            // The two view-dependent reasons. Without a context they are not asked at all — see the header:
            // an author must be able to pick an element on a screen the game is not showing.
            if ( self == UISkipCause::None && ctx != nullptr && tree.Has<UIScreenData>( e ) )
            {
                const std::string& name      = tree.Get<UIScreenData>( e )->Name;
                const bool         isCurrent = ( name == ctx->Screen );
                const bool         isLeaving = ( name == ctx->ScreenFrom && ctx->ScreenT < 1.0f );
                if ( !isCurrent && !isLeaving )
                    self = UISkipCause::ScreenNotCurrent;
            }
            if ( self == UISkipCause::None && ( ctx != nullptr || scope.Row != nullptr ) &&
                 BindingHidesElement( tree, e, ctx, scope.Row ) )
                self = UISkipCause::BindingHidden;

            if ( scope.SkippedBy != NodeId::Null )
            {
                node.Cause   = UISkipCause::AncestorSkipped;
                node.CauseBy = scope.SkippedBy;
            }
            else if ( self != UISkipCause::None )
            {
                node.Cause   = self;
                node.CauseBy = e;
            }
            else
            {
                node.Drawn = true;
                node.Order = order++;
            }

            // --- The rect, resolved exactly as the renderer resolves it --------------------------------
            Rect rect = scope.Parent;
            if ( forcedRect )
                rect = *forcedRect; // positioned + sized by the parent's auto-layout group
            else if ( hasLayout )
            {
                const auto& L = *tree.Get<UILayoutData>( e );
                rect          = ResolveRect( L.AnchorMin, L.AnchorMax, L.OffsetMin * scale, L.OffsetMax * scale,
                                             L.CustomMinimumSize * scale, scope.Parent );
            }
            if ( hasLayout )
            {
                const auto& L = *tree.Get<UILayoutData>( e );
                rect          = ApplyAspectFit( rect, L.AspectRatio, static_cast<int>( L.AspectMode ) );
                if ( ( L.FitWidth || L.FitHeight ) && tree.Has<UILayoutGroupData>( e ) )
                {
                    const glm::vec2 content = GroupContentPx( tree, e, scale );
                    if ( L.FitWidth )
                        rect.W = content.x;
                    if ( L.FitHeight )
                        rect.H = content.y;
                }
            }

            const glm::mat3 xform = AccumulateTransform( tree, e, rect, scope.Xform );
            node.RectPx           = rect;
            node.Xform            = xform;
            node.ScreenPx         = ScreenBoundsOf( xform, rect );
            node.ClipRegion       = scope.Clip;

            // The pixels this element may actually own: its screen box, cut by every clip above it and by
            // the viewport. The walk does not cull, so a drawn element whose VisiblePx is empty still costs
            // vertices and a draw call — which is a finding, and the reason this is a stored field rather
            // than something the panel recomputes.
            const Rect clipBox = Rect{ scope.Clip.Box.x, scope.Clip.Box.y, scope.Clip.Box.z, scope.Clip.Box.w };
            node.VisiblePx     = IntersectRectPx( node.ScreenPx, IntersectRectPx( clipBox, viewportPx ) );

            // Fully cut is TWO questions once a clipper may be rotated, and asking only the first is how a
            // box-shaped middle link would drop the oblique half: an element a turned clipper removed
            // entirely still has a non-empty box intersection with that clipper's box. The quad is only
            // built when there is an oblique plane to ask, so an unrotated canvas pays nothing.
            bool cutByPlanes = false;
            if ( node.Drawn && scope.Clip.PlaneCount > 0 )
            {
                glm::vec2 quad[4];
                ScreenQuadOf( xform, rect, quad );
                cutByPlanes = Graphic::Render2D::ClipPlanesRejectQuad( scope.Clip, quad );
            }
            node.Clipped = node.Drawn && ( node.VisiblePx.W <= 0.0f || node.VisiblePx.H <= 0.0f || cutByPlanes );

            // --- Children ------------------------------------------------------------------------------
            Rect childParent = rect;
            bool clip        = hasLayout && tree.Get<UILayoutData>( e )->ClipContents;

            if ( tree.Has<UIScrollViewData>( e ) )
            {
                // READ-ONLY where the renderer writes. The walk clamps ScrollY back into the component
                // because it also consumes wheel input; a query must not, or opening a debug panel would
                // silently edit the scene. Clamping the value locally gives the same number for any
                // ScrollY the renderer could have left behind.
                const auto& sv        = *tree.Get<UIScrollViewData>( e );
                const float scrollMax = std::max( 0.0f, sv.ContentHeight * scale - rect.H );
                const float maxDesign = scale > 0.0f ? scrollMax / scale : 0.0f;
                childParent.Y -= std::clamp( sv.ScrollY, 0.0f, maxDesign ) * scale;
                clip = true;
            }

            // The virtualized list (Ю17) clips exactly as the scroll view does; its rows are placed from
            // the window solved below rather than from childParent, so nothing is shifted here.
            ListWindow listWindow;
            const bool          isList    = tree.Has<UIListViewData>( e );
            const UICollection* boundRows = nullptr;
            if ( isList )
            {
                // READ-ONLY where the renderer writes, for the reason the scroll view states above: a
                // query must not edit the scene. SolveListWindow clamps the offset it was given, so the
                // number used here is the same one for any ScrollY the renderer could have left behind.
                const auto& lv = *tree.Get<UIListViewData>( e );
                boundRows = lv.Collection.empty() ? nullptr : UIDataStore::Get().FindCollection( lv.Collection );
                std::size_t n  = tree.ChildCount( e );
                if ( !lv.Collection.empty() )
                    n = static_cast<std::size_t>( boundRows != nullptr ? boundRows->Size() : 0 );
                listWindow = SolveListWindow( static_cast<int>( n ), lv.ItemHeight, lv.Spacing, lv.Overscan,
                                              lv.ScrollY, rect.H, scale );
                clip       = true;
            }
            node.ClipsChildren = clip;

            // Narrowed by THE SAME FUNCTION UICanvasRenderer2D calls beside DrawList2D::PushClipRect, with
            // the same matrix and the same rect — so the pointer is refused exactly where the geometry was
            // cut, because there is one implementation of "where" and not two that agree. The return value
            // is ignored on purpose: the draw list has already logged an inexact region, and both halves
            // get the same superset either way.
            Graphic::Render2D::ClipRegion2D childClip = scope.Clip;
            if ( clip )
                (void)Graphic::Render2D::IntersectClipRegion( childClip, xform, { rect.X, rect.Y },
                                                              { rect.X + rect.W, rect.Y + rect.H } );

            out.push_back( node );

            const std::size_t childCount = tree.ChildCount( e );
            if ( childCount == 0 )
                return;

            EnumScope child;
            child.Parent       = childParent;
            child.Clip         = childClip;
            child.Xform        = xform;
            child.Depth        = scope.Depth + 1;
            child.ParentEntity = e;
            child.Elect     = scope.Elect && ( hitTest == UIHitTest::All || hitTest == UIHitTest::ChildrenOnly );
            child.SkippedBy    = scope.SkippedBy != NodeId::Null ? scope.SkippedBy : node.Drawn ? NodeId::Null : e;
            child.Row       = scope.Row;
            child.RowIndex  = scope.RowIndex;

            if ( isList && !tree.Get<UIListViewData>( e )->Collection.empty() )
            {
                // BOUND (UIL1): only the WINDOW's records are enumerated, each as one walk of the entry
                // template. Unlike the child rows below, an off-screen record has no element to report —
                // it is data, not a node — and enumerating the template once per record would be the
                // whole-collection walk, per query, that binding exists to avoid. The renderer walks the
                // same [First, Last] with the same record per row, so the two describe one frame.
                const NodeId entry = childCount == 1 ? tree.ChildAt( e, 0 ) : NodeId::Null;
                if ( boundRows != nullptr && tree.Valid( entry ) )
                {
                    for ( int i = listWindow.First; i <= listWindow.Last; ++i )
                    {
                        const Rect rowRect = ListRowRect( rect, listWindow, i );
                        EnumScope  row     = child;
                        row.Row            = &boundRows->Record( i );
                        row.RowIndex       = i;
                        EnumRecurse( tree, entry, row, scale, viewportPx, ctx, out, order, &rowRect );
                    }
                }
            }
            else if ( isList )
            {
                // EVERY row is enumerated, and only the window's rows are DRAWN. A query that reported the
                // window alone would lose the other nineteen thousand rows from the outliner and from the
                // UI Debugger — the answer somebody opened the panel to get is precisely "where is row
                // 12 000 and why can I not see it". The rect is real for all of them: a row's position is
                // arithmetic on its index, so an off-screen row can say where it is rather than refusing.
                for ( std::size_t i = 0; i < childCount; ++i )
                {
                    const NodeId rowNode = tree.ChildAt( e, i );
                    if ( !tree.Valid( rowNode ) )
                    {
                        continue;
                    }
                    const int  index   = static_cast<int>( i );
                    const Rect rowRect = ListRowRect( rect, listWindow, index );
                    EnumScope  row     = child;
                    row.OutsideWindow  = index < listWindow.First || index > listWindow.Last;
                    EnumRecurse( tree, rowNode, row, scale, viewportPx, ctx, out, order, &rowRect );
                }
            }
            else if ( tree.Has<UILayoutGroupData>( e ) )
            {
                std::vector<NodeId>       kids;
                std::vector<ArrangedSlot> arranged;
                SolveGroupChildren( tree, e, childParent, scale, kids, arranged );
                for ( std::size_t i = 0; i < kids.size(); ++i )
                    EnumRecurse( tree, kids[i], child, scale * arranged[i].Scale, viewportPx, ctx, out, order,
                                 &arranged[i].R );

                // A Collapsed child is given NO SLOT by the group, so it has no position to report — and
                // reporting it at its anchored rect would be a lie about where it is not. It is still
                // enumerated, because "this element exists and the group dropped it" is the answer
                // somebody opened the panel to get.
                for ( std::size_t i = 0; i < childCount; ++i )
                {
                    const NodeId c = tree.ChildAt( e, i );
                    if ( !tree.Valid( c ) || TakesLayoutSpace( tree, c ) )
                        continue;
                    UIElementNode slotless;
                    slotless.Entity = c;
                    slotless.Parent = e;
                    slotless.Depth  = child.Depth;
                    slotless.Cause      = child.SkippedBy != NodeId::Null ? UISkipCause::AncestorSkipped
                                                                          : UISkipCause::SelfCollapsed;
                    slotless.CauseBy    = child.SkippedBy != NodeId::Null ? child.SkippedBy : c;
                    slotless.ClipRegion = childClip;
                    slotless.TakesSlot = false;
                    out.push_back( slotless );
                }
            }
            else
            {
                for ( std::size_t i = 0; i < childCount; ++i )
                    if ( const NodeId c = tree.ChildAt( e, i ); tree.Valid( c ) )
                        EnumRecurse( tree, c, child, scale, viewportPx, ctx, out, order, nullptr );
            }
        }
    } // namespace

    const char* UISkipCauseName( UISkipCause cause )
    {
        switch ( cause )
        {
            case UISkipCause::None:
                return "drawn";
            case UISkipCause::SelfHidden:
                return "Hidden";
            case UISkipCause::SelfCollapsed:
                return "Collapsed";
            case UISkipCause::BindingHidden:
                return "binding says hidden";
            case UISkipCause::ScreenNotCurrent:
                return "screen not current";
            case UISkipCause::OutsideWindow:
                return "outside the list's window";
            case UISkipCause::AncestorSkipped:
                return "ancestor not drawn";
            case UISkipCause::Count:
                break;
        }
        return "?";
    }

    bool BindingHidesElement( const IUITree& tree, NodeId e, const UICanvasContext* ctx, const UIDataStore* row )
    {
        if ( !tree.Valid( e ) || !tree.Has<UIBindingData>( e ) )
            return false;
        const auto& b = *tree.Get<UIBindingData>( e );
        if ( b.Target != UIBindTarget::Visible || b.Key.empty() )
            return false;
        const auto v = BindingStore( ctx, b.Key, row ).Bool( b.Key );
        return v.has_value() && !*v;
    }

    Common::BoolResultStr EnumerateCanvas( const IUITree& tree, NodeId canvas, const Rect& viewportPx,
                                           std::vector<UIElementNode>& out, const UICanvasContext* ctx )
    {
        out.clear();

        const auto fit = ResolveNamedCanvas( tree, canvas, viewportPx );
        if ( !fit )
            return Common::MakeError( fit.GetError() );

        const float scale = fit.GetValue().Scale;
        const auto& cd    = *tree.Get<UICanvasData>( canvas );

        // AN OVERLAY CANVAS IS DISPLACED, AND THIS QUERY HAS TO KNOW IT. RenderCanvas2D shifts the whole
        // canvas root by the placement the overlay update computed, so a context menu draws where it was
        // opened rather than where it was authored. Resolving it here without the shift is the middle link
        // that drops a property — the menu would be clickable at the authored position and visible at the
        // placed one — which is exactly what the note at the top of this file warns about.
        //
        // WITHOUT A CONTEXT there is no placement to apply and none is applied: an authoring host asks
        // about the canvas as it was authored, which is where its handles and its marquee belong.
        Rect       rootRect  = fit.GetValue().Root;
        const bool isOverlay = tree.Has<UIOverlayData>( canvas );
        if ( isOverlay && ctx != nullptr )
        {
            rootRect.X += ctx->OverlayShift.x;
            rootRect.Y += ctx->OverlayShift.y;
        }

        const Rect childRoot = InsetRect( rootRect, cd.SafeArea.x * scale, cd.SafeArea.y * scale,
                                          cd.SafeArea.z * scale, cd.SafeArea.w * scale );

        // The canvas is drawn into the viewport and nowhere else, so that is the outermost clip — built as a
        // region, at identity, exactly as RenderCanvas2D builds its own, so every level below narrows ONE
        // type. It is BOUNDED from the start, which is what keeps "the clip is empty" a different statement
        // from "there is no clip": reading an empty box as unclipped is how two disjoint nested clips came
        // to draw over the whole viewport.
        EnumScope root;
        root.Parent = childRoot;

        // A CLOSED OVERLAY IS NOT AN EMPTY CANVAS. Its tree is enumerated as usual and every node comes back
        // with CauseBy = the canvas, so "why can I not see this button" answers "the overlay it is in is
        // closed" instead of answering nothing at all. Reporting an empty list would be the empty successful
        // answer the contract forbids: the caller could not tell it from a canvas with no children.
        if ( isOverlay && ctx != nullptr && !ctx->OverlayOpen )
            root.SkippedBy = canvas;
        (void)Graphic::Render2D::IntersectClipRegion(
             root.Clip, glm::mat3( 1.0f ), { viewportPx.X, viewportPx.Y },
             { viewportPx.X + viewportPx.W, viewportPx.Y + viewportPx.H } );
        int order = 0;
        for ( std::size_t i = 0, count = tree.ChildCount( canvas ); i < count; ++i )
            if ( const NodeId c = tree.ChildAt( canvas, i ); tree.Valid( c ) )
                EnumRecurse( tree, c, root, scale, viewportPx, ctx, out, order, nullptr );
        return BOOLSUCCESS;
    }

    NodeId PickElement( const IUITree& tree, NodeId canvas, const glm::vec2& pointPx, const Rect& viewportPx )
    {
        std::vector<UIElementNode> nodes;
        if ( const auto walked = EnumerateCanvas( tree, canvas, viewportPx, nodes, nullptr ); !walked )
            return NodeId::Null; // nothing on screen to hit; the refusal's text belongs to the caller's own log

        // Later in draw order = drawn on top, so the LAST match wins — a small button in front of a
        // full-screen panel is picked instead of the panel. The clip is honoured here and was not before
        // this walk was shared: a row scrolled out of its list is not drawn, so it must not be pickable.
        //
        // Asked of the REGION and pointwise, which is the pointer's half of the same object the draw list
        // cut the geometry with — so the corner of a rotated clipper's bounding box, where the pixels were
        // removed, refuses the pick too. `n.RectPx` is tested against the UNDONE pointer and the region
        // against the screen one, because they live in different spaces on purpose.
        NodeId hit = NodeId::Null;
        for ( const UIElementNode& n : nodes )
        {
            if ( !n.Drawn || !n.OwnRect || !n.RectValid )
                continue;
            if ( !Graphic::Render2D::ClipRegionContains( n.ClipRegion, pointPx ) )
                continue;
            const glm::vec2 local = UndoTransform( n.Xform, pointPx );
            if ( local.x >= n.RectPx.X && local.x <= n.RectPx.X + n.RectPx.W && local.y >= n.RectPx.Y &&
                 local.y <= n.RectPx.Y + n.RectPx.H )
                hit = n.Entity;
        }
        return hit;
    }

    bool GetElementRect( const IUITree& tree, NodeId canvas, NodeId target, const Rect& viewportPx, Rect& out,
                         glm::mat3* outXform )
    {
        // Cleared up front, so a caller that reads it after a `false` gets the identity rather than
        // whatever it happened to hold — and so `target == canvas` below reports one too.
        if ( outXform )
            *outXform = glm::mat3( 1.0f );

        const auto fit = ResolveNamedCanvas( tree, canvas, viewportPx );
        if ( !fit )
            return false;

        if ( target == canvas )
        {
            out = fit.GetValue().Root;
            return true;
        }

        std::vector<UIElementNode> nodes;
        if ( const auto walked = EnumerateCanvas( tree, canvas, viewportPx, nodes, nullptr ); !walked )
            return false;

        for ( const UIElementNode& n : nodes )
        {
            if ( n.Entity != target )
                continue;
            if ( !n.RectValid )
                return false; // enumerated, but a layout group left it no slot: it has no position
            out = n.RectPx;
            if ( outXform )
                *outXform = n.Xform;
            return true;
        }
        return false;
    }

    Common::ResultStr<float> CanvasScale( const IUITree& tree, NodeId canvas, const Rect& viewportPx )
    {
        const auto fit = ResolveNamedCanvas( tree, canvas, viewportPx );
        if ( !fit )
            return Common::MakeError<float>( fit.GetError() );
        return Common::MakeSuccess( fit.GetValue().Scale );
    }
} // namespace Desert::UI
