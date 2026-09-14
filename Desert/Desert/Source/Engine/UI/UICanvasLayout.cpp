#include "UICanvasLayout.hpp"

#include <Engine/ECS/Components.hpp>
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
    entt::entity CanvasOf( entt::registry& reg, entt::entity e )
    {
        // Bounded by the entity count rather than trusting the tree to be acyclic: a Parent cycle is
        // authorable (the hierarchy panel can reparent), and an unbounded walk here would hang the editor
        // instead of returning "not under a canvas".
        const std::size_t limit = reg.size() + 1;
        std::size_t       steps = 0;
        for ( entt::entity cur = e; cur != entt::null && reg.valid( cur ) && steps < limit; ++steps )
        {
            if ( reg.has<ECS::UICanvasComponent>( cur ) )
                return cur;
            cur = reg.has<ECS::RelationshipComponent>( cur ) ? reg.get<ECS::RelationshipComponent>( cur ).Parent
                                                             : entt::null;
        }
        return entt::null;
    }

    std::size_t CanvasCount( entt::registry& reg )
    {
        std::size_t n = 0;
        for ( [[maybe_unused]] const auto e : reg.view<ECS::UICanvasComponent>() )
            ++n;
        return n;
    }

    std::vector<entt::entity> CanvasesInDrawOrder( entt::registry& reg )
    {
        // The entity's INDEX, with entt's version bits stripped off. Two canvases with the same authored
        // Sort Order are decided by it, and it is the order the registry handed the ids out — which, for a
        // level being loaded, is the order the file lists the canvases in.
        //
        // WHY NOT SIMPLY THE ORDER `reg.view<>()` HANDS THEM OUT, which is what a stable sort over the view
        // would have kept: MEASURED, it is the REVERSE of creation order, because the pool is walked
        // backwards. A comment claiming the view iterates in creation order would have been wrong in the
        // one direction that matters, and the test that caught it is
        // UICanvasContextPair.CanvasesAreOrderedByTheirAuthoredSortOrder.
        //
        // And the version bits have to come off, or the tie-break is not creation order at all: a recycled
        // id carries a bumped version in the high bits, so a raw comparison would sort every canvas that
        // ever reused an id after every canvas that did not, whenever they were made.
        const auto creationIndex = []( entt::entity e )
        {
            using Traits = entt::entt_traits<std::underlying_type_t<entt::entity>>;
            return static_cast<std::uint32_t>( entt::to_integral( e ) & Traits::entity_mask );
        };

        std::vector<entt::entity> out;
        for ( const auto e : reg.view<ECS::UICanvasComponent>() )
            out.push_back( e );

        std::sort( out.begin(), out.end(),
                   [&reg, &creationIndex]( entt::entity a, entt::entity b )
                   {
                       const auto& ca = reg.get<ECS::UICanvasComponent>( a ).Data;
                       const auto& cb = reg.get<ECS::UICanvasComponent>( b ).Data;
                       if ( ca.SortOrder != cb.SortOrder )
                           return ca.SortOrder < cb.SortOrder;
                       return creationIndex( a ) < creationIndex( b );
                   } );
        return out;
    }

    Common::ResultStr<entt::entity> SoleCanvas( entt::registry& reg )
    {
        entt::entity first = entt::null;
        std::size_t  n     = 0;
        for ( const auto e : reg.view<ECS::UICanvasComponent>() )
        {
            if ( n == 0 )
                first = e;
            ++n;
        }
        if ( n == 1 )
            return Common::MakeSuccess( first );
        if ( n == 0 )
            return Common::MakeFormattedError<entt::entity>(
                 "[UI] the scene has no UI canvas, so no host can be given one to draw or measure" );
        return Common::MakeFormattedError<entt::entity>(
             "[UI] the scene has {} UI canvases and this host did not name one; it is NOT the first one's "
             "job to win by iteration order — name the canvas (UI::CanvasOf on an element of it, or the "
             "host's own document/subject)",
             n );
    }

    bool TakesLayoutSpace( entt::registry& reg, entt::entity e )
    {
        if ( !reg.valid( e ) || !reg.has<ECS::UILayoutComponent>( e ) )
            return true;
        return reg.get<ECS::UILayoutComponent>( e ).Data.Visibility != ECS::UIVisibility::Collapsed;
    }

    bool IsElementVisible( entt::registry& reg, entt::entity e )
    {
        if ( !reg.valid( e ) || !reg.has<ECS::UILayoutComponent>( e ) )
            return true;
        return reg.get<ECS::UILayoutComponent>( e ).Data.Visibility == ECS::UIVisibility::Visible;
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

        CanvasFit ResolveCanvas( const ECS::UICanvasData& d, const Rect& viewportPx )
        {
            switch ( d.ScaleMode )
            {
                case ECS::UICanvasScaleMode::ScaleWithScreen:
                {
                    const float sx = d.ReferenceWidth > 0.0f ? viewportPx.W / d.ReferenceWidth : 1.0f;
                    const float sy = d.ReferenceHeight > 0.0f ? viewportPx.H / d.ReferenceHeight : 1.0f;
                    const float m  = std::clamp( d.MatchWidthHeight, 0.0f, 1.0f );
                    return { viewportPx, sx * ( 1.0f - m ) + sy * m };
                }
                case ECS::UICanvasScaleMode::Letterbox:
                {
                    const Rect fit = CanvasRect( d.ReferenceWidth, d.ReferenceHeight, viewportPx.W, viewportPx.H );
                    const float scale = d.ReferenceWidth > 0.0f ? fit.W / d.ReferenceWidth : 1.0f;
                    return { Rect{ viewportPx.X + fit.X, viewportPx.Y + fit.Y, fit.W, fit.H }, scale };
                }
                case ECS::UICanvasScaleMode::Stretch:
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
        Common::ResultStr<CanvasFit> ResolveNamedCanvas( entt::registry& reg, entt::entity canvas,
                                                         const Rect& viewportPx )
        {
            if ( canvas == entt::null || !reg.valid( canvas ) )
                return Common::MakeFormattedError<CanvasFit>(
                     "[UI] no canvas was named for this layout query (entity {})",
                     static_cast<std::uint32_t>( canvas ) );
            if ( !reg.has<ECS::UICanvasComponent>( canvas ) )
                return Common::MakeFormattedError<CanvasFit>(
                     "[UI] entity {} was named as a canvas but carries no UICanvasComponent",
                     static_cast<std::uint32_t>( canvas ) );

            const auto& canvasData = reg.get<ECS::UICanvasComponent>( canvas ).Data;
            if ( !canvasData.Visible )
                return Common::MakeFormattedError<CanvasFit>(
                     "[UI] canvas {} is not Visible, so it has no on-screen layout to report",
                     static_cast<std::uint32_t>( canvas ) );

            return Common::MakeSuccess( ResolveCanvas( canvasData, viewportPx ) );
        }

        // Content size (px) a layout-group container needs to hug its children — mirrors the renderer's
        // GroupContentPx so the Content Size Fitter picks with the same rect it draws.
        glm::vec2 GroupContentPx( entt::registry& reg, entt::entity e, float scale )
        {
            if ( !reg.has<ECS::UILayoutGroupComponent>( e ) || !reg.has<ECS::RelationshipComponent>( e ) )
                return { 0.0f, 0.0f };
            const auto&            g = reg.get<ECS::UILayoutGroupComponent>( e ).Data;
            std::vector<glm::vec2> sizes;
            for ( auto c : reg.get<ECS::RelationshipComponent>( e ).Children )
            {
                if ( !reg.valid( c ) || !TakesLayoutSpace( reg, c ) )
                    continue; // a Collapsed child is not in the group, so it is not in its content size
                glm::vec2 pref( 0.0f );
                if ( reg.has<ECS::UILayoutComponent>( c ) )
                {
                    const auto& L = reg.get<ECS::UILayoutComponent>( c ).Data;
                    pref          = glm::max( L.CustomMinimumSize, L.OffsetMax - L.OffsetMin );
                }
                sizes.push_back( pref * scale );
            }
            LayoutGroupParams params;
            params.Type     = g.Type == ECS::UILayoutType::Horizontal ? LayoutGroupType::Horizontal
                              : g.Type == ECS::UILayoutType::Grid     ? LayoutGroupType::Grid
                                                                      : LayoutGroupType::Vertical;
            params.PaddingL = g.Padding.x * scale;
            params.PaddingT = g.Padding.y * scale;
            params.PaddingR = g.Padding.z * scale;
            params.PaddingB = g.Padding.w * scale;
            params.Spacing  = g.Spacing * scale;
            params.CellSize = g.CellSize * scale;
            params.Columns  = g.Columns;
            return MeasureLayoutGroup( params, sizes );
        }

        // If `e` is an auto-layout container, solve its children's rects exactly like the renderer's
        // DrawElement does — so hit-testing / handles match the drawn positions (children of a VBox/HBox/Grid
        // are placed by the group, NOT their own anchors). Fills kids + one rect each; empty when not a group.
        void SolveGroupChildren( entt::registry& reg, entt::entity e, const Rect& container, float scale,
                                 std::vector<entt::entity>& kids, std::vector<Rect>& rects )
        {
            if ( !reg.has<ECS::UILayoutGroupComponent>( e ) || !reg.has<ECS::RelationshipComponent>( e ) )
                return;
            const auto&            g = reg.get<ECS::UILayoutGroupComponent>( e ).Data;
            std::vector<glm::vec2> sizes;
            std::vector<float>     flex;
            for ( auto c : reg.get<ECS::RelationshipComponent>( e ).Children )
            {
                if ( !reg.valid( c ) || !TakesLayoutSpace( reg, c ) )
                    continue; // Collapsed: no slot here, exactly as in the renderer's own group solve
                glm::vec2 pref( 0.0f );
                float     fg = 0.0f;
                if ( reg.has<ECS::UILayoutComponent>( c ) )
                {
                    const auto& L = reg.get<ECS::UILayoutComponent>( c ).Data;
                    pref          = glm::max( L.CustomMinimumSize, L.OffsetMax - L.OffsetMin );
                    fg            = L.FlexGrow;
                }
                kids.push_back( c );
                sizes.push_back( pref * scale );
                flex.push_back( fg );
            }
            LayoutGroupParams params;
            params.Type         = g.Type == ECS::UILayoutType::Horizontal ? LayoutGroupType::Horizontal
                                  : g.Type == ECS::UILayoutType::Grid     ? LayoutGroupType::Grid
                                                                          : LayoutGroupType::Vertical;
            params.PaddingL     = g.Padding.x * scale;
            params.PaddingT     = g.Padding.y * scale;
            params.PaddingR     = g.Padding.z * scale;
            params.PaddingB     = g.Padding.w * scale;
            params.Spacing      = g.Spacing * scale;
            params.StretchCross = g.StretchCross;
            params.CellSize     = g.CellSize * scale;
            params.Columns      = g.Columns;
            rects               = SolveLayoutGroup( container, params, sizes, flex );
        }

        // The element's accumulated transform: its parent's, with its own composed inside it. THE SAME
        // COMPOSITION ORDER THE RENDERER USES (DrawList2D::PushTransform), because a pick that composed
        // the other way round would be right for one level and wrong for two.
        glm::mat3 AccumulateTransform( entt::registry& reg, entt::entity e, const Rect& rect,
                                       const glm::mat3& parentXform )
        {
            if ( !reg.has<ECS::UILayoutComponent>( e ) )
                return parentXform;
            const auto& L = reg.get<ECS::UILayoutComponent>( e ).Data;
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
            entt::entity ParentEntity = entt::null;
            bool         Elect        = true;       // may the pointer stop anywhere in here
            entt::entity SkippedBy    = entt::null; // the ancestor that stopped the walk, if one did
        };

        void EnumRecurse( entt::registry& reg, entt::entity e, const EnumScope& scope, float scale,
                          const Rect& viewportPx, const UICanvasContext* ctx, std::vector<UIElementNode>& out,
                          int& order, const Rect* forcedRect )
        {
            const bool hasLayout = reg.has<ECS::UILayoutComponent>( e );

            UIElementNode node;
            node.Entity    = e;
            node.Parent    = scope.ParentEntity;
            node.Depth     = scope.Depth;
            node.TakesSlot = TakesLayoutSpace( reg, e );
            node.OwnRect   = ( forcedRect != nullptr ) || hasLayout;
            node.RectValid = true;

            // WHY THIS ELEMENT IS NOT DRAWN, in the order the walk asks it. An ancestor that already
            // stopped wins over anything this element says about itself: the walk never reached it, so
            // its own Visibility was never read and reporting it would invent a reason.
            const ECS::UIHitTest hitTest =
                 hasLayout ? reg.get<ECS::UILayoutComponent>( e ).Data.HitTest : ECS::UIHitTest::All;
            node.HitTest = hitTest;
            node.ElectsSelf =
                 scope.Elect && ( hitTest == ECS::UIHitTest::All || hitTest == ECS::UIHitTest::Blocking );

            UISkipCause self = UISkipCause::None;
            if ( hasLayout )
            {
                switch ( reg.get<ECS::UILayoutComponent>( e ).Data.Visibility )
                {
                    case ECS::UIVisibility::Hidden:
                        self = UISkipCause::SelfHidden;
                        break;
                    case ECS::UIVisibility::Collapsed:
                        self = UISkipCause::SelfCollapsed;
                        break;
                    default:
                        break;
                }
            }
            // The two view-dependent reasons. Without a context they are not asked at all — see the header:
            // an author must be able to pick an element on a screen the game is not showing.
            if ( self == UISkipCause::None && ctx != nullptr && reg.has<ECS::UIScreenComponent>( e ) )
            {
                const std::string& name      = reg.get<ECS::UIScreenComponent>( e ).Data.Name;
                const bool         isCurrent = ( name == ctx->Screen );
                const bool         isLeaving = ( name == ctx->ScreenFrom && ctx->ScreenT < 1.0f );
                if ( !isCurrent && !isLeaving )
                    self = UISkipCause::ScreenNotCurrent;
            }
            if ( self == UISkipCause::None && ctx != nullptr && BindingHidesElement( reg, e, ctx ) )
                self = UISkipCause::BindingHidden;

            if ( scope.SkippedBy != entt::null )
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
                const auto& L = reg.get<ECS::UILayoutComponent>( e ).Data;
                rect          = ResolveRect( L.AnchorMin, L.AnchorMax, L.OffsetMin * scale, L.OffsetMax * scale,
                                             L.CustomMinimumSize * scale, scope.Parent );
            }
            if ( hasLayout )
            {
                const auto& L = reg.get<ECS::UILayoutComponent>( e ).Data;
                rect          = ApplyAspectFit( rect, L.AspectRatio, static_cast<int>( L.AspectMode ) );
                if ( ( L.FitWidth || L.FitHeight ) && reg.has<ECS::UILayoutGroupComponent>( e ) )
                {
                    const glm::vec2 content = GroupContentPx( reg, e, scale );
                    if ( L.FitWidth )
                        rect.W = content.x;
                    if ( L.FitHeight )
                        rect.H = content.y;
                }
            }

            const glm::mat3 xform = AccumulateTransform( reg, e, rect, scope.Xform );
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
            bool clip        = hasLayout && reg.get<ECS::UILayoutComponent>( e ).Data.ClipContents;

            if ( reg.has<ECS::UIScrollViewComponent>( e ) )
            {
                // READ-ONLY where the renderer writes. The walk clamps ScrollY back into the component
                // because it also consumes wheel input; a query must not, or opening a debug panel would
                // silently edit the scene. Clamping the value locally gives the same number for any
                // ScrollY the renderer could have left behind.
                const auto& sv        = reg.get<ECS::UIScrollViewComponent>( e ).Data;
                const float scrollMax = std::max( 0.0f, sv.ContentHeight * scale - rect.H );
                const float maxDesign = scale > 0.0f ? scrollMax / scale : 0.0f;
                childParent.Y -= std::clamp( sv.ScrollY, 0.0f, maxDesign ) * scale;
                clip = true;
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

            if ( !reg.has<ECS::RelationshipComponent>( e ) )
                return;

            EnumScope child;
            child.Parent       = childParent;
            child.Clip         = childClip;
            child.Xform        = xform;
            child.Depth        = scope.Depth + 1;
            child.ParentEntity = e;
            child.Elect =
                 scope.Elect && ( hitTest == ECS::UIHitTest::All || hitTest == ECS::UIHitTest::ChildrenOnly );
            child.SkippedBy = scope.SkippedBy != entt::null ? scope.SkippedBy : node.Drawn ? entt::null : e;

            const auto& children = reg.get<ECS::RelationshipComponent>( e ).Children;
            if ( reg.has<ECS::UILayoutGroupComponent>( e ) )
            {
                std::vector<entt::entity> kids;
                std::vector<Rect>         rects;
                SolveGroupChildren( reg, e, childParent, scale, kids, rects );
                for ( std::size_t i = 0; i < kids.size(); ++i )
                    EnumRecurse( reg, kids[i], child, scale, viewportPx, ctx, out, order, &rects[i] );

                // A Collapsed child is given NO SLOT by the group, so it has no position to report — and
                // reporting it at its anchored rect would be a lie about where it is not. It is still
                // enumerated, because "this element exists and the group dropped it" is the answer
                // somebody opened the panel to get.
                for ( auto c : children )
                {
                    if ( !reg.valid( c ) || TakesLayoutSpace( reg, c ) )
                        continue;
                    UIElementNode slotless;
                    slotless.Entity = c;
                    slotless.Parent = e;
                    slotless.Depth  = child.Depth;
                    slotless.Cause =
                         child.SkippedBy != entt::null ? UISkipCause::AncestorSkipped : UISkipCause::SelfCollapsed;
                    slotless.CauseBy    = child.SkippedBy != entt::null ? child.SkippedBy : c;
                    slotless.ClipRegion = childClip;
                    slotless.TakesSlot = false;
                    out.push_back( slotless );
                }
            }
            else
            {
                for ( auto c : children )
                    if ( reg.valid( c ) )
                        EnumRecurse( reg, c, child, scale, viewportPx, ctx, out, order, nullptr );
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
            case UISkipCause::AncestorSkipped:
                return "ancestor not drawn";
        }
        return "?";
    }

    bool BindingHidesElement( entt::registry& reg, entt::entity e, const UICanvasContext* ctx )
    {
        if ( !reg.valid( e ) || !reg.has<ECS::UIBindingComponent>( e ) )
            return false;
        const auto& b = reg.get<ECS::UIBindingComponent>( e ).Data;
        if ( b.Target != ECS::UIBindTarget::Visible || b.Key.empty() )
            return false;
        const auto v = BindingStore( ctx, b.Key ).Bool( b.Key );
        return v.has_value() && !*v;
    }

    Common::BoolResultStr EnumerateCanvas( entt::registry& reg, entt::entity canvas, const Rect& viewportPx,
                                           std::vector<UIElementNode>& out, const UICanvasContext* ctx )
    {
        out.clear();

        const auto fit = ResolveNamedCanvas( reg, canvas, viewportPx );
        if ( !fit )
            return Common::MakeError( fit.GetError() );

        const float scale = fit.GetValue().Scale;
        const auto& cd    = reg.get<ECS::UICanvasComponent>( canvas ).Data;

        // AN OVERLAY CANVAS IS DISPLACED, AND THIS QUERY HAS TO KNOW IT. RenderCanvas2D shifts the whole
        // canvas root by the placement the overlay update computed, so a context menu draws where it was
        // opened rather than where it was authored. Resolving it here without the shift is the middle link
        // that drops a property — the menu would be clickable at the authored position and visible at the
        // placed one — which is exactly what the note at the top of this file warns about.
        //
        // WITHOUT A CONTEXT there is no placement to apply and none is applied: an authoring host asks
        // about the canvas as it was authored, which is where its handles and its marquee belong.
        Rect       rootRect  = fit.GetValue().Root;
        const bool isOverlay = reg.has<ECS::UIOverlayComponent>( canvas );
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
        if ( reg.has<ECS::RelationshipComponent>( canvas ) )
            for ( auto c : reg.get<ECS::RelationshipComponent>( canvas ).Children )
                if ( reg.valid( c ) )
                    EnumRecurse( reg, c, root, scale, viewportPx, ctx, out, order, nullptr );
        return BOOLSUCCESS;
    }

    entt::entity PickElement( entt::registry& reg, entt::entity canvas, const glm::vec2& pointPx,
                              const Rect& viewportPx )
    {
        std::vector<UIElementNode> nodes;
        if ( const auto walked = EnumerateCanvas( reg, canvas, viewportPx, nodes, nullptr ); !walked )
            return entt::null; // nothing on screen to hit; the refusal's text belongs to the caller's own log

        // Later in draw order = drawn on top, so the LAST match wins — a small button in front of a
        // full-screen panel is picked instead of the panel. The clip is honoured here and was not before
        // this walk was shared: a row scrolled out of its list is not drawn, so it must not be pickable.
        //
        // Asked of the REGION and pointwise, which is the pointer's half of the same object the draw list
        // cut the geometry with — so the corner of a rotated clipper's bounding box, where the pixels were
        // removed, refuses the pick too. `n.RectPx` is tested against the UNDONE pointer and the region
        // against the screen one, because they live in different spaces on purpose.
        entt::entity hit = entt::null;
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

    bool GetElementRect( entt::registry& reg, entt::entity canvas, entt::entity target, const Rect& viewportPx,
                         Rect& out, glm::mat3* outXform )
    {
        // Cleared up front, so a caller that reads it after a `false` gets the identity rather than
        // whatever it happened to hold — and so `target == canvas` below reports one too.
        if ( outXform )
            *outXform = glm::mat3( 1.0f );

        const auto fit = ResolveNamedCanvas( reg, canvas, viewportPx );
        if ( !fit )
            return false;

        if ( target == canvas )
        {
            out = fit.GetValue().Root;
            return true;
        }

        std::vector<UIElementNode> nodes;
        if ( const auto walked = EnumerateCanvas( reg, canvas, viewportPx, nodes, nullptr ); !walked )
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

    Common::ResultStr<float> CanvasScale( entt::registry& reg, entt::entity canvas, const Rect& viewportPx )
    {
        const auto fit = ResolveNamedCanvas( reg, canvas, viewportPx );
        if ( !fit )
            return Common::MakeError<float>( fit.GetError() );
        return Common::MakeSuccess( fit.GetValue().Scale );
    }
} // namespace Desert::UI
