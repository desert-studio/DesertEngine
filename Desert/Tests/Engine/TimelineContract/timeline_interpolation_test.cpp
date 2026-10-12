// THE TIMELINE CORE'S CONTRACT, SEQ1a: one FloatChannel evaluated under each key interpolation and the two
// easing presets a level sequence leans on (ease-in-out for doors and lifts, overshoot for parts that fly
// together). UE mapping: Constant / Linear / Cubic = ERichCurveInterpMode RCIM_Constant / RCIM_Linear /
// RCIM_Cubic; TangentMode Auto / User = RCTM_Auto / RCTM_User; the presets author real cubic keys with User
// tangents (QuadInOut = EMovieSceneBuiltInEasing::QuadInOut as keys; BackOut = a cubic key whose User tangent
// overshoots, as an animator draws it in UE's curve editor).

#include "TimelineFixtures.hpp"

#include <algorithm>

using namespace TimelineFixtures;

namespace
{
    constexpr FrameRate kRate{ 60, 1 }; // 60 ticks = 1 s, so a tangent of N is N per 60 ticks
} // namespace

TEST( TimelineInterpolation, ConstantHoldsTheKeyUntilTheNextTick )
{
    const FloatChannel channel{ { Key( 0, 10.0F, KeyInterp::Constant ), Key( 60, 20.0F ) }, 0.0F };
    EXPECT_EQ( Evaluate( channel, At( 0 ), kRate ), 10.0F );
    EXPECT_EQ( Evaluate( channel, At( 30 ), kRate ), 10.0F );
    EXPECT_EQ( Evaluate( channel, At( 59, 0.9F ), kRate ), 10.0F );
    EXPECT_EQ( Evaluate( channel, At( 60 ), kRate ), 20.0F );
}

TEST( TimelineInterpolation, LinearHitsTheKeysAndIsTheStraightLineBetween )
{
    const FloatChannel channel{ { Key( 0, 10.0F ), Key( 60, 20.0F ) }, 0.0F };
    EXPECT_EQ( Evaluate( channel, At( 0 ), kRate ), 10.0F );
    EXPECT_NEAR( Evaluate( channel, At( 15 ), kRate ), 12.5F, 1e-5F );
    EXPECT_NEAR( Evaluate( channel, At( 30 ), kRate ), 15.0F, 1e-5F );
    EXPECT_EQ( Evaluate( channel, At( 60 ), kRate ), 20.0F );
}

TEST( TimelineInterpolation, CubicUserTangentsAreTheHermiteOfThoseSlopes )
{
    ScalarKey start    = Key( 0, 0.0F, KeyInterp::Cubic );
    start.Mode         = TangentMode::User;
    start.LeaveTangent = 2.0F; // value per second
    ScalarKey end      = Key( 60, 1.0F, KeyInterp::Cubic );
    end.Mode           = TangentMode::User;
    end.ArriveTangent  = 0.0F;
    const FloatChannel channel{ { start, end }, 0.0F };
    EXPECT_EQ( Evaluate( channel, At( 0 ), kRate ), 0.0F );
    EXPECT_EQ( Evaluate( channel, At( 60 ), kRate ), 1.0F );
    // h00(.5)*0 + h10(.5)*2*1s + h01(.5)*1 + h11(.5)*0 = 0.125*2 + 0.5
    EXPECT_NEAR( Evaluate( channel, At( 30 ), kRate ), 0.75F, 1e-5F );
    // The same keys with flat tangents are the smoothstep: 0.5 at the middle. The tangent is what moved it.
    FloatChannel flat         = channel;
    flat.Keys[0].LeaveTangent = 0.0F;
    EXPECT_NEAR( Evaluate( flat, At( 30 ), kRate ), 0.5F, 1e-5F );
}

TEST( TimelineInterpolation, CubicAutoTangentsFlattenAnExtremumAndStaySymmetric )
{
    std::vector<ScalarKey> keys = { Key( 0, 0.0F, KeyInterp::Cubic ), Key( 60, 1.0F, KeyInterp::Cubic ),
                                    Key( 120, 0.0F, KeyInterp::Cubic ) };
    AutoSetTangents( keys, kRate );
    EXPECT_EQ( keys[1].ArriveTangent, 0.0F ) << "an extremum gets a flat tangent";
    EXPECT_EQ( keys[1].LeaveTangent, 0.0F );
    const FloatChannel channel{ keys, 0.0F };
    EXPECT_EQ( Evaluate( channel, At( 60 ), kRate ), 1.0F );
    const float rising = Evaluate( channel, At( 30 ), kRate );
    EXPECT_GT( rising, 0.0F );
    EXPECT_LT( rising, 1.0F ) << "a flat-topped hill does not overshoot its keys";
    EXPECT_NEAR( Evaluate( channel, At( 90 ), kRate ), rising, 1e-5F );
}

TEST( TimelineInterpolation, EaseInOutIsSymmetricAboutTheMiddle )
{
    for ( const EasingPreset preset : { EasingPreset::QuadInOut, EasingPreset::CubicInOut } )
    {
        std::vector<ScalarKey> keys   = { Key( 0, 0.0F ), Key( 60, 100.0F ) }; // cm
        const auto             result = ApplyEasingPreset( keys, 1, preset, kRate, kRate );
        ASSERT_TRUE( result.IsSuccess() ) << result.GetError();
        const FloatChannel channel{ keys, 0.0F };
        EXPECT_EQ( Evaluate( channel, At( 0 ), kRate ), 0.0F );
        EXPECT_EQ( Evaluate( channel, At( 60 ), kRate ), 100.0F );
        EXPECT_NEAR( Evaluate( channel, At( 30 ), kRate ), 50.0F, 1e-3F );
        for ( int32_t tick = 1; tick < 30; ++tick )
        {
            const float early = Evaluate( channel, At( tick ), kRate );
            const float late  = Evaluate( channel, At( 60 - tick ), kRate );
            EXPECT_NEAR( early + late, 100.0F, 1e-3F ) << "tick " << tick;
            EXPECT_LT( early, static_cast<float>( tick ) * 100.0F / 60.0F ) << "eases in: slower than linear";
        }
    }
}

TEST( TimelineInterpolation, OvershootPassesTheEndValueAndSettlesOnIt )
{
    std::vector<ScalarKey> keys   = { Key( 0, 0.0F ), Key( 60, 100.0F ) };
    const auto             result = ApplyEasingPreset( keys, 1, EasingPreset::BackOut, kRate, kRate );
    ASSERT_TRUE( result.IsSuccess() ) << result.GetError();
    EXPECT_EQ( result.GetValue().InsertedKeys, 0U ) << "BackOut is one cubic";
    const FloatChannel channel{ keys, 0.0F };
    float              peak = 0.0F;
    for ( int32_t tick = 0; tick <= 60; ++tick )
        peak = std::max( peak, Evaluate( channel, At( tick ), kRate ) );
    EXPECT_GT( peak, 105.0F ) << "the part flies past its seat";
    EXPECT_EQ( Evaluate( channel, At( 60 ), kRate ), 100.0F );
    EXPECT_EQ( Evaluate( channel, At( 90 ), kRate ), 100.0F ) << "held after the last key";
}
