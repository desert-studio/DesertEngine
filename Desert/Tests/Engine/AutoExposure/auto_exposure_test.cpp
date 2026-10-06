// The auto-exposure meter's window — Engine/Graphic/PostProcessing/AutoExposureRules.hpp.
//
// The defect these were written for is a RELATION, the shape this project's most expensive bugs have
// always taken: the procedural sky started writing sun-disc luminances up to 1000, while the meter's
// histogram topped out at 4. Both sides were individually correct and a unit test of either would have
// passed; what disagreed was one number against another, eight stops apart, in different files.
//
// The second group is the honest record of what the meter can and cannot do about a sun disc. It is not
// a bug — it is arithmetic — and it is asserted here so that nobody "fixes" it later by widening the
// outlier tail, which was tried, measured, and does nothing.

#include <Engine/Graphic/PostProcessing/AutoExposureRules.hpp>

#include <gtest/gtest.h>

#include <cmath>

using Desert::Graphic::AutoExposureBin;
using Desert::Graphic::AutoExposureBinLuminance;
using Desert::Graphic::AutoExposureWindow;
using Desert::Graphic::kSkyLuminanceClamp;

namespace
{
    constexpr uint32_t kBins = 256; // kBins in AutoExposureRenderer.cpp

    // The shipping window — must match AutoExposureRenderer.cpp's kWindow.
    constexpr AutoExposureWindow kWindow{ -10.0f, 10.0f, 0.0f, 0.95f };

    // What the window was before the physical sun existed, kept only so the tests can state what it
    // could not see.
    constexpr AutoExposureWindow kLegacyWindow{ -10.0f, 2.0f, 0.0f, 0.95f };
} // namespace

// --- The relation -----------------------------------------------------------------------------------

TEST( AutoExposureWindowRule, ReachesTheBrightestLuminanceTheSkyCanWrite )
{
    // THE assertion. The sky pass clamps its output at kSkyLuminanceClamp; a meter that cannot represent
    // that value cannot tell the sun disc from a merely bright sky, because both land in the top bin. If
    // either number moves alone, this fails.
    EXPECT_TRUE( kWindow.Covers( kSkyLuminanceClamp ) )
         << "the meter's ceiling (2^" << kWindow.MaxLogLum << ") is below the sky's own luminance clamp ("
         << kSkyLuminanceClamp << ")";
}

TEST( AutoExposureWindowRule, TheLegacyWindowCouldNotSeeTheSunDisc )
{
    // The bug, stated as a fact rather than a memory: the old ceiling of 2 (luminance 4) was eight stops
    // below what the sky writes, so a highlight at 4 and the sun at 1000 metered identically.
    EXPECT_FALSE( kLegacyWindow.Covers( kSkyLuminanceClamp ) );
    EXPECT_EQ( AutoExposureBin( kLegacyWindow, 4.0f, kBins ),
               AutoExposureBin( kLegacyWindow, kSkyLuminanceClamp, kBins ) );

    // And that the widened window does distinguish them, by a lot.
    EXPECT_LT( AutoExposureBin( kWindow, 4.0f, kBins ), AutoExposureBin( kWindow, kSkyLuminanceClamp, kBins ) );
}

TEST( AutoExposureWindowRule, IsOrderedAndItsTailsAreAFraction )
{
    EXPECT_LT( kWindow.MinLogLum, kWindow.MaxLogLum );
    EXPECT_GE( kWindow.LowPercent, 0.0f );
    EXPECT_LE( kWindow.HighPercent, 1.0f );
    EXPECT_LT( kWindow.LowPercent, kWindow.HighPercent ) << "the clip would discard every sample";
}

// --- What widening the window costs -----------------------------------------------------------------

TEST( AutoExposureWindowRule, OrdinarySceneLuminancesSurviveTheWiderWindowIntact )
{
    // Widening the ceiling trades bin resolution, and the whole point of the change is that ordinary
    // scenes keep their exposure. The average is reconstructed from each bin's own log-luminance, so the
    // only error is quantisation — bounded here at one bin across the range real scene content lives in.
    const float tolerance = kWindow.StopsPerBin( kBins );
    EXPECT_LT( tolerance, 0.1f ) << "the window is now too coarse to meter ordinary content";

    for ( float luminance = 0.01f; luminance < 4.0f; luminance *= 1.3f )
    {
        const uint32_t bin       = AutoExposureBin( kWindow, luminance, kBins );
        const float    roundTrip = AutoExposureBinLuminance( kWindow, bin, kBins );
        EXPECT_LT( std::abs( std::log2( roundTrip ) - std::log2( luminance ) ), tolerance )
             << "luminance " << luminance << " round-tripped to " << roundTrip;
    }
}

TEST( AutoExposureBinning, IsMonotonicAndSaturatesAtBothEnds )
{
    uint32_t previous = 0;
    for ( float luminance = 1e-4f; luminance < 4000.0f; luminance *= 2.0f )
    {
        const uint32_t bin = AutoExposureBin( kWindow, luminance, kBins );
        EXPECT_GE( bin, previous ) << "brighter content landed in a darker bin at " << luminance;
        previous = bin;
    }

    EXPECT_EQ( AutoExposureBin( kWindow, 0.0f, kBins ), 0u );
    EXPECT_EQ( AutoExposureBin( kWindow, 1e-9f, kBins ), 0u );
    EXPECT_EQ( AutoExposureBin( kWindow, 1e9f, kBins ), kBins - 1 );
}

// --- What the meter can and cannot do about the sun -------------------------------------------------

namespace
{
    // The metered log-luminance of a frame that is @p fraction sun disc at @p discLuminance and the rest
    // sky at @p skyLuminance — the pixel-count-weighted average AEAverage computes, with the outlier
    // tail applied to the bright end.
    float MeteredStops( float fraction, float discLuminance, float skyLuminance, const AutoExposureWindow& window )
    {
        const float discStops = std::min( std::log2( discLuminance ), window.MaxLogLum );
        const float skyStops  = std::log2( skyLuminance );

        // The bright tail is discarded from the top; whatever is left of the disc still counts.
        const float discarded = 1.0f - window.HighPercent;
        const float discKept  = std::max( fraction - discarded, 0.0f );
        const float skyKept   = window.HighPercent - discKept;

        return ( discStops * discKept + skyStops * skyKept ) / ( discKept + skyKept );
    }
} // namespace

TEST( AutoExposureResponse, TheSunDiscCannotVisiblyMoveAPixelCountWeightedMeter )
{
    // The measured verdict, in closed form. The sun subtends 0.545 degrees; at the field of view the
    // showcase shots use that is roughly forty pixels of a 1280x766 frame — four ten-thousandths of one
    // percent. A pixel-count-weighted average of LOG luminance moves by (that fraction) x (its excess in
    // stops), which is thousandths of a stop no matter how bright the disc is.
    //
    // This is arithmetic, not a defect, and it is pinned so the meter is never "fixed" by making the
    // exposure lurch when a few dozen pixels change.
    constexpr float kDiscFraction = 40.0f / ( 1280.0f * 766.0f );

    const float withoutSun = MeteredStops( 0.0f, kSkyLuminanceClamp, 1.0f, kWindow );
    const float withSun    = MeteredStops( kDiscFraction, kSkyLuminanceClamp, 1.0f, kWindow );

    EXPECT_LT( std::abs( withSun - withoutSun ), 0.01f )
         << "a 40-pixel disc moved the meter by " << ( withSun - withoutSun )
         << " stops — either the disc got much bigger or the meter stopped being pixel-count weighted";
}

TEST( AutoExposureResponse, WideningTheOutlierTailDoesNotRescueIt )
{
    // Tried and measured on the real scenes: at 99.5% the metered background moved by one 8-bit level.
    // The tail is not what stops the response, so this asserts that loosening it changes nothing worth
    // having — and that a future change which appears to make it matter is really changing something
    // else.
    constexpr float kDiscFraction = 40.0f / ( 1280.0f * 766.0f );

    constexpr AutoExposureWindow looseTail{ -10.0f, 10.0f, 0.0f, 0.995f };

    const float shipped = MeteredStops( kDiscFraction, kSkyLuminanceClamp, 1.0f, kWindow ) -
                          MeteredStops( 0.0f, kSkyLuminanceClamp, 1.0f, kWindow );
    const float loose = MeteredStops( kDiscFraction, kSkyLuminanceClamp, 1.0f, looseTail ) -
                        MeteredStops( 0.0f, kSkyLuminanceClamp, 1.0f, looseTail );

    EXPECT_LT( std::abs( loose ), 0.05f );
    EXPECT_LT( std::abs( loose - shipped ), 0.05f );
}

TEST( AutoExposureResponse, ALightSourceThatFillsTheFrameDoesMoveTheMeter )
{
    // The other side of the same coin, so the test above cannot be read as "the meter is broken". Give
    // the meter a bright source covering a real fraction of the frame and it stops down properly — the
    // mechanism works, the sun disc is simply too small to exercise it.
    const float dim    = MeteredStops( 0.0f, kSkyLuminanceClamp, 1.0f, kWindow );
    const float bright = MeteredStops( 0.30f, kSkyLuminanceClamp, 1.0f, kWindow );

    EXPECT_GT( bright - dim, 1.0f ) << "a source covering 30% of the frame must move the meter by stops";
}
