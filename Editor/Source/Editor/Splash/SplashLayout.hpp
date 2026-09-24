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
    inline constexpr float kCounterFontSize = 12.0f; // monospaced digits, white at 62 %
    inline constexpr float kVersionFontSize = 12.0f; // regular, white at 62 %
    inline constexpr float kStageAlpha      = 0.62f;

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
        Rect Counter; // right-aligned, on the stage's line
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

    /// How much of the start is done, in [0, 1]. @p index is the step NOW RUNNING (0-based), so the
    /// first step shows an empty bar and the last one shows (total-1)/total: the bar never claims a step
    /// is finished while its label is still on screen. `total == 0` means the editor does not know its
    /// plan yet (the renderer is still coming up), and that is an empty bar, not a division.
    [[nodiscard]] constexpr double ProgressFraction( const std::size_t index, const std::size_t total )
    {
        if ( total == 0 )
            return 0.0;
        return static_cast<double>( std::min( index, total ) ) / static_cast<double>( total );
    }

    /// "N / M   P%" — the step being run (1-based, so the last step reads M / M) and the share of the
    /// start that is done. Empty while the plan is unknown: a counter reading "1 / 0" or "0 / 0   0%"
    /// would be a number that says nothing, drawn where a person looks for progress.
    [[nodiscard]] inline std::string FormatProgress( const std::size_t index, const std::size_t total )
    {
        if ( total == 0 )
            return {};
        const std::size_t step    = std::min( index + 1, total );
        const int         percent = static_cast<int>( ProgressFraction( index, total ) * 100.0 );
        return std::to_string( step ) + " / " + std::to_string( total ) + "   " + std::to_string( percent ) + "%";
    }

    [[nodiscard]] constexpr Layout ComputeLayout( const double fraction )
    {
        const double clamped = fraction < 0.0 ? 0.0 : ( fraction > 1.0 ? 1.0 : fraction );
        const float  inner   = kWidth - 2.0f * kMargin;
        // Half of each text column: the stage label takes the left, the counter the right. A label longer
        // than its half is truncated by the platform, never drawn under the counter.
        const float half = inner * 0.5f;

        Layout layout;
        layout.Project  = { kMargin, 58.0f, half, LineHeight( kProjectFontSize ) };
        layout.Stage    = { kMargin, 36.0f, half, LineHeight( kStageFontSize ) };
        layout.Counter  = { kMargin + half, 36.0f, half, LineHeight( kCounterFontSize ) };
        layout.Version  = { kMargin + half, 58.0f, half, LineHeight( kVersionFontSize ) };
        layout.BarTrack = { kMargin, kBarY, inner, kBarHeight };
        layout.BarFill  = { kMargin, kBarY, static_cast<float>( inner * clamped ), kBarHeight };
        return layout;
    }

    /// HOW MUCH THE DESIGN IS SHRUNK TO FIT A SMALL SCREEN, never enlarged. The design is 1200x675 points,
    /// which is most of a 13-inch laptop's screen; a splash wider than the screen would lose its right-hand
    /// counter off the edge, so it is scaled as ONE picture (text, bar and image together) to at most
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
