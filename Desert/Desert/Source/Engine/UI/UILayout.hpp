#pragma once

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
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
        Horizontal,  // HBox: children left -> right
        Vertical,    // VBox: children top -> bottom
        Grid,        // fixed cells, wrapping into rows
        Wrap,        // UE SWrapBox: preferred sizes in a line, a new line when the next would not fit
        Overlay,     // UE SOverlay: every child in the same inner rect, later children on top (Z stack)
        UniformGrid, // UE SUniformGridPanel: every cell the size of the largest child, cells share the rect
        SizeBox,     // UE SSizeBox: the children's desired size clamped by Min/Max or replaced by Override
        ScaleBox     // UE SScaleBox: the content keeps its desired size and is SCALED to the rect
    };

    // UE EStretch, the ScaleBox's rule for turning the content's desired size into a scale.
    enum class LayoutScaleStretch
    {
        None,         // scale 1
        Fill,         // the content's rect is the whole inner rect, unscaled (non-uniform stretch)
        ScaleToFit,   // the largest uniform scale that keeps the whole content inside
        ScaleToFitX,  // uniform scale that matches the width
        ScaleToFitY,  // uniform scale that matches the height
        ScaleToFill,  // the smallest uniform scale that covers the whole rect (overflow is clipped by the author)
        UserSpecified // LayoutGroupParams::UserScale
    };

    // UE EStretchDirection: which way the ScaleBox may scale.
    enum class LayoutScaleDirection
    {
        Both,
        DownOnly, // never enlarges
        UpOnly    // never shrinks
    };

    struct LayoutGroupParams
    {
        LayoutGroupType Type     = LayoutGroupType::Vertical;
        float           PaddingL = 0.0f, PaddingT = 0.0f, PaddingR = 0.0f, PaddingB = 0.0f;
        float           Spacing = 0.0f;       // gap between children (both axes for Grid / Wrap / UniformGrid)
        bool      StretchCross  = true; // stretch children across the minor axis (else keep preferred + centre)
        glm::vec2 CellSize      = { 100.0f, 100.0f }; // Grid only
        int             Columns       = 0; // Grid: 0 = auto-fit by width; UniformGrid: 0 = ceil(sqrt n)

        // Wrap: the line length in px (0 = the container's inner extent, as UE's bExplicitWrapSize=false)
        // and the line direction (false = lines run left -> right and stack downwards).
        float WrapSize     = 0.0f;
        bool  WrapVertical = false;

        // UniformGrid: the smallest a cell may be (UE MinDesiredSlotWidth/Height), px.
        glm::vec2 MinSlotSize = { 0.0f, 0.0f };

        // SizeBox, px, per axis; NEGATIVE = not set (0 is a legal size, so it cannot be the sentinel).
        glm::vec2 SizeMin      = { -1.0f, -1.0f };
        glm::vec2 SizeMax      = { -1.0f, -1.0f };
        glm::vec2 SizeOverride = { -1.0f, -1.0f };

        // ScaleBox.
        LayoutScaleStretch   Stretch          = LayoutScaleStretch::ScaleToFit;
        LayoutScaleDirection StretchDirection = LayoutScaleDirection::Both;
        float                UserScale        = 1.0f;
    };

    // One child of a layout group, as the group sees it. Pref = desired size, Min = the size shrinking
    // stops at, Grow / Shrink = the Slate StretchContent coefficients (grow shares the leftover main-axis
    // space by weight; shrink takes the overflow back in proportion to Shrink * Pref, the flexbox rule
    // Slate ports in LayoutUtils.h ArrangeChildrenInStack).
    struct LayoutSlot
    {
        glm::vec2 Pref   = { 0.0f, 0.0f };
        glm::vec2 Min    = { 0.0f, 0.0f };
        float     Grow   = 0.0f;
        float     Shrink = 0.0f;
    };

    // Where a child goes and at what layout scale. Scale is 1 for every group but the ScaleBox, whose
    // child is laid out (text, padding, its own children) at the parent's scale times this one.
    struct ArrangedSlot
    {
        Rect  R;
        float Scale = 1.0f;
    };

    namespace LayoutDetail
    {
        inline glm::vec2 MaxOf( const std::vector<glm::vec2>& sizes )
        {
            glm::vec2 m( 0.0f );
            for ( const glm::vec2& s : sizes )
                m = glm::max( m, s );
            return m;
        }

        inline int UniformColumns( const LayoutGroupParams& p, int n )
        {
            if ( p.Columns > 0 )
                return p.Columns;
            return std::max( 1, static_cast<int>( std::ceil( std::sqrt( static_cast<float>( n ) ) ) ) );
        }

        // SizeBox desired size on one axis: Override wins, else the content clamped by Min then Max (UE
        // SSizeBox::ComputeDesiredSize applies them in that order, so Max wins over a larger Min).
        inline float SizeBoxAxis( float content, float mn, float mx, float ov )
        {
            if ( ov >= 0.0f )
                return ov;
            float v = content;
            if ( mn >= 0.0f )
                v = std::max( v, mn );
            if ( mx >= 0.0f )
                v = std::min( v, mx );
            return v;
        }

        // Wrap lines: each line's first child index, main extent and cross extent.
        struct WrapLine
        {
            std::size_t First = 0, Count = 0;
            float       Main = 0.0f, Cross = 0.0f;
        };

        inline std::vector<WrapLine> WrapLines( const std::vector<glm::vec2>& sizes, float lineLength,
                                                float spacing, bool vertical )
        {
            std::vector<WrapLine> lines;
            for ( std::size_t i = 0; i < sizes.size(); ++i )
            {
                const float m = vertical ? sizes[i].y : sizes[i].x;
                const float c = vertical ? sizes[i].x : sizes[i].y;
                // A child that does not fit opens a new line — unless the line is empty, because a child
                // longer than the line must still be placed somewhere (UE puts it alone on its own line).
                if ( lines.empty() || ( lines.back().Count > 0 && lines.back().Main + spacing + m > lineLength ) )
                    lines.push_back( { i, 0, 0.0f, 0.0f } );
                WrapLine& line = lines.back();
                line.Main += ( line.Count > 0 ? spacing : 0.0f ) + m;
                line.Cross = std::max( line.Cross, c );
                ++line.Count;
            }
            return lines;
        }

        // UE SScaleBox::ComputeContentScale, for a content of @p content px in an area of @p area px.
        inline float ScaleBoxScale( const LayoutGroupParams& p, const glm::vec2& content, const glm::vec2& area )
        {
            float       s  = 1.0f;
            const float sx = content.x > 0.0f ? area.x / content.x : 1.0f;
            const float sy = content.y > 0.0f ? area.y / content.y : 1.0f;
            switch ( p.Stretch )
            {
                case LayoutScaleStretch::None:
                case LayoutScaleStretch::Fill:
                    s = 1.0f;
                    break;
                case LayoutScaleStretch::ScaleToFit:
                    s = std::min( sx, sy );
                    break;
                case LayoutScaleStretch::ScaleToFitX:
                    s = sx;
                    break;
                case LayoutScaleStretch::ScaleToFitY:
                    s = sy;
                    break;
                case LayoutScaleStretch::ScaleToFill:
                    s = std::max( sx, sy );
                    break;
                case LayoutScaleStretch::UserSpecified:
                    s = p.UserScale;
                    break;
            }
            if ( p.StretchDirection == LayoutScaleDirection::DownOnly )
                s = std::min( s, 1.0f );
            else if ( p.StretchDirection == LayoutScaleDirection::UpOnly )
                s = std::max( s, 1.0f );
            return std::max( 0.0f, s );
        }
    } // namespace LayoutDetail

    // Total content size (px) a layout group needs for `childSizes` — main axis = sum + spacing, cross axis =
    // max child — plus padding. Used by the Content Size Fitter to size a container to its children, and
    // as the desired size of a SizeBox inside its parent's group. NO CHILDREN = NO CONTENT: the padding is
    // the whole answer (zero when there is none), for every group type.
    inline glm::vec2 MeasureLayoutGroup( const LayoutGroupParams& p, const std::vector<glm::vec2>& childSizes )
    {
        const int       n       = static_cast<int>( childSizes.size() );
        const float     spacing = n > 1 ? p.Spacing * ( n - 1 ) : 0.0f;
        const glm::vec2 pad( p.PaddingL + p.PaddingR, p.PaddingT + p.PaddingB );
        switch ( p.Type )
        {
            case LayoutGroupType::Horizontal:
            case LayoutGroupType::Vertical:
            {
                const bool horiz    = p.Type == LayoutGroupType::Horizontal;
                float      mainSum  = 0.0f;
                float      crossMax = 0.0f;
                for ( const glm::vec2& s : childSizes )
                {
                    mainSum += horiz ? s.x : s.y;
                    crossMax = std::max( crossMax, horiz ? s.y : s.x );
                }
                const float mainTotal  = mainSum + spacing + ( horiz ? pad.x : pad.y );
                const float crossTotal = crossMax + ( horiz ? pad.y : pad.x );
                return horiz ? glm::vec2( mainTotal, crossTotal ) : glm::vec2( crossTotal, mainTotal );
            }
            case LayoutGroupType::Grid:
            {
                const float cw   = std::max( 1.0f, p.CellSize.x );
                const float ch   = std::max( 1.0f, p.CellSize.y );
                const int   cols = n > 0 ? ( p.Columns > 0 ? p.Columns : n ) : 0;
                const int   rows = n > 0 ? ( n + cols - 1 ) / cols : 0;
                return { cols * cw + std::max( 0, cols - 1 ) * p.Spacing + pad.x,
                         rows * ch + std::max( 0, rows - 1 ) * p.Spacing + pad.y };
            }
            case LayoutGroupType::Wrap:
            {
                // Without an explicit WrapSize there is no line length to wrap at before the container exists,
                // so the desired size is the single unwrapped line (UE SWrapBox with bExplicitWrapSize=false).
                const float line = p.WrapSize > 0.0f ? p.WrapSize : std::numeric_limits<float>::max();
                float       main = 0.0f, cross = 0.0f;
                const auto  lines = LayoutDetail::WrapLines( childSizes, line, p.Spacing, p.WrapVertical );
                for ( const auto& l : lines )
                {
                    main = std::max( main, l.Main );
                    cross += l.Cross;
                }
                cross += lines.size() > 1 ? p.Spacing * static_cast<float>( lines.size() - 1 ) : 0.0f;
                return ( p.WrapVertical ? glm::vec2( cross, main ) : glm::vec2( main, cross ) ) + pad;
            }
            case LayoutGroupType::Overlay:
                return LayoutDetail::MaxOf( childSizes ) + pad;
            case LayoutGroupType::UniformGrid:
            {
                if ( n == 0 )
                    return pad;
                const glm::vec2 cell = glm::max( LayoutDetail::MaxOf( childSizes ), p.MinSlotSize );
                const int       cols = LayoutDetail::UniformColumns( p, n );
                const int       rows = ( n + cols - 1 ) / cols;
                return { cols * cell.x + ( cols - 1 ) * p.Spacing + pad.x,
                         rows * cell.y + ( rows - 1 ) * p.Spacing + pad.y };
            }
            case LayoutGroupType::SizeBox:
            {
                const glm::vec2 content = LayoutDetail::MaxOf( childSizes ) + pad;
                return { LayoutDetail::SizeBoxAxis( content.x, p.SizeMin.x, p.SizeMax.x, p.SizeOverride.x ),
                         LayoutDetail::SizeBoxAxis( content.y, p.SizeMin.y, p.SizeMax.y, p.SizeOverride.y ) };
            }
            case LayoutGroupType::ScaleBox:
            {
                const float userScale = p.Stretch == LayoutScaleStretch::UserSpecified ? p.UserScale : 1.0f;
                return LayoutDetail::MaxOf( childSizes ) * userScale + pad;
            }
        }
        return pad;
    }

    // Lay the children out inside `container`: one ArrangedSlot per slot, in order.
    inline std::vector<ArrangedSlot> ArrangeLayoutGroup( const Rect& container, const LayoutGroupParams& p,
                                                         const std::vector<LayoutSlot>& slots )
    {
        std::vector<ArrangedSlot> out;
        out.reserve( slots.size() );

        const float x0     = container.X + p.PaddingL;
        const float y0     = container.Y + p.PaddingT;
        const float innerW = std::max( 0.0f, container.W - p.PaddingL - p.PaddingR );
        const float innerH = std::max( 0.0f, container.H - p.PaddingT - p.PaddingB );
        const int   n      = static_cast<int>( slots.size() );

        // A child at its preferred size inside @p cell: the whole cell when StretchCross, else centred.
        auto placeIn = [&]( const Rect& cell, const glm::vec2& pref ) -> Rect
        {
            if ( p.StretchCross )
                return cell;
            const float w = std::min( pref.x, cell.W );
            const float h = std::min( pref.y, cell.H );
            return { cell.X + ( cell.W - w ) * 0.5f, cell.Y + ( cell.H - h ) * 0.5f, w, h };
        };

        switch ( p.Type )
        {
            case LayoutGroupType::Horizontal:
            case LayoutGroupType::Vertical:
            {
                const bool         horiz = p.Type == LayoutGroupType::Horizontal;
                const float        avail = horiz ? innerW : innerH;
                std::vector<float> main( slots.size() );
                float              used      = n > 1 ? p.Spacing * ( n - 1 ) : 0.0f;
                float              growTotal = 0.0f;
                for ( std::size_t i = 0; i < slots.size(); ++i )
                {
                    main[i] = horiz ? slots[i].Pref.x : slots[i].Pref.y;
                    used += main[i];
                    growTotal += std::max( 0.0f, slots[i].Grow );
                }

                if ( used <= avail )
                {
                    // Grow: the leftover shared by weight.
                    const float leftover = avail - used;
                    if ( growTotal > 0.0f )
                        for ( std::size_t i = 0; i < slots.size(); ++i )
                            main[i] += leftover * std::max( 0.0f, slots[i].Grow ) / growTotal;
                }
                else
                {
                    // Shrink (Slate ArrangeChildrenInStack, StretchContent): the overflow is taken back in
                    // proportion to Shrink * basis; a child that reaches its Min is frozen there and the rest of
                    // the overflow goes round again among the others. One pass per child at most solves it.
                    float             overflow = used - avail;
                    std::vector<bool> frozen( slots.size(), false );
                    for ( std::size_t i = 0; i < slots.size(); ++i )
                        frozen[i] = slots[i].Shrink <= 0.0f;
                    for ( int pass = 0; pass < n && overflow > 1e-4f; ++pass )
                    {
                        float total = 0.0f;
                        for ( std::size_t i = 0; i < slots.size(); ++i )
                            if ( !frozen[i] )
                                total += slots[i].Shrink * ( horiz ? slots[i].Pref.x : slots[i].Pref.y );
                        if ( total <= 1e-6f )
                            break;
                        float taken = 0.0f;
                        for ( std::size_t i = 0; i < slots.size(); ++i )
                        {
                            if ( frozen[i] )
                                continue;
                            const float basis = horiz ? slots[i].Pref.x : slots[i].Pref.y;
                            const float mn    = horiz ? slots[i].Min.x : slots[i].Min.y;
                            const float cut   = overflow * slots[i].Shrink * basis / total;
                            if ( main[i] - cut <= mn )
                            {
                                taken += main[i] - std::max( 0.0f, mn );
                                main[i]   = std::max( 0.0f, mn );
                                frozen[i] = true;
                            }
                            else
                            {
                                taken += cut;
                                main[i] -= cut;
                            }
                        }
                        overflow -= taken;
                    }
                }

                float pos = horiz ? x0 : y0;
                for ( std::size_t i = 0; i < slots.size(); ++i )
                {
                    const glm::vec2& s = slots[i].Pref;
                    if ( horiz )
                    {
                        const float h = p.StretchCross ? innerH : s.y;
                        const float y = p.StretchCross ? y0 : y0 + ( innerH - h ) * 0.5f;
                        out.push_back( { { pos, y, main[i], h } } );
                    }
                    else
                    {
                        const float w = p.StretchCross ? innerW : s.x;
                        const float x = p.StretchCross ? x0 : x0 + ( innerW - w ) * 0.5f;
                        out.push_back( { { x, pos, w, main[i] } } );
                    }
                    pos += main[i] + p.Spacing;
                }
                break;
            }
            case LayoutGroupType::Grid:
            {
                const float cw = std::max( 1.0f, p.CellSize.x );
                const float ch = std::max( 1.0f, p.CellSize.y );
                const int   cols =
                     p.Columns > 0
                            ? p.Columns
                            : std::max( 1, static_cast<int>( ( innerW + p.Spacing ) / ( cw + p.Spacing ) ) );
                for ( int i = 0; i < n; ++i )
                    out.push_back( { { x0 + ( i % cols ) * ( cw + p.Spacing ),
                                       y0 + ( i / cols ) * ( ch + p.Spacing ), cw, ch } } );
                break;
            }
            case LayoutGroupType::Wrap:
            {
                std::vector<glm::vec2> sizes;
                sizes.reserve( slots.size() );
                for ( const LayoutSlot& s : slots )
                    sizes.push_back( s.Pref );
                const float line  = p.WrapSize > 0.0f ? p.WrapSize : ( p.WrapVertical ? innerH : innerW );
                float       cross = p.WrapVertical ? x0 : y0;
                for ( const auto& l : LayoutDetail::WrapLines( sizes, line, p.Spacing, p.WrapVertical ) )
                {
                    float main = p.WrapVertical ? y0 : x0;
                    for ( std::size_t i = l.First; i < l.First + l.Count; ++i )
                    {
                        const glm::vec2& s = sizes[i];
                        if ( p.WrapVertical )
                        {
                            const float w = p.StretchCross ? l.Cross : s.x;
                            out.push_back( { { cross + ( l.Cross - w ) * 0.5f, main, w, s.y } } );
                            main += s.y + p.Spacing;
                        }
                        else
                        {
                            const float h = p.StretchCross ? l.Cross : s.y;
                            out.push_back( { { main, cross + ( l.Cross - h ) * 0.5f, s.x, h } } );
                            main += s.x + p.Spacing;
                        }
                    }
                    cross += l.Cross + p.Spacing;
                }
                break;
            }
            case LayoutGroupType::Overlay:
            case LayoutGroupType::SizeBox:
            {
                // Both arrange every child over the whole inner rect (UE SOverlay / SSizeBox::OnArrangeChildren);
                // the SizeBox's constraints act on its DESIRED size, which is MeasureLayoutGroup's business.
                const Rect inner{ x0, y0, innerW, innerH };
                for ( const LayoutSlot& s : slots )
                    out.push_back( { placeIn( inner, s.Pref ) } );
                break;
            }
            case LayoutGroupType::UniformGrid:
            {
                if ( n == 0 )
                    break;
                const int   cols  = LayoutDetail::UniformColumns( p, n );
                const int   rows  = ( n + cols - 1 ) / cols;
                const float cellW = std::max( 0.0f, ( innerW - p.Spacing * ( cols - 1 ) ) / cols );
                const float cellH = std::max( 0.0f, ( innerH - p.Spacing * ( rows - 1 ) ) / rows );
                for ( int i = 0; i < n; ++i )
                {
                    const Rect cell{ x0 + ( i % cols ) * ( cellW + p.Spacing ),
                                     y0 + ( i / cols ) * ( cellH + p.Spacing ), cellW, cellH };
                    out.push_back( { placeIn( cell, slots[i].Pref ) } );
                }
                break;
            }
            case LayoutGroupType::ScaleBox:
            {
                // Every child is scaled by the one factor that fits the CONTENT (their union desired size), so a
                // ScaleBox of several children scales them as one picture, centred in the inner rect.
                std::vector<glm::vec2> sizes;
                sizes.reserve( slots.size() );
                for ( const LayoutSlot& s : slots )
                    sizes.push_back( s.Pref );
                const glm::vec2 content = LayoutDetail::MaxOf( sizes );
                if ( p.Stretch == LayoutScaleStretch::Fill )
                {
                    for ( std::size_t i = 0; i < slots.size(); ++i )
                        out.push_back( { { x0, y0, innerW, innerH } } );
                    break;
                }
                const float s = LayoutDetail::ScaleBoxScale( p, content, { innerW, innerH } );
                for ( const LayoutSlot& slot : slots )
                {
                    const float w = slot.Pref.x * s;
                    const float h = slot.Pref.y * s;
                    out.push_back( { { x0 + ( innerW - w ) * 0.5f, y0 + ( innerH - h ) * 0.5f, w, h }, s } );
                }
                break;
            }
        }
        return out;
    }

    // The rect-only form, for a caller with preferred sizes and grow weights and nothing else: every other
    // slot property at its neutral value. `childFlex` empty = all 0.
    inline std::vector<Rect> SolveLayoutGroup( const Rect& container, const LayoutGroupParams& p,
                                               const std::vector<glm::vec2>& childSizes,
                                               const std::vector<float>&     childFlex = {} )
    {
        std::vector<LayoutSlot> slots( childSizes.size() );
        for ( std::size_t i = 0; i < childSizes.size(); ++i )
        {
            slots[i].Pref = childSizes[i];
            slots[i].Grow = i < childFlex.size() ? childFlex[i] : 0.0f;
        }
        std::vector<Rect> out;
        out.reserve( slots.size() );
        for ( const ArrangedSlot& a : ArrangeLayoutGroup( container, p, slots ) )
            out.push_back( a.R );
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
