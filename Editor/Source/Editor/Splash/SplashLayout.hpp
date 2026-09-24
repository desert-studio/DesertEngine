#pragma once

// WHERE EVERYTHING THE SPLASH DRAWS GOES, AND WHAT ITS COUNTER SAYS — as pure functions, so that the
// two native implementations (AppKit layers on macOS, a GDI window on Windows) cannot disagree about the
// picture and a suite can pin it without opening a window. Everything that does NOT change while the
// editor starts (the photograph, the DESERT / ENGINE wordmark and its glow, the darkening at the bottom,
// the copyright) is baked into `Resources/Splash/Splash.jpg` by `Tools/SplashBake`; only what changes is
// drawn live, and this file is the whole of where it goes.
//
// Units are POINTS of the 1200x675 design, with the origin at the BOTTOM-LEFT, because that is how the
// design was written ("y = 58 from the bottom") and how AppKit layers measure. A platform whose origin is
// at the top converts with `FlipY` below rather than keeping a second copy of the numbers.

#include <algorithm>
#include <cstddef>
#include <string>

namespace Desert::Editor::Splash
{
    inline constexpr float kWidth  = 1200.0f;
    inline constexpr float kHeight = 675.0f;
    // The left and right margin of every live element, the bar included.
    inline constexpr float kMargin = 48.0f;

    inline constexpr float kProjectFontSize = 14.0f; // semibold, white
    inline constexpr float kStageFontSize   = 12.0f; // regular, white at 62 %
    inline constexpr float kPercentFontSize = 12.0f; // monospaced digits, white at 62 %
    inline constexpr float kItemFontSize    = 11.0f; // regular, white at 45 %
    inline constexpr float kVersionFontSize = 12.0f; // regular, white at 62 %
    inline constexpr float kStageAlpha      = 0.62f;
    inline constexpr float kItemAlpha       = 0.45f;

    // The three text lines, bottom edges from the bottom of the splash: the project over the stage over
    // the item the stage is working on, all above the bar and all below the wordmark.
    inline constexpr float kProjectY = 70.0f;
    inline constexpr float kStageY   = 52.0f;
    inline constexpr float kItemY    = 36.0f;

    inline constexpr float kBarY          = 22.0f;
    inline constexpr float kBarHeight     = 2.0f;
    inline constexpr float kBarTrackAlpha = 0.18f; // white
    // The fill is the sand of the ENGINE half of the wordmark, so the one moving thing on the splash is
    // the same colour as the brand it sits under.
    inline constexpr float kBarFillRed   = 0.95f;
    inline constexpr float kBarFillGreen = 0.74f;
    inline constexpr float kBarFillBlue  = 0.45f;

    struct Rect
    {
        float X = 0.0f;
        float Y = 0.0f; // bottom edge, from the bottom of the splash
        float W = 0.0f;
        float H = 0.0f;
    };

    struct Layout
    {
        Rect Project; // left-aligned
        Rect Stage;   // left-aligned
        Rect Percent; // right-aligned, on the stage's line
        Rect Item;    // left-aligned, the whole width: "Cooking texture T_Rock_Albedo (37 / 212)"
        Rect Version; // right-aligned, on the project's line
        Rect BarTrack;
        Rect BarFill; // same origin as the track, width = track * fraction
    };

    /// A text box is its font's line height tall. 1.3 em holds the descenders of every system face
    /// either platform picks; the box's bottom is what the design's "y" names.
    [[nodiscard]] constexpr float LineHeight( const float fontSize )
    {
        return fontSize * 1.3f;
    }

    // WHERE THE PICTURE'S "ENGINE" IS, which no live line may reach: its glyphs, measured off the pixels
    // of `Resources/Splash/Splash.jpg` (drawn by `Tools/SplashBake/Compose.swift` at x 52, y 94), span
    // x 53.5..194 and y 99.5..113.5. The project line at y 80 sat 1.3 points under them with the glow on
    // top, and read as one block with the wordmark.
    inline constexpr Rect kWordmarkEngine = { 53.0f, 99.0f, 142.0f, 15.0f };
    // The clear space kept under it: the glyphs' glow (radius 12, most of it faint) is not part of the
    // measured box.
    inline constexpr float kWordmarkClearance = 8.0f;

    [[nodiscard]] constexpr Layout ComputeLayout( const double fraction )
    {
        const double clamped = fraction < 0.0 ? 0.0 : ( fraction > 1.0 ? 1.0 : fraction );
        const float  inner   = kWidth - 2.0f * kMargin;
        // Half of each text column: the stage label takes the left, the percentage the right. A label
        // longer than its half is truncated by the platform, never drawn under the percentage. The item
        // line has no right-hand neighbour and takes the whole width: asset names are long.
        const float half = inner * 0.5f;

        Layout layout;
        layout.Project  = { kMargin, kProjectY, half, LineHeight( kProjectFontSize ) };
        layout.Stage    = { kMargin, kStageY, half, LineHeight( kStageFontSize ) };
        layout.Percent  = { kMargin + half, kStageY, half, LineHeight( kPercentFontSize ) };
        layout.Item     = { kMargin, kItemY, inner, LineHeight( kItemFontSize ) };
        layout.Version  = { kMargin + half, kProjectY, half, LineHeight( kVersionFontSize ) };
        layout.BarTrack = { kMargin, kBarY, inner, kBarHeight };
        layout.BarFill  = { kMargin, kBarY, static_cast<float>( inner * clamped ), kBarHeight };
        return layout;
    }

    /// HOW MUCH THE DESIGN IS SHRUNK TO FIT A SMALL SCREEN, never enlarged. The design is 1200x675 points,
    /// which is most of a 13-inch laptop's screen; a splash wider than the screen would lose its right-hand
    /// percentage off the edge, so it is scaled as ONE picture (text, bar and image together) to at most
    /// @p screenShare of the usable area in each direction.
    [[nodiscard]] constexpr float FitScale( const float screenWidth, const float screenHeight,
                                            const float screenShare = 0.8f )
    {
        if ( screenWidth <= 0.0f || screenHeight <= 0.0f )
            return 1.0f;
        const float byWidth  = screenWidth * screenShare / kWidth;
        const float byHeight = screenHeight * screenShare / kHeight;
        const float fit      = byWidth < byHeight ? byWidth : byHeight;
        return fit < 1.0f ? fit : 1.0f;
    }

    // ===== The only motion on the splash =====
    //
    // A fade in at the start and a crossfade into the editor at the end. The picture itself holds still
    // (owner, 2026-09-24: the push-in was removed); the live stage text and the bar are what show the
    // start is alive.
    inline constexpr float kFadeInSeconds   = 0.2f;
    inline constexpr float kFadeOutSeconds  = 0.2f;

    [[nodiscard]] constexpr float Clamp01( const float v )
    {
        return v < 0.0f ? 0.0f : ( v > 1.0f ? 1.0f : v );
    }


    /// Opacity of the whole splash @p seconds after it appeared (fade in), or @p seconds after it was
    /// asked to close when @p closing (fade out).
    [[nodiscard]] constexpr float SplashOpacity( const float seconds, const bool closing )
    {
        return closing ? 1.0f - Clamp01( seconds / kFadeOutSeconds ) : Clamp01( seconds / kFadeInSeconds );
    }

    /// The same rectangle measured from the TOP, for a platform whose origin is there (GDI).
    [[nodiscard]] constexpr Rect FlipY( const Rect& rect )
    {
        return { rect.X, kHeight - rect.Y - rect.H, rect.W, rect.H };
    }
} // namespace Desert::Editor::Splash
