// ImageDiffMath — does the instrument that measures history artefacts actually measure them?
//
// WHAT IS UNDER TEST. Tools/ImageDiff/Source/ImageDiffMath.hpp, the whole of the arithmetic behind every
// number ImageDiff prints. Task HV decides whether four of Unreal's history-validation checks catch
// anything on our material, and it decides it by comparing rendered frames. If the comparison is wrong,
// every conclusion drawn from it is wrong in the same direction and nothing in a render would reveal it —
// which is exactly the class of defect this programme keeps paying for.
//
// THE CENTRAL CLAIM IS ABOUT `coherence`, NOT ABOUT THE COUNTS. Counting differing pixels is arithmetic
// anybody trusts. The reason this instrument was built is that a scalar difference cannot tell a GHOST
// (history kept when it should have been dropped: a smooth, displaced copy) from SPECKLE (history dropped
// when it should have been kept: sign-flipping single samples), and those two are the two directions a
// validation check trades between. So the tests below construct one field of each KIND, with the same mean
// absolute error, and require the instrument to separate them.

#include <ImageDiffMath.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <vector>

using Desert::Tools::ImageDiff::CoherenceOf;
using Desert::Tools::ImageDiff::Compare;
using Desert::Tools::ImageDiff::DiffResult;
using Desert::Tools::ImageDiff::Luma255;

namespace
{
    constexpr int kW        = 64;
    constexpr int kH        = 48;
    constexpr int kChannels = 4;

    /// A flat grey image, the base every field below is a perturbation of. Mid grey rather than black so
    /// that an error of either sign fits without clipping — a field clipped at zero would have a first
    /// difference the arithmetic never saw, and the test would be measuring the clip.
    ///
    /// OPAQUE, and that is not decoration: the first version of this helper filled all four components
    /// with the grey and the first assertion caught it, reporting a maximum difference of 127 where 10
    /// was expected. The instrument compares every channel it is given, a rendered frame is opaque, and
    /// a fixture whose alpha moves would have priced every measurement in this task against an alpha
    /// step nobody meant to make.
    std::vector<std::uint8_t> Flat( std::uint8_t value )
    {
        std::vector<std::uint8_t> image( static_cast<std::size_t>( kW ) * kH * kChannels, value );
        for ( std::size_t i = 3; i < image.size(); i += kChannels )
            image[i] = 255;
        return image;
    }

    void SetPixel( std::vector<std::uint8_t>& img, int x, int y, int r, int g, int b )
    {
        const std::size_t i = ( static_cast<std::size_t>( y ) * kW + x ) * kChannels;
        img[i + 0]          = static_cast<std::uint8_t>( r );
        img[i + 1]          = static_cast<std::uint8_t>( g );
        img[i + 2]          = static_cast<std::uint8_t>( b );
        img[i + 3]          = 255;
    }

    DiffResult Diff( const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b )
    {
        DiffResult d;
        EXPECT_TRUE( Compare( a.data(), b.data(), kW, kH, kChannels, 0, 0, kW, kH, d ) );
        return d;
    }
} // namespace

// ---------------------------------------------------------------------------------------------------
// The counts, which have to be right before the shape means anything
// ---------------------------------------------------------------------------------------------------

TEST( ImageDiffMath, IdenticalImagesReportZeroOfEverything )
{
    const auto a = Flat( 128 );
    const auto d = Diff( a, a );

    EXPECT_EQ( d.Pixels, static_cast<std::size_t>( kW ) * kH );
    EXPECT_EQ( d.Differing, 0u );
    EXPECT_EQ( d.MaxAbs, 0 );
    EXPECT_DOUBLE_EQ( d.MeanAbs, 0.0 );
    EXPECT_DOUBLE_EQ( d.RmsAbs, 0.0 );
    EXPECT_DOUBLE_EQ( d.LumaBias, 0.0 );

    // A zero field has no shape. 0/0 is not a coherence of any value, so the instrument answers 0.0 and
    // the reader is expected to look at Differing next to it. Asserted rather than assumed because a NaN
    // here would print as "nan" in a calibration table and be copied into a conclusion.
    EXPECT_DOUBLE_EQ( d.Coherence, 0.0 );
    EXPECT_FALSE( std::isnan( d.Coherence ) );
}

TEST( ImageDiffMath, OneChangedPixelIsCountedOnceAndLocated )
{
    const auto a = Flat( 128 );
    auto       b = Flat( 128 );
    SetPixel( b, 17, 29, 128, 138, 128 );

    const auto d = Diff( a, b );

    EXPECT_EQ( d.Differing, 1u );
    EXPECT_EQ( d.MaxAbs, 10 );
    EXPECT_EQ( d.MaxX, 17 );
    EXPECT_EQ( d.MaxY, 29 );

    // One channel of one pixel moved by 10, over kW*kH*4 samples.
    const double samples = static_cast<double>( kW ) * kH * kChannels;
    EXPECT_NEAR( d.MeanAbs, 10.0 / samples, 1e-12 );
    EXPECT_NEAR( d.RmsAbs, std::sqrt( 100.0 / samples ), 1e-12 );
}

TEST( ImageDiffMath, BiasIsSignedAndMeanAbsoluteIsNot )
{
    // Half the rows brighter by 4, half darker by 4: the absolute error is 4 everywhere and the bias is
    // zero. A tool that reported only one of the two would call this frame identical or call it uniformly
    // brighter, and both readings have been made in this programme's history.
    const auto a = Flat( 128 );
    auto       b = Flat( 128 );
    for ( int y = 0; y < kH; ++y )
        for ( int x = 0; x < kW; ++x )
            SetPixel( b, x, y, y < kH / 2 ? 132 : 124, y < kH / 2 ? 132 : 124, y < kH / 2 ? 132 : 124 );

    const auto d = Diff( a, b );

    EXPECT_NEAR( d.LumaBias, 0.0, 1e-9 );
    EXPECT_NEAR( d.MeanAbsLuma, 4.0, 1e-9 );
    EXPECT_EQ( d.Differing, static_cast<std::size_t>( kW ) * kH );
}

// ---------------------------------------------------------------------------------------------------
// The shape, which is the reason the instrument exists
// ---------------------------------------------------------------------------------------------------

TEST( ImageDiffMath, SpeckleAndSmearHaveTheSameSizeAndDifferentCoherence )
{
    const auto a = Flat( 128 );

    // SPECKLE: the error changes sign between every pair of neighbours, which is what a pixel falling
    // back on a single quarter-resolution sample looks like. Constructed as a checkerboard rather than
    // from a random generator so the expected first difference is exact arithmetic: every neighbour pair
    // differs by 8, and every pixel's absolute error is 4.
    auto speckle = Flat( 128 );
    for ( int y = 0; y < kH; ++y )
        for ( int x = 0; x < kW; ++x )
        {
            const int v = ( ( x + y ) & 1 ) ? 132 : 124;
            SetPixel( speckle, x, y, v, v, v );
        }

    // SMEAR: the same mean absolute error, spread as a slow ramp across the rectangle, which is what a
    // ghost looks like — a displaced copy of a smooth image. A triangle from -8 to +8 and back has mean
    // absolute value 4, matching the checkerboard, and a first difference of 32/kW per step.
    auto smear = Flat( 128 );
    for ( int y = 0; y < kH; ++y )
        for ( int x = 0; x < kW; ++x )
        {
            const double t     = static_cast<double>( x ) / ( kW - 1 ); // 0..1
            const double tri   = 1.0 - 4.0 * std::fabs( t - 0.5 );      // +1 .. -1 .. +1
            const int    value = 128 + static_cast<int>( std::lround( 8.0 * tri ) );
            SetPixel( smear, x, y, value, value, value );
        }

    const auto dSpeckle = Diff( a, speckle );
    const auto dSmear   = Diff( a, smear );

    // The two fields are the same SIZE of error, to within the rounding of the ramp to whole levels.
    EXPECT_NEAR( dSpeckle.MeanAbsLuma, 4.0, 0.01 );
    EXPECT_NEAR( dSmear.MeanAbsLuma, 4.0, 0.2 );

    // And a completely different SHAPE. The checkerboard's first difference is 8 everywhere along both
    // axes, so its coherence is 4/8 = 0.5; the ramp's is 32/kW per step, so its coherence is about
    // 4 / (16/kW) with the second axis contributing nothing at all.
    EXPECT_NEAR( dSpeckle.Coherence, 0.5, 0.02 );
    EXPECT_GT( dSmear.Coherence, 10.0 );

    // The claim in one line, which is the one a reader of the calibration table needs: the instrument
    // separates them by more than an order of magnitude while their scalar sizes agree.
    EXPECT_GT( dSmear.Coherence, 20.0 * dSpeckle.Coherence );
}

TEST( ImageDiffMath, CoherenceRisesWhenTheSameErrorIsSpreadOverMorePixels )
{
    // A monotone relation, which is what makes the number readable as "how smooth". Square waves of
    // period 2, 8 and 32 all have absolute error 4 everywhere; only the number of sign changes differs.
    const auto a = Flat( 128 );

    auto build = [&]( int period )
    {
        auto img = Flat( 128 );
        for ( int y = 0; y < kH; ++y )
            for ( int x = 0; x < kW; ++x )
            {
                const int v = ( ( x / ( period / 2 ) ) & 1 ) ? 132 : 124;
                SetPixel( img, x, y, v, v, v );
            }
        return img;
    };

    const auto fine   = Diff( a, build( 2 ) );
    const auto medium = Diff( a, build( 8 ) );
    const auto coarse = Diff( a, build( 32 ) );

    EXPECT_NEAR( fine.MeanAbsLuma, 4.0, 1e-6 );
    EXPECT_NEAR( medium.MeanAbsLuma, 4.0, 1e-6 );
    EXPECT_NEAR( coarse.MeanAbsLuma, 4.0, 1e-6 );

    EXPECT_LT( fine.Coherence, medium.Coherence );
    EXPECT_LT( medium.Coherence, coarse.Coherence );
}

TEST( ImageDiffMath, BothAxesEnterTheShape )
{
    // A field that is smooth along rows and ragged along columns must NOT read as smooth. LineJump was
    // built because the column half of a defect went unnoticed for a stage and a half; the same mistake
    // is available here and this is what closes it.
    const auto a = Flat( 128 );

    auto rowsSmooth = Flat( 128 ); // constant along x, alternating along y
    auto colsSmooth = Flat( 128 ); // alternating along x, constant along y
    for ( int y = 0; y < kH; ++y )
        for ( int x = 0; x < kW; ++x )
        {
            const int vy = ( y & 1 ) ? 132 : 124;
            const int vx = ( x & 1 ) ? 132 : 124;
            SetPixel( rowsSmooth, x, y, vy, vy, vy );
            SetPixel( colsSmooth, x, y, vx, vx, vx );
        }

    const auto dRows = Diff( a, rowsSmooth );
    const auto dCols = Diff( a, colsSmooth );

    // Each is flat along one axis and alternating along the other, so each averages a first difference of
    // 0 with one of 8: the mean gradient is 4 and the coherence is 4/4 = 1. The two must AGREE — the
    // instrument has no preferred axis — and both must sit above the fully ragged checkerboard's 0.5.
    EXPECT_NEAR( dRows.Coherence, dCols.Coherence, 1e-9 );
    EXPECT_NEAR( dRows.Coherence, 1.0, 0.02 );
}

// ---------------------------------------------------------------------------------------------------
// Refusals. A rectangle that silently moved would make two runs comparable by accident
// ---------------------------------------------------------------------------------------------------

TEST( ImageDiffMath, RectanglesOutsideTheImageAreRefusedRatherThanClamped )
{
    const auto a = Flat( 128 );
    DiffResult d;

    EXPECT_FALSE( Compare( a.data(), a.data(), kW, kH, kChannels, -1, 0, kW, kH, d ) );
    EXPECT_FALSE( Compare( a.data(), a.data(), kW, kH, kChannels, 0, -1, kW, kH, d ) );
    EXPECT_FALSE( Compare( a.data(), a.data(), kW, kH, kChannels, 0, 0, kW + 1, kH, d ) );
    EXPECT_FALSE( Compare( a.data(), a.data(), kW, kH, kChannels, 0, 0, kW, kH + 1, d ) );

    // Degenerate: a first difference needs two lines on each axis, and a rectangle one pixel wide would
    // report a mean gradient of zero along that axis and therefore an inflated coherence. Refused.
    EXPECT_FALSE( Compare( a.data(), a.data(), kW, kH, kChannels, 0, 0, 1, kH, d ) );
    EXPECT_FALSE( Compare( a.data(), a.data(), kW, kH, kChannels, 0, 0, kW, 1, d ) );

    // Fewer than three components is not an image this tool can take a luma from.
    EXPECT_FALSE( Compare( a.data(), a.data(), kW, kH, 2, 0, 0, kW, kH, d ) );
    EXPECT_FALSE( Compare( nullptr, a.data(), kW, kH, kChannels, 0, 0, kW, kH, d ) );
}

TEST( ImageDiffMath, LumaMatchesRec709AndCoherenceGuardsAgainstDivisionByZero )
{
    EXPECT_NEAR( Luma255( 255, 255, 255 ), 255.0, 1e-9 );
    EXPECT_NEAR( Luma255( 0, 0, 0 ), 0.0, 1e-9 );
    EXPECT_NEAR( Luma255( 255, 0, 0 ), 0.2126 * 255.0, 1e-9 );
    EXPECT_NEAR( Luma255( 0, 255, 0 ), 0.7152 * 255.0, 1e-9 );
    EXPECT_NEAR( Luma255( 0, 0, 255 ), 0.0722 * 255.0, 1e-9 );

    EXPECT_DOUBLE_EQ( CoherenceOf( 4.0, 0.0 ), 0.0 );
    EXPECT_DOUBLE_EQ( CoherenceOf( 4.0, 2.0 ), 2.0 );
}

TEST( ImageDiffMath, ASubRectangleSeesOnlyItsOwnPixels )
{
    // The rectangle is how every measurement in CALIBRATION.md is scoped — "the full width by the upper
    // 71.9%" — so a rectangle that leaked would corrupt every one of them.
    const auto a = Flat( 128 );
    auto       b = Flat( 128 );
    SetPixel( b, 5, 5, 128, 200, 128 );

    DiffResult inside;
    DiffResult outside;
    EXPECT_TRUE( Compare( a.data(), b.data(), kW, kH, kChannels, 0, 0, 10, 10, inside ) );
    EXPECT_TRUE( Compare( a.data(), b.data(), kW, kH, kChannels, 20, 20, 40, 40, outside ) );

    EXPECT_EQ( inside.Differing, 1u );
    EXPECT_EQ( outside.Differing, 0u );
    EXPECT_EQ( inside.Pixels, 100u );
    EXPECT_EQ( outside.Pixels, 400u );
}

TEST( ImageDiffMath, AnOffsetRectangleReadsItsOwnROWS )
{
    // ADDED AFTER A SABOTAGE STAYED GREEN. Replacing the pixel index `y * width + x` with
    // `(y - y0) * width + x` — the row offset applied twice, which is the single likeliest slip in this
    // arithmetic — passed the whole suite. The test above could not see it because both of its
    // rectangles start at row 0 or sit in a column range the difference does not reach.
    //
    // So: one marked pixel per row, at the SAME column, over three consecutive rows, with different
    // magnitudes. A rectangle covering rows 21 and 22 must report exactly those two and the larger of
    // their two magnitudes. An offset applied twice would read rows 0 and 1, where nothing was marked,
    // and report no difference at all.
    //
    // TWO ROWS AND NOT ONE, because the coherence needs a first difference on each axis and Compare
    // refuses a rectangle thinner than that — see RectanglesOutsideTheImageAreRefusedRatherThanClamped.
    const auto a = Flat( 128 );
    auto       b = Flat( 128 );
    SetPixel( b, 33, 20, 128, 128 + 30, 128 );
    SetPixel( b, 33, 21, 128, 128 + 12, 128 );
    SetPixel( b, 33, 22, 128, 128 + 7, 128 );

    DiffResult d;
    EXPECT_TRUE( Compare( a.data(), b.data(), kW, kH, kChannels, 30, 21, 40, 23, d ) );

    EXPECT_EQ( d.Differing, 2u );
    EXPECT_EQ( d.MaxAbs, 12 );
    EXPECT_EQ( d.MaxX, 33 );
    EXPECT_EQ( d.MaxY, 21 );
}

TEST( ImageDiffMath, TheMaximumIsTheLARGESTDifferenceAndNotTheFirstOrTheLast )
{
    // ADDED AFTER A SABOTAGE STAYED GREEN. Replacing `abs > MaxAbs` with "keep the first non-zero" passed
    // the whole suite, because every fixture had exactly one difference in it. A maximum that is really a
    // first-hit would understate every frame this task measures, and the number is quoted in the
    // calibration table as "max N of 255".
    //
    // Three differences of different sizes, deliberately NOT in scan order, so a "keep the first", a
    // "keep the last" and a "keep the largest" all give different answers.
    const auto a = Flat( 128 );
    auto       b = Flat( 128 );
    SetPixel( b, 1, 1, 128, 128 + 9, 128 );  // first in scan order, small
    SetPixel( b, 2, 5, 128, 128 + 40, 128 ); // the real maximum, in the middle
    SetPixel( b, 3, 9, 128, 128 + 20, 128 ); // last in scan order, middling

    const auto d = Diff( a, b );

    EXPECT_EQ( d.Differing, 3u );
    EXPECT_EQ( d.MaxAbs, 40 );
    EXPECT_EQ( d.MaxX, 2 );
    EXPECT_EQ( d.MaxY, 5 );
}

TEST( ImageDiffMath, EveryNEIGHBOUR_PAIR_ENTERS_THE_GRADIENT_INCLUDING_THE_FIRST )
{
    // ADDED AFTER A SABOTAGE STAYED GREEN. Starting the horizontal first-difference loop at x = 2 instead
    // of x = 1 — one pair of 63 — passed every other test in this file, because each of them measures a
    // field whose gradient is the same at every pair, and dropping one of many identical terms does not
    // move their mean. That is exactly how an off-by-one in an instrument survives: the fixtures are all
    // uniform.
    //
    // So a field with EXACTLY ONE step in it, at the first pair. Column 0 stays at the base and every
    // other column is 8 levels above it, so along x there is one pair of magnitude 8 and rectW - 2 pairs
    // of zero, and along y there is nothing at all. The expected mean is arithmetic that can be written
    // down: 0.5 * (8 / (kW - 1) + 0).
    const auto a = Flat( 128 );
    auto       b = Flat( 128 );
    for ( int y = 0; y < kH; ++y )
        for ( int x = 1; x < kW; ++x )
            SetPixel( b, x, y, 136, 136, 136 );

    const auto d = Diff( a, b );

    const double expectedGrad = 0.5 * ( 8.0 / ( kW - 1 ) + 0.0 );
    EXPECT_NEAR( d.MeanGradLuma, expectedGrad, 1e-9 );

    // And the same claim from the other side: the field's own size is known too, so the coherence is a
    // number rather than an ordering. (kW - 1) columns carry 8 and one carries 0.
    EXPECT_NEAR( d.MeanAbsLuma, 8.0 * ( kW - 1 ) / kW, 1e-9 );
    EXPECT_NEAR( d.Coherence, d.MeanAbsLuma / expectedGrad, 1e-9 );
}
