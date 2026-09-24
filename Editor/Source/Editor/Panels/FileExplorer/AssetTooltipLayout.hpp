#pragma once

#include <algorithm>

// Where and how big the Content Browser's hover tooltip is — a pure function, so the rule that the
// tooltip stays small and on-screen is tested without ImGui (Desert/Tests/Editor/AssetTooltipLayout).
//
// Why it exists: selecting an asset used to reserve a 175 px info strip at the bottom of the Assets
// panel, and in a short docked panel that strip took nearly all of it. UE's Content Browser does it
// the other way round — a click only selects, and the asset's details appear in a tooltip after the
// cursor rests on the tile — so the information costs no layout space and never covers the grid.
namespace Desert::Editor::AssetTooltipLayout
{
    // UE shows its asset tooltip after the cursor rests for roughly half a second.
    inline constexpr float kHoverDelaySeconds = 0.5f;
    // Upper bounds: never wider than this, never taller than this share of the window.
    inline constexpr float kMaxWidth          = 420.0f;
    inline constexpr float kMaxHeightFraction = 0.5f;
    // The tooltip sits this far right/below the cursor so the pointer never hides its corner, and
    // keeps this far from the window edge when it is clamped.
    inline constexpr float kCursorOffset = 16.0f;
    inline constexpr float kEdgeMargin   = 4.0f;

    struct Rect
    {
        float X      = 0.0f;
        float Y      = 0.0f;
        float Width  = 0.0f;
        float Height = 0.0f;
    };

    inline bool ShouldShow( const float hoveredSeconds )
    {
        return hoveredSeconds >= kHoverDelaySeconds;
    }

    /// `wanted*` is the content's natural size; `window` is the editor window's rectangle. The
    /// result is capped at kMaxWidth x (kMaxHeightFraction * window height), placed below-right of
    /// the cursor, flipped to the other side of the cursor when that side has no room, and finally
    /// clamped so no edge leaves the window.
    inline Rect Compute( const float cursorX, const float cursorY, const float wantedWidth,
                         const float wantedHeight, const Rect& window )
    {
        const float usableW = std::max( 0.0f, window.Width - 2.0f * kEdgeMargin );
        const float usableH = std::max( 0.0f, window.Height - 2.0f * kEdgeMargin );

        Rect r;
        r.Width  = std::min( { wantedWidth, kMaxWidth, usableW } );
        r.Height = std::min( { wantedHeight, window.Height * kMaxHeightFraction, usableH } );

        const float right  = window.X + window.Width - kEdgeMargin;
        const float bottom = window.Y + window.Height - kEdgeMargin;

        r.X = cursorX + kCursorOffset;
        if ( r.X + r.Width > right )
            r.X = cursorX - kCursorOffset - r.Width;
        r.Y = cursorY + kCursorOffset;
        if ( r.Y + r.Height > bottom )
            r.Y = cursorY - kCursorOffset - r.Height;

        r.X = std::clamp( r.X, window.X + kEdgeMargin, std::max( window.X + kEdgeMargin, right - r.Width ) );
        r.Y = std::clamp( r.Y, window.Y + kEdgeMargin, std::max( window.Y + kEdgeMargin, bottom - r.Height ) );
        return r;
    }
} // namespace Desert::Editor::AssetTooltipLayout
