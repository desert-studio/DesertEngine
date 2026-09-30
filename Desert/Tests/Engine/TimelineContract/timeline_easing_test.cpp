// THE TIMELINE CORE'S CONTRACT, group 6: easing presets, the UI key model's replacement (ANIM-I23,
// ApplyEasingPreset in Channel.cpp). `PresetOf` is pinned at compile time in Hosts.cpp (it needs
// Components.hpp, which this suite does not build).

#include "TimelineFixtures.hpp"

using namespace TimelineFixtures;

// ── 6. Easing presets ───────────────────────────────────────────────────────────────────────────────

TEST( TimelineEasing, CubicOutIsExactOnTwoKeys )
{
    std::vector<ScalarKey> keys = { Key( 0, 0.0F ), Key( 60, 1.0F ) };
    const FrameRate        rate{ 60, 1 };
    const auto             result = ApplyEasingPreset( keys, 1, EasingPreset::CubicOut, rate, rate );
    ASSERT_TRUE( result.IsSuccess() ) << result.GetError();
    EXPECT_EQ( result.GetValue().InsertedKeys, 0U );
    EXPECT_EQ( result.GetValue().MaxDeviation, 0.0F );
    EXPECT_EQ( keys[1].Mode, TangentMode::User ) << "AutoSetTangents must not undo a preset";

    const FloatChannel channel{ keys, 0.0F };
    EXPECT_NEAR( Evaluate( channel, At( 30 ), rate ), 1.0F - 0.125F, 1e-5F ); // 1 - (1 - 0.5)^3
    EXPECT_NEAR( Evaluate( channel, At( 15 ), rate ), 1.0F - 0.421875F, 1e-5F );
}

TEST( TimelineEasing, InOutInsertsOneMiddleKeyAndElasticReportsItsBake )
{
    const FrameRate        rate{ 60, 1 };
    std::vector<ScalarKey> inOut = { Key( 0, 0.0F ), Key( 60, 1.0F ) };
    const auto             a     = ApplyEasingPreset( inOut, 1, EasingPreset::QuadInOut, rate, rate );
    ASSERT_TRUE( a.IsSuccess() );
    EXPECT_EQ( a.GetValue().InsertedKeys, 1U );
    ASSERT_EQ( inOut.size(), 3U );
    EXPECT_EQ( inOut[1].Tick, Tick( 30 ) );

    std::vector<ScalarKey> elastic = { Key( 0, 0.0F ), Key( 60, 1.0F ) };
    const auto             b       = ApplyEasingPreset( elastic, 1, EasingPreset::ElasticOut, rate, rate );
    ASSERT_TRUE( b.IsSuccess() );
    EXPECT_GT( b.GetValue().InsertedKeys, 0U );
    EXPECT_GE( b.GetValue().MaxDeviation, 0.0F );
}

TEST( TimelineEasing, TheFirstKeyOwnsNoSegment )
{
    std::vector<ScalarKey> keys = { Key( 0, 0.0F ), Key( 60, 1.0F ) };
    EXPECT_FALSE( ApplyEasingPreset( keys, 0, EasingPreset::QuadIn, FrameRate{}, FrameRate{} ).IsSuccess() );
    EXPECT_FALSE( ApplyEasingPreset( keys, 2, EasingPreset::QuadIn, FrameRate{}, FrameRate{} ).IsSuccess() );
}

TEST( TimelineEasing, EveryOneCubicPresetIsTheFormulaExactly )
{
    const FrameRate rate{ 60, 1 };
    struct Case
    {
        EasingPreset Preset;
        float        At15;
        float        At30;
    };
    // 1 + c3 (u-1)^3 + c1 (u-1)^2, c1 = 1.70158, c3 = 2.70158 (UICanvasRenderer2D's BackOut).
    const auto backOut = []( float u )
    { return 1.0F + 2.70158F * std::pow( u - 1.0F, 3.0F ) + 1.70158F * std::pow( u - 1.0F, 2.0F ); };
    for ( const Case& c :
          { Case{ EasingPreset::Linear, 0.25F, 0.5F }, Case{ EasingPreset::QuadIn, 0.0625F, 0.25F },
            Case{ EasingPreset::QuadOut, 0.4375F, 0.75F }, Case{ EasingPreset::CubicIn, 0.015625F, 0.125F },
            Case{ EasingPreset::BackOut, backOut( 0.25F ), backOut( 0.5F ) } } )
    {
        std::vector<ScalarKey> keys   = { Key( 0, 2.0F ), Key( 60, 6.0F ) };
        const auto             result = ApplyEasingPreset( keys, 1, c.Preset, rate, rate );
        ASSERT_TRUE( result.IsSuccess() ) << result.GetError();
        EXPECT_EQ( result.GetValue().InsertedKeys, 0U );
        EXPECT_EQ( result.GetValue().MaxDeviation, 0.0F );
        const FloatChannel channel{ keys, 0.0F };
        EXPECT_NEAR( Evaluate( channel, At( 15 ), rate ), 2.0F + 4.0F * c.At15, 1e-4F ) << int( c.Preset );
        EXPECT_NEAR( Evaluate( channel, At( 30 ), rate ), 2.0F + 4.0F * c.At30, 1e-4F ) << int( c.Preset );
    }
}

TEST( TimelineEasing, AnInOutOnAnEvenSpanIsExactAndAnOddOneReportsItsSplit )
{
    const FrameRate        rate{ 60, 1 };
    std::vector<ScalarKey> even  = { Key( 0, 0.0F ), Key( 60, 1.0F ) };
    const auto             exact = ApplyEasingPreset( even, 1, EasingPreset::CubicInOut, rate, rate );
    ASSERT_TRUE( exact.IsSuccess() ) << exact.GetError();
    EXPECT_EQ( exact.GetValue().MaxDeviation, 0.0F );
    const FloatChannel channel{ even, 0.0F };
    EXPECT_NEAR( Evaluate( channel, At( 15 ), rate ), 4.0F * 0.015625F, 1e-5F ); // 4u^3 at u = 1/4
    EXPECT_NEAR( Evaluate( channel, At( 45 ), rate ), 1.0F - 4.0F * 0.015625F, 1e-5F );
    EXPECT_EQ( even[1].Mode, TangentMode::User );

    std::vector<ScalarKey> odd   = { Key( 0, 0.0F ), Key( 7, 1.0F ) };
    const auto             split = ApplyEasingPreset( odd, 1, EasingPreset::QuadInOut, rate, rate );
    ASSERT_TRUE( split.IsSuccess() ) << split.GetError();
    EXPECT_EQ( odd[1].Tick, Tick( 3 ) );
    EXPECT_GT( split.GetValue().MaxDeviation, 0.0F ) << "the middle tick is not the middle";

    std::vector<ScalarKey> oneTick = { Key( 0, 0.0F ), Key( 1, 1.0F ) };
    EXPECT_FALSE( ApplyEasingPreset( oneTick, 1, EasingPreset::QuadInOut, rate, rate ).IsSuccess() )
         << "no middle tick to put the second cubic's key on";
}

TEST( TimelineEasing, ABakeLandsOnTheDisplayGridAndPassesThroughTheFormula )
{
    const FrameRate        tickRate{ 24000, 1 };
    const FrameRate        displayRate{ 30, 1 };
    std::vector<ScalarKey> keys   = { Key( 0, 0.0F ), Key( 24000, 1.0F ) }; // one second = 30 display frames
    const auto             result = ApplyEasingPreset( keys, 1, EasingPreset::BounceOut, tickRate, displayRate );
    ASSERT_TRUE( result.IsSuccess() ) << result.GetError();
    EXPECT_EQ( result.GetValue().InsertedKeys, 29U );
    ASSERT_EQ( keys.size(), 31U );
    for ( size_t i = 1; i + 1 < keys.size(); ++i )
    {
        EXPECT_EQ( keys[i].Tick.Value % 800, 0 ) << "key " << i << " is off the 30 fps grid";
    }
    EXPECT_LT( result.GetValue().MaxDeviation, 0.1F ) << "a bake is close, and says how close";
}
