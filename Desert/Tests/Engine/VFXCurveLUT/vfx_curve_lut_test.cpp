// VFX-05. The curve LUT (Engine/VFX/VFXCurveLUT), a port of UE's curve data interface table
// (NiagaraDataInterfaceCurveBase UpdateLUT, NiagaraDataInterfaceVectorCurve UpdateTimeRanges / BuildLUT /
// SampleCurveInternal<LUT>). What is pinned:
//
//   - the table read back (the CPU twin of the shader's VFX_CurveSample) stays within the discretisation error
//     of the curve itself: h * |slope jump| / 4 at a linear kink, h^2 / 8 * max|f''| on a smooth cubic;
//   - outside the keys the end values hold, in the curve evaluator and in the table alike;
//   - the time range is the union of the channels' key ranges; a one-key curve is flat, never NaN;
//   - the adapter shapes a segment by the EARLIER key's interpolation (UE's rule);
//   - the system atlas holds every enabled particle-group curve at its own offset, nothing else.
#include <gtest/gtest.h>

#include <Engine/VFX/VFXCurveLUT.hpp>

#include <cmath>
#include <vector>

namespace
{
    namespace S   = Desert::Assets::Serialization;
    namespace VFX = Desert::VFX;
    using Desert::Animation::KeyInterp;

    S::VFXCurveKey Key( float time, float value, KeyInterp interp = KeyInterp::Linear )
    {
        S::VFXCurveKey k;
        k.Time   = time;
        k.Value  = value;
        k.Interp = interp;
        return k;
    }

    using Channels = std::vector<std::vector<S::VFXCurveKey>>;

    // The largest |table - curve| over a dense sweep of [0, 1].
    float MaxError( const Channels& channels, uint32_t channel )
    {
        std::vector<float> atlas;
        const auto         entry = VFX::BakeCurveLUT( channels, atlas );
        float              worst = 0.0f;
        for ( int i = 0; i <= 4000; ++i )
        {
            const float t = static_cast<float>( i ) / 4000.0f;
            worst         = std::max(
                 worst, std::abs( VFX::SampleCurveLUT( atlas, entry, t )[static_cast<glm::length_t>( channel )] -
                                          VFX::EvaluateCurve( channels[channel], t ) ) );
        }
        return worst;
    }

    constexpr float kStep = 1.0f / static_cast<float>( VFX::kVFXCurveLUTWidth - 1 );
} // namespace

TEST( VFXCurveLUT, ALinearKinkIsWithinItsDiscretisationError )
{
    // The kink at 0.3 is not on the sample grid (0.3 * 127 = 38.1): the worst error is at the kink,
    // h * |slope jump| / 4 for a lerp across it.
    const Channels channels  = { { Key( 0.0f, 0.0f ), Key( 0.3f, 1.0f ), Key( 1.0f, 0.5f ) } };
    const float    slopeJump = std::abs( ( 0.5f - 1.0f ) / 0.7f - 1.0f / 0.3f );
    const float    epsilon   = kStep * slopeJump / 4.0f + 1e-5f;
    EXPECT_LE( MaxError( channels, 0 ), epsilon );
    EXPECT_GT( MaxError( channels, 0 ), 1e-4f ) << "the kink must cost something, or the sweep missed it";
}

TEST( VFXCurveLUT, ASmoothCubicIsWithinItsSecondOrderError )
{
    // Flat tangents at both ends: 3t^2 - 2t^3, |f''| <= 6, so the lerp error is at most h^2 / 8 * 6.
    const Channels channels = { { Key( 0.0f, 0.0f, KeyInterp::Cubic ), Key( 1.0f, 1.0f, KeyInterp::Cubic ) } };
    EXPECT_NEAR( VFX::EvaluateCurve( channels[0], 0.25f ), 3 * 0.0625f - 2 * 0.015625f, 1e-6f );
    EXPECT_LE( MaxError( channels, 0 ), kStep * kStep / 8.0f * 6.0f + 1e-6f );
}

TEST( VFXCurveLUT, OutsideTheKeysTheEndValuesHold )
{
    const Channels     channels = { { Key( 0.2f, 2.0f ), Key( 0.8f, 5.0f ) } };
    std::vector<float> atlas;
    const auto         entry = VFX::BakeCurveLUT( channels, atlas );
    EXPECT_FLOAT_EQ( entry.MinTime, 0.2f );
    EXPECT_FLOAT_EQ( entry.MaxTime, 0.8f );
    for ( const float t : { -1.0f, 0.0f, 0.1f, 0.2f } )
    {
        EXPECT_FLOAT_EQ( VFX::EvaluateCurve( channels[0], t ), 2.0f ) << t;
        EXPECT_FLOAT_EQ( VFX::SampleCurveLUT( atlas, entry, t ).x, 2.0f ) << t;
    }
    for ( const float t : { 0.8f, 0.9f, 1.0f, 3.0f } )
    {
        EXPECT_FLOAT_EQ( VFX::EvaluateCurve( channels[0], t ), 5.0f ) << t;
        EXPECT_FLOAT_EQ( VFX::SampleCurveLUT( atlas, entry, t ).x, 5.0f ) << t;
    }
    EXPECT_NEAR( VFX::SampleCurveLUT( atlas, entry, 0.5f ).x, 3.5f, 1e-5f );
}

TEST( VFXCurveLUT, TheTimeRangeIsTheUnionOfTheChannels )
{
    const Channels     channels = { { Key( 0.1f, 1.0f ), Key( 0.5f, 3.0f ) },
                                    { Key( 0.3f, -1.0f ), Key( 0.9f, -4.0f ) } };
    std::vector<float> atlas( 7, 0.0f ); // something already in the atlas
    const auto         entry = VFX::BakeCurveLUT( channels, atlas );
    EXPECT_EQ( entry.Offset, 7u );
    EXPECT_EQ( entry.Channels, 2u );
    EXPECT_EQ( atlas.size(), 7u + 2u * VFX::kVFXCurveLUTWidth );
    EXPECT_FLOAT_EQ( entry.MinTime, 0.1f );
    EXPECT_FLOAT_EQ( entry.MaxTime, 0.9f );
    // Inside the range, each channel holds its own end value outside its own keys.
    const glm::vec4 late = VFX::SampleCurveLUT( atlas, entry, 0.7f );
    EXPECT_FLOAT_EQ( late.x, 3.0f );
    EXPECT_NEAR( late.y, -3.0f, 1e-3f );
    EXPECT_FLOAT_EQ( late.z, 0.0f );
    const glm::vec4 early = VFX::SampleCurveLUT( atlas, entry, 0.2f );
    EXPECT_NEAR( early.x, 1.5f, 1e-3f );
    EXPECT_FLOAT_EQ( early.y, -1.0f );
}

TEST( VFXCurveLUT, AOneKeyCurveIsFlatAndFinite )
{
    std::vector<float> atlas;
    const auto         entry = VFX::BakeCurveLUT( { { Key( 0.5f, 7.0f ) } }, atlas );
    EXPECT_EQ( entry.InvTimeRange, 0.0f );
    for ( const float t : { -2.0f, 0.0f, 0.5f, 1.0f, 9.0f } )
        EXPECT_EQ( VFX::SampleCurveLUT( atlas, entry, t ).x, 7.0f ) << t;
    const glm::vec4 row = VFX::CurveParamRow( entry );
    EXPECT_TRUE( std::isfinite( row.x ) && std::isfinite( row.y ) );
    EXPECT_EQ( row.w, static_cast<float>( VFX::kVFXCurveLUTWidth - 1 ) );
}

TEST( VFXCurveLUT, TheEarlierKeyShapesTheSegment )
{
    // A Constant key holds until the next key; a Linear key before a Constant one still ramps.
    const std::vector<S::VFXCurveKey> held = { Key( 0.0f, 1.0f, KeyInterp::Constant ), Key( 1.0f, 3.0f ) };
    EXPECT_FLOAT_EQ( VFX::EvaluateCurve( held, 0.9f ), 1.0f );
    const std::vector<S::VFXCurveKey> ramp = { Key( 0.0f, 1.0f ), Key( 1.0f, 3.0f, KeyInterp::Constant ) };
    EXPECT_FLOAT_EQ( VFX::EvaluateCurve( ramp, 0.5f ), 2.0f );
}

TEST( VFXCurveLUT, TheAtlasHoldsEveryEnabledParticleCurveAtItsOwnOffset )
{
    const auto curve = []( std::string name, S::VFXValueType type, Channels channels )
    {
        S::VFXModuleInput in;
        in.Name   = std::move( name );
        in.Type   = type;
        in.Source = S::VFXInputSource::Curve;
        in.Curve  = std::move( channels );
        return in;
    };
    S::VFXModuleInput value;
    value.Name  = "Speed";
    value.Value = glm::vec4( 3.0f );

    S::VFXSystemData  system;
    S::VFXEmitterData a;
    S::VFXEmitterData b;
    a.Stack.ParticleSpawn.push_back(
         { "engine:A",
           true,
           { value, curve( "Size", S::VFXValueType::Float, { { Key( 0, 1 ), Key( 1, 2 ) } } ) } } );
    a.Stack.ParticleUpdate.push_back(
         { "engine:B", false, { curve( "Off", S::VFXValueType::Float, { { Key( 0, 9 ) } } ) } } );
    a.Stack.EmitterUpdate.push_back(
         { "engine:C", true, { curve( "Rate", S::VFXValueType::Float, { { Key( 0, 9 ) } } ) } } );
    b.Stack.ParticleUpdate.push_back(
         { "engine:D",
           true,
           { curve( "Color", S::VFXValueType::Vec2, { { Key( 0, 0 ), Key( 1, 1 ) }, { Key( 0, 5 ) } } ) } } );
    system.Emitters = { a, b };

    const auto atlas = VFX::BuildCurveAtlas( system );
    ASSERT_TRUE( atlas.IsSuccess() ) << atlas.GetError();
    // Emitter 0 spawn Size, emitter 1 update Color; the disabled module and the CPU group bake nothing.
    ASSERT_EQ( atlas.GetValue().Entries.size(), 2u );
    const VFX::VFXCurveRef size{ 0, VFX::VFXStackGroup::ParticleSpawn, 0, "Size" };
    const VFX::VFXCurveRef color{ 1, VFX::VFXStackGroup::ParticleUpdate, 0, "Color" };
    ASSERT_NE( atlas.GetValue().Find( size ), nullptr );
    ASSERT_NE( atlas.GetValue().Find( color ), nullptr );
    EXPECT_EQ( atlas.GetValue().Find( VFX::VFXCurveRef{ 0, VFX::VFXStackGroup::ParticleUpdate, 0, "Off" } ),
               nullptr );
    EXPECT_EQ( atlas.GetValue().Find( size )->Offset, 0u );
    EXPECT_EQ( atlas.GetValue().Find( color )->Offset, VFX::kVFXCurveLUTWidth );
    EXPECT_EQ( atlas.GetValue().Floats.size(), 3u * VFX::kVFXCurveLUTWidth );

    const auto& floats = atlas.GetValue().Floats;
    EXPECT_FLOAT_EQ( VFX::SampleCurveLUT( floats, *atlas.GetValue().Find( size ), 1.0f ).x, 2.0f );
    const glm::vec4 c = VFX::SampleCurveLUT( floats, *atlas.GetValue().Find( color ), 0.5f );
    EXPECT_NEAR( c.x, 0.5f, 1e-5f );
    EXPECT_FLOAT_EQ( c.y, 5.0f );
}
