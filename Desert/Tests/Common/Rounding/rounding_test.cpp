// WHAT `static_cast<int>( x + 0.5 )` GETS WRONG, PINNED AS A NUMBER.
//
// The engine wrote that form fourteen times. It is not rounding: it adds a half and then truncates TOWARD
// ZERO, so every negative value comes back one too large. Ten of the fourteen sites clamped to a
// non-negative range first and were therefore right; the four that did not looked exactly the same.
//
// These tests are the gate for the replacement. THEY GO RED ON THE OLD FORM: substitute
// `static_cast<long long>( value + 0.5 )` into RoundToNearest and NegativeValuesRoundAwayFromZero fails
// on -2.5, -1.5, -0.6 and -2.7; substitute it into QuantiseUnitToByte and nothing fails, which is the
// measurement that says the clamped sites really were safe and the unclamped ones really were not.

#include <gtest/gtest.h>

#include <Common/Core/Math/Rounding.hpp>

#include <cmath>
#include <cstdint>
#include <limits>

using Common::Math::QuantiseUnitToByte;
using Common::Math::RoundToNearest;

TEST( Rounding, NegativeValuesRoundAwayFromZero )
{
    // The left column is what `(long long)( x + 0.5 )` answers and the right is the nearest integer.
    // -0.6 is the one that matters most: it is the smallest magnitude where the two disagree, so a value
    // that merely drifts slightly below zero starts answering 0 instead of -1.
    EXPECT_EQ( RoundToNearest( -0.6 ), -1 ); // the old form: 0
    EXPECT_EQ( RoundToNearest( -1.5 ), -2 ); // the old form: -1
    EXPECT_EQ( RoundToNearest( -2.5 ), -3 ); // the old form: -2
    EXPECT_EQ( RoundToNearest( -2.7 ), -3 ); // the old form: -2
    EXPECT_EQ( RoundToNearest( -0.4 ), 0 );
}

TEST( Rounding, PositiveValuesAgreeWithTheFormItReplaces )
{
    // Half the point of the change is that it costs NOTHING where the old form was right, so the ten
    // clamped sites are provably unmoved.
    for ( int hundredths = 0; hundredths <= 1000; ++hundredths )
    {
        const double value = hundredths / 100.0;
        // NOLINTNEXTLINE(bugprone-incorrect-roundings) — the old form is the SUBJECT of this assertion.
        // The check is right about it everywhere else, which is why the suppression is one line wide and
        // names the check rather than switching it off for the file.
        EXPECT_EQ( RoundToNearest( value ), static_cast<long long>( value + 0.5 ) ) << "at " << value;
    }
}

TEST( Rounding, TheFloatOverloadDoesNotWidenAndNarrow )
{
    EXPECT_EQ( RoundToNearest( -1.5f ), -2 );
    EXPECT_EQ( RoundToNearest( 2.5f ), 3 );
}

// ---------------------------------------------------------------------------------------- unorm ----

TEST( Rounding, TheUnitRangeMapsToTheWholeByteRange )
{
    EXPECT_EQ( QuantiseUnitToByte( 0.0f ), 0 );
    EXPECT_EQ( QuantiseUnitToByte( 1.0f ), 255 );
    EXPECT_EQ( QuantiseUnitToByte( 0.5f ), 128 ); // 127.5 rounds away from zero
}

TEST( Rounding, EveryByteSurvivesTheRoundTrip )
{
    // 255 and not 256 is what makes this hold, and it is the property the cloud volume bakers depend on:
    // a voxel written, read back and written again must not drift a level per generation.
    for ( int byte = 0; byte <= 255; ++byte )
        EXPECT_EQ( QuantiseUnitToByte( static_cast<float>( byte ) / 255.0f ), byte ) << "at " << byte;
}

TEST( Rounding, OutOfRangeAndNaNClampRatherThanWrap )
{
    EXPECT_EQ( QuantiseUnitToByte( -0.0f ), 0 );
    EXPECT_EQ( QuantiseUnitToByte( -5.0f ), 0 );
    EXPECT_EQ( QuantiseUnitToByte( 1.5f ), 255 );
    EXPECT_EQ( QuantiseUnitToByte( std::numeric_limits<float>::quiet_NaN() ), 0 );
    EXPECT_EQ( QuantiseUnitToByte( std::numeric_limits<float>::infinity() ), 255 );
    EXPECT_EQ( QuantiseUnitToByte( -std::numeric_limits<float>::infinity() ), 0 );
}

// THE CLOUD BAKES MUST NOT MOVE, AND THIS IS THE MEASUREMENT RATHER THAN THE HOPE.
//
// Ten call sites swapped `clamp( x ) * 255.0f + 0.5f` for this function, and four of them write voxels
// that the renderer samples — a one-level drift would be a picture change carried in a cache nobody
// re-bakes. Comparing the two expressions over ten million samples of the unit interval is cheaper and
// stronger evidence than a frame: a frame shows the samples the camera happened to hit.
TEST( Rounding, TheQuantiserIsBitForBitTheExpressionItReplaced )
{
    constexpr int kSamples = 10'000'000;
    for ( int i = 0; i <= kSamples; ++i )
    {
        const float unit = static_cast<float>( i ) / static_cast<float>( kSamples );
        // NOLINTNEXTLINE(bugprone-incorrect-roundings) — the replaced expression is the subject here.
        const auto previous = static_cast<std::uint8_t>( unit * 255.0f + 0.5f );
        ASSERT_EQ( QuantiseUnitToByte( unit ), previous ) << "at " << unit;
    }
}

TEST( Rounding, TheQuantiserIsMonotonic )
{
    // A bound rather than a table: it catches an off-by-one in the scale, an inverted clamp and a wrap,
    // none of which a handful of spot values will.
    int previous = -1;
    for ( int step = 0; step <= 10000; ++step )
    {
        const int current = QuantiseUnitToByte( static_cast<float>( step ) / 10000.0f );
        EXPECT_GE( current, previous ) << "at step " << step;
        EXPECT_LE( current, 255 );
        previous = current;
    }
    EXPECT_EQ( previous, 255 );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
