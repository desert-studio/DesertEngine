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
//
// WHAT THE LINES SAY is here too (`StageText` … `LogText`), from one `Desert::Core::StartupView` the splash's own
// thread samples off the start's progress (STARTUP-PROGRESS): both platforms draw the same words.

#include <Engine/Core/StartupProgress.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
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
    inline constexpr float kTimeFontSize    = 11.0f; // monospaced digits, white at 45 %
    inline constexpr float kLogFontSize     = 11.0f; // regular, white at 45 %
    inline constexpr float kStageAlpha      = 0.62f;
    inline constexpr float kItemAlpha       = 0.45f;
    inline constexpr float kLogAlpha        = 0.45f;

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

    // THE DETAIL LOG (Unity's Background Tasks list): the last finished phases and long items, in the
    // RIGHT half above the version line, so it never reaches the wordmark (left half). Its bottom edge at
    // y 96 clears the version box (y 70 + 18.2) by 7.8 points. Black at 35 % under the lines keeps them
    // legible over the sky.
    inline constexpr float  kLogPanelX      = 648.0f;
    inline constexpr float  kLogPanelY      = 96.0f;
    inline constexpr float  kLogPanelW      = 504.0f; // to the right margin: 648 + 504 = 1200 - 48
    inline constexpr float  kLogPanelH      = 90.0f;
    inline constexpr float  kLogPanelAlpha  = 0.35f;
    inline constexpr float  kLogPanelRadius = 6.0f;
    inline constexpr float  kLogInset       = 10.0f; // the lines' left and right padding inside the panel
    inline constexpr std::size_t kLogLines  = Desert::Core::StartupProgress::kLogShown;

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
        Rect Item;    // left-aligned, the left 2/3: "DeferredLighting Direct · 12.4 s,  Bistro_Facade.tex"
        Rect Time;    // right-aligned, the right 1/3 of the item's line: "3:12 · about 1:05 left"
        Rect Version; // right-aligned, on the project's line
        Rect BarTrack;
        Rect BarFill; // same origin as the track, width = track * fraction
        Rect LogPanel; // the detail log's backing, black at LogPanelAlpha, corners kLogPanelRadius
        // Left-aligned, inside the panel; [0] is the BOTTOM line and holds the newest entry.
        std::array<Rect, kLogLines> LogLines{};
        float                       LogPanelAlpha = kLogPanelAlpha;
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
        // line gives its left two thirds to the running items (names are long) and the right third to
        // the elapsed time and the time left.
        const float half      = inner * 0.5f;
        const float itemWidth = inner * 2.0f / 3.0f;
        const float timeWidth = inner - itemWidth;
        const float logLine   = LineHeight( kLogFontSize );
        // The lines are centred in the panel: 6 x 14.3 = 85.8 of its 90 points.
        const float logBottom = kLogPanelY + ( kLogPanelH - logLine * static_cast<float>( kLogLines ) ) * 0.5f;

        Layout layout;
        layout.Project  = { kMargin, kProjectY, half, LineHeight( kProjectFontSize ) };
        layout.Stage    = { kMargin, kStageY, half, LineHeight( kStageFontSize ) };
        layout.Percent  = { kMargin + half, kStageY, half, LineHeight( kPercentFontSize ) };
        layout.Item     = { kMargin, kItemY, itemWidth, LineHeight( kItemFontSize ) };
        layout.Time     = { kMargin + itemWidth, kItemY, timeWidth, LineHeight( kTimeFontSize ) };
        layout.Version  = { kMargin + half, kProjectY, half, LineHeight( kVersionFontSize ) };
        layout.BarTrack = { kMargin, kBarY, inner, kBarHeight };
        layout.BarFill  = { kMargin, kBarY, static_cast<float>( inner * clamped ), kBarHeight };
        layout.LogPanel = { kLogPanelX, kLogPanelY, kLogPanelW, kLogPanelH };
        for ( std::size_t i = 0; i < kLogLines; ++i )
            layout.LogLines[i] = { kLogPanelX + kLogInset, logBottom + logLine * static_cast<float>( i ),
                                   kLogPanelW - 2.0f * kLogInset, logLine };
        layout.LogPanelAlpha = kLogPanelAlpha;
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

    // ===== What the lines say, from one sample of the start's progress =====

    // U+00B7 in UTF-8: the separator of every live line.
    inline constexpr const char* kDot = "\xC2\xB7";
    // A running item shows its age once it has run this long; a shorter one is just its name.
    inline constexpr double kAgeShownSeconds = 1.0;

    /// "Loading the scene · phase 13 / 14 · 1240 / 1910" (the count only for a counted phase); "Closing..."
    /// once a person asked the start to stop; empty before the first phase.
    [[nodiscard]] inline std::string StageText( const Desert::Core::StartupView& view )
    {
        if ( view.StopRequested )
            return "Closing...";
        if ( view.Phase.empty() )
            return {};
        std::string text = view.Phase + "  " + kDot + "  phase " + std::to_string( view.PhaseNumber ) + " / " +
                           std::to_string( view.PhaseCount );
        if ( view.Total > 0 )
            text += std::string( "  " ) + kDot + "  " + std::to_string( view.Done ) + " / " +
                    std::to_string( view.Total );
        return text;
    }

    /// "62 %", rounded DOWN so that "100 %" is never shown before the start is over; empty before the first
    /// phase (a "0 %" there would be a number that says nothing).
    [[nodiscard]] inline std::string PercentText( const Desert::Core::StartupView& view )
    {
        if ( view.Phase.empty() && !view.Finished )
            return {};
        const double clamped = view.Fraction < 0.0 ? 0.0 : ( view.Fraction > 1.0 ? 1.0 : view.Fraction );
        return std::to_string( static_cast<int>( std::floor( clamped * 100.0 ) ) ) + " %";
    }

    /// One running item: "DeferredLighting Direct · 12.4 s", or just the name while it is younger than
    /// kAgeShownSeconds.
    [[nodiscard]] inline std::string ActiveText( const Desert::Core::ActiveWork& work )
    {
        if ( work.Seconds < kAgeShownSeconds )
            return work.Name;
        char seconds[32];
        std::snprintf( seconds, sizeof( seconds ), "%.1f s", work.Seconds );
        return work.Name + " " + kDot + " " + seconds;
    }

    /// The running items, oldest first (the sample holds at most kActiveShown); in `scene.reveal` they are
    /// what the reveal waits for. Empty once a person asked to stop.
    [[nodiscard]] inline std::string ItemText( const Desert::Core::StartupView& view )
    {
        if ( view.StopRequested )
            return {};
        std::string text;
        for ( const Desert::Core::ActiveWork& work : view.Active )
        {
            if ( !text.empty() )
                text += ",  ";
            text += ActiveText( work );
        }
        return text;
    }

    /// "3:12 · about 1:05 left"; only the elapsed time once the start is over; empty before the first phase.
    [[nodiscard]] inline std::string TimeText( const Desert::Core::StartupView& view )
    {
        if ( view.Phase.empty() && !view.Finished )
            return {};
        const std::string elapsed = Desert::Core::FormatDuration( view.ElapsedSeconds );
        if ( view.Finished )
            return elapsed;
        return elapsed + " " + kDot + " about " + Desert::Core::FormatDuration( view.RemainingSeconds ) + " left";
    }

    /// One line of the detail log: "0:31  Building shader maps  2254 items  95.4 s" — the time since the
    /// process started, then what the progress noted.
    [[nodiscard]] inline std::string LogText( const Desert::Core::StartupLogLine& line )
    {
        return Desert::Core::FormatDuration( line.AtSeconds ) + "  " + line.Text;
    }

    /// The text of log line @p row of the layout ([0] the bottom one): the newest entry at the bottom, older
    /// ones above it, empty where the log is shorter than the panel.
    [[nodiscard]] inline std::string LogRowText( const Desert::Core::StartupView& view, const std::size_t row )
    {
        if ( row >= view.Log.size() )
            return {};
        return LogText( view.Log[view.Log.size() - 1 - row] );
    }

    // ===== --splash-capture's progress.csv: one row per sample, the same on both platforms =====

    inline constexpr const char* kCaptureCsvHeader = "ms,fraction,phase,done,total,remaining,active0";

    /// One CSV field, quoted: phase labels and item names may hold commas.
    [[nodiscard]] inline std::string CsvField( const std::string& text )
    {
        std::string quoted = "\"";
        for ( const char c : text )
        {
            if ( c == '"' )
                quoted += '"';
            quoted += c;
        }
        return quoted + "\"";
    }

    /// The row of one sample, without the line end: ms since the process started, the fraction, the phase,
    /// its count, the seconds left and the oldest running item.
    [[nodiscard]] inline std::string CaptureCsvRow( const Desert::Core::StartupView& view )
    {
        char head[64];
        std::snprintf( head, sizeof( head ), "%.0f,%.6f,", view.ElapsedSeconds * 1000.0,
                       static_cast<double>( view.Fraction ) );
        char remaining[32];
        std::snprintf( remaining, sizeof( remaining ), "%.3f,", view.RemainingSeconds );
        return std::string( head ) + CsvField( view.Phase ) + "," + std::to_string( view.Done ) + "," +
               std::to_string( view.Total ) + "," + remaining +
               CsvField( view.Active.empty() ? std::string{} : view.Active.front().Name );
    }
} // namespace Desert::Editor::Splash
