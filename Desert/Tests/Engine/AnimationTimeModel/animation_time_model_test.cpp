// THE TIME MODEL, AND THE THREE PROPERTIES FLOAT SECONDS COULD NOT GIVE.
//
// This file links ONE engine source (`Animation/TimeModel.cpp`) and needs no rig, no clip, no GPU — the
// same split `Solvers/TwoBoneIK` has, for the same reason: a decision about a data format has to be
// checkable without the data.
//
// What is asserted, in the order the file comment argues it:
//
//   1. THE TICK RATE DIVIDES EVERY DISPLAY RATE WE SHIP. 24000 is not a nice round number, it is the one
//      that makes every conversion below an integer multiply. If a future rate does not divide it, this
//      suite says so BEFORE a clip is authored at that rate.
//   2. EQUALITY IS DECIDABLE. Two times are the same tick or they are not; `0.1 + 0.2 != 0.3` has no
//      analogue here, and the test states the float version it replaces so the difference is visible.
//   3. THE LOOP WRAP AND THE CLOCK DO NOT DRIFT. Measured over an hour of simulated playback against the
//      float accumulator the engine used to carry.

#include <Engine/Animation/TimeModel.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <limits>

using Desert::Animation::AdvanceFrameTime;
using Desert::Animation::ConvertTick;
using Desert::Animation::DEFAULT_DISPLAY_RATE;
using Desert::Animation::DisplayFrameIndex;
using Desert::Animation::FrameNumber;
using Desert::Animation::FrameRate;
using Desert::Animation::FrameTime;
using Desert::Animation::FrameTimeToSeconds;
using Desert::Animation::NearestTick;
using Desert::Animation::PROJECT_TICK_RATE;
using Desert::Animation::SecondsToFrameTime;
using Desert::Animation::SnapToDisplayRate;
using Desert::Animation::WrapFrameTime;

// ---------------------------------------------------------------- 1. the rate that was chosen

TEST( AnimationTimeModel, TheProjectTickRateDividesEveryDisplayRateThisEngineCanShip )
{
    // The reason 24000 and not 1000, 10000 or "whatever float gives". Each of these is a rate a clip can
    // arrive at from an exporter or be authored at in the Sequencer.
    for ( const int32_t rate : { 1, 2, 4, 5, 8, 10, 12, 15, 20, 24, 25, 30, 40, 48, 50, 60, 75, 80, 100, 120,
                                 125, 150, 200, 240, 250, 300, 375, 400, 480, 500, 600, 750, 800, 1000, 1200,
                                 1500, 2000, 2400, 3000, 4000, 4800, 6000, 8000, 12000, 24000 } )
    {
        EXPECT_EQ( PROJECT_TICK_RATE.Numerator % rate, 0 )
             << rate << " fps does not divide the project tick rate, so a key authored on that grid cannot "
                        "be stored exactly and every conversion through it would round.";
    }

    // assimp's two common synthetic rates: FBX ticks and glTF milliseconds.
    EXPECT_EQ( PROJECT_TICK_RATE.Numerator % 30, 0 );
    EXPECT_EQ( PROJECT_TICK_RATE.Numerator % 1000, 0 );

    EXPECT_TRUE( PROJECT_TICK_RATE.IsValid() );
    EXPECT_TRUE( DEFAULT_DISPLAY_RATE.IsValid() );
}

TEST( AnimationTimeModel, ARateIsComparedBYVALUEAndNotBySpelling )
{
    // 30/1 and 60/2 are the same grid. A caller asking "is this clip already at the project rate" means
    // the rate, and a field-by-field comparison would resample a clip that needs nothing done to it.
    EXPECT_EQ( ( FrameRate{ 30, 1 } ), ( FrameRate{ 60, 2 } ) );
    EXPECT_EQ( ( FrameRate{ 24000, 1 } ), ( FrameRate{ 48000, 2 } ) );
    EXPECT_NE( ( FrameRate{ 30, 1 } ), ( FrameRate{ 30000, 1001 } ) );

    // A zero or negative rate is not a slow clock, it is a missing one.
    EXPECT_FALSE( ( FrameRate{ 0, 1 } ).IsValid() );
    EXPECT_FALSE( ( FrameRate{ 30, 0 } ).IsValid() );
    EXPECT_FALSE( ( FrameRate{ -30, 1 } ).IsValid() );
}

// ---------------------------------------------------------------- 2. what the importer boundary does

TEST( AnimationTimeModel, EveryCommonExporterRateConvertsIntoProjectTicksWithNoRounding )
{
    // THE ANSWER TO "WHAT HAPPENED TO PRECISION AT THE ASSIMP BOUNDARY". assimp hands over key times
    // already counted in the exporter's ticks; the old importer narrowed each one to a float. Here the
    // same tick becomes project ticks by an integer multiply, and the result SAYS whether it was exact.
    struct Case
    {
        FrameRate Source;
        int32_t   Tick;
        int32_t   Expected;
    };

    const Case cases[] = {
        { FrameRate{ 30, 1 }, 0, 0 },          { FrameRate{ 30, 1 }, 1, 800 },
        { FrameRate{ 30, 1 }, 1800, 1440000 }, { FrameRate{ 24, 1 }, 1, 1000 },
        { FrameRate{ 25, 1 }, 1, 960 },        { FrameRate{ 60, 1 }, 1, 400 },
        { FrameRate{ 1000, 1 }, 1, 24 },       { FrameRate{ 1000, 1 }, 16, 384 },
        { FrameRate{ 48, 1 }, 7, 3500 },       { FrameRate{ 120, 1 }, 3, 600 },
    };

    for ( const Case& c : cases )
    {
        const auto converted = ConvertTick( FrameNumber{ c.Tick }, c.Source, PROJECT_TICK_RATE );
        EXPECT_TRUE( converted.Exact ) << c.Source.Numerator << " fps tick " << c.Tick << " needed rounding";
        EXPECT_EQ( converted.Ticks.Value, c.Expected );
        EXPECT_EQ( converted.RoundedAwayMicro, 0 );
    }
}

TEST( AnimationTimeModel, ARateThatDoesNotDivideSaysSoRatherThanRoundingQuietly )
{
    // NTSC. 1001 ticks at 30000/1001 is exactly one second, but ONE tick is not a whole project tick, and
    // that is a fact a caller has to be able to report rather than discover in a frame.
    const auto ntsc = ConvertTick( FrameNumber{ 1 }, FrameRate{ 30000, 1001 }, PROJECT_TICK_RATE );
    EXPECT_FALSE( ntsc.Exact );
    EXPECT_EQ( ntsc.Ticks.Value, 801 ); // 800.8 rounded
    EXPECT_NE( ntsc.RoundedAwayMicro, 0 );

    // A whole 1001 SECONDS of NTSC is exact, which is the other half of the same statement — and the
    // number is a reminder of what the rate means: 30000 NTSC ticks take 1001 seconds, not one.
    const auto exactSpan = ConvertTick( FrameNumber{ 30000 }, FrameRate{ 30000, 1001 }, PROJECT_TICK_RATE );
    EXPECT_TRUE( exactSpan.Exact );
    EXPECT_EQ( exactSpan.Ticks.Value, 1001 * 24000 );

    // An invalid rate reports NOT EXACT and changes nothing — never a converted-looking zero.
    const auto broken = ConvertTick( FrameNumber{ 42 }, FrameRate{ 0, 1 }, PROJECT_TICK_RATE );
    EXPECT_FALSE( broken.Exact );
    EXPECT_EQ( broken.Ticks.Value, 42 );
}

// ---------------------------------------------------------------- 3. equality, which float could not give

TEST( AnimationTimeModel, TwoTimesAreTheSameTickOrTheyAreNot )
{
    // THE FLOAT THIS REPLACES, stated with a case that actually fails in FLOAT rather than the textbook
    // `0.1 + 0.2` one — which is a DOUBLE fact and holds exactly in float32, as this line records. The
    // engine's own fixed step is the honest example: sixty sixtieths of a second are not one second.
    EXPECT_EQ( 0.1F + 0.2F, 0.3F );
    float sixtieths = 0.0F;
    for ( int i = 0; i < 60; ++i )
    {
        sixtieths += 1.0F / 60.0F;
    }
    EXPECT_NE( sixtieths, 1.0F );

    // Three tenths of a second, reached two ways on the tick grid. 24000 ticks/s makes a tenth 2400.
    const FrameTime viaSum   = FrameTime{ FrameNumber{ 2400 + 4800 }, 0.0F };
    const FrameTime viaWhole = SecondsToFrameTime( 0.3, PROJECT_TICK_RATE );
    EXPECT_EQ( viaSum.Frame, viaWhole.Frame );

    // THE ROUND TRIP RECOVERS THE TICK — through `NearestTick`, and the distinction is the point rather
    // than a weakening. A tick that has been through seconds comes back as a POSITION 1e-13 of a tick
    // away from where it started, and 798.9999999999999 floors to 798. "Which tick am I in" and "which
    // tick do I mean" are two questions, and only one of them is a floor.
    for ( const int32_t tick : { 0, 1, 799, 800, 2400, 24000, 1440000 } )
    {
        const double    seconds = FrameTimeToSeconds( FrameTime{ FrameNumber{ tick }, 0.0F }, PROJECT_TICK_RATE );
        const FrameTime back    = SecondsToFrameTime( seconds, PROJECT_TICK_RATE );
        EXPECT_EQ( NearestTick( back ).Value, tick ) << "tick " << tick << " did not survive a round trip";
        EXPECT_NEAR( back.AsTicks(), static_cast<double>( tick ), 1.0e-6 );
    }
}

// ---------------------------------------------------------------- 4. the clock, and the drift it removed

TEST( AnimationTimeModel, AnHourOfFixedStepPlaybackDoesNotDrift )
{
    // The engine's fixed gameplay step, which `--play` also uses. 216 000 of them is an hour.
    constexpr double STEP     = 1.0 / 60.0;
    constexpr int    STEPS    = 216000;
    constexpr double EXPECTED = STEP * STEPS;

    FrameTime clock;
    for ( int i = 0; i < STEPS; ++i )
    {
        clock = AdvanceFrameTime( clock, STEP, PROJECT_TICK_RATE );
    }
    const double integerClock = FrameTimeToSeconds( clock, PROJECT_TICK_RATE );

    // THE FLOAT ACCUMULATOR THIS REPLACES, written out here so the comparison is measured and not
    // asserted: `Animator::UpdatePlayback` did `playback.Time += deltaTime * tps` on a float.
    float floatClock = 0.0F;
    for ( int i = 0; i < STEPS; ++i )
    {
        floatClock += static_cast<float>( STEP );
    }

    const double integerError = std::fabs( integerClock - EXPECTED );
    const double floatError   = std::fabs( static_cast<double>( floatClock ) - EXPECTED );

    // One tick is 1/24000 s. The integer clock's error is bounded by the sub-tick it carries; the float
    // one's grows with the value already in it.
    EXPECT_LT( integerError, 1.0 / 24000.0 )
         << "integer clock drifted " << integerError << " s over an hour";
    EXPECT_GT( floatError, integerError * 100.0 )
         << "the float accumulator drifted " << floatError << " s and the integer clock " << integerError
         << " s — if these are close, this rig no longer measures the thing the change was made for";
}

TEST( AnimationTimeModel, TheLoopWrapIsExactAndNeverLeavesTheClip )
{
    const FrameNumber duration{ 48000 }; // two seconds at the project rate

    // Exactly on the end wraps to exactly the start — the case `fmod` on a float could not promise.
    EXPECT_EQ( WrapFrameTime( FrameTime{ duration, 0.0F }, duration ).Frame.Value, 0 );
    EXPECT_EQ( WrapFrameTime( FrameTime{ FrameNumber{ 48001 }, 0.0F }, duration ).Frame.Value, 1 );

    // A thousand laps, and the phase is the tick it started at rather than the tick plus a residue.
    FrameTime clock{ FrameNumber{ 1234 }, 0.25F };
    for ( int lap = 0; lap < 1000; ++lap )
    {
        clock = AdvanceFrameTime( clock, 2.0, PROJECT_TICK_RATE );
        clock = WrapFrameTime( clock, duration );
    }
    EXPECT_EQ( clock.Frame.Value, 1234 );
    EXPECT_FLOAT_EQ( clock.Subframe, 0.25F );

    // Backwards is defined: scrubbing to -1 is the last tick, not a position before the first.
    EXPECT_EQ( WrapFrameTime( FrameTime{ FrameNumber{ -1 }, 0.0F }, duration ).Frame.Value, 47999 );

    // A clip of no length has one position and it is the start; returning the input would hand back a
    // tick inside a clip with no inside.
    EXPECT_EQ( WrapFrameTime( FrameTime{ FrameNumber{ 77 }, 0.5F }, FrameNumber{ 0 } ).Frame.Value, 0 );
}

// ---------------------------------------------------------------- 5. the display rate, which is separate

TEST( AnimationTimeModel, TheDisplayRateIsASecondGridAndSnappingLandsOnIt )
{
    // 30 fps on a 24000 tick grid is one display frame every 800 ticks.
    EXPECT_EQ( SnapToDisplayRate( FrameTime{ FrameNumber{ 0 }, 0.0F }, PROJECT_TICK_RATE, DEFAULT_DISPLAY_RATE )
                    .Value,
               0 );
    EXPECT_EQ( SnapToDisplayRate( FrameTime{ FrameNumber{ 399 }, 0.0F }, PROJECT_TICK_RATE,
                                  DEFAULT_DISPLAY_RATE )
                    .Value,
               0 );
    EXPECT_EQ( SnapToDisplayRate( FrameTime{ FrameNumber{ 401 }, 0.0F }, PROJECT_TICK_RATE,
                                  DEFAULT_DISPLAY_RATE )
                    .Value,
               800 );
    EXPECT_EQ( SnapToDisplayRate( FrameTime{ FrameNumber{ 2399 }, 0.9F }, PROJECT_TICK_RATE,
                                  DEFAULT_DISPLAY_RATE )
                    .Value,
               2400 );

    // A 24 fps clip snaps to a DIFFERENT grid on the same storage, which is the whole reason the two
    // numbers are not one number.
    EXPECT_EQ(
         SnapToDisplayRate( FrameTime{ FrameNumber{ 900 }, 0.0F }, PROJECT_TICK_RATE, FrameRate{ 24, 1 } ).Value,
         1000 );

    // Same grid: nothing moves, and no rounding is invented.
    EXPECT_EQ( SnapToDisplayRate( FrameTime{ FrameNumber{ 12345 }, 0.5F }, PROJECT_TICK_RATE,
                                  PROJECT_TICK_RATE )
                    .Value,
               12345 );

    // The ruler's frame numbers.
    EXPECT_EQ( DisplayFrameIndex( FrameNumber{ 0 }, PROJECT_TICK_RATE, DEFAULT_DISPLAY_RATE ), 0 );
    EXPECT_EQ( DisplayFrameIndex( FrameNumber{ 799 }, PROJECT_TICK_RATE, DEFAULT_DISPLAY_RATE ), 0 );
    EXPECT_EQ( DisplayFrameIndex( FrameNumber{ 800 }, PROJECT_TICK_RATE, DEFAULT_DISPLAY_RATE ), 1 );
    EXPECT_EQ( DisplayFrameIndex( FrameNumber{ 24000 }, PROJECT_TICK_RATE, DEFAULT_DISPLAY_RATE ), 30 );
}

TEST( AnimationTimeModel, NothingHereProducesANonFiniteOrRunawayTick )
{
    // A corrupt file is the caller this has to survive: a NaN duration, an infinite key time, a rate of
    // zero. None of them may become a tick that indexes something.
    const double nan      = std::nan( "" );
    const double infinity = std::numeric_limits<double>::infinity();
    EXPECT_EQ( SecondsToFrameTime( nan, PROJECT_TICK_RATE ).Frame.Value, 0 );
    EXPECT_EQ( SecondsToFrameTime( infinity, PROJECT_TICK_RATE ).Frame.Value, 2147483647 );
    EXPECT_EQ( SecondsToFrameTime( -infinity, PROJECT_TICK_RATE ).Frame.Value, -2147483648 );
    EXPECT_FLOAT_EQ( SecondsToFrameTime( infinity, PROJECT_TICK_RATE ).Subframe, 0.0F );
    EXPECT_EQ( SecondsToFrameTime( 1.0, FrameRate{ 0, 0 } ).Frame.Value, 0 );

    FrameTime clock{ FrameNumber{ 5 }, 0.0F };
    EXPECT_EQ( AdvanceFrameTime( clock, nan, PROJECT_TICK_RATE ).Frame.Value, 5 );
    EXPECT_EQ( AdvanceFrameTime( clock, 1.0, FrameRate{ 0, 1 } ).Frame.Value, 5 );
    EXPECT_EQ( FrameTimeToSeconds( clock, FrameRate{ 0, 1 } ), 0.0 );
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
