#pragma once

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

// Godot-Control-style UI layout resolution — pure math, decoupled from ECS (takes raw anchor/offset/size
// values) so it is unit-testable and reusable by both the renderer and the editor preview.
namespace Desert::UI
{
    struct Rect
    {
        float X = 0.0f;
        float Y = 0.0f;
        float W = 0.0f;
        float H = 0.0f;
    };

    // Resolve an element's pixel rect from anchors (fraction 0..1 of the parent rect), pixel offsets from the
    // anchored edges (OffsetMin from the AnchorMin/left+top edge, OffsetMax from the AnchorMax/right+bottom
    // edge) and a custom minimum size, against the parent's pixel rect. AnchorMin==AnchorMax gives a
    // fixed-size element positioned by the offsets; spread anchors make it stretch with the parent.
    inline Rect ResolveRect( const glm::vec2& anchorMin, const glm::vec2& anchorMax, const glm::vec2& offsetMin,
                             const glm::vec2& offsetMax, const glm::vec2& minSize, const Rect& parent )
    {
        const float left   = parent.X + anchorMin.x * parent.W + offsetMin.x;
        const float top    = parent.Y + anchorMin.y * parent.H + offsetMin.y;
        const float right  = parent.X + anchorMax.x * parent.W + offsetMax.x;
        const float bottom = parent.Y + anchorMax.y * parent.H + offsetMax.y;

        Rect r;
        r.X = left;
        r.Y = top;
        r.W = std::max( right - left, std::max( 0.0f, minSize.x ) );
        r.H = std::max( bottom - top, std::max( 0.0f, minSize.y ) );
        return r;
    }

    // Aspect Ratio Fitter: reshape `r` to a target width/height ratio, keeping it centred on its resolved
    // centre. mode 1 = derive WIDTH from height; mode 2 = derive HEIGHT from width. ratio<=0 / mode 0 = off.
    // Shared by the renderer + the editor hit-test so a fitted element draws and picks with the same rect.
    inline Rect ApplyAspectFit( const Rect& r, float ratio, int mode )
    {
        if ( ratio <= 0.0f || mode == 0 )
            return r;
        Rect out = r;
        if ( mode == 1 )
        {
            const float w = r.H * ratio;
            out.X         = r.X + ( r.W - w ) * 0.5f;
            out.W         = w;
        }
        else if ( mode == 2 )
        {
            const float h = r.W / ratio;
            out.Y         = r.Y + ( r.H - h ) * 0.5f;
            out.H         = h;
        }
        return out;
    }

    // Canvas root rect: the design (reference) resolution scaled uniformly to FIT the viewport, centred
    // (letterboxed) — so a layout authored at the reference size keeps its proportions on any window size.
    inline Rect CanvasRect( float refW, float refH, float viewW, float viewH )
    {
        if ( refW <= 0.0f || refH <= 0.0f )
            return { 0.0f, 0.0f, viewW, viewH };
        const float scale = std::min( viewW / refW, viewH / refH );
        const float w     = refW * scale;
        const float h     = refH * scale;
        return { ( viewW - w ) * 0.5f, ( viewH - h ) * 0.5f, w, h };
    }

    // Shrink `r` by per-edge insets (px) — used for the Canvas safe-area (mobile notches / rounded corners):
    // top-level content lays out inside the inset rect. Never goes negative.
    inline Rect InsetRect( const Rect& r, float left, float top, float right, float bottom )
    {
        return { r.X + left, r.Y + top, std::max( 0.0f, r.W - left - right ),
                 std::max( 0.0f, r.H - top - bottom ) };
    }

    // ---------------------------------------------------------------------------------------------------
    // OVERLAY PLACEMENT — ONE MECHANISM, NOT FOUR
    //
    // A tooltip at the right edge of the view must FLIP to the other side of the pointer, not slide until
    // it touches the border and not hang off it. So must a context menu, and so must every submenu that
    // menu opens. Writing that per feature is how three of the four end up subtly different — the defect
    // shape this project keeps paying for — so it is written once, here, as pure math with no ECS, no view
    // and no context in it.
    //
    // THE ORIGIN IS A RECT AND NOT A POINT, because the two cases are the same case: a tooltip is placed
    // against the pointer (a zero-size rect) and a submenu against its parent item (a real one). Giving
    // them one parameter is what makes "flip about the thing I am attached to" mean the same sentence in
    // both.
    //
    // WHAT EACH AXIS DOES. The ADVANCE axis is the one the box moves along to get clear of the origin:
    // downward for a tooltip and a menu, sideways for a submenu. On that axis the box prefers the FAR side
    // of the origin and flips to the NEAR side. On the CROSS axis it prefers to line its near edge up with
    // the origin's near edge and flips to lining the far edges up — which is what makes a menu at the right
    // border open leftwards instead of being pushed out of alignment with what opened it.
    //
    // MODAL AND TOAST DO NOT USE THIS, and that is a decision rather than an omission: neither has an
    // origin. A modal is centred on the view and a toast stack sits in a corner of it, both stated by the
    // canvas's own anchors, and handing them a fabricated origin would be a mechanism that moves nothing.

    enum class OverlayAxis
    {
        Vertical,  // the box gets clear of the origin downwards (a tooltip, a context menu)
        Horizontal // ... sideways (a submenu opening beside its parent item)
    };

    // One axis of the decision. Prefer @p pref; if the box would leave [lo, hi) there, take @p alt; if
    // neither side fits — the box is wider than the room on both sides of the origin — keep the preferred
    // side and clamp, because a box that is too big for the view must still be inside it.
    inline float PlaceOverlayAxis( float pref, float alt, float size, float lo, float hi )
    {
        if ( pref >= lo && pref + size <= hi )
            return pref;
        if ( alt >= lo && alt + size <= hi )
            return alt;
        return std::clamp( pref, lo, std::max( lo, hi - size ) );
    }

    // Where a box of @p size goes when it is attached to @p origin with @p gap px of air, inside @p bounds.
    // The result is always inside @p bounds unless the box is larger than it, in which case it is pinned to
    // the top-left of @p bounds and overflows the far edges only.
    inline Rect PlaceOverlay( const glm::vec2& size, const Rect& origin, const glm::vec2& gap, const Rect& bounds,
                              OverlayAxis advance )
    {
        const float loX = bounds.X;
        const float hiX = bounds.X + bounds.W;
        const float loY = bounds.Y;
        const float hiY = bounds.Y + bounds.H;

        float x = 0.0f;
        float y = 0.0f;
        if ( advance == OverlayAxis::Vertical )
        {
            y = PlaceOverlayAxis( origin.Y + origin.H + gap.y, origin.Y - gap.y - size.y, size.y, loY, hiY );
            x = PlaceOverlayAxis( origin.X + gap.x, origin.X + origin.W - gap.x - size.x, size.x, loX, hiX );
        }
        else
        {
            x = PlaceOverlayAxis( origin.X + origin.W + gap.x, origin.X - gap.x - size.x, size.x, loX, hiX );
            y = PlaceOverlayAxis( origin.Y + gap.y, origin.Y + origin.H - gap.y - size.y, size.y, loY, hiY );
        }
        return { x, y, size.x, size.y };
    }

    // ---------------------------------------------------------------------------------------------------
    // Auto-layout groups (VBox / HBox / Grid). A container with a layout group POSITIONS + SIZES its direct
    // children automatically, overriding their own anchors — the Unity/Godot "layout group" model. Pure math
    // (takes the container rect + each child's preferred size) so it is unit-testable and shared by the
    // renderer + editor. `Spacing`/`Padding`/`CellSize` are in the SAME pixel space as the container rect
    // (the caller pre-multiplies by the canvas scale).

    enum class LayoutGroupType
    {
        Horizontal, // HBox: children left -> right
        Vertical,   // VBox: children top -> bottom
        Grid        // fixed cells, wrapping into rows
    };

    struct LayoutGroupParams
    {
        LayoutGroupType Type     = LayoutGroupType::Vertical;
        float           PaddingL = 0.0f, PaddingT = 0.0f, PaddingR = 0.0f, PaddingB = 0.0f;
        float           Spacing = 0.0f; // gap between children (both axes for Grid)
        bool      StretchCross  = true; // stretch children across the minor axis (else keep preferred + centre)
        glm::vec2 CellSize      = { 100.0f, 100.0f }; // Grid only
        int       Columns       = 0;                  // Grid: fixed column count, 0 = auto-fit by width
    };

    // Total content size (px) a layout group needs for `childSizes` — main axis = sum + spacing, cross axis =
    // max child — plus padding. Used by the Content Size Fitter to size a container to its children.
    inline glm::vec2 MeasureLayoutGroup( const LayoutGroupParams& p, const std::vector<glm::vec2>& childSizes )
    {
        const int   n       = static_cast<int>( childSizes.size() );
        const float spacing = n > 1 ? p.Spacing * ( n - 1 ) : 0.0f;
        if ( p.Type == LayoutGroupType::Horizontal || p.Type == LayoutGroupType::Vertical )
        {
            const bool horiz    = p.Type == LayoutGroupType::Horizontal;
            float      mainSum  = 0.0f;
            float      crossMax = 0.0f;
            for ( const glm::vec2& s : childSizes )
            {
                mainSum += horiz ? s.x : s.y;
                crossMax = std::max( crossMax, horiz ? s.y : s.x );
            }
            const float mainTotal =
                 mainSum + spacing + ( horiz ? p.PaddingL + p.PaddingR : p.PaddingT + p.PaddingB );
            const float crossTotal = crossMax + ( horiz ? p.PaddingT + p.PaddingB : p.PaddingL + p.PaddingR );
            return horiz ? glm::vec2( mainTotal, crossTotal ) : glm::vec2( crossTotal, mainTotal );
        }
        const float cw   = std::max( 1.0f, p.CellSize.x );
        const float ch   = std::max( 1.0f, p.CellSize.y );
        const int   cols = p.Columns > 0 ? p.Columns : std::max( 1, n );
        const int   rows = n > 0 ? ( n + cols - 1 ) / cols : 0;
        return { cols * cw + std::max( 0, cols - 1 ) * p.Spacing + p.PaddingL + p.PaddingR,
                 rows * ch + std::max( 0, rows - 1 ) * p.Spacing + p.PaddingT + p.PaddingB };
    }

    // Lay `childSizes` (each child's preferred px size) out inside `container`, returning one rect per child.
    // `childFlex` (Layout Element grow weights; empty = all 0) share the leftover MAIN-axis space among the
    // flexible children proportionally, so a child can stretch to fill or act as a spacer.
    inline std::vector<Rect> SolveLayoutGroup( const Rect& container, const LayoutGroupParams& p,
                                               const std::vector<glm::vec2>& childSizes,
                                               const std::vector<float>&     childFlex = {} )
    {
        std::vector<Rect> out;
        out.reserve( childSizes.size() );

        const float x0     = container.X + p.PaddingL;
        const float y0     = container.Y + p.PaddingT;
        const float innerW = std::max( 0.0f, container.W - p.PaddingL - p.PaddingR );
        const float innerH = std::max( 0.0f, container.H - p.PaddingT - p.PaddingB );

        // Leftover main-axis space to hand to flexible children (linear box model, Horizontal/Vertical only).
        auto flexOf = [&]( std::size_t i )
        { return i < childFlex.size() ? std::max( 0.0f, childFlex[i] ) : 0.0f; };
        float flexTotal = 0.0f;
        for ( std::size_t i = 0; i < childSizes.size(); ++i )
            flexTotal += flexOf( i );

        if ( p.Type == LayoutGroupType::Horizontal || p.Type == LayoutGroupType::Vertical )
        {
            const bool horiz = p.Type == LayoutGroupType::Horizontal;
            const int  n     = static_cast<int>( childSizes.size() );
            float      used  = ( n > 1 ? p.Spacing * ( n - 1 ) : 0.0f );
            for ( const glm::vec2& s : childSizes )
                used += horiz ? s.x : s.y;
            const float leftover = std::max( 0.0f, ( horiz ? innerW : innerH ) - used );

            float pos = horiz ? x0 : y0;
            for ( std::size_t i = 0; i < childSizes.size(); ++i )
            {
                const glm::vec2& s     = childSizes[i];
                const float      extra = flexTotal > 0.0f ? leftover * flexOf( i ) / flexTotal : 0.0f;
                if ( horiz )
                {
                    const float w = s.x + extra;
                    const float h = p.StretchCross ? innerH : s.y;
                    const float y = p.StretchCross ? y0 : y0 + ( innerH - h ) * 0.5f;
                    out.push_back( { pos, y, w, h } );
                    pos += w + p.Spacing;
                }
                else
                {
                    const float h = s.y + extra;
                    const float w = p.StretchCross ? innerW : s.x;
                    const float x = p.StretchCross ? x0 : x0 + ( innerW - w ) * 0.5f;
                    out.push_back( { x, pos, w, h } );
                    pos += h + p.Spacing;
                }
            }
        }
        else // Grid
        {
            const float cw   = std::max( 1.0f, p.CellSize.x );
            const float ch   = std::max( 1.0f, p.CellSize.y );
            int         cols = p.Columns > 0
                                    ? p.Columns
                                    : std::max( 1, static_cast<int>( ( innerW + p.Spacing ) / ( cw + p.Spacing ) ) );
            for ( std::size_t i = 0; i < childSizes.size(); ++i )
            {
                const int col = static_cast<int>( i ) % cols;
                const int row = static_cast<int>( i ) / cols;
                out.push_back( { x0 + col * ( cw + p.Spacing ), y0 + row * ( ch + p.Spacing ), cw, ch } );
            }
        }
        return out;
    }

    // --- The virtualized list's window (Ю17) --------------------------------------------------------
    //
    // WHICH ROWS A FRAME MUST WALK, computed here and nowhere else. Two walks read it — the renderer's
    // DrawElement and UICanvasLayout's EnumerateCanvas — and a list whose rows are drawn at one place and
    // enumerated at another is this project's recurring defect in its purest form: the click would land
    // where the row is not. So it is one function returning one answer, the same shape SolveLayoutGroup
    // already has for the auto-layout containers.

    struct ListWindow
    {
        // Inclusive row indices. Last < First is a legitimate answer and means "no row is on screen" —
        // an empty list, or one scrolled past its own content.
        int First = 0;
        int Last  = -1;

        float PitchPx     = 0.0f; // row height + spacing, in pixels
        float RowHeightPx = 0.0f;
        float ContentPx   = 0.0f; // every row plus the gaps between them
        float ScrollPx    = 0.0f; // the CLAMPED offset, which is what both walks must shift rows by
        float ScrollMaxPx = 0.0f; // 0 when the content fits, so >0 is also "show the scrollbar"
    };

    // @p itemHeight and @p spacing are DESIGN px; @p viewportH is the container's own height in PIXELS,
    // and @p scale converts between them. @p scrollY is design px as authored, and comes back clamped in
    // ScrollPx (pixels) — the caller writes ScrollPx/scale back into the component, so the clamp lives
    // here too rather than being re-derived by each walk.
    //
    // ITEM HEIGHT IS FLOORED AT ONE DESIGN PIXEL. A pitch of zero makes the window unbounded — every row
    // of the list, which is the whole-list walk this container exists to delete — so the one input that
    // could quietly turn virtualization off is not allowed to.
    inline ListWindow SolveListWindow( int itemCount, float itemHeight, float spacing, int overscan, float scrollY,
                                       float viewportH, float scale )
    {
        ListWindow w;
        if ( itemCount <= 0 || scale <= 0.0f )
        {
            return w;
        }

        w.RowHeightPx = std::max( 1.0f, itemHeight ) * scale;
        w.PitchPx     = w.RowHeightPx + std::max( 0.0f, spacing ) * scale;
        w.ContentPx   = static_cast<float>( itemCount ) * w.PitchPx - std::max( 0.0f, spacing ) * scale;
        w.ScrollMaxPx = std::max( 0.0f, w.ContentPx - viewportH );
        w.ScrollPx    = std::clamp( scrollY * scale, 0.0f, w.ScrollMaxPx );

        const int over = std::max( 0, overscan );
        // The last row is the one whose TOP is still above the bottom edge: ceil of the edge in pitches,
        // minus one. Taking floor here instead would keep a row that starts exactly at the bottom edge and
        // covers no pixel, which is a whole row of walk for nothing at every scroll position that lands on
        // a multiple of the pitch.
        const int first = static_cast<int>( std::floor( w.ScrollPx / w.PitchPx ) ) - over;
        const int last  = static_cast<int>( std::ceil( ( w.ScrollPx + viewportH ) / w.PitchPx ) ) - 1 + over;

        w.First = std::clamp( first, 0, itemCount - 1 );
        w.Last  = std::clamp( last, 0, itemCount - 1 );
        return w;
    }

    // Where row @p index lands inside @p container, in pixels. Full width: a list row spans the container
    // and the scrollbar is drawn over it, exactly as UIScrollView draws its own.
    inline Rect ListRowRect( const Rect& container, const ListWindow& w, int index )
    {
        return Rect{ container.X, container.Y + static_cast<float>( index ) * w.PitchPx - w.ScrollPx, container.W,
                     w.RowHeightPx };
    }
} // namespace Desert::UI
