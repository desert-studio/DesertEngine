// The cloud shadow map, tested as the RELATION it is rather than as two functions that happen to exist.
//
// The subsystem's defect taxonomy (DEV_CONTRACT §2.3.1) is one shape repeated: two sides that must agree,
// each individually correct, and nothing asserting the agreement. This feature has four such pairs, and
// this suite is one assertion per pair:
//
//   1. THE ENCODE AND THE DECODE. One shader writes (frontDepthKm, meanExtinctionPerKm, maxOpticalDepth)
//      and a completely different one reads it back at a depth the writer never knew. If they disagree
//      the picture is a shadow of the wrong strength, which reads as a badly tuned extinction and gets
//      "fixed" by moving the extinction. The assertion is that the reconstructed transmittance equals the
//      Beer-Lambert integral the LIGHT MARCH computes along the same ray — which is the requirement in
//      the task, and the reason the medium here is synthetic: a slab has a closed-form answer.
//
//   2. THE C++ MIRROR AND THE SHADER'S OWN CONSTANTS. Graphic::CloudShadowPayload.hpp restates six
//      numbers and three functions that Common/CloudShadowMap.glslh owns. Both are compiled here and
//      compared, so a drifting mirror is a red test rather than a shadow in the wrong place.
//
//   3. THE MAP'S TEXEL AND THE MARCH'S OWN RESOLUTION. The extent was DERIVED from the finest cloud chord
//      the view march can find; if either side moves, the derivation is void. Asserted as the inequality
//      it is, against Common/CloudGeometry.glslh's own schedule.
//
//   4. THE SNAP AND THE FRAME BEFORE IT. The anti-shimmer property is not a property of one frame at all:
//      it is that two frames with the camera in different places produce the SAME matrix. That is
//      invisible in any single rendered frame by construction, and it is one line here.
//
// Everything in this file is GPU-free. Common/CloudShadowMap.glslh and Common/CloudGeometry.glslh are
// compiled AS C++ through CloudShadowReference.hpp, so every assertion is about the text the GPU runs.

#include "CloudShadowReference.hpp"

#include <Engine/Graphic/Clouds/CloudQuality.hpp>
#include <Engine/Graphic/Clouds/CloudShadowPayload.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace Desert::Tests::CloudShadowRef;

namespace
{
    // The shipped layer of Clouds_Demo: a 6360 km planet with the built-in cumulus congestus' envelope,
    // 2.2 to 5.8 km. Written as numbers rather than read from the component because this suite is about
    // the map, not about the defaults — ComponentReflection owns those.
    constexpr float kPlanetKm = 6360.0f;
    constexpr float kBaseKm   = 2.2f;
    constexpr float kTopKm    = 5.8f;

    CloudLayer MakeLayer()
    {
        return CloudMakeLayer( kPlanetKm, kBaseKm, kTopKm - kBaseKm );
    }

    // The ray one texel of the map marches, expressed the way both the shader and this test need it.
    struct TexelRay
    {
        vec3  OriginKm;
        vec3  Direction;
        float EnterKm = 0.0f;
        float ExitKm  = 0.0f;
    };

    // A ray starting well above the layer and aimed at the planet, i.e. what a shadow-map texel always
    // is. @p tiltX moves it off vertical so the "sun is not overhead" cases are covered too.
    TexelRay MakeTexelRay( float startAltitudeKm, float tiltX )
    {
        CloudLayer layer = MakeLayer();

        TexelRay ray;
        ray.OriginKm  = vec3( 0.0f, kPlanetKm + startAltitudeKm, 0.0f );
        ray.Direction = glm::normalize( vec3( tiltX, -1.0f, 0.0f ) );

        const vec2 segment = CloudLayerIntersect( layer, ray.OriginKm, ray.Direction );
        ray.EnterKm        = segment.x;
        ray.ExitKm         = segment.y;
        return ray;
    }

    // WHAT THE LIGHT MARCH WOULD SAY, computed at a step fine enough that its own discretisation is not
    // what the comparison is measuring: the optical depth from the ray's origin to @p depthKm, integrated
    // over the SAME extinction function the texel was built from.
    //
    // This is the reference side of assertion 1. It is deliberately NOT the map's own accumulation — it
    // is the quantity the renderer computes the other way, by marching from the shaded point toward the
    // sun (CloudRaymarch.shader's CloudLightOpticalDepth), and the encoding's whole claim is that one
    // fetch reproduces it.
    double ReferenceOpticalDepth( const TexelRay& ray, float depthKm, int steps = 200000 )
    {
        const double dt  = static_cast<double>( depthKm ) / steps;
        double       tau = 0.0;
        for ( int i = 0; i < steps; ++i )
        {
            const double t   = ( i + 0.5 ) * dt;
            const vec3   pos = ray.OriginKm + ray.Direction * static_cast<float>( t );
            tau += static_cast<double>( CloudShadowTestExtinction( pos ) ) * dt;
        }
        return tau;
    }

    void SetUniformSlab( float extinctionPerKm )
    {
        g_Medium.BottomRadiusKm     = kPlanetKm + kBaseKm;
        g_Medium.TopRadiusKm        = kPlanetKm + kTopKm;
        g_Medium.ExtinctionPerKm    = extinctionPerKm;
        g_Medium.ModulationAmount   = 0.0f;
        g_Medium.ModulationPeriodKm = 1.0f;
    }
} // namespace

// ---------------------------------------------------------------------------------------------------
// 1. THE ENCODE AND THE DECODE — the relation this feature exists to get right
// ---------------------------------------------------------------------------------------------------

// A homogeneous slab is the case in which the encoding is not an approximation at all, so the assertion
// can be an equality rather than a tolerance. If this fails, the triple means something other than what
// the consumer thinks it means, and no amount of tuning the extinction will put the shadow right.
TEST( CloudShadowMap, ReconstructionMatchesTheMarchThroughAUniformSlab )
{
    SetUniformSlab( 8.0f ); // the component's shipped Extinction Scale

    const TexelRay         ray     = MakeTexelRay( 60.0f, 0.0f );
    const float            farKm   = CloudShadowFarDepthKm( CLOUD_SHADOWMAP_EXTENT_KM );
    const CloudShadowTexel data    = CloudShadowTraceRay( ray.OriginKm, ray.Direction, ray.EnterKm, ray.ExitKm,
                                                          static_cast<int>( CLOUD_SHADOWMAP_BASE_SAMPLES ), farKm );
    const vec3             encoded = CloudShadowEncode( data );

    // Sampled from above the cloud, at its front, through it, and out the far side onto the ground.
    const float thicknessKm = ray.ExitKm - ray.EnterKm;
    const float probes[]    = { ray.EnterKm - 5.0f,
                                ray.EnterKm,
                                ray.EnterKm + 0.25f * thicknessKm,
                                ray.EnterKm + 0.5f * thicknessKm,
                                ray.EnterKm + 0.9f * thicknessKm,
                                ray.ExitKm,
                                ray.ExitKm + 20.0f };

    for ( float depthKm : probes )
    {
        const double referenceTau  = ReferenceOpticalDepth( ray, depthKm );
        const float  reconstructed = CloudShadowTransmittance( encoded, depthKm, 1.0f );
        const double referenceT    = std::exp( -referenceTau );

        // 1e-3 of transmittance is two orders finer than anything a frame can show and an order finer
        // than the 8-bit target the shot is written to. The residual is the map's 32-sample quadrature
        // against the reference's 200 000.
        EXPECT_NEAR( reconstructed, static_cast<float>( referenceT ), 1e-3f )
             << "depth " << depthKm << " km along the ray";
    }
}

// WHERE THE ENCODING IS EXACT AND WHERE IT IS A LINEAR APPROXIMATION, stated as an assertion so that
// nobody has to discover it from a frame. Once the medium VARIES along the ray a single mean extinction
// cannot reproduce a profile, and this test pins which half of the picture that costs:
//
//   * A RECEIVER BELOW THE LAYER — the ground, which is what this whole feature is for — is EXACT. Its
//     depth is past the far side, the min() clamps to MaxOpticalDepth, and MaxOpticalDepth is the ray's
//     own integral, stored verbatim. The only residual is the 32-sample quadrature.
//   * A RECEIVER INSIDE THE LAYER gets the mean, which straightens the profile: it under-shades where the
//     medium is thinner than average and over-shades where it is denser. The error is BOUNDED by the
//     ray's total optical depth, which is what this asserts; it cannot invent material that is not on the
//     ray, only redistribute it along it.
//
// The medium here is deliberately brutal — a 0.9-amplitude modulation over a 0.4 km period inside a
// 3.6 km slab, far coarser structure than a real vertical profile — so the bound is measured against the
// worst case rather than against a flattering one.
TEST( CloudShadowMap, ExactBelowTheLayerAndBoundedInsideIt )
{
    SetUniformSlab( 8.0f );
    g_Medium.ModulationAmount   = 0.9f;
    g_Medium.ModulationPeriodKm = 0.4f;

    const TexelRay         ray     = MakeTexelRay( 60.0f, 0.35f );
    const float            farKm   = CloudShadowFarDepthKm( CLOUD_SHADOWMAP_EXTENT_KM );
    const CloudShadowTexel data    = CloudShadowTraceRay( ray.OriginKm, ray.Direction, ray.EnterKm, ray.ExitKm,
                                                          static_cast<int>( CLOUD_SHADOWMAP_BASE_SAMPLES ), farKm );
    const vec3             encoded = CloudShadowEncode( data );

    const double referenceTotal = ReferenceOpticalDepth( ray, ray.ExitKm );

    // THE GROUND CASE, and it is an equality. Two per cent is the 32-sample quadrature against the
    // reference's 200 000, not slack in the encoding.
    EXPECT_NEAR( data.MaxOpticalDepth, static_cast<float>( referenceTotal ), 0.02 * referenceTotal );

    const float belowKm = ray.ExitKm + 50.0f;
    EXPECT_NEAR( CloudShadowOpticalDepth( data, belowKm ), data.MaxOpticalDepth, 1e-5f );
    EXPECT_NEAR( CloudShadowTransmittance( encoded, belowKm, 1.0f ),
                 static_cast<float>( std::exp( -referenceTotal ) ),
                 0.02f * static_cast<float>( std::exp( -referenceTotal ) ) + 1e-6f );

    // Far below, the answer has stopped changing entirely: there is no more material to cross, and
    // without the min() the mean extinction would keep accumulating through the clear air under the
    // cloud until the ground went black at a distance rather than under a cloud.
    EXPECT_FLOAT_EQ( CloudShadowTransmittance( encoded, ray.ExitKm + 50.0f, 1.0f ),
                     CloudShadowTransmittance( encoded, farKm, 1.0f ) );

    // THE INSIDE CASE, and it is a bound. The worst pointwise error is printed so the number is on the
    // record rather than implied by a tolerance.
    float worstError = 0.0f;
    for ( int i = 0; i <= 40; ++i )
    {
        const float  depthKm = ray.EnterKm + ( ray.ExitKm - ray.EnterKm ) * ( i / 40.0f );
        const double refTau  = ReferenceOpticalDepth( ray, depthKm, 20000 );
        const float  mapTau  = CloudShadowOpticalDepth( data, depthKm );

        worstError = std::max( worstError, std::abs( mapTau - static_cast<float>( refTau ) ) );

        EXPECT_LE( mapTau, data.MaxOpticalDepth + 1e-4f )
             << "depth " << depthKm << " km: the map may redistribute material along the ray, never add it";
    }
    EXPECT_LE( worstError, static_cast<float>( referenceTotal ) );
    std::printf( "[cloud shadow] worst in-layer optical-depth error %.3f against a ray total of %.3f\n",
                 worstError, referenceTotal );
}

// THE MEAN IS OVER THE MATERIAL, NOT OVER THE CHORD — the one line of CloudShadowTraceRay that looks
// like tidiness and is not.
//
// `extinctionSum / max(1, hitCount)` counts only the samples that found something. Dividing by the SAMPLE
// COUNT instead is the obvious simplification, it is what a reader reaches for, and it is wrong in a way
// that gets worse the taller the shell is: the mean is multiplied by the depth travelled PAST THE FRONT of
// the cloud, so it has to be the extinction of the CLOUD and not of the whole chord including the clear air
// above and below it.
//
// This case is the one that separates them, and the suite did not have it until a deliberate breakage
// showed that every other test here passes with the wrong divisor: a THIN medium inside a TALL shell. Half
// a kilometre of cloud inside a 3.6 km envelope makes the two divisors differ by seven times, and the
// symptom in a frame would be a stratus deck that casts almost no shadow while a congestus filling the
// same shell casts a correct one — which reads as a problem with the stratus.
TEST( CloudShadowMap, TheMeanIsOverTheMaterialAndNotOverTheChord )
{
    SetUniformSlab( 8.0f );
    // The medium occupies 3.0 to 3.5 km of a shell that runs 2.2 to 5.8.
    g_Medium.BottomRadiusKm = kPlanetKm + 3.0f;
    g_Medium.TopRadiusKm    = kPlanetKm + 3.5f;

    const TexelRay         ray     = MakeTexelRay( 60.0f, 0.0f );
    const float            farKm   = CloudShadowFarDepthKm( CLOUD_SHADOWMAP_EXTENT_KM );
    const CloudShadowTexel data    = CloudShadowTraceRay( ray.OriginKm, ray.Direction, ray.EnterKm, ray.ExitKm,
                                                          static_cast<int>( CLOUD_SHADOWMAP_BASE_SAMPLES ), farKm );
    const vec3             encoded = CloudShadowEncode( data );

    // The stored mean is the medium's own extinction, not a seventh of it.
    EXPECT_NEAR( data.MeanExtinctionPerKm, 8.0f, 0.05f );

    // And the reconstruction is right at the BOTTOM OF THE CLOUD, which is where the two divisors part
    // company: at the shell's own exit both are clamped by MaxOpticalDepth and agree by accident.
    const float  bottomOfCloudKm = data.FrontDepthKm + 0.5f;
    const double referenceTau    = ReferenceOpticalDepth( ray, bottomOfCloudKm );
    EXPECT_NEAR( CloudShadowTransmittance( encoded, bottomOfCloudKm, 1.0f ),
                 static_cast<float>( std::exp( -referenceTau ) ), 5e-3f );

    // The front depth is the CLOUD's front and not the shell's, which is the other half of the same
    // statement: a shell entered 0.8 km above the cloud must not shade the 0.8 km of clear air under it.
    EXPECT_GT( data.FrontDepthKm, ray.EnterKm + 0.5f );
    EXPECT_FLOAT_EQ( CloudShadowTransmittance( encoded, ray.EnterKm + 0.2f, 1.0f ), 1.0f );
}

// Monotonicity. More of the ray behind you is more material in the way, always — an inversion here would
// mean a receiver deeper in the cloud lit MORE brightly than one at its edge, which reads as a normal
// problem rather than as a shadow problem.
TEST( CloudShadowMap, OpticalDepthNeverDecreasesWithDepth )
{
    SetUniformSlab( 8.0f );

    const TexelRay         ray  = MakeTexelRay( 60.0f, 0.2f );
    const float            farKm = CloudShadowFarDepthKm( CLOUD_SHADOWMAP_EXTENT_KM );
    const CloudShadowTexel data  = CloudShadowTraceRay( ray.OriginKm, ray.Direction, ray.EnterKm, ray.ExitKm,
                                                        static_cast<int>( CLOUD_SHADOWMAP_BASE_SAMPLES ), farKm );

    float previous = -1.0f;
    for ( int i = 0; i <= 200; ++i )
    {
        const float depthKm = farKm * ( i / 200.0f );
        const float tau     = CloudShadowOpticalDepth( data, depthKm );
        EXPECT_GE( tau, previous );
        previous = tau;
    }
}

// A ray that never meets the shell must read as fully lit at EVERY depth, and it must do so by the
// arithmetic rather than by a special case — the clear texel puts the front depth at the far plane, which
// no receiver can be past.
TEST( CloudShadowMap, AMissedShellIsFullyLitEverywhere )
{
    SetUniformSlab( 8.0f );

    const float farKm = CloudShadowFarDepthKm( CLOUD_SHADOWMAP_EXTENT_KM );
    // tExit <= tEnter is CloudLayerIntersect's own encoding of "nothing to march".
    const CloudShadowTexel data    = CloudShadowTraceRay( vec3( 0.0f, kPlanetKm + 60.0f, 0.0f ),
                                                          vec3( 0.0f, -1.0f, 0.0f ), 1.0f, -1.0f, 32, farKm );
    const vec3             encoded = CloudShadowEncode( data );

    for ( int i = 0; i <= 20; ++i )
        EXPECT_FLOAT_EQ( CloudShadowTransmittance( encoded, farKm * ( i / 20.0f ), 1.0f ), 1.0f );
}

// The strength is applied at READ time rather than baked into the map, which is where this parts company
// with Unreal. That is only legitimate if it commutes with the clamp — and it does, because both
// arguments of the min() scale together. Asserted rather than argued.
TEST( CloudShadowMap, StrengthScalesTheOpticalDepthAndCommutesWithTheClamp )
{
    SetUniformSlab( 8.0f );

    const TexelRay         ray  = MakeTexelRay( 60.0f, 0.0f );
    const float            farKm = CloudShadowFarDepthKm( CLOUD_SHADOWMAP_EXTENT_KM );
    const CloudShadowTexel data  = CloudShadowTraceRay( ray.OriginKm, ray.Direction, ray.EnterKm, ray.ExitKm,
                                                        static_cast<int>( CLOUD_SHADOWMAP_BASE_SAMPLES ), farKm );

    // The same texel with both channels pre-scaled — Unreal's arrangement — must give the same answer as
    // scaling the reconstruction.
    CloudShadowTexel baked    = data;
    const float      strength = 0.35f;
    baked.MeanExtinctionPerKm *= strength;
    baked.MaxOpticalDepth *= strength;

    const vec3 encodedRaw   = CloudShadowEncode( data );
    const vec3 encodedBaked = CloudShadowEncode( baked );

    for ( int i = 0; i <= 40; ++i )
    {
        const float depthKm = farKm * ( i / 40.0f );
        EXPECT_NEAR( CloudShadowTransmittance( encodedRaw, depthKm, strength ),
                     CloudShadowTransmittance( encodedBaked, depthKm, 1.0f ), 1e-6f );
    }

    // Zero strength is fully lit everywhere, which is what makes the component's Shadow Strength dial
    // reach "off" exactly rather than nearly.
    EXPECT_FLOAT_EQ( CloudShadowTransmittance( encodedRaw, farKm, 0.0f ), 1.0f );
}

// ---------------------------------------------------------------------------------------------------
// 2. THE C++ MIRROR AND THE SHADER'S OWN CONSTANTS
// ---------------------------------------------------------------------------------------------------

TEST( CloudShadowMap, TheCppMirrorAgreesWithTheShaderConstants )
{
    EXPECT_FLOAT_EQ( static_cast<float>( Desert::Graphic::kCloudShadowMapResolution ),
                     CLOUD_SHADOWMAP_RESOLUTION );
    EXPECT_FLOAT_EQ( Desert::Graphic::kCloudShadowExtentKm, CLOUD_SHADOWMAP_EXTENT_KM );
    EXPECT_FLOAT_EQ( Desert::Graphic::kCloudShadowSnapKm, CLOUD_SHADOWMAP_SNAP_KM );
    EXPECT_FLOAT_EQ( Desert::Graphic::kCloudShadowBorderFadeKm, CLOUD_SHADOWMAP_BORDER_FADE_KM );
    EXPECT_FLOAT_EQ( Desert::Graphic::kCloudShadowBaseSamples, CLOUD_SHADOWMAP_BASE_SAMPLES );
    EXPECT_FLOAT_EQ( Desert::Graphic::kCloudShadowHorizonMultiplier, CLOUD_SHADOWMAP_HORIZON_MULTIPLIER );
}

TEST( CloudShadowMap, TheCppMirrorAgreesWithTheShaderFunctions )
{
    for ( int i = -400; i <= 400; ++i )
    {
        const float value = i * 137.0f;
        EXPECT_FLOAT_EQ( Desert::Graphic::CloudShadowSnapToGrid( value, 2000.0f ),
                         CloudShadowSnapToGrid( value, 2000.0f ) );
    }

    for ( int i = -256; i <= 256; ++i )
    {
        const float clipComponent = i / 200.0f;
        EXPECT_FLOAT_EQ( Desert::Graphic::CloudShadowSnapToPixelGrid( clipComponent, CLOUD_SHADOWMAP_RESOLUTION ),
                         CloudShadowSnapToPixelGrid( clipComponent, CLOUD_SHADOWMAP_RESOLUTION ) );
    }

    for ( int i = 0; i <= 90; ++i )
    {
        const float sunUpDot = std::sin( glm::radians( static_cast<float>( i ) ) );
        EXPECT_FLOAT_EQ( Desert::Graphic::CloudShadowSampleCount( sunUpDot, CLOUD_SHADOWMAP_BASE_SAMPLES,
                                                                  CLOUD_SHADOWMAP_HORIZON_MULTIPLIER ),
                         CloudShadowSampleCount( sunUpDot ) );
    }

    // The border fade is a DERIVED quantity on both sides now — a fixed world width over whatever side
    // the tier gave the map — so the two expressions are compared over the extents a tier can produce
    // rather than at the one value they used to be written as.
    for ( int i = 1; i <= 60; ++i )
    {
        const float extentKm = static_cast<float>( i );
        EXPECT_FLOAT_EQ( Desert::Graphic::CloudShadowBorderFadeUv( extentKm ),
                         CloudShadowBorderFadeUv( extentKm ) );
    }
}

// The sample count is a COST as well as a quality, so both ends of it are pinned rather than trusted.
//
// AND THE HIGH END IS NOT THE BASE COUNT, which is worth an assertion of its own because it is
// surprising: Unreal's horizon factor is `clamp(0.2 / sin(elevation), 0, 1)`, which is 0.2 and not 0 with
// the sun straight overhead. The count therefore never falls below 1.2x the base — 38.4 samples where a
// reading of the constant alone would predict 32. That is twenty per cent of this pass's whole cost, and
// it is the difference between the price list in CALIBRATION.md being right and being a fifth optimistic.
TEST( CloudShadowMap, TheSampleCountStaysBetweenItsTwoEnds )
{
    for ( int i = 0; i <= 90; ++i )
    {
        const float count = CloudShadowSampleCount( std::sin( glm::radians( static_cast<float>( i ) ) ) );
        EXPECT_GE( count, CLOUD_SHADOWMAP_BASE_SAMPLES );
        EXPECT_LE( count, CLOUD_SHADOWMAP_BASE_SAMPLES * CLOUD_SHADOWMAP_HORIZON_MULTIPLIER );
    }

    // Sun overhead: the floor of the formula, 1.2x the base.
    EXPECT_FLOAT_EQ( CloudShadowSampleCount( 1.0f ), CLOUD_SHADOWMAP_BASE_SAMPLES * 1.2f );
    // Sun at or below 11.5 degrees of elevation, where the factor saturates: the full multiplier.
    EXPECT_FLOAT_EQ( CloudShadowSampleCount( 0.0f ),
                     CLOUD_SHADOWMAP_BASE_SAMPLES * CLOUD_SHADOWMAP_HORIZON_MULTIPLIER );
    EXPECT_FLOAT_EQ( CloudShadowSampleCount( 0.2f ),
                     CLOUD_SHADOWMAP_BASE_SAMPLES * CLOUD_SHADOWMAP_HORIZON_MULTIPLIER );
    // And the count rises as the sun falls, everywhere in between.
    float previous = 0.0f;
    for ( int i = 90; i >= 0; --i )
    {
        const float count = CloudShadowSampleCount( std::sin( glm::radians( static_cast<float>( i ) ) ) );
        EXPECT_GE( count, previous );
        previous = count;
    }
}

// ---------------------------------------------------------------------------------------------------
// 3. THE MAP'S TEXEL AND THE MARCH'S OWN RESOLUTION
// ---------------------------------------------------------------------------------------------------

// The extent was derived FROM the resolution and the finest chord the view march can find. This is that
// derivation, asserted: a texel coarser than the chord throws away detail the sky visibly has, and one
// much finer stores detail the producer cannot make. If somebody moves the step schedule, the component's
// default Max Steps or the extent, this is the line that says the three no longer add up.
TEST( CloudShadowMap, TheTexelResolvesWhatTheMarchCanFind )
{
    // The component's default ceiling. Stated here rather than read from the component for the reason the
    // layer numbers above are: this suite owns the relation, ComponentReflection owns the default.
    constexpr float kDefaultMaxSteps = 256.0f;

    const float texelKm = CloudShadowTexelKm( CLOUD_SHADOWMAP_EXTENT_KM, CLOUD_SHADOWMAP_RESOLUTION );
    const float chordKm = CloudFinestResolvableChordKm( kDefaultMaxSteps );

    EXPECT_LE( texelKm, chordKm ) << "the shadow map is coarser than the finest cloud the march can find";
    EXPECT_GE( texelKm, chordKm * 0.5f )
         << "the shadow map is storing detail the march cannot produce — resolution being spent on nothing";

    // And the vertical axis of the same statement: the sample spacing across the shipped congestus
    // envelope must resolve the same chord, or the map's two axes disagree about what a cloud is.
    const float envelopeKm      = kTopKm - kBaseKm;
    const float sampleSpacingKm = envelopeKm / CLOUD_SHADOWMAP_BASE_SAMPLES;
    EXPECT_LE( sampleSpacingKm, chordKm );
}

// ---------------------------------------------------------------------------------------------------
// 3a. THE SAME RELATIONS, ON EVERY QUALITY TIER AND NOT ONLY ON THE DEFAULT
// ---------------------------------------------------------------------------------------------------
//
// A tier that breaks an invariant is worse than a tier that does not exist, so the three assertions above
// are re-run against the numbers Graphic::CloudQualityFor produces for every enumerator rather than
// against the constants that happen to be compiled in. That is what makes the tier a mechanism instead of
// three sets of magic numbers: adding a fourth tier cannot silently coarsen the map past what the march
// can find, because this loop will say so with the two figures side by side.

namespace
{
    // Every enumerator, listed once. A new tier that is not added here is a tier nothing checks, which is
    // why the count is asserted below rather than left to the reader.
    constexpr Common::Settings::CloudQuality kAllTiers[] = {
         Common::Settings::CloudQuality::Low,
         Common::Settings::CloudQuality::Medium,
         Common::Settings::CloudQuality::High,
    };

    const char* TierName( Common::Settings::CloudQuality tier )
    {
        switch ( tier )
        {
            case Common::Settings::CloudQuality::Low:
                return "Low";
            case Common::Settings::CloudQuality::Medium:
                return "Medium";
            case Common::Settings::CloudQuality::High:
                return "High";
        }
        return "?";
    }

    // The component's default Max Steps, which no tier lowers and which
    // Desert/Tests/Engine/CloudType explains at length that no tier MAY lower: the shipped library's
    // tightest types sit 1.10x above the chord this number buys, so there is nothing to spend.
    constexpr float kDefaultMaxStepsAllTiers = 256.0f;
} // namespace

TEST( CloudShadowMapTiers, EveryTierKeepsTheTexelInsideTheChordTheMarchCanFind )
{
    using namespace Desert::Graphic;

    // QUALIFIED, because there are two of it now and neither is wrong. This suite compiles
    // Common/CloudGeometry.glslh as C++ — that is the whole point of it — and phase Э5 also gave
    // Engine/Graphic/Clouds/CloudPayload.hpp a C++ mirror of the same function, because the procedural
    // generator places its lumps on the CPU and has to size them against what the march can find. The
    // shader's own text is the one this test wants: it is the thing being held to a relation.
    const float chordKm = Desert::Tests::CloudShadowRef::CloudFinestResolvableChordKm( kDefaultMaxStepsAllTiers );

    for ( const Common::Settings::CloudQuality tier : kAllTiers )
    {
        const CloudQualityScale scale      = CloudQualityFor( tier );
        const float             extentKm   = CloudShadowExtentKmForScale( scale.ShadowMapScale );
        const uint32_t          resolution = CloudShadowResolutionForScale( scale.ShadowMapScale );
        const float             texelKm    = CloudShadowTexelKm( extentKm, static_cast<float>( resolution ) );

        std::printf( "[CloudShadowMapTiers] %-6s  map %ux%u over %.1f km radius -> texel %.1f m, chord "
                     "%.1f m (%.2fx), samples <= %d\n",
                     TierName( tier ), resolution, resolution, extentKm, texelKm * 1000.0f, chordKm * 1000.0f,
                     texelKm / chordKm, scale.LightMarchSampleCeiling );

        EXPECT_LE( texelKm, chordKm )
             << TierName( tier ) << " gives the shadow map a texel of " << texelKm * 1000.0f << " m against the "
             << chordKm * 1000.0f
             << " m chord the view march can be relied on to find, so the map would be quantizing cloud "
                "the sky visibly has. Scale the extent WITH the resolution — that is what "
                "CloudQualityScale::ShadowMapScale is a single number for — rather than dropping the "
                "resolution alone";

        EXPECT_GE( texelKm, chordKm * 0.5f )
             << TierName( tier )
             << " is storing detail the march cannot produce — resolution spent on "
                "nothing, which is the opposite failure and just as much a defect";
    }
}

TEST( CloudShadowMapTiers, EveryTierResolvesTheEnvelopeVerticallyToo )
{
    // The map's OTHER axis, on every tier. The base sample count is not scaled by the tier at all, and
    // this is the assertion that says why it cannot be: 3.6 km of shipped congestus over 32 samples is
    // 112 m against a 125 m chord, so sixteen — the count the shadow-map task priced at 2.75 ms — would
    // be 225 m and the map's two axes would stop agreeing about what a cloud is.
    const float chordKm         = CloudFinestResolvableChordKm( kDefaultMaxStepsAllTiers );
    const float envelopeKm      = kTopKm - kBaseKm;
    const float sampleSpacingKm = envelopeKm / CLOUD_SHADOWMAP_BASE_SAMPLES;

    EXPECT_LE( sampleSpacingKm, chordKm );

    // And the number that says how little room there is: the lowest count that still resolves the
    // envelope. Printed rather than asserted as a literal, so the next person reads it instead of
    // rediscovering it.
    const float lowestLegalCount = envelopeKm / chordKm;
    std::printf( "[CloudShadowMapTiers] the envelope is %.2f km; the lowest base sample count that still "
                 "resolves the %.1f m chord is %.1f, and the shipped count is %.0f\n",
                 envelopeKm, chordKm * 1000.0f, lowestLegalCount, CLOUD_SHADOWMAP_BASE_SAMPLES );
    EXPECT_GE( CLOUD_SHADOWMAP_BASE_SAMPLES, lowestLegalCount );
}

TEST( CloudShadowMapTiers, EveryTierFadesItsBorderOverAtLeastTenTexels )
{
    using namespace Desert::Graphic;

    for ( const Common::Settings::CloudQuality tier : kAllTiers )
    {
        const CloudQualityScale scale      = CloudQualityFor( tier );
        const float             extentKm   = CloudShadowExtentKmForScale( scale.ShadowMapScale );
        const uint32_t          resolution = CloudShadowResolutionForScale( scale.ShadowMapScale );
        const float             texelKm    = CloudShadowTexelKm( extentKm, static_cast<float>( resolution ) );
        const float             bandKm = 2.0f * extentKm * Desert::Graphic::CloudShadowBorderFadeUv( extentKm );

        // The world width is the same on every tier BY CONSTRUCTION — that is the whole reason the fade
        // stopped being a UV constant — so the ten-texel span survives a tier that halves the map.
        EXPECT_NEAR( bandKm, CLOUD_SHADOWMAP_BORDER_FADE_KM, 1e-4f );
        EXPECT_GT( bandKm, 10.0f * texelKm )
             << TierName( tier )
             << " fades its border over fewer than ten texels, which bands instead of blending";
    }
}

TEST( CloudShadowMapTiers, EveryTierGuaranteesCoverageAroundTheCameraAndTheLadderIsMonotone )
{
    using namespace Desert::Graphic;

    // The centre lands within half a snap of the camera, so what a tier actually PROMISES is
    // extent - snap/2. The snap scales with the extent precisely so this stays positive and stays the
    // same FRACTION of the map; a snap left at its reference value would eat a halved extent from the
    // inside and could take the guarantee to zero.
    float previousCoverageKm = 0.0f;
    float previousCostShare  = 0.0f;

    for ( const Common::Settings::CloudQuality tier : kAllTiers ) // Low -> Medium -> High
    {
        const CloudQualityScale scale      = CloudQualityFor( tier );
        const float             extentKm   = CloudShadowExtentKmForScale( scale.ShadowMapScale );
        const float             snapKm     = CloudShadowSnapKmForScale( scale.ShadowMapScale );
        const uint32_t          resolution = CloudShadowResolutionForScale( scale.ShadowMapScale );

        const float coverageKm = extentKm - 0.5f * snapKm;
        EXPECT_GT( coverageKm, 0.0f ) << TierName( tier ) << " guarantees no cloud shadow anywhere at all";
        EXPECT_NEAR( coverageKm / extentKm, 2.0f / 3.0f, 1e-5f )
             << TierName( tier ) << " does not keep the snap in proportion to the extent";

        // MONOTONE IN BOTH DIRECTIONS, which is the property that makes this a ladder rather than three
        // opinions: a more expensive tier must never cover less world, and must never cost less.
        EXPECT_GE( coverageKm, previousCoverageKm );
        previousCoverageKm = coverageKm;

        // The pass marches resolution^2 rays, so this is its cost in the only unit that matters here.
        const float costShare = static_cast<float>( resolution ) * static_cast<float>( resolution );
        EXPECT_GE( costShare, previousCostShare );
        previousCostShare = costShare;

        std::printf( "[CloudShadowMapTiers] %-6s  guaranteed coverage %.1f km, %u^2 texel-rays\n",
                     TierName( tier ), coverageKm, resolution );
    }
}

TEST( CloudShadowMapTiers, TheReferenceTierIsExactlyWhatWasShippedBeforeTheTierExisted )
{
    using namespace Desert::Graphic;

    // The one assertion that says this task changed no picture at High: the reference tier reproduces the
    // three constants the map was shipped with, to the digit.
    const CloudQualityScale high = CloudQualityFor( Common::Settings::CloudQuality::High );

    EXPECT_FLOAT_EQ( high.ShadowMapScale, 1.0f );
    EXPECT_EQ( CloudShadowResolutionForScale( high.ShadowMapScale ), kCloudShadowMapResolution );
    EXPECT_FLOAT_EQ( CloudShadowExtentKmForScale( high.ShadowMapScale ), kCloudShadowExtentKm );
    EXPECT_FLOAT_EQ( CloudShadowSnapKmForScale( high.ShadowMapScale ), kCloudShadowSnapKm );
    // And it caps nothing the component's own Range does not, so an authored 32 reaches the GPU as 32.
    EXPECT_EQ( high.LightMarchSampleCeiling, Desert::ECS::kCloudLightMarchMaxSamples );
}

// ---------------------------------------------------------------------------------------------------
// 4. THE SNAP AND THE FRAME BEFORE IT — the whole anti-shimmer property, which no frame can show
// ---------------------------------------------------------------------------------------------------

namespace
{
    using Desert::Graphic::CloudBuildShadowMapView;
    using Desert::Graphic::CloudShadowMapView;
    using Desert::Graphic::kCloudShadowExtentKm;
    using Desert::Graphic::kCloudShadowMapResolution;
    using Desert::Graphic::kCloudShadowSnapKm;
    using Desert::Graphic::kCloudWorldUnitsPerKm;

    CloudShadowMapView BuildView( const glm::vec3& cameraWorld, const glm::vec3& toSun )
    {
        return CloudBuildShadowMapView( cameraWorld, toSun, kPlanetKm, kCloudShadowExtentKm, kCloudShadowSnapKm,
                                        static_cast<float>( kCloudShadowMapResolution ) );
    }

    const glm::vec3 kSun = glm::normalize( glm::vec3( 0.30f, 0.75f, 0.55f ) );
} // namespace

// THE ASSERTION THE WHOLE SNAP EXISTS FOR. The camera walks four kilometres and the projection does not
// move by one bit, so every piece of world stays in the texel it was in and the shadow is nailed to the
// ground. Without the snap this test fails on the second sample, and the rendered symptom is a shadow
// that boils in place under a moving camera — which is invisible in a still frame BY CONSTRUCTION,
// because a still frame has exactly one projection.
TEST( CloudShadowMap, TheProjectionIsIdenticalWhileTheCameraStaysInsideOneSnapCell )
{
    const CloudShadowMapView reference = BuildView( glm::vec3( 0.0f, 150000.0f, 0.0f ), kSun );

    for ( int i = 0; i <= 40; ++i )
    {
        // Four kilometres of travel in 100 m steps, plus a climb — well inside the 20 km grid.
        const glm::vec3          camera( i * 10000.0f, 150000.0f + i * 2000.0f, i * 7000.0f );
        const CloudShadowMapView view = BuildView( camera, kSun );

        for ( int c = 0; c < 4; ++c )
            for ( int r = 0; r < 4; ++r )
                EXPECT_FLOAT_EQ( view.WorldToMap[c][r], reference.WorldToMap[c][r] )
                     << "camera step " << i << ", matrix element (" << c << ", " << r << ")";
    }
}

// The other half of the same property: when the camera DOES leave the cell the map moves by exactly one
// grid step, not by an arbitrary amount. A snap that drifted would be a snap that only postpones the
// shimmer.
TEST( CloudShadowMap, LeavingTheCellMovesTheMapByExactlyOneGridStep )
{
    const float snapWorld = kCloudShadowSnapKm * kCloudWorldUnitsPerKm;

    // Two cameras exactly one grid step apart. Stated that way rather than as "either side of a cell
    // boundary" because the boundary is not where a first reading puts it: the map is centred on the
    // point of the planet's SURFACE under the camera, which sits inside the camera's own radius by the
    // curvature, so a camera at half a step is still in the first cell.
    const CloudShadowMapView a = BuildView( glm::vec3( 0.0f, 150000.0f, 0.0f ), kSun );
    const CloudShadowMapView b = BuildView( glm::vec3( snapWorld, 150000.0f, 0.0f ), kSun );

    // The world point that sits at the centre of each map — the map's own anchor — differs by one step.
    const glm::vec4 centreA = a.MapToWorld * glm::vec4( 0.0f, 0.0f, 0.5f, 1.0f );
    const glm::vec4 centreB = b.MapToWorld * glm::vec4( 0.0f, 0.0f, 0.5f, 1.0f );

    // ONE GRID STEP, to within ONE TEXEL — and the residual is not slop, it is the second snap. The pixel
    // grid snap that follows moves the finished projection by up to half a texel in each axis so the
    // world origin lands on the lattice, so the two centres cannot be exactly a grid step apart and must
    // not be: that half texel is the whole point of the second snap.
    const float texelWorld =
         CloudShadowTexelKm( kCloudShadowExtentKm, static_cast<float>( kCloudShadowMapResolution ) ) *
         kCloudWorldUnitsPerKm;

    EXPECT_NEAR( centreB.x - centreA.x, snapWorld, texelWorld );
    EXPECT_NEAR( centreB.z - centreA.z, 0.0f, texelWorld );
}

// BOTH SNAPS, ON EVERY TIER. The anti-boil property is the one thing a smaller map could plausibly have
// broken, because the tier scales the SNAP along with the extent: a cheaper tier re-anchors its map twice
// as often as the camera travels. Twice as often is not the same as less stable, and this is the
// assertion of that distinction — inside a cell the projection is still bit-for-bit constant, and leaving
// one still moves the map by exactly one step of that tier's own grid. Neither statement was true of the
// coarse snap alone; both survive being scaled.
//
// It matters because no still frame can show it. A still frame has exactly one projection, so a tier that
// boiled would render identically to one that did not until the camera moved.
TEST( CloudShadowMapTiers, EveryTierKeepsBothSnapsUnderAMovingCamera )
{
    using namespace Desert::Graphic;

    for ( const Common::Settings::CloudQuality tier : kAllTiers )
    {
        const CloudQualityScale scale      = CloudQualityFor( tier );
        const float             extentKm   = CloudShadowExtentKmForScale( scale.ShadowMapScale );
        const float             snapKm     = CloudShadowSnapKmForScale( scale.ShadowMapScale );
        const uint32_t          resolution = CloudShadowResolutionForScale( scale.ShadowMapScale );
        const float             snapWorld  = snapKm * kCloudWorldUnitsPerKm;

        auto build = [&]( const glm::vec3& cameraWorld )
        {
            return CloudBuildShadowMapView( cameraWorld, kSun, kPlanetKm, extentKm, snapKm,
                                            static_cast<float>( resolution ) );
        };

        // 1. Inside one cell the matrix does not move at all. The walk is a fifth of this tier's own
        //    grid, so it scales with the tier instead of being a distance that happens to fit the
        //    reference one.
        const CloudShadowMapView reference = build( glm::vec3( 0.0f, 150000.0f, 0.0f ) );
        for ( int i = 0; i <= 40; ++i )
        {
            const float              t = static_cast<float>( i ) / 40.0f * 0.2f * snapWorld;
            const glm::vec3          camera( t, 150000.0f + t * 0.2f, t * 0.7f );
            const CloudShadowMapView view = build( camera );

            for ( int c = 0; c < 4; ++c )
                for ( int r = 0; r < 4; ++r )
                    EXPECT_FLOAT_EQ( view.WorldToMap[c][r], reference.WorldToMap[c][r] )
                         << TierName( tier ) << ", camera step " << i << ", element (" << c << ", " << r
                         << ") — this tier's shadow boils under a moving camera";
        }

        // 2. Leaving the cell moves the map by exactly one step of THIS tier's grid, to within one of
        //    THIS tier's texels — the residual being the pixel-lattice snap, which is the point of it.
        const CloudShadowMapView b       = build( glm::vec3( snapWorld, 150000.0f, 0.0f ) );
        const glm::vec4          centreA = reference.MapToWorld * glm::vec4( 0.0f, 0.0f, 0.5f, 1.0f );
        const glm::vec4          centreB = b.MapToWorld * glm::vec4( 0.0f, 0.0f, 0.5f, 1.0f );
        const float              texelWorld =
             CloudShadowTexelKm( extentKm, static_cast<float>( resolution ) ) * kCloudWorldUnitsPerKm;

        EXPECT_NEAR( centreB.x - centreA.x, snapWorld, texelWorld ) << TierName( tier );
        EXPECT_NEAR( centreB.z - centreA.z, 0.0f, texelWorld ) << TierName( tier );

        // 3. And the sub-texel residual the second snap removes is the SAME SIZE on every tier, because
        //    the texel is. That is the number that decides whether a shadow shimmers, and it is why
        //    halving the snap costs nothing in stability: re-anchoring more often is not re-anchoring
        //    less accurately.
        EXPECT_NEAR( texelWorld,
                     CloudShadowTexelKm( kCloudShadowExtentKm, static_cast<float>( kCloudShadowMapResolution ) ) *
                          kCloudWorldUnitsPerKm,
                     1.0f )
             << TierName( tier ) << " changed the size of the lattice the second snap works on";
    }
}

// The pixel-grid snap, stated as what it achieves rather than as what it does: a FIXED world point — the
// origin — lands on the map's lattice exactly. It is the residual the coarse snap cannot remove, because
// the coarse snap freezes where the map is and not which way it points.
TEST( CloudShadowMap, TheWorldOriginLandsOnThePixelLattice )
{
    for ( int i = 0; i <= 24; ++i )
    {
        // A sun sweeping from near the horizon to overhead, which is what rotates the basis.
        const float     elevation = glm::radians( 4.0f + i * 3.5f );
        const glm::vec3 toSun( std::cos( elevation ) * 0.8f, std::sin( elevation ), std::cos( elevation ) * 0.6f );

        const CloudShadowMapView view =
             BuildView( glm::vec3( 12345.0f, 150000.0f, -6789.0f ), glm::normalize( toSun ) );

        const glm::vec4 originClip = view.WorldToMap * glm::vec4( 0.0f, 0.0f, 0.0f, 1.0f );
        const float     half       = static_cast<float>( kCloudShadowMapResolution ) * 0.5f;

        for ( int axis = 0; axis < 2; ++axis )
        {
            const float lattice = originClip[axis] * half;
            EXPECT_NEAR( lattice, std::floor( lattice + 0.5f ), 1e-3f )
                 << "sun elevation " << glm::degrees( elevation ) << " deg, axis " << axis;
        }
    }
}

// ---------------------------------------------------------------------------------------------------
// The map's bounds and its depth axis — the arithmetic the consumer performs on every lit pixel
// ---------------------------------------------------------------------------------------------------

// THE FAR DEPTH THE PROJECTION SPANS AND THE FAR DEPTH THE ENCODING ASSUMES ARE ONE NUMBER, and since
// this task they are written in two places: CloudShadowFarDepthKm in the shared header, and the
// `4 x extent` the orthographic matrix is built from in CloudBuildShadowMapView. They used to be one
// place because the producer compiled the header's constant; now the producer is HANDED the projection's
// own answer on a push constant, which is what lets a quality tier scale the extent at all. This is the
// assertion that keeps the two honest — over every extent a tier can produce, not at the one value that
// happens to be compiled in. Get it wrong and every front depth in the map is off by the ratio, which
// reads as a shadow of the wrong strength rather than as a scale.
TEST( CloudShadowMap, TheProjectionsFarDepthIsTheOneTheEncodingAssumes )
{
    for ( int i = 5; i <= 60; ++i )
    {
        const float                               extentKm = static_cast<float>( i );
        const Desert::Graphic::CloudShadowMapView view =
             Desert::Graphic::CloudBuildShadowMapView( glm::vec3( 1234.0f, 500.0f, -987.0f ), kSun, kPlanetKm,
                                                       extentKm, Desert::Graphic::kCloudShadowSnapKm, 512.0f );

        EXPECT_NEAR( view.FarDepthKm, CloudShadowFarDepthKm( extentKm ), 1e-3f * extentKm )
             << "at an extent of " << extentKm
             << " km the matrix spans a different depth from the one the texel encoding assumes";
    }
}

TEST( CloudShadowMap, TheProjectionCoversExactlyTheExtentAndInvertsExactly )
{
    const CloudShadowMapView view = BuildView( glm::vec3( 0.0f, 20000.0f, 0.0f ), kSun );

    // The anchor sits at the centre of the map, at the light distance from the near plane — which is half
    // the far plane, so exactly the middle of the depth axis. That number is the one the consumer scales
    // by, and it is the one that would be wrong if the projection's near/far were ever swapped.
    const glm::vec4 anchorClip = view.WorldToMap * ( view.MapToWorld * glm::vec4( 0.0f, 0.0f, 0.5f, 1.0f ) );
    EXPECT_NEAR( anchorClip.x, 0.0f, 1e-4f );
    EXPECT_NEAR( anchorClip.y, 0.0f, 1e-4f );
    EXPECT_NEAR( anchorClip.z, 0.5f, 1e-4f );

    EXPECT_NEAR( view.FarDepthKm, 4.0f * kCloudShadowExtentKm, 1e-3f );
    EXPECT_NEAR( CloudShadowDepthKm( 0.5f, view.FarDepthKm ), 2.0f * kCloudShadowExtentKm, 1e-3f );

    // A round trip through both matrices returns the point it started at, to well inside a texel.
    const glm::vec3 probe( 123456.0f, -1000.0f, -654321.0f );
    const glm::vec4 clip = view.WorldToMap * glm::vec4( probe, 1.0f );
    const glm::vec4 back = view.MapToWorld * clip;
    EXPECT_NEAR( back.x / back.w, probe.x, 10.0f );
    EXPECT_NEAR( back.y / back.w, probe.y, 10.0f );
    EXPECT_NEAR( back.z / back.w, probe.z, 10.0f );
}

// The border, and the fade that keeps it from being a straight line of light across a terrain.
TEST( CloudShadowMap, TheBorderFadeReachesBothEndsAndOnlyAtTheBorder )
{
    const float fadeUv = CloudShadowBorderFadeUv( CLOUD_SHADOWMAP_EXTENT_KM );

    EXPECT_FLOAT_EQ( CloudShadowBorderFade( vec2( 0.5f, 0.5f ), fadeUv ), 1.0f );
    EXPECT_FLOAT_EQ( CloudShadowBorderFade( vec2( 0.0f, 0.5f ), fadeUv ), 0.0f );
    EXPECT_FLOAT_EQ( CloudShadowBorderFade( vec2( 1.0f, 0.5f ), fadeUv ), 0.0f );
    EXPECT_FLOAT_EQ( CloudShadowBorderFade( vec2( 0.5f, -0.3f ), fadeUv ), 0.0f );

    // Monotone across the band, so the fade is a gradient rather than a second edge.
    float previous = -1.0f;
    for ( int i = 0; i <= 40; ++i )
    {
        const float u    = fadeUv * ( i / 40.0f );
        const float fade = CloudShadowBorderFade( vec2( u, 0.5f ), fadeUv );
        EXPECT_GE( fade, previous );
        previous = fade;
    }

    // AND THE FADE IS STILL 0.02 OF UV AT THE REFERENCE TIER, to the digit it was written as before it
    // became a world width. This is the assertion that says the change of statement was not a change of
    // picture: 1.2 km over a 60 km side is exactly the constant this used to be.
    EXPECT_FLOAT_EQ( fadeUv, 0.02f );
}

// The map's clip space and the UV the producer stores through are one mapping, written once. A texel
// centre must round-trip to its own index — half a texel of disagreement between the store and the fetch
// is a shadow that does not sit under its cloud.
TEST( CloudShadowMap, ClipAndUvAreOneMapping )
{
    const int resolution = static_cast<int>( CLOUD_SHADOWMAP_RESOLUTION );
    for ( int i : { 0, 1, 37, resolution / 2, resolution - 2, resolution - 1 } )
    {
        const float uvIn  = ( static_cast<float>( i ) + 0.5f ) / static_cast<float>( resolution );
        const float clip  = uvIn * 2.0f - 1.0f;
        const vec2  uvOut = CloudShadowClipToUv( vec2( clip, clip ) );

        EXPECT_NEAR( uvOut.x, uvIn, 1e-6f );
        EXPECT_EQ( static_cast<int>( uvOut.x * static_cast<float>( resolution ) ), i );
    }
}

// ---------------------------------------------------------------------------------------------------
// PAIR 5: THE TWO RENDER PATHS. Every surface the sun lights reads ONE map through ONE text.
//
// The defect this was written for, and it is the same shape as the four above. `CloudShadowFactor` was
// declared inside Programs/Deferred/DeferredLighting.shader and nowhere else in the tree, so the ONLY
// surfaces in the engine that received a cloud shadow were the ones a deferred composite happened to
// shade. Everything drawn forward — every scene on RenderingPath 0, and in a deferred scene the skinned
// meshes, the glass and the terrain, all of which are drawn forward OVER the composite — stood in full
// sun under a deck that was visibly shading the ground beside them. Measured on the ground of
// Clouds_Showcase from 0,200,0 with clouds on: 64.93/255 deferred against 109.11/255 forward, a factor
// of 1.68, and the whole of what remained between the paths after the ambient (Р16) and the direct-light
// BRDF (Р20) were made one text each.
//
// A numeric comparison here would be theatre — it would call one C++ function twice and present the
// agreement as evidence. What can actually regress is a shader growing its own copy of the wrapper, or a
// material packing the uniform block itself, and both are facts about SOURCES. So that is what these
// read.
// ---------------------------------------------------------------------------------------------------

namespace
{
    std::filesystem::path RepositoryRoot()
    {
        // The test binary lives in build/Bin/Tests/<config>; walk up to the repository root.
        std::filesystem::path root = std::filesystem::current_path();
        for ( int up = 0; up < 8 && !std::filesystem::exists( root / "Editor" / "Resources" / "Shaders" ); ++up )
            root = root.parent_path();
        return root;
    }

    std::string ReadFile( const std::filesystem::path& file )
    {
        std::ifstream      in( file, std::ios::binary );
        std::ostringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }

    // The same text with its `//` comments removed, so the notes that QUOTE a deleted form (this codebase
    // records its defects at the site) are not read as code.
    std::string StripLineComments( const std::string& source )
    {
        std::istringstream in( source );
        std::ostringstream out;
        std::string        line;
        while ( std::getline( in, line ) )
        {
            const std::size_t comment = line.find( "//" );
            if ( comment != std::string::npos )
                line.erase( comment );
            out << line << '\n';
        }
        return out.str();
    }
} // namespace

TEST( CloudShadowReceiver, EverySunLitShaderReachesTheOneSharedFactor )
{
    const std::filesystem::path root = RepositoryRoot();
    ASSERT_TRUE( std::filesystem::exists( root / "Editor" / "Resources" / "Shaders" ) )
         << "could not find Editor/Resources/Shaders above " << std::filesystem::current_path();

    const std::filesystem::path shaders = root / "Editor" / "Resources" / "Shaders";

    // Every shader in the tree that shades a surface with the DIRECTIONAL light, and therefore every
    // shader a cloud must be able to stand in front of. The G-buffer pass is here for a different reason
    // and is asserted separately below.
    const char* kConsumers[] = {
         "Programs/Deferred/DeferredLighting.shader",   // the deferred composite
         "Programs/PBR/StaticMeshPBR.shader",           // the forward sun
         "Programs/PBR/StaticMeshPBR_Instanced.shader", // foliage / instanced statics
         "Programs/PBR/SkinnedMeshPBR.shader",          // drawn FORWARD even in Deferred
         "Programs/PBR/StaticMeshGlass.shader",         // drawn FORWARD over the composite
         "Programs/Terrain/Terrain.shader",             // drawn by neither mesh path
         // Programs/Grass/Grass.shader was the seventh row until Г25. It shaded the PROCEDURAL grass
         // blades, which had to take the same cloud shadow as the ground beneath them or a field became
         // bright fuzz over dark soil. The generator is gone - grass is a mesh asset now, so it is drawn
         // by StaticMeshPBR_Instanced, which is already the third row above and already carries the
         // receiver. The row is removed rather than kept pointing at a file: this census ASSERTS each
         // path exists, so a stale row is a red suite and not a silent gap.
    };

    for ( const char* relative : kConsumers )
    {
        const std::filesystem::path file = shaders / relative;
        ASSERT_TRUE( std::filesystem::exists( file ) ) << file.string();

        const std::string source = ReadFile( file );

        EXPECT_NE( source.find( "Common/CloudShadowReceiver.glslh" ), std::string::npos )
             << relative << " does not compile the shared cloud-shadow receiver";
        EXPECT_NE( StripLineComments( source ).find( "CloudShadowFactor(" ), std::string::npos )
             << relative << " does not attenuate its sun by CloudShadowFactor(worldPos)";
        // The receiver names three symbols the includer owes it. A shader that includes the text without
        // declaring them does not link, but it fails at cook time on one machine and this fails here.
        for ( const char* symbol : { "u_CloudShadowMap", "u_CloudShadowWorldToMap", "u_CloudShadowParams" } )
            EXPECT_NE( source.find( symbol ), std::string::npos )
                 << relative << " includes the receiver without declaring " << symbol;
    }
}

TEST( CloudShadowReceiver, NoShaderReconstructsTheShadowForItself )
{
    // The construction stated as the thing that must stay true: the reconstruction calls
    // (CloudShadowTransmittance / CloudShadowBorderFade / CloudShadowDepthKm / CloudShadowClipToUv)
    // appear in exactly TWO files of code in the whole shader tree — the receiver that consumes the map
    // and the producer that fills it — and nowhere else. One shader assembling them itself is how this
    // subsystem came to have a single consumer in the first place.
    const std::filesystem::path shaders = RepositoryRoot() / "Editor" / "Resources" / "Shaders";
    ASSERT_TRUE( std::filesystem::exists( shaders ) );

    int receivers = 0;
    for ( const auto& entry : std::filesystem::recursive_directory_iterator( shaders ) )
    {
        if ( !entry.is_regular_file() )
            continue;
        const std::string ext = entry.path().extension().string();
        if ( ext != ".shader" && ext != ".glslh" )
            continue;

        const std::string name = entry.path().filename().string();
        // The receiver IS the reconstruction's reader; CloudShadowMap.glslh defines it; the producer
        // marches it. Everyone else must go through the receiver.
        if ( name == "CloudShadowReceiver.glslh" )
        {
            receivers++;
            continue;
        }
        if ( name == "CloudShadowMap.glslh" || name == "CloudShadowMap.shader" )
            continue;

        const std::string code = StripLineComments( ReadFile( entry.path() ) );
        EXPECT_EQ( code.find( "CloudShadowTransmittance(" ), std::string::npos )
             << name << " reconstructs the cloud shadow itself instead of calling CloudShadowFactor";
        EXPECT_EQ( code.find( "CloudShadowBorderFade(" ), std::string::npos )
             << name << " applies the border fade itself instead of calling CloudShadowFactor";
    }

    EXPECT_EQ( receivers, 1 ) << "Common/CloudShadowReceiver.glslh was not found under the shader root";
}

TEST( CloudShadowReceiver, NoMaterialPacksTheUniformBlockForItself )
{
    // The C++ half of the same relation. `CloudShadowUniforms` is filled in exactly ONE place —
    // CloudShadowPackUniforms, beside the struct — and reaches a descriptor set through exactly one
    // writer, CloudShadowBind. A material that assembles the block itself is free to decide differently
    // what `Params.y` means, and the symptom is one render path shading with a shadow the other has
    // switched off.
    const std::filesystem::path engine = RepositoryRoot() / "Desert" / "Desert" / "Source";
    ASSERT_TRUE( std::filesystem::exists( engine ) ) << engine.string();

    int declarations = 0;
    int writers      = 0;
    for ( const auto& entry : std::filesystem::recursive_directory_iterator( engine ) )
    {
        if ( !entry.is_regular_file() )
            continue;
        const std::string ext = entry.path().extension().string();
        if ( ext != ".cpp" && ext != ".hpp" )
            continue;

        const std::string name = entry.path().filename().string();
        // The struct and the function that fills it.
        if ( name == "CloudShadowPayload.hpp" )
        {
            declarations++;
            continue;
        }
        // The one place a filled block reaches a descriptor set.
        if ( name == "CloudShadowBinding.hpp" )
        {
            writers++;
            continue;
        }

        const std::string code = StripLineComments( ReadFile( entry.path() ) );
        EXPECT_EQ( code.find( "CloudShadowUniforms" ), std::string::npos )
             << name << " builds the cloud-shadow uniform block itself; call CloudShadowBind instead";
    }

    EXPECT_EQ( declarations, 1 ) << "Engine/Graphic/Clouds/CloudShadowPayload.hpp was not found";
    EXPECT_EQ( writers, 1 ) << "Engine/Graphic/Clouds/CloudShadowBinding.hpp was not found";
}

// ---------------------------------------------------------------------------------------------------
// The packing itself: what the shader is told, from what the renderer gathered.
// ---------------------------------------------------------------------------------------------------

TEST( CloudShadowReceiver, TheShaderIsToldToFetchExactlyWhenThereIsSomethingToFetch )
{
    // `Params.y` is the one number Common/CloudShadowReceiver.glslh tests before it touches the sampler,
    // so it must be 1 in exactly the states where a map is really bound. FOUR ways to be off, and each
    // of them was a real state in this engine: no cloud component at all, the layer switched off, the
    // layer not casting, and the artist's strength at zero.
    Desert::Graphic::Image2D* const kMap =
         reinterpret_cast<Desert::Graphic::Image2D*>( static_cast<std::uintptr_t>( 0x1000 ) );

    Desert::Graphic::CloudShadowInput live;
    live.Map          = kMap;
    live.WorldToMap   = glm::mat4( 2.0f );
    live.FarDepthKm   = 120.0f;
    live.Strength     = 0.75f;
    live.BorderFadeUv = 0.02f;
    live.Enabled      = true;

    EXPECT_TRUE( live.IsLive() );
    EXPECT_FLOAT_EQ( Desert::Graphic::CloudShadowPackUniforms( live ).Params.y, 1.0f );

    // Every field the shader reads is the one it was handed — no re-derivation from a constant that no
    // longer describes this frame's map.
    const Desert::Graphic::CloudShadowUniforms packed = Desert::Graphic::CloudShadowPackUniforms( live );
    EXPECT_FLOAT_EQ( packed.Params.x, live.FarDepthKm );
    EXPECT_FLOAT_EQ( packed.Params.z, live.BorderFadeUv );
    EXPECT_FLOAT_EQ( packed.Params.w, live.Strength );
    EXPECT_EQ( packed.WorldToMap, live.WorldToMap );

    Desert::Graphic::CloudShadowInput off = live;
    off.Enabled                           = false;
    EXPECT_FALSE( off.IsLive() );
    EXPECT_FLOAT_EQ( Desert::Graphic::CloudShadowPackUniforms( off ).Params.y, 0.0f );

    off     = live;
    off.Map = nullptr;
    EXPECT_FALSE( off.IsLive() );
    EXPECT_FLOAT_EQ( Desert::Graphic::CloudShadowPackUniforms( off ).Params.y, 0.0f );

    off          = live;
    off.Strength = 0.0f;
    EXPECT_FALSE( off.IsLive() );
    EXPECT_FLOAT_EQ( Desert::Graphic::CloudShadowPackUniforms( off ).Params.y, 0.0f );

    // A default-constructed payload — what a scene with no sky at all produces — is off, and its matrix
    // is the identity rather than uninitialised memory.
    const Desert::Graphic::CloudShadowInput none;
    EXPECT_FALSE( none.IsLive() );
    EXPECT_FLOAT_EQ( Desert::Graphic::CloudShadowPackUniforms( none ).Params.y, 0.0f );
    EXPECT_EQ( Desert::Graphic::CloudShadowPackUniforms( none ).WorldToMap, glm::mat4( 1.0f ) );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
