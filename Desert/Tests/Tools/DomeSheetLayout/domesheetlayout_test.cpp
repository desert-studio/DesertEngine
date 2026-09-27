// The relation DomeSheet exists to keep: the LABEL a reader sees burnt into a tile and the `--look`
// VECTOR the editor was pointed along to produce it are two views of one number.
//
// Testing either side alone proves nothing, which is this project's most repeated defect shape: a
// silhouette threshold that was correct and a profile that was correct disagreed; a fade end that was
// correct and a view distance that was correct disagreed; a shadow map and a cone march that were each
// right disagreed. So the assertions below are all of the form "these two must agree", and the one that
// matters most is the round trip — label -> angles -> vector -> the same vector the plan handed the
// capture script.

#include <gtest/gtest.h>

#include "../../../../Tools/DomeSheet/Source/DomeSheetLayout.hpp"

#include <cmath>
#include <set>

using namespace Desert::DomeSheet;

namespace
{
    constexpr float kTol = 1e-5f;
}

// ---- the relation ------------------------------------------------------------------------------------

TEST( DomeSheet, TheLabelAndTheLookVectorAreTheSameNumber )
{
    // Every tile of a full plan: read the label back, rebuild the vector from what it says, and require
    // it to be the vector the capture was actually given. A script that formatted the label separately
    // from the vector would pass every test of either and fail this one.
    const std::vector<DomeSample> plan = MakeDomePlan( { 5, 25, 45, 65, 85 }, 8 );
    ASSERT_EQ( plan.size(), 40u );

    for ( const DomeSample& sample : plan )
    {
        int azimuth   = -1;
        int elevation = -1;
        ASSERT_TRUE( ParseDomeLabel( DomeLabel( sample ), azimuth, elevation ) )
             << "label '" << DomeLabel( sample ) << "' does not read back";

        EXPECT_EQ( azimuth, sample.AzimuthDegrees );
        EXPECT_EQ( elevation, sample.ElevationDegrees );

        const DomeSample rebuilt = MakeDomeSample( azimuth, elevation );
        EXPECT_FLOAT_EQ( rebuilt.LookX, sample.LookX );
        EXPECT_FLOAT_EQ( rebuilt.LookY, sample.LookY );
        EXPECT_FLOAT_EQ( rebuilt.LookZ, sample.LookZ );
    }
}

TEST( DomeSheet, TheFileStemAndTheLabelNameTheSameTile )
{
    // The second half of the same relation, and the half a rename breaks. The stem is what appears on
    // disk and in a commit; the label is what appears in the picture. They must not be able to differ.
    for ( const DomeSample& sample : MakeDomePlan( { 5, 45, 85 }, 8 ) )
    {
        char expected[32];
        std::snprintf( expected, sizeof( expected ), "az%03d_el%02d", sample.AzimuthDegrees,
                       sample.ElevationDegrees );
        EXPECT_EQ( DomeStem( sample ), std::string( expected ) );

        int azimuth   = -1;
        int elevation = -1;
        ASSERT_TRUE( ParseDomeLabel( DomeLabel( sample ), azimuth, elevation ) );
        char fromLabel[32];
        std::snprintf( fromLabel, sizeof( fromLabel ), "az%03d_el%02d", azimuth, elevation );
        EXPECT_EQ( DomeStem( sample ), std::string( fromLabel ) );
    }
}

TEST( DomeSheet, ALabelThisFileDidNotWriteIsRejected )
{
    int azimuth   = 0;
    int elevation = 0;
    EXPECT_FALSE( ParseDomeLabel( "", azimuth, elevation ) );
    EXPECT_FALSE( ParseDomeLabel( "AZ 045", azimuth, elevation ) );
    EXPECT_FALSE( ParseDomeLabel( "elevation 45", azimuth, elevation ) );
    // A prefix that parses must not be accepted as the whole label — the failure mode the editor's own
    // command line was rewritten to remove.
    EXPECT_FALSE( ParseDomeLabel( "AZ 045  EL 25 and then some", azimuth, elevation ) );
}

// ---- the dome ----------------------------------------------------------------------------------------

TEST( DomeSheet, AzimuthZeroIsMinusZAndAzimuth180IsPlusZ )
{
    // The convention that makes the old six-point protocol a SUBSET of this dome rather than a different
    // set of rays. `--look 0,0.9,-1` is 42 degrees away from the sun; `0,0.9,1` is 42 degrees into it.
    const DomeSample away = MakeDomeSample( 0, 42 );
    EXPECT_NEAR( away.LookX, 0.0f, kTol );
    EXPECT_GT( away.LookY, 0.0f );
    EXPECT_LT( away.LookZ, 0.0f );

    const DomeSample sun = MakeDomeSample( 180, 42 );
    EXPECT_NEAR( sun.LookX, 0.0f, kTol );
    EXPECT_GT( sun.LookY, 0.0f );
    EXPECT_GT( sun.LookZ, 0.0f );

    // And the ray is the SAME ray the protocol shot, not merely one at the same elevation: `--look` is
    // not normalized, so the check is on the direction. 0,0.9,-1 normalizes to elevation atan(0.9).
    const float protocolElevation = std::atan( 0.9f / 1.0f ) * 180.0f / 3.14159265358979323846f;
    EXPECT_NEAR( protocolElevation, 41.987f, 0.01f );
    const DomeSample matching = MakeDomeSample( 0, 42 );
    // Direction agreement to within the half-degree the integer label rounds to.
    const float dot = matching.LookY * 0.9f / std::sqrt( 1.81f ) + matching.LookZ * -1.0f / std::sqrt( 1.81f );
    EXPECT_GT( dot, std::cos( 0.5f * 3.14159265358979323846f / 180.0f ) );
}

TEST( DomeSheet, EveryLookVectorIsAUnitDirectionAtItsOwnElevation )
{
    for ( const DomeSample& sample : MakeDomePlan( { 0, 5, 25, 45, 65, 85, 90 }, 12 ) )
    {
        const float length =
             std::sqrt( sample.LookX * sample.LookX + sample.LookY * sample.LookY + sample.LookZ * sample.LookZ );
        EXPECT_NEAR( length, 1.0f, 1e-5f );

        // The elevation the vector actually has is the elevation the label claims. This is the bound
        // that catches a sine/cosine swap, which a spot check at 45 degrees never would.
        const float elevation = std::asin( sample.LookY ) * 180.0f / 3.14159265358979323846f;
        EXPECT_NEAR( elevation, static_cast<float>( sample.ElevationDegrees ), 1e-3f );
    }
}

TEST( DomeSheet, ThePlanCoversTheWholeDomeWithoutRepeatingARay )
{
    const std::vector<int>        elevations{ 5, 25, 45, 65, 85 };
    const std::vector<DomeSample> plan = MakeDomePlan( elevations, 8 );

    // Row-major, horizon first: the sheet is read top to bottom and the sky bottom to top, so the first
    // row must be the lowest elevation or every reader has to invert it in their head.
    EXPECT_EQ( plan.front().ElevationDegrees, 5 );
    EXPECT_EQ( plan.back().ElevationDegrees, 85 );
    EXPECT_EQ( plan.front().AzimuthDegrees, 0 );

    std::set<std::pair<int, int>> seen;
    for ( const DomeSample& sample : plan )
    {
        EXPECT_TRUE( seen.insert( { sample.AzimuthDegrees, sample.ElevationDegrees } ).second )
             << "duplicate ray at " << DomeLabel( sample );
        EXPECT_GE( sample.AzimuthDegrees, 0 );
        EXPECT_LT( sample.AzimuthDegrees, 360 ); // 360 would be a second copy of column 0
    }
    EXPECT_EQ( seen.size(), plan.size() );
}

TEST( DomeSheet, TheAzimuthStepDividesTheCircleAndTheFirstColumnIsAlwaysZero )
{
    for ( const int columns : { 4, 6, 7, 8, 12 } )
    {
        const std::vector<DomeSample> row = MakeDomePlan( { 30 }, columns );
        ASSERT_EQ( static_cast<int>( row.size() ), columns );
        EXPECT_EQ( row.front().AzimuthDegrees, 0 );
        // The last column must be strictly inside the circle: a sweep whose last tile is 360 shoots
        // column 0 twice and leaves a wedge of sky unshot, which is exactly the kind of hole this
        // instrument exists to close.
        EXPECT_LT( row.back().AzimuthDegrees, 360 );
        EXPECT_GE( row.back().AzimuthDegrees, 360 - ( 360 / columns ) - 1 );
    }
}

TEST( DomeSheet, AnEmptyOrDegeneratePlanIsEmptyRatherThanWrong )
{
    EXPECT_TRUE( MakeDomePlan( { 5, 45 }, 0 ).empty() );
    EXPECT_TRUE( MakeDomePlan( {}, 8 ).empty() );
}

// ---- the sheet ---------------------------------------------------------------------------------------

TEST( DomeSheet, EveryCellFitsInsideTheSheetItsGeometryDeclared )
{
    // The pair that must agree: the size the sheet is allocated at and the position the last tile is
    // written to. They were two separate expressions in the first draft of this file, which is how a
    // sheet ends up one gap short and silently clips its bottom row.
    const int           tiles    = 40;
    const SheetGeometry geometry = MakeSheetGeometry( tiles, 8, 320, 191, 6, 30 );

    EXPECT_EQ( geometry.Rows, 5 );
    for ( int i = 0; i < tiles; ++i )
    {
        EXPECT_GE( geometry.CellX( i ), 0 );
        EXPECT_GE( geometry.CellY( i ), geometry.CaptionHeight );
        EXPECT_LE( geometry.CellX( i ) + geometry.CellWidth, geometry.SheetWidth() );
        EXPECT_LE( geometry.CellY( i ) + geometry.CellHeight, geometry.SheetHeight() );
    }
}

TEST( DomeSheet, APartialLastRowStillGetsARow )
{
    const SheetGeometry geometry = MakeSheetGeometry( 33, 8, 100, 100, 4, 0 );
    EXPECT_EQ( geometry.Rows, 5 );
    EXPECT_LE( geometry.CellY( 32 ) + geometry.CellHeight, geometry.SheetHeight() );
}

TEST( DomeSheet, TheBoxFilterConservesTheMeanRatherThanDarkeningIt )
{
    // A truncating box filter loses half a level on every reduction, and this sheet is read beside
    // frames that were not reduced. A constant image must survive exactly.
    Image flat  = MakeImage( 16, 16, 133 );
    Image smallOne = BoxDownscale( flat, 4 );
    ASSERT_EQ( smallOne.Width, 4 );
    ASSERT_EQ( smallOne.Height, 4 );
    for ( int y = 0; y < smallOne.Height; ++y )
        for ( int x = 0; x < smallOne.Width; ++x )
            EXPECT_EQ( smallOne.At( x, y )[0], 133 );

    // And a single bright pixel in a 4x4 block must SURVIVE as a raised mean rather than being point
    // sampled away — the thin-cirrus case the sheet would otherwise report as empty sky.
    Image sparse         = MakeImage( 4, 4, 0 );
    sparse.At( 3, 3 )[0] = 240;
    const Image reduced  = BoxDownscale( sparse, 4 );
    ASSERT_EQ( reduced.Width, 1 );
    EXPECT_EQ( reduced.At( 0, 0 )[0], 15 ); // 240/16
}

TEST( DomeSheet, ScaleOneIsTheIdentity )
{
    Image image         = MakeImage( 3, 2, 7 );
    image.At( 1, 1 )[2] = 200;
    const Image same    = BoxDownscale( image, 1 );
    EXPECT_EQ( same.Width, 3 );
    EXPECT_EQ( same.Height, 2 );
    EXPECT_EQ( same.At( 1, 1 )[2], 200 );
}

TEST( DomeSheet, TheLabelIsBurntInAndDoesNotLeaveTheCell )
{
    // The stamp writes white glyphs over a darkened bar. Two things are asserted: something was written
    // (a label that silently draws nothing is the failure this sheet cannot survive), and nothing was
    // written outside the bar.
    Image             cell  = MakeImage( 200, 100, 128 );
    const std::string label = DomeLabel( MakeDomeSample( 135, 25 ) );
    ASSERT_EQ( label, "AZ 135  EL 25" );

    StampLabel( cell, label, 0, 0, cell.Width, 2 );

    const int barHigh = kGlyphHeight * 2 + 2 * ( 2 * 2 );
    int       white   = 0;
    for ( int y = 0; y < barHigh; ++y )
        for ( int x = 0; x < cell.Width; ++x )
            if ( cell.At( x, y )[0] == 255 )
                ++white;
    EXPECT_GT( white, 100 ) << "the label drew nothing";

    for ( int y = barHigh; y < cell.Height; ++y )
        for ( int x = 0; x < cell.Width; ++x )
            EXPECT_EQ( cell.At( x, y )[0], 128 ) << "the label escaped its bar at " << x << "," << y;
}

TEST( DomeSheet, TheLabelFitsTheTileItIsBurntInto )
{
    // The relation between the text and the cell: at the shipped 1/4 reduction of a 1280-wide capture a
    // tile is 320 px, and the label must fit inside it or the sheet crops the elevation off the end and
    // the reader is left with half a coordinate.
    const int tileWidth = 1280 / 4;
    EXPECT_LE( TextWidth( DomeLabel( MakeDomeSample( 315, 85 ) ), 2 ) + 8, tileWidth );
}

TEST( DomeSheet, EveryCharacterALabelCanProduceHasAGlyph )
{
    // Not "the table is 65 long" but "nothing a label contains falls through to '?'", which is the
    // relation that survives someone changing the label format.
    for ( const DomeSample& sample : MakeDomePlan( { 0, 5, 85, 90 }, 8 ) )
    {
        for ( const char character : DomeLabel( sample ) )
        {
            if ( character == '?' )
                continue;
            EXPECT_NE( GlyphFor( character ), GlyphFor( '?' ) )
                 << "no glyph for '" << character << "' in " << DomeLabel( sample );
        }
    }
}

TEST( DomeSheet, BlitClipsInsteadOfWritingOutsideTheSheet )
{
    Image sheet = MakeImage( 10, 10, 0 );
    Image tile  = MakeImage( 6, 6, 200 );
    Blit( sheet, tile, 7, 7 );
    EXPECT_EQ( sheet.At( 9, 9 )[0], 200 );
    EXPECT_EQ( sheet.At( 6, 6 )[0], 0 );
    EXPECT_EQ( sheet.Pixels.size(), 10u * 10u * 3u );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
