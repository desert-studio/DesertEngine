// THE CURVE, AND THE FOUR THINGS A LINEAR-ONLY CHANNEL COULD NOT DO.
//
// This suite links two engine sources — `KeyInterpolation.cpp` and the tick grid it measures spans on —
// and needs no clip, no rig and no GPU. Same split as `Solvers/TwoBoneIK` and `TimeModel`, for the same
// reason: a decision about how a curve is shaped has to be checkable without the data it shapes.
//
// What is pinned, in the order the header argues it:
//
//   1. THE SEGMENT IS SHAPED BY ITS LATER KEY, and the three modes do three different things.
//   2. AUTO-TANGENTS ARE NOT "COMPUTE A SLOPE". Flat endpoints, flat extrema, and the monotone clamp —
//      each asserted by the overshoot it prevents, not by the number it produces.
//   3. INSERTING A KEY DOES NOT MOVE THE POSE (report 05 §936).
//   4. THE TANGENT UNIT IS VALUE-PER-SECOND, which is a claim with consequences: the same curve on a
//      different tick rate is the same curve.

#include <Engine/Animation/KeyInterpolation.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

using Desert::Animation::AutoSetTangents;
using Desert::Animation::EvaluateSegment;
using Desert::Animation::FrameNumber;
using Desert::Animation::FrameRate;
using Desert::Animation::KeyInterp;
using Desert::Animation::PROJECT_TICK_RATE;
using Desert::Animation::ScalarKey;
using Desert::Animation::SlopeAt;
using Desert::Animation::TangentMode;
using Desert::Animation::ToString;

namespace
{
    ScalarKey Key( int32_t tick, float value, KeyInterp interp = KeyInterp::Cubic,
                   TangentMode mode = TangentMode::Auto )
    {
        ScalarKey key;
        key.Tick   = FrameNumber{ tick };
        key.Value  = value;
        key.Interp = interp;
        key.Mode   = mode;
        return key;
    }

    /// Sample the whole channel by walking its segments — what a player does, written here so the suite
    /// tests the same path playback takes rather than a private one.
    float SampleAt( const std::vector<ScalarKey>& keys, double tick, FrameRate rate )
    {
        if ( keys.empty() )
        {
            return 0.0F;
        }
        if ( tick <= keys.front().Tick.Value )
        {
            return keys.front().Value;
        }
        if ( tick >= keys.back().Tick.Value )
        {
            return keys.back().Value;
        }

        for ( std::size_t i = 1; i < keys.size(); ++i )
        {
            if ( tick > keys[i].Tick.Value )
            {
                continue;
            }
            const double span = static_cast<double>( keys[i].Tick.Value - keys[i - 1].Tick.Value );
            const auto   t    = static_cast<float>( ( tick - keys[i - 1].Tick.Value ) / span );
            const double seconds =
                 span * static_cast<double>( rate.Denominator ) / static_cast<double>( rate.Numerator );
            return EvaluateSegment( keys[i - 1].Value, keys[i - 1].LeaveTangent, keys[i].Value,
                                    keys[i].ArriveTangent, keys[i].Interp, seconds, t );
        }
        return keys.back().Value;
    }

    /// The largest value the channel reaches over its whole range, sampled densely. The overshoot tests
    /// are written against this rather than against a tangent number: an animator reports "it goes past
    /// my key", not "the tangent is 4.2".
    float PeakOf( const std::vector<ScalarKey>& keys, FrameRate rate )
    {
        float peak = keys.front().Value;
        for ( int step = 0; step <= 2000; ++step )
        {
            const double tick = keys.front().Tick.Value +
                                ( keys.back().Tick.Value - keys.front().Tick.Value ) * ( step / 2000.0 );
            peak = std::max( peak, SampleAt( keys, tick, rate ) );
        }
        return peak;
    }
} // namespace

// ---------------------------------------------------------------- 1. the three modes

TEST( KeyInterpolation, ConstantHoldsTheWholeSegmentAndStepsAtTheLaterKey )
{
    // A hold that steps half way to the next key is a rounding, not a hold — and holding a pose is the
    // thing a linear-only channel could not express without faking it with extra keys.
    EXPECT_FLOAT_EQ( EvaluateSegment( 10.0F, 0.0F, 20.0F, 0.0F, KeyInterp::Constant, 1.0, 0.0F ), 10.0F );
    EXPECT_FLOAT_EQ( EvaluateSegment( 10.0F, 0.0F, 20.0F, 0.0F, KeyInterp::Constant, 1.0, 0.49F ), 10.0F );
    EXPECT_FLOAT_EQ( EvaluateSegment( 10.0F, 0.0F, 20.0F, 0.0F, KeyInterp::Constant, 1.0, 0.99F ), 10.0F );
}

TEST( KeyInterpolation, LinearIsTheStraightLineThisEngineUsedToHaveEverywhere )
{
    EXPECT_FLOAT_EQ( EvaluateSegment( 0.0F, 0.0F, 10.0F, 0.0F, KeyInterp::Linear, 1.0, 0.0F ), 0.0F );
    EXPECT_FLOAT_EQ( EvaluateSegment( 0.0F, 0.0F, 10.0F, 0.0F, KeyInterp::Linear, 1.0, 0.5F ), 5.0F );
    EXPECT_FLOAT_EQ( EvaluateSegment( 0.0F, 0.0F, 10.0F, 0.0F, KeyInterp::Linear, 1.0, 1.0F ), 10.0F );
}

TEST( KeyInterpolation, CubicPassesThroughBothKeysAndLeavesThemAtTheStatedSlope )
{
    // A curve that does not pass through its own keys is not an interpolation, and the endpoints are the
    // cheapest place for a control-point mistake to hide.
    EXPECT_FLOAT_EQ( EvaluateSegment( 3.0F, 7.0F, 11.0F, -2.0F, KeyInterp::Cubic, 1.0, 0.0F ), 3.0F );
    EXPECT_FLOAT_EQ( EvaluateSegment( 3.0F, 7.0F, 11.0F, -2.0F, KeyInterp::Cubic, 1.0, 1.0F ), 11.0F );

    // The slope at t=0 is the leave tangent, in value units per second. Measured as a difference quotient
    // over a thousandth of the segment, which is what makes it a statement about the UNIT and not about
    // the control point.
    constexpr double SPAN  = 2.0; // seconds
    constexpr float  SLOPE = 6.0F;
    // A difference quotient, over a step small enough that the segment's own curvature does not show up
    // in it: at 1/1000 of this span the second-order term is already 0.14, which is a measurement of the
    // curve rather than of the tangent.
    constexpr double EPS      = 1.0e-5;
    const float      at0      = EvaluateSegment( 0.0F, SLOPE, 100.0F, 0.0F, KeyInterp::Cubic, SPAN, 0.0F );
    const float      atEps    = EvaluateSegment( 0.0F, SLOPE, 100.0F, 0.0F, KeyInterp::Cubic, SPAN,
                                                 static_cast<float>( EPS ) );
    const double     measured = ( atEps - at0 ) / ( EPS * SPAN );
    EXPECT_NEAR( measured, SLOPE, 0.01 ) << "the leave tangent is not value-per-second";
}

TEST( KeyInterpolation, TwoKeysOnOneTickGiveTheLaterValueRatherThanADivisionByZero )
{
    // A span of zero is a state a file can hold. Both the linear and the cubic path must answer the same
    // thing for it, or the two disagree exactly where a format is most likely to be malformed.
    EXPECT_FLOAT_EQ( EvaluateSegment( 1.0F, 5.0F, 9.0F, 5.0F, KeyInterp::Cubic, 0.0, 0.5F ), 9.0F );
    EXPECT_FALSE( std::isnan( EvaluateSegment( 1.0F, 5.0F, 9.0F, 5.0F, KeyInterp::Cubic, 0.0, 0.5F ) ) );
}

// ---------------------------------------------------------------- 2. the auto-tangent rules

TEST( KeyInterpolation, TheFirstAndLastKeyAreFlat )
{
    std::vector<ScalarKey> keys{ Key( 0, 0.0F ), Key( 12000, 10.0F ), Key( 24000, 20.0F ) };
    AutoSetTangents( keys, PROJECT_TICK_RATE );

    EXPECT_FLOAT_EQ( keys.front().LeaveTangent, 0.0F );
    EXPECT_FLOAT_EQ( keys.back().ArriveTangent, 0.0F );
    // ...and the middle key is NOT flat, or this test would pass on a function that zeroes everything.
    EXPECT_GT( keys[1].LeaveTangent, 0.0F );

    // AND THE PROPERTY, not just the number: a first key that left with the adjacent secant would bulge
    // the opening segment above its own two keys. Pinning the consequence is what survives a refactor of
    // how the tangent is computed.
    float peakOfFirstSegment = keys.front().Value;
    for ( int step = 0; step <= 1000; ++step )
    {
        peakOfFirstSegment =
             std::max( peakOfFirstSegment, SampleAt( keys, 12000.0 * ( step / 1000.0 ), PROJECT_TICK_RATE ) );
    }
    EXPECT_LE( peakOfFirstSegment, keys[1].Value + 1.0e-3F )
         << "the opening segment reached " << peakOfFirstSegment << ", above its own later key of "
         << keys[1].Value;
}

TEST( KeyInterpolation, APeakIsFlatSoTheCurveNeverSailsPastTheKeyTheAnimatorAuthored )
{
    // The single most reported curve-editor bug: a key at the top of an arc, and the curve going higher.
    //
    // THE PEAK IS ASYMMETRIC ON PURPOSE, and the first version of this test was not — which is a hole a
    // mutation found rather than a reviewer. On a symmetric peak the two secants are +X and -X, so the
    // average of them is ZERO and a build with the extremum rule DELETED produces the same tangent by
    // arithmetic accident. The test passed against code that did not contain the rule it was named after.
    std::vector<ScalarKey> keys{ Key( 0, 0.0F ), Key( 12000, 100.0F ), Key( 24000, 40.0F ) };
    AutoSetTangents( keys, PROJECT_TICK_RATE );

    EXPECT_FLOAT_EQ( keys[1].ArriveTangent, 0.0F ) << "the extremum is not flat";
    EXPECT_FLOAT_EQ( keys[1].LeaveTangent, 0.0F );
    EXPECT_NEAR( PeakOf( keys, PROJECT_TICK_RATE ), 100.0F, 1.0e-3F )
         << "the curve went above the highest key";
}

TEST( KeyInterpolation, TheMonotoneClampStopsASteepNeighbourFromOvershootingAGentleSegment )
{
    // A long flat approach into a sudden rise. Without the clamp the middle key's tangent is the average
    // of a tiny secant and a huge one, and the gentle segment before it dips BELOW its own two keys.
    std::vector<ScalarKey> keys{ Key( 0, 0.0F ), Key( 12000, 1.0F ), Key( 13000, 100.0F ),
                                 Key( 24000, 101.0F ) };
    AutoSetTangents( keys, PROJECT_TICK_RATE );

    float trough = keys.front().Value;
    for ( int step = 0; step <= 2000; ++step )
    {
        trough = std::min( trough, SampleAt( keys, 12000.0 * ( step / 2000.0 ), PROJECT_TICK_RATE ) );
    }
    EXPECT_GE( trough, -1.0e-3F ) << "the first segment dipped to " << trough
                                  << ", below both of its keys — the clamp is not holding";

    // And the same statement from the other end: nothing anywhere exceeds the highest key.
    EXPECT_NEAR( PeakOf( keys, PROJECT_TICK_RATE ), 101.0F, 1.0e-2F );
}

TEST( KeyInterpolation, AUserTangentSurvivesAnAutoPassAndAnAutoOneDoesNot )
{
    std::vector<ScalarKey> keys{ Key( 0, 0.0F ), Key( 12000, 10.0F, KeyInterp::Cubic, TangentMode::User ),
                                 Key( 24000, 20.0F ) };
    keys[1].ArriveTangent = 42.0F;
    keys[1].LeaveTangent  = -17.0F;

    AutoSetTangents( keys, PROJECT_TICK_RATE );

    // The animator set this one. An auto pass that overwrote it would undo their work on the next
    // unrelated edit ANYWHERE in the channel — which is why the mode is on the key and not on the channel.
    EXPECT_FLOAT_EQ( keys[1].ArriveTangent, 42.0F );
    EXPECT_FLOAT_EQ( keys[1].LeaveTangent, -17.0F );

    keys[1].Mode = TangentMode::Auto;
    AutoSetTangents( keys, PROJECT_TICK_RATE );
    EXPECT_NE( keys[1].ArriveTangent, 42.0F );
}

TEST( KeyInterpolation, TheWholeChannelIsRecomputedBecauseATangentIsNotAPropertyOfItsKey )
{
    std::vector<ScalarKey> keys{ Key( 0, 0.0F ), Key( 12000, 10.0F ), Key( 24000, 20.0F ) };
    AutoSetTangents( keys, PROJECT_TICK_RATE );
    const float before = keys[1].LeaveTangent;

    // Move a NEIGHBOUR. The middle key was not touched, and its tangent must still change.
    keys[2].Value = 200.0F;
    AutoSetTangents( keys, PROJECT_TICK_RATE );
    EXPECT_NE( keys[1].LeaveTangent, before )
         << "a key's tangent did not follow its neighbour — recomputing only the edited key is the version "
            "that looks right until the second edit";
}

// ---------------------------------------------------------------- 3. inserting a key

TEST( KeyInterpolation, AKeySeededFromTheCurrentSlopeDoesNotMoveThePoseWhereTheAnimatorIsLooking )
{
    // Report 05 §936. A key added with ZERO tangents flattens the curve through the point it was added
    // at, which moves every frame around it: the animator asked to record where the curve IS and the tool
    // answered by reshaping it. This is the difference measured, not asserted.
    std::vector<ScalarKey> keys{ Key( 0, 0.0F ), Key( 12000, 100.0F ), Key( 24000, 40.0F ) };
    AutoSetTangents( keys, PROJECT_TICK_RATE );

    const double at       = 6000.0;
    const float  original = SampleAt( keys, at, PROJECT_TICK_RATE );
    const float  slope    = SlopeAt( keys, FrameNumber{ 6000 }, PROJECT_TICK_RATE );
    EXPECT_NE( slope, 0.0F ) << "there is a slope here to seed from";

    const auto sampleWith = [&]( float arrive, float leave )
    {
        std::vector<ScalarKey> edited = keys;
        ScalarKey inserted            = Key( 6000, original, KeyInterp::Cubic, TangentMode::User );
        inserted.ArriveTangent        = arrive;
        inserted.LeaveTangent         = leave;
        edited.insert( edited.begin() + 1, inserted );
        return edited;
    };

    const std::vector<ScalarKey> seeded   = sampleWith( slope, slope );
    const std::vector<ScalarKey> flattened = sampleWith( 0.0F, 0.0F );

    // EXACT where the key went in: the value was read off the curve, so it cannot have moved.
    EXPECT_FLOAT_EQ( SampleAt( seeded, at, PROJECT_TICK_RATE ), original );

    // And either side of it the seeded curve stays close while the flattened one does not. The residual
    // is not zero and this test does not pretend it is: subdividing a cubic EXACTLY would also rewrite
    // the two neighbours' tangents, and an insertion that edits its neighbours is a different feature.
    float worstSeeded = 0.0F;
    float worstFlat   = 0.0F;
    for ( const double probe : { 1500.0, 3000.0, 4500.0, 7500.0, 9000.0, 10500.0 } )
    {
        const float reference = SampleAt( keys, probe, PROJECT_TICK_RATE );
        worstSeeded = std::max( worstSeeded, std::fabs( SampleAt( seeded, probe, PROJECT_TICK_RATE ) - reference ) );
        worstFlat = std::max( worstFlat, std::fabs( SampleAt( flattened, probe, PROJECT_TICK_RATE ) - reference ) );
    }

    EXPECT_LT( worstSeeded, 3.0F ) << "seeding drifted by " << worstSeeded << " over a 100-unit curve";
    EXPECT_GT( worstFlat, worstSeeded * 3.0F )
         << "a zero-tangent insertion drifted " << worstFlat << " and a seeded one " << worstSeeded
         << " — if these are close, this rig no longer measures what seeding is for";
}

TEST( KeyInterpolation, TheSeededSlopeIsTheCURVESSlopeAndIsZeroOutsideTheKeyedRange )
{
    std::vector<ScalarKey> keys{ Key( 0, 0.0F ), Key( 24000, 100.0F ) };

    // On a LINEAR segment the curve's slope IS the secant: 100 units over one second (24000 ticks).
    keys[1].Interp = KeyInterp::Linear;
    EXPECT_NEAR( SlopeAt( keys, FrameNumber{ 12000 }, PROJECT_TICK_RATE ), 100.0F, 1.0e-3F );

    // On a CONSTANT one there is no slope at all — seeding a key inside a deliberate hold with anything
    // else would tilt a segment the animator made flat on purpose.
    keys[1].Interp = KeyInterp::Constant;
    EXPECT_FLOAT_EQ( SlopeAt( keys, FrameNumber{ 12000 }, PROJECT_TICK_RATE ), 0.0F );

    // And on a cubic with flat endpoints the midpoint is the steepest point of the S, which is FASTER
    // than the secant — the number that tells the two rules apart.
    keys[1].Interp = KeyInterp::Cubic;
    AutoSetTangents( keys, PROJECT_TICK_RATE );
    EXPECT_GT( SlopeAt( keys, FrameNumber{ 12000 }, PROJECT_TICK_RATE ), 110.0F );

    // Outside the range the channel is constant, and a constant has no slope to seed from — returning the
    // last segment's slope there would tilt a key appended after the end.
    EXPECT_FLOAT_EQ( SlopeAt( keys, FrameNumber{ 0 }, PROJECT_TICK_RATE ), 0.0F );
    EXPECT_FLOAT_EQ( SlopeAt( keys, FrameNumber{ 24000 }, PROJECT_TICK_RATE ), 0.0F );
    EXPECT_FLOAT_EQ( SlopeAt( keys, FrameNumber{ 30000 }, PROJECT_TICK_RATE ), 0.0F );

    const std::vector<ScalarKey> single{ Key( 0, 5.0F ) };
    EXPECT_FLOAT_EQ( SlopeAt( single, FrameNumber{ 0 }, PROJECT_TICK_RATE ), 0.0F );
}

// ---------------------------------------------------------------- 4. the unit

TEST( KeyInterpolation, TheSAMECurveOnADifferentTickRateIsTheSameCurve )
{
    // THE CLAIM THE UNIT MAKES. Tangents are value-per-second, so a channel authored on the project grid
    // and the identical channel authored on a coarser one produce the same shape — a per-tick tangent
    // would differ by the ratio of the rates, and A5 made that rate a number a FILE states.
    const FrameRate coarse{ 240, 1 };

    std::vector<ScalarKey> fine{ Key( 0, 0.0F ), Key( 12000, 30.0F ), Key( 24000, 10.0F ) };
    std::vector<ScalarKey> rough{ Key( 0, 0.0F ), Key( 120, 30.0F ), Key( 240, 10.0F ) };
    AutoSetTangents( fine, PROJECT_TICK_RATE );
    AutoSetTangents( rough, coarse );

    for ( std::size_t i = 0; i < fine.size(); ++i )
    {
        EXPECT_NEAR( fine[i].ArriveTangent, rough[i].ArriveTangent, 1.0e-3F ) << "key " << i;
        EXPECT_NEAR( fine[i].LeaveTangent, rough[i].LeaveTangent, 1.0e-3F ) << "key " << i;
    }

    for ( const double u : { 0.1, 0.25, 0.5, 0.75, 0.9 } )
    {
        EXPECT_NEAR( SampleAt( fine, 24000.0 * u, PROJECT_TICK_RATE ),
                     SampleAt( rough, 240.0 * u, coarse ), 1.0e-3F )
             << "the two grids disagree at u = " << u;
    }
}

TEST( KeyInterpolation, EveryModeAndEveryTangentModeHasAName )
{
    for ( const auto interp : { KeyInterp::Constant, KeyInterp::Linear, KeyInterp::Cubic } )
    {
        EXPECT_STRNE( ToString( interp ), "?" );
    }
    for ( const auto mode : { TangentMode::Auto, TangentMode::User, TangentMode::Break } )
    {
        EXPECT_STRNE( ToString( mode ), "?" );
    }
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
