// The scattering maths of the cloud march, tested against the physics it claims to implement.
//
// Common/CloudLighting.glslh is compiled here AS C++ (CloudLightingReference.hpp), so every assertion is
// about the text the march compiles. Each test is a RELATION rather than a spot value, because a spot
// value of a phase function is a number nobody can check by eye:
//
//   * THE PHASE FUNCTION INTEGRATES TO ONE over the sphere. That is what makes it a redistribution of
//     light rather than a gain applied to it, and it is the property a hand-tuned normalization breaks
//     while still looking plausible in a frame.
//   * BEER'S LAW IS MULTIPLICATIVE across a split. The march multiplies transmittance step by step, so if
//     the function were not multiplicative the answer would depend on the step schedule.
//   * THE SCATTER INTEGRAL IS THE CLOSED FORM, pinned against a numeric integration of the same step —
//     the way Desert/Tests/Engine/HeightFog pins its own closed form against a Riemann sum — and, the
//     property that motivates it at all, INVARIANT to how the distance is subdivided. A naive `S * dt`
//     passes every check that looks at one step and fails this one, and the symptom would be that a cloud
//     changes brightness with distance and with the quality tier.
//   * THE AMBIENT OCCLUSION IS REMOVABLE BY ITS STRENGTH and monotone in the profile.

#include "CloudLightingReference.hpp"

#include <gtest/gtest.h>

// For the PROCEDURAL MODELLING VOLUME'S extents only, so that "half of it on every axis" is a relation
// this suite checks rather than a sentence in a comment. Constants only — nothing here is called, so the
// test still links against nothing.
#include <Engine/Assets/CloudProceduralVolume.hpp>

// For the shipped multiple-scattering defaults and the octave ceiling, so the reference below measures
// the series the artist actually gets rather than a copy of its numbers that can drift from it.
#include <Engine/Graphic/Clouds/CloudMaterialValues.hpp>
#include <Engine/ECS/VolumetricCloudComponent.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <vector>

using namespace Desert::Tests::CloudLightingRef;

namespace
{
    // 2*pi * integral over mu in [-1, 1], by the midpoint rule. The phase depends on the direction only
    // through cos(theta), so the azimuth contributes its 2*pi analytically and the quadrature is
    // one-dimensional. Half a million samples puts the rule's error orders below the tolerances used
    // against it, even at the sharpest g tested here.
    double IntegratePhaseOverTheSphere( float g, int samples )
    {
        const double dmu = 2.0 / samples;

        double sum = 0.0;
        for ( int i = 0; i < samples; ++i )
        {
            const double mu = -1.0 + ( static_cast<double>( i ) + 0.5 ) * dmu;
            sum += static_cast<double>( CloudPhaseHG( static_cast<float>( mu ), g ) ) * dmu;
        }

        return sum * 2.0 * static_cast<double>( CLOUD_PI );
    }

    // The integral the closed form claims to be: in-scattered radiance S attenuated by the medium it is
    // travelling through, across one step. Midpoint rule, 4096 steps.
    double NumericScatterIntegral( double scattering, double extinctionPerKm, double stepKm )
    {
        constexpr int n = 4096;

        const double h   = stepKm / n;
        double       sum = 0.0;
        for ( int i = 0; i < n; ++i )
        {
            const double s = ( static_cast<double>( i ) + 0.5 ) * h;
            sum += scattering * std::exp( -extinctionPerKm * s ) * h;
        }
        return sum;
    }

    // The relative tolerance a float32 evaluation of (S - S*T)/sigma deserves at a given optical depth.
    //
    // The subtraction is a CANCELLATION: at sigma*dt = 5e-5 the two operands agree to four digits, so the
    // difference keeps only what is left of float32's seven — roughly eps/(sigma*dt) of relative error,
    // which is 0.2 per cent there and negligible by sigma*dt = 0.1. Writing the tolerance as the formula
    // rather than as one loose number keeps the test tight where the arithmetic is accurate, which is
    // where a wrong closed form would hide.
    double ScatterTolerance( double opticalDepth )
    {
        return 1e-4 + 3e-7 / std::max( opticalDepth, 1e-9 );
    }
} // namespace

// ---------------------------------------------------------------------------------------------------
// CloudPhaseHG
// ---------------------------------------------------------------------------------------------------

TEST( CloudLightingPhase, ItIntegratesToOneOverTheSphereAtEveryAsymmetryTheComponentAllows )
{
    // THE DEFINING PROPERTY, and the one that says the function redistributes light rather than creating
    // it. The component's Phase G range is -0.9 to 0.9; the sampled set spans it.
    for ( const float g : { -0.8f, -0.6f, -0.3f, 0.0f, 0.3f, 0.6f, 0.8f } )
    {
        const double integral = IntegratePhaseOverTheSphere( g, 500000 );
        EXPECT_NEAR( integral, 1.0, 2e-3 ) << "g = " << g << " integrates to " << integral;
    }
}

TEST( CloudLightingPhase, AtZeroAsymmetryItIsExactlyTheIsotropicPhaseTheOctavesBlendToward )
{
    // The two are used together — the multiple-scattering octaves mix from this phase toward
    // CLOUD_ISOTROPIC_PHASE — so they have to agree at g = 0 or the first octave would step.
    for ( const float cosTheta : { -1.0f, -0.5f, 0.0f, 0.37f, 1.0f } )
        EXPECT_NEAR( CloudPhaseHG( cosTheta, 0.0f ), CLOUD_ISOTROPIC_PHASE, 1e-8f ) << "cos = " << cosTheta;
}

TEST( CloudLightingPhase, PositiveAsymmetryScattersFORWARDAndNegativeBackward )
{
    // The sign convention, which is what puts the bright rim on the cloud you look at through the sun.
    // Asserted as monotonicity across the whole range rather than at the two ends, because a function
    // that is merely brighter at cos = 1 could still be non-monotone in between.
    for ( const float g : { 0.2f, 0.6f, 0.9f } )
    {
        float previous = -1.0f;
        for ( int step = 0; step <= 200; ++step )
        {
            const float cosTheta = -1.0f + 0.01f * static_cast<float>( step );
            const float phase    = CloudPhaseHG( cosTheta, g );
            EXPECT_GE( phase, previous ) << "g = " << g << ", cos = " << cosTheta << " went DOWN";
            previous = phase;
        }

        // The forward/backward ratio of Henyey-Greenstein is ((1+g)/(1-g))^3 exactly, which is 3.4 at
        // g = 0.2 and 64 at g = 0.6. Asserted against that closed form rather than against a round
        // number, so the test says how strongly it forward-scatters and not merely that it does.
        const float ratio = std::pow( ( 1.0f + g ) / ( 1.0f - g ), 3.0f );
        EXPECT_NEAR( CloudPhaseHG( 1.0f, g ) / CloudPhaseHG( -1.0f, g ), ratio, ratio * 1e-3f ) << "g = " << g;

        EXPECT_LT( CloudPhaseHG( 1.0f, -g ), CloudPhaseHG( -1.0f, -g ) ) << "g = " << -g;
    }
}

TEST( CloudLightingPhase, ItStaysFiniteAndPositiveAtTheOneAlignmentThatWouldDivideByZero )
{
    // g -> 1 with the view, the sun and the sample in a line drives the denominator to zero, and the
    // unguarded pow(0, 1.5) is an infinity that propagates through the integral and lands as a
    // permanently white pixel. It needs one exact alignment, so no sampled frame is evidence against it.
    for ( const float g : { 0.9f, 0.95f, 0.999f, 1.0f, 2.0f, -1.0f, -2.0f } )
    {
        for ( const float cosTheta : { -1.0f, -0.999f, 0.0f, 0.999f, 1.0f } )
        {
            const float phase = CloudPhaseHG( cosTheta, g );
            EXPECT_TRUE( std::isfinite( phase ) ) << "g = " << g << ", cos = " << cosTheta;
            EXPECT_GT( phase, 0.0f ) << "g = " << g << ", cos = " << cosTheta;
        }
    }
}

// ---------------------------------------------------------------------------------------------------
// CloudBeerTransmittance
// ---------------------------------------------------------------------------------------------------

TEST( CloudLightingBeer, OneAtZeroDistanceMonotoneDecreasingAndEqualToTheExponential )
{
    constexpr float kSigma = 8.0f; // the component's Extinction Scale default, per kilometre

    EXPECT_FLOAT_EQ( CloudBeerTransmittance( kSigma, 0.0f ), 1.0f );

    float previous = 1.0f;
    for ( int step = 0; step <= 200; ++step )
    {
        const float distanceKm = 0.01f * static_cast<float>( step );
        const float actual     = CloudBeerTransmittance( kSigma, distanceKm );

        EXPECT_NEAR( actual, std::exp( -kSigma * distanceKm ), 1e-6f ) << "d = " << distanceKm;
        EXPECT_LE( actual, previous ) << "d = " << distanceKm << " rose";
        EXPECT_GE( actual, 0.0f );
        EXPECT_LE( actual, 1.0f );
        previous = actual;
    }
}

TEST( CloudLightingBeer, ItIsMultiplicativeAcrossASplitWhichIsWhatMakesTheMarchStepwise )
{
    // The march multiplies the running transmittance by one step's worth at a time. If the function were
    // not multiplicative the total would depend on the step schedule, which is to say on the distance
    // from the camera — the same class of defect the scatter integral below is written to avoid.
    constexpr float kSigma = 8.0f;

    for ( const float total : { 0.05f, 0.4f, 1.7f } )
    {
        float product = 1.0f;
        for ( int i = 0; i < 10; ++i )
            product *= CloudBeerTransmittance( kSigma, total / 10.0f );

        EXPECT_NEAR( product, CloudBeerTransmittance( kSigma, total ), 1e-5f ) << "total = " << total;
    }
}

TEST( CloudLightingBeer, NegativeInputsAreClampedRatherThanAmplifying )
{
    // A negative distance is what a step schedule produces the moment a segment comes back inverted, and
    // exp of a positive product is a transmittance ABOVE one — light created out of nothing, which reads
    // as a blown-out band rather than as an error.
    EXPECT_FLOAT_EQ( CloudBeerTransmittance( 8.0f, -1.0f ), 1.0f );
    EXPECT_FLOAT_EQ( CloudBeerTransmittance( -8.0f, 1.0f ), 1.0f );
}

// ---------------------------------------------------------------------------------------------------
// CloudScatterIntegral — the closed form, and the reason it is not `S * dt`
// ---------------------------------------------------------------------------------------------------

TEST( CloudLightingScatter, TheClosedFormMatchesANumericIntegrationOfTheSameStep )
{
    // The pin, in the shape HeightFog uses for its own closed form: the analytic expression against a
    // brute-force integration of the medium it claims to integrate. A lost factor, a sign, or an
    // extinction applied once too often breaks this and nothing else.
    for ( const float sigma : { 0.05f, 1.0f, 8.0f, 45.0f } )
    {
        for ( const float stepKm : { 0.001f, 0.02f, 0.25f, 2.0f } )
        {
            const vec3 scattering( 0.7f, 1.3f, 2.9f );
            const vec3 actual = CloudScatterIntegral( scattering, sigma, stepKm );

            const double tolerance = ScatterTolerance( static_cast<double>( sigma ) * stepKm );

            for ( int channel = 0; channel < 3; ++channel )
            {
                const double expected = NumericScatterIntegral( scattering[channel], sigma, stepKm );
                EXPECT_NEAR( actual[channel], expected, expected * tolerance )
                     << "sigma " << sigma << ", step " << stepKm << ", channel " << channel;
            }
        }
    }
}

TEST( CloudLightingScatter, TheSameDistanceInOneStepAndInTenGivesTheSameRadiance )
{
    // THE PROPERTY THAT MOTIVATES THE CLOSED FORM. The march accumulates
    // `luminance += transmittance * CloudScatterIntegral(...)` and then attenuates the running
    // transmittance, so subdividing a step must not change the total. If it does, a cloud is brighter at
    // one quality tier than another and brighter near the camera than far from it — two tiers that
    // disagree about how bright a cloud is, which is the same defect as two shaders that disagree about
    // a constant and just as invisible in a unit test of either alone.
    for ( const float sigma : { 0.05f, 1.0f, 8.0f, 45.0f } )
    {
        for ( const float totalKm : { 0.01f, 0.2f, 1.5f } )
        {
            const vec3 scattering( 1.0f, 1.0f, 1.0f );

            const vec3 oneStep = CloudScatterIntegral( scattering, sigma, totalKm );

            vec3  tenSteps( 0.0f );
            float transmittance = 1.0f;
            for ( int i = 0; i < 10; ++i )
            {
                tenSteps += transmittance * CloudScatterIntegral( scattering, sigma, totalKm / 10.0f );
                transmittance *= CloudBeerTransmittance( sigma, totalKm / 10.0f );
            }

            const double opticalDepth = static_cast<double>( sigma ) * totalKm;
            const double tolerance    = ScatterTolerance( opticalDepth / 10.0 );

            EXPECT_NEAR( tenSteps.x, oneStep.x, oneStep.x * tolerance )
                 << "sigma " << sigma << ", total " << totalKm;

            // The naive product is what the tolerance above has to discriminate against, so the margin is
            // MEASURED rather than assumed. It only discriminates once the segment is optically thick
            // enough for the attenuation across it to matter — at an optical depth of 0.0005 the two
            // forms genuinely agree, and asserting otherwise would be asserting noise.
            if ( opticalDepth < 0.1 )
                continue;

            float naiveTransmittance = 1.0f;
            float naiveTen           = 0.0f;
            for ( int i = 0; i < 10; ++i )
            {
                naiveTen += naiveTransmittance * scattering.x * ( totalKm / 10.0f );
                naiveTransmittance *= CloudBeerTransmittance( sigma, totalKm / 10.0f );
            }
            const float naiveOne = scattering.x * totalKm;
            EXPECT_GT( std::abs( naiveOne - naiveTen ), oneStep.x * tolerance * 100.0 )
                 << "sigma " << sigma << ", total " << totalKm
                 << ": the naive form is indistinguishable here, so this configuration proves nothing";
        }
    }
}

TEST( CloudLightingScatter, TheZeroExtinctionCaseIsSAFEEvenThoughTheFloorDoesNotReachTheDocumentedLimit )
{
    // The header says the floor of 1e-6 "reproduces [the S*dt limit] to well inside half-float precision
    // while keeping the function branchless". THE FIRST HALF OF THAT IS NOT TRUE IN FLOAT32 and the
    // numbers printed below say so: with sigma floored to 1e-6, any step shorter than about a kilometre
    // makes sigma*dt smaller than float32's resolution at 1, so exp(-sigma*dt) rounds to EXACTLY one, the
    // numerator cancels to zero, and the function returns 0 rather than S*dt.
    //
    // It is harmless where it is called, which is why this is a documented measurement and not a red
    // test: at the call site `scattering` is itself `inScatter * albedo * sigma_t * scatterFactor`, so a
    // zero extinction makes the numerator zero on its own and the correct answer IS zero. What the test
    // therefore asserts is the safety envelope — finite, non-negative, never more than the unattenuated
    // product — which is what the march actually depends on.
    const vec3 scattering( 2.0f, 2.0f, 2.0f );

    for ( const float stepKm : { 0.01f, 0.5f, 3.0f } )
    {
        const vec3 actual = CloudScatterIntegral( scattering, 0.0f, stepKm );

        std::printf( "[CloudLighting] sigma 0, step %.2f km: integral %.6g, S*dt %.6g\n", stepKm, actual.x,
                     scattering.x * stepKm );

        EXPECT_TRUE( std::isfinite( actual.x ) ) << "step " << stepKm;
        EXPECT_GE( actual.x, 0.0f ) << "step " << stepKm;
        EXPECT_LE( actual.x, scattering.x * stepKm * ( 1.0f + 1e-4f ) ) << "step " << stepKm;
    }

    // Where the optical depth IS representable the limit holds, which is what says the closed form is the
    // right expression and only its floor is optimistic.
    for ( const float stepKm : { 0.05f, 0.5f } )
    {
        constexpr float kSmallSigma = 1e-3f;
        const vec3      actual      = CloudScatterIntegral( scattering, kSmallSigma, stepKm );
        EXPECT_NEAR( actual.x, scattering.x * stepKm, scattering.x * stepKm * 1e-2f ) << "step " << stepKm;
    }
}

TEST( CloudLightingScatter, ItNeverExceedsTheUnattenuatedProductAndRisesWithTheStep )
{
    // Two bounds the march relies on without stating them: one step can never in-scatter more than the
    // medium would without any absorption at all, and a longer step through the same medium can never
    // return less.
    const vec3 scattering( 1.0f, 1.0f, 1.0f );

    for ( const float sigma : { 0.5f, 8.0f, 45.0f } )
    {
        float previous = -1.0f;
        for ( int step = 0; step <= 100; ++step )
        {
            const float stepKm = 0.02f * static_cast<float>( step );
            const float value  = CloudScatterIntegral( scattering, sigma, stepKm ).x;

            EXPECT_LE( value, scattering.x * stepKm + 1e-6f ) << "sigma " << sigma << ", step " << stepKm;
            EXPECT_GE( value, previous ) << "sigma " << sigma << ", step " << stepKm << " went DOWN";
            EXPECT_TRUE( std::isfinite( value ) );
            previous = value;
        }
    }

    // A negative step is one inverted segment away and must not scatter light backwards into the frame.
    EXPECT_FLOAT_EQ( CloudScatterIntegral( scattering, 8.0f, -1.0f ).x, 0.0f );
}

// ---------------------------------------------------------------------------------------------------
// CloudAmbientOcclusion
// ---------------------------------------------------------------------------------------------------

TEST( CloudLightingAmbient, AtZeroStrengthItIsExactlyOneWhateverTheProfile )
{
    // The term has to be removable, or an artist cannot tell what it is doing. At strength 0 it must be
    // the identity for every profile, including the ends where the pow is evaluated at 0 and 1.
    for ( int step = 0; step <= 100; ++step )
    {
        const float profile = 0.01f * static_cast<float>( step );
        EXPECT_FLOAT_EQ( CloudAmbientOcclusion( profile, 0.0f ), 1.0f ) << "profile " << profile;
    }
}

TEST( CloudLightingAmbient, ItDecreasesWithDepthInsideTheBodyAndIsTheDecksSquareRoot )
{
    // Deck p.144: pow(1 - dimensional_profile, 0.5). The deeper inside the body a sample is, the less of
    // the sky's light reaches it — without this the core of a three-kilometre cumulus is lit exactly as
    // brightly as a wisp on its edge, and the cloud reads as a flat white cut-out.
    float previous = 2.0f;
    for ( int step = 0; step <= 100; ++step )
    {
        const float profile   = 0.01f * static_cast<float>( step );
        const float occlusion = CloudAmbientOcclusion( profile, 1.0f );

        EXPECT_NEAR( occlusion, std::sqrt( 1.0f - profile ), 1e-5f ) << "profile " << profile;
        EXPECT_LE( occlusion, previous ) << "profile " << profile << " rose";
        previous = occlusion;
    }

    EXPECT_FLOAT_EQ( CloudAmbientOcclusion( 0.0f, 1.0f ), 1.0f ) << "at the surface nothing is occluded";
    EXPECT_FLOAT_EQ( CloudAmbientOcclusion( 1.0f, 1.0f ), 0.0f ) << "in the core nothing gets through";
}

TEST( CloudLightingAmbient, TheStrengthBlendsBetweenNoOcclusionAndTheFullTermAndBothInputsAreClamped )
{
    for ( const float profile : { 0.1f, 0.5f, 0.9f } )
    {
        const float full = CloudAmbientOcclusion( profile, 1.0f );
        const float half = CloudAmbientOcclusion( profile, 0.5f );

        EXPECT_NEAR( half, 0.5f * ( 1.0f + full ), 1e-5f ) << "profile " << profile;
        EXPECT_GT( half, full );
        EXPECT_LT( half, 1.0f );
    }

    // Out-of-range inputs are one hand-edited scene away, and pow of a negative base is a NaN that
    // renders as a black hole in the layer rather than as an error.
    for ( const float profile : { -1.0f, 2.0f } )
        for ( const float strength : { -1.0f, 2.0f } )
            EXPECT_TRUE( std::isfinite( CloudAmbientOcclusion( profile, strength ) ) )
                 << "profile " << profile << ", strength " << strength;

    EXPECT_FLOAT_EQ( CloudAmbientOcclusion( -1.0f, 1.0f ), 1.0f );
    EXPECT_FLOAT_EQ( CloudAmbientOcclusion( 2.0f, 1.0f ), 0.0f );
}

// ---------------------------------------------------------------------------------------------------
// THE SKY-LIGHT OCCLUSION VOLUME — the other occluder, and the relations that make it mean anything
// ---------------------------------------------------------------------------------------------------

namespace
{
    // 2 * E3(tau) = 2 * integral(0..1) mu * exp(-tau / mu) dmu — the EXACT cosine-weighted hemispherical
    // transmittance of a horizontally uniform slab of vertical optical depth `tau`. Midpoint rule; the
    // integrand is smooth on (0, 1] and vanishes at mu -> 0 faster than any power, so the quadrature
    // converges quickly and 200 000 samples is far more than the tolerances below need.
    //
    // THIS IS THE THING THE SHADER APPROXIMATES, and it is integrated here rather than quoted because the
    // whole point of the assertion is to measure the size of the approximation instead of asserting that
    // somebody's remembered constant equals itself.
    double IntegrateDiffuseTransmittance( double tau, int samples )
    {
        const double dmu = 1.0 / samples;

        double sum = 0.0;
        for ( int i = 0; i < samples; ++i )
        {
            const double mu = ( static_cast<double>( i ) + 0.5 ) * dmu;
            sum += mu * std::exp( -tau / mu ) * dmu;
        }
        return 2.0 * sum;
    }
} // namespace

TEST( CloudSkyOcclusion, TheHemisphereSeesMoreCloudThanTheVerticalDoes )
{
    // THE RELATION THE DIFFUSIVITY FACTOR EXISTS FOR, and the one that decides whether the constant is
    // above or below 1 at all. Every slant path through a slab is LONGER than the vertical one — tau/mu
    // with mu <= 1 — so the hemispherical transmittance is strictly below exp(-tau) for every tau > 0.
    // A factor of 1 (the naive "just use the column") would light the shaded side of every deck too
    // brightly, and a factor below 1 would be backwards.
    EXPECT_FLOAT_EQ( CloudSkyDiffuseTransmittance( 0.0f ), 1.0f ) << "no cloud above must occlude nothing";

    float previous = 2.0f;
    for ( int step = 1; step <= 200; ++step )
    {
        const float tau = 0.05f * static_cast<float>( step );
        const float t   = CloudSkyDiffuseTransmittance( tau );

        EXPECT_LT( t, std::exp( -tau ) ) << "tau " << tau << ": the hemisphere is not darker than the column";
        EXPECT_LT( t, previous ) << "tau " << tau << " rose";
        EXPECT_GE( t, 0.0f );
        EXPECT_LE( t, 1.0f );
        previous = t;
    }

    // A negative depth is one hand-edited scene away from a hand-edited extinction, and exp of a positive
    // argument is a transmittance above 1, which is a cloud that AMPLIFIES the sky.
    EXPECT_FLOAT_EQ( CloudSkyDiffuseTransmittance( -1.0f ), 1.0f );
}

TEST( CloudSkyOcclusion, TheClosedFormIsTheHemisphericalIntegralAndNotAFittedStandInForIt )
{
    // TWO IMPLEMENTATIONS OF ONE QUANTITY, asserted equal — the house pattern, and here it is what lets
    // the shader carry an identity rather than a constant somebody would later "correct" on one camera
    // angle. The left side is the definition, 2 * integral(0..1) mu exp(-tau/mu) dmu, integrated
    // numerically; the right is exp(-tau)(1-tau) + tau^2 E1(tau) as the march's own header evaluates it.
    //
    // THE PREVIOUS VERSION OF THIS TEST FAILED AND THE CODE CHANGED RATHER THAN THE TOLERANCE. It measured
    // the diffusivity factor — one slant path at a fixed secant of 1.66, radiative transfer's usual
    // shortcut, which is what the header carried first — at up to 26.9 % off over [0, 1.5], and worse, at
    // an error that CHANGES SIGN near tau 0.4. That is recorded here so nobody reinstates it as the
    // cheaper option: it is not cheaper anywhere it matters, because this runs once per texel of the
    // volume and not once per march sample.
    std::printf( "\n  vertical tau   2*E3(tau) exact   header's value   relative error\n" );

    double worst = 0.0;
    for ( int step = 0; step <= 60; ++step )
    {
        const double tau      = 0.1 * static_cast<double>( step );
        const double exact    = IntegrateDiffuseTransmittance( tau, 200000 );
        const double header   = CloudSkyDiffuseTransmittance( static_cast<float>( tau ) );
        const double relative = std::abs( header - exact ) / std::max( exact, 1e-9 );

        worst = std::max( worst, relative );

        if ( step % 10 == 0 )
            std::printf( "  %10.2f   %15.7f   %14.7f   %13.4f %%\n", tau, exact, header, 100.0 * relative );
    }

    std::printf( "  worst relative error over tau in [0, 6]: %.4f %%\n\n", 100.0 * worst );

    // A HALF PER CENT over the whole range, and it is dominated by the float cancellation in
    // exp(-tau)(1 - tau) + tau^2 E1(tau) at the far end where both terms are large and the answer is
    // tiny — where the value is also below what a half-float volume can store. Stated as a bound so that
    // a future edit to either side has to move it deliberately.
    EXPECT_LT( worst, 0.005 );
}

TEST( CloudSkyOcclusion, TheStrengthRemovesItExactlyAsItRemovesTheProfileTerm )
{
    // ONE KNOB, TWO GEOMETRIES. The component has a single AmbientOcclusionStrength and its flag chooses
    // which occluder it applies to, so the two must blend the same way — otherwise turning the volume on
    // would change what the strength MEANS as well as what it measures, and no scene could be compared
    // with itself across the flag.
    for ( const float transmittance : { 0.0f, 0.25f, 0.5f, 0.9f, 1.0f } )
    {
        // What the strength blends TOWARD is the sphere, not the hemisphere — see the relation test below.
        const float sphere = CLOUD_SKY_LOWER_HEMISPHERE + ( 1.0f - CLOUD_SKY_LOWER_HEMISPHERE ) * transmittance;

        EXPECT_FLOAT_EQ( CloudSkyOcclusion( transmittance, 0.0f ), 1.0f ) << "t " << transmittance;
        EXPECT_FLOAT_EQ( CloudSkyOcclusion( transmittance, 1.0f ), sphere ) << "t " << transmittance;
        EXPECT_NEAR( CloudSkyOcclusion( transmittance, 0.5f ), 0.5f * ( 1.0f + sphere ), 1e-6f );
    }

    // Both inputs clamped, for the reason the profile term's own test gives: an out-of-range value is one
    // hand-edited scene away, and an occlusion above 1 is a cloud that brightens the sky behind it.
    for ( const float t : { -1.0f, 2.0f } )
        for ( const float strength : { -1.0f, 2.0f } )
        {
            const float occlusion = CloudSkyOcclusion( t, strength );
            EXPECT_TRUE( std::isfinite( occlusion ) );
            EXPECT_GE( occlusion, 0.0f );
            EXPECT_LE( occlusion, 1.0f );
        }
}

TEST( CloudSkyOcclusion, AHemisphericalTransmittanceIsComposedIntoTheSphereTheAmbientIsAMeanOver )
{
    // THE TWO SIDES THAT MUST AGREE, and the one Р7 found disagreeing. CloudRaymarch.shader multiplies
    // this term into SKY_DISTANT_LIGHT_SPHERE_TEXEL — the mean radiance over the FULL SPHERE — while the
    // volume that feeds it integrates material ABOVE the sample only and CloudSkyDiffuseTransmittance
    // converts that into an UPPER-HEMISPHERE transmittance. Scaling one by the other asserts that cloud
    // overhead also blocks the open sky underneath, which is how the deck lost every cool contributor it
    // had and turned brown at full strength.
    //
    // Asserted as the composition rather than as a table of outputs: what is being pinned is that a
    // half-sphere quantity enters a full-sphere one through its own solid angle.
    for ( const float transmittance : { 0.0f, 1e-7f, 0.25f, 0.5f, 0.9f, 1.0f } )
    {
        const float lower = 1.0f; // nothing this volume knows about occludes it
        const float upper = transmittance;
        const float mean  = CLOUD_SKY_LOWER_HEMISPHERE * lower + ( 1.0f - CLOUD_SKY_LOWER_HEMISPHERE ) * upper;

        EXPECT_NEAR( CloudSkyOcclusion( transmittance, 1.0f ), mean, 1e-6f ) << "t " << transmittance;
    }

    // The hemispheres are equal solid angles of the sphere, so the split is one half exactly. A fitted
    // constant here would be the "second constant tuned on one camera angle" the header refuses.
    EXPECT_FLOAT_EQ( CLOUD_SKY_LOWER_HEMISPHERE, 0.5f );

    // THE FLOOR IS THE UNOCCLUDED HEMISPHERE AND NOT ZERO. This is the bound the previous form violated:
    // however much cloud is stacked overhead, a sample still sees half the sphere. Stated for the worst
    // case the shipped scene can reach — Clouds_Protocol's ExtinctionScale of 75/km over a 3.6 km deck
    // drives the stored transmittance to numerically zero (8/km already reached about 1e-7).
    for ( const float strength : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f } )
    {
        EXPECT_GE( CloudSkyOcclusion( 0.0f, strength ), CLOUD_SKY_LOWER_HEMISPHERE ) << "s " << strength;
        EXPECT_LE( CloudSkyOcclusion( 0.0f, strength ), 1.0f ) << "s " << strength;
    }

    // Clear sky above must occlude NOTHING at any strength, or the term would darken a cloud that has no
    // cloud over it — which is the one case the whole feature is supposed to leave alone.
    for ( const float strength : { 0.0f, 0.5f, 1.0f } )
        EXPECT_FLOAT_EQ( CloudSkyOcclusion( 1.0f, strength ), 1.0f ) << "s " << strength;

    // Monotone in both arguments: more cloud above is never more sky, and more strength is never less
    // occlusion. Catches an inversion that a spot value cannot.
    float previous = 2.0f;
    for ( int i = 0; i <= 20; ++i )
    {
        const float occlusion = CloudSkyOcclusion( static_cast<float>( i ) / 20.0f, 1.0f );
        EXPECT_GE( occlusion, previous == 2.0f ? 0.0f : previous );
        previous = occlusion;
    }

    previous = 2.0f;
    for ( int i = 0; i <= 20; ++i )
    {
        const float occlusion = CloudSkyOcclusion( 0.0f, static_cast<float>( i ) / 20.0f );
        EXPECT_LE( occlusion, previous );
        previous = occlusion;
    }
}

TEST( CloudSkyOcclusion, TheVolumeIsHalfTheModellingVolumeOnEveryAxisAndItsTexelStaysInsideTheLayer )
{
    // THE RELATION THAT SIZES THE VOLUME, asserted rather than left as two numbers in two files. What it
    // stores is a hemispherical integral whose own horizontal footprint is the height of the cloud above
    // the sample — kilometres — so a texel finer than the layer is thick would store structure the
    // quantity does not have, and one much coarser would lose the cloud-scale variation that IS the
    // point. Half the modelling volume on every axis lands comfortably between the two.
    EXPECT_FLOAT_EQ( CLOUD_SKY_OCCLUSION_RESOLUTION * 2.0f,
                     static_cast<float>( Desert::Assets::kCloudProceduralVolumeSide ) );
    EXPECT_FLOAT_EQ( CLOUD_SKY_OCCLUSION_RESOLUTION * 2.0f,
                     static_cast<float>( Desert::Assets::kCloudProceduralVolumeSide ) );
    EXPECT_FLOAT_EQ( CLOUD_SKY_OCCLUSION_SLICES * 2.0f,
                     static_cast<float>( Desert::Assets::kCloudProceduralVolumeHeight ) );

    // AND THE TEXEL IS BRACKETED BY TWO REAL QUANTITIES, which is what makes the halving a relation rather
    // than a convenience. The shipped region is 48 km across (Clouds_Protocol's RegionSize) over a 3.6 km
    // congestus deck.
    const float regionKm     = 48.0f;
    const float thicknessKm  = 3.6f;
    const float texelKm      = regionKm / CLOUD_SKY_OCCLUSION_RESOLUTION;
    const float fieldTexelKm = regionKm / static_cast<float>( Desert::Assets::kCloudProceduralVolumeSide );

    // ABOVE: the term's own horizontal footprint is the height of the cloud over the sample, and nothing
    // above the layer contributes at all — so a grid coarser than the deck is thick would average across
    // clouds that do not share a sky, and the shaded side would stop following the body casting it.
    EXPECT_LT( texelKm, thicknessKm ) << "a texel coarser than the deck is thick cannot resolve a cloud";

    // BELOW: the field this integrates has no structure finer than the volume it is fetched from, so a
    // grid finer than THAT stores interpolation and calls it detail — at four times the cost per octave.
    EXPECT_GE( texelKm, fieldTexelKm ) << "finer than the field it integrates is interpolation, not detail";
}

TEST( CloudSkyOcclusion, TheAddressingIsTheModellingVolumesOwnFrameAndItsTopSliceCannotWrapToItsBottom )
{
    // THE ADDRESSING IS ONE MAPPING USED IN TWO DIRECTIONS — the producer places its columns on this grid
    // and the march reads them back through this function — so what has to be true is that a point at the
    // region's minimum corner lands at uv 0 and one a full side away lands at uv 1, in the SAME frame the
    // profile fetch beside it uses (Common/CloudField.glslh, CloudProceduralLevelUvw).
    const vec2  originKm{ -24.0f, 7.0f };
    const float sideKm    = 48.0f;
    const float invSideKm = 1.0f / sideKm;

    const vec3 corner = CloudSkyOcclusionUvw( originKm, invSideKm, 0.5f, vec3( -24.0f, 1.0f, 7.0f ) );
    EXPECT_NEAR( corner.x, 0.0f, 1e-5f );
    EXPECT_NEAR( corner.z, 0.0f, 1e-5f );

    const vec3 farUvw = CloudSkyOcclusionUvw( originKm, invSideKm, 0.5f, vec3( 24.0f, 1.0f, 55.0f ) );
    EXPECT_NEAR( farUvw.x, 1.0f, 1e-5f );
    EXPECT_NEAR( farUvw.z, 1.0f, 1e-5f );

    // HORIZONTALLY UNCLAMPED AND IT MUST STAY SO: the modelling volume is exactly periodic over the
    // region, so REPEAT past the footprint is the same sky again — which is what makes this term reach the
    // 47 km of shell a 7-degree ray crosses, where the shadow map's 30 km square could not.
    const vec3 outside = CloudSkyOcclusionUvw( originKm, invSideKm, 0.5f, vec3( 120.0f, 1.0f, -90.0f ) );
    EXPECT_GT( outside.x, 1.0f );
    EXPECT_LT( outside.z, 0.0f );

    // VERTICALLY CLAMPED TO THE TEXEL CENTRES, because every sampler in this engine is REPEAT: without it
    // a height fraction of 1 wraps to the bottom slice, and the top of the layer would be told that the
    // whole deck stands above it.
    const float halfV = 0.5f / CLOUD_SKY_OCCLUSION_SLICES;
    EXPECT_NEAR( CloudSkyOcclusionUvw( originKm, invSideKm, 1.0f, vec3( 0.0f ) ).y, 1.0f - halfV, 1e-6f );
    EXPECT_NEAR( CloudSkyOcclusionUvw( originKm, invSideKm, 0.0f, vec3( 0.0f ) ).y, halfV, 1e-6f );
    EXPECT_NEAR( CloudSkyOcclusionUvw( originKm, invSideKm, 2.0f, vec3( 0.0f ) ).y, 1.0f - halfV, 1e-6f );
    EXPECT_NEAR( CloudSkyOcclusionUvw( originKm, invSideKm, -1.0f, vec3( 0.0f ) ).y, halfV, 1e-6f );
}

TEST( CloudSkyOcclusion, AColumnOfCloudDarkensASampleUNDERItAndLeavesTheOneABOVEItAlone )
{
    // THE MONOTONICITY THAT IS THE WHOLE FEATURE, and the property the term it replaces cannot have: more
    // cloud above a sample means less sky reaches it, and a sample at the TOP of a deck is untouched
    // however thick the deck under it is.
    //
    // The producer accumulates downward, so this drives the same arithmetic with a synthetic column:
    // sixteen slices of a uniform deck, the top eight empty and the bottom eight solid.
    const float thicknessKm = 3.6f;
    const float sigma       = 4.0f; // per km, the shipped extinction times a mid density
    const float sliceKm     = thicknessKm / CLOUD_SKY_OCCLUSION_SLICES;

    float tau      = 0.0f;
    float previous = 2.0f;
    for ( int slice = static_cast<int>( CLOUD_SKY_OCCLUSION_SLICES ) - 1; slice >= 0; --slice )
    {
        const float transmittance = CloudSkyDiffuseTransmittance( tau );

        if ( slice >= 12 )
            EXPECT_FLOAT_EQ( transmittance, 1.0f ) << "slice " << slice << " has clear sky above it";

        EXPECT_LE( transmittance, previous ) << "slice " << slice << " sees MORE sky than the one above it";
        previous = transmittance;

        if ( slice < 12 )
            tau += sigma * sliceKm;
    }

    // And the bottom of a solid deck is DARK rather than merely dimmer. Eleven slices of 4/km over 3.6 km
    // is a vertical tau of 9.9, which the hemispherical integral turns into about eight parts in a
    // million: the UPPER hemisphere over that sample is shut.
    EXPECT_LT( previous, 1e-4f );

    // WHAT THAT DOES AND DOES NOT LICENCE. This sentence used to end "that gap between 0.5 and ~0 is the
    // feature", and taking it at its word is what shipped a brown deck: the ~0 is a fact about the
    // hemisphere the volume measures, not about the sphere the ambient is a mean over. Composed through
    // CloudSkyOcclusion the same column leaves the sample its open lower half, so the multiplier the march
    // applies bottoms at CLOUD_SKY_LOWER_HEMISPHERE and not at zero. The feature is the gap between the
    // profile term's flat 0.5 and a number that MOVES with what is actually overhead — 1.0 under clear sky
    // here, 0.5 under eleven slices of deck — not the gap between 0.5 and nothing.
    EXPECT_FLOAT_EQ( CloudAmbientOcclusion( 1.0f, 0.5f ), 0.5f ) << "the flat floor this replaces";
    EXPECT_NEAR( CloudSkyOcclusion( previous, 1.0f ), CLOUD_SKY_LOWER_HEMISPHERE, 1e-4f );
    EXPECT_FLOAT_EQ( CloudSkyOcclusion( 1.0f, 1.0f ), 1.0f ) << "the top of the deck is untouched";
}

// ---------------------------------------------------------------------------------------------------
// CloudMultiScatterStep — the multiple-scattering series, and the converged march it is judged against
// ---------------------------------------------------------------------------------------------------
//
// WHY THERE IS A MONTE CARLO IN A UNIT TEST. Task Р18 was asked whether the cloud's extinction can be
// raised from the shipped 8 /km toward a cumulus' physical ~45, on the argument that the depth the eye
// integrates before the cloud is opaque — 657 m against an erosion that decorrelates in 160 m — is the
// binding constraint on how much surface a cloud can have (Common/CloudField.glslh, task Р9). The
// obstacle named in VolumetricCloudComponent.hpp was the LIGHTING: that at a physical extinction the
// octave approximation collapses and the cloud renders uniformly grey.
//
// That claim could not be checked, because there was nothing to check it against. Every other assertion
// in this file is about an identity — a phase function integrates to one, a closed form equals its own
// numeric integral — and an approximation has no identity to hold it to. It has an ERROR, and an error
// needs a truth. The truth is here: a backward path tracer through a homogeneous lobe with next-event
// estimation, every scattering order, no approximation but its own variance.
//
// WHAT IT IS AND IS NOT. It solves the radiative transfer equation in a HOMOGENEOUS SPHERE. It is
// therefore the truth about the SCATTERING and says nothing about the field, the march schedule or the
// erosion — which is exactly why it is a useful instrument: it isolates the one thing the octaves
// approximate. The reference's own single-scattering order is validated against an independent analytic
// march below, so the estimator is pinned before anything is measured with it.
namespace
{
    // NOT M_PI. It is a POSIX extension that MSVC only defines behind _USE_MATH_DEFINES, and this
    // project has already lost a Windows build to exactly that class of assumption. The header under test
    // carries its own CLOUD_PI as a float; the reference wants it in double.
    constexpr double kPi = 3.14159265358979323846;

    // A homogeneous lobe: radius in kilometres, extinction per kilometre, scattering albedo.
    struct ReferenceLobe
    {
        double RadiusKm   = 0.5;
        double SigmaPerKm = 8.0;
        double Albedo     = 0.98;
    };

    using dvec3 = glm::dvec3;

    // Distance from a point INSIDE the lobe to its boundary along a unit direction.
    double ExitDistanceKm( const ReferenceLobe& lobe, const dvec3& p, const dvec3& d )
    {
        const double b    = glm::dot( p, d );
        const double c    = glm::dot( p, p ) - lobe.RadiusKm * lobe.RadiusKm;
        const double disc = b * b - c;
        return disc <= 0.0 ? 0.0 : -b + std::sqrt( disc );
    }

    // Entry and exit of a ray that starts outside. False when it misses.
    bool IntersectLobe( const ReferenceLobe& lobe, const dvec3& o, const dvec3& d, double& t0, double& t1 )
    {
        const double b    = glm::dot( o, d );
        const double c    = glm::dot( o, o ) - lobe.RadiusKm * lobe.RadiusKm;
        const double disc = b * b - c;
        if ( disc <= 0.0 )
            return false;
        const double q = std::sqrt( disc );
        t0             = -b - q;
        t1             = -b + q;
        return t1 > 0.0;
    }

    // Henyey-Greenstein in double precision. The reference's own phase — NOT the march's dual lobe, which
    // is itself part of the approximation being measured (its near-isotropic second lobe exists to carry
    // the body, which is multiple scattering's job).
    double PhaseHG( double cosTheta, double g )
    {
        const double g2    = g * g;
        const double denom = 1.0 + g2 - 2.0 * g * cosTheta;
        return ( 1.0 - g2 ) / ( 4.0 * kPi * std::pow( std::max( denom, 1e-4 ), 1.5 ) );
    }

    // xorshift64* — deterministic, so a failure reproduces exactly. std::mt19937 would do as well; this is
    // one line and has no state to seed wrongly.
    struct Rng
    {
        std::uint64_t State;
        explicit Rng( std::uint64_t seed ) : State( seed * 6364136223846793005ULL + 1442695040888963407ULL )
        {
        }
        double Next()
        {
            State ^= State >> 12;
            State ^= State << 25;
            State ^= State >> 27;
            return static_cast<double>( ( State * 2685821657736338717ULL ) >> 11 ) * ( 1.0 / 9007199254740992.0 );
        }
    };

    // A direction drawn from the Henyey-Greenstein lobe around @p w.
    dvec3 SampleHG( const dvec3& w, double g, Rng& rng )
    {
        const double u1 = rng.Next();
        const double u2 = rng.Next();

        double cosT;
        if ( std::fabs( g ) < 1e-4 )
            cosT = 1.0 - 2.0 * u1;
        else
        {
            const double sq = ( 1.0 - g * g ) / ( 1.0 - g + 2.0 * g * u1 );
            cosT            = ( 1.0 + g * g - sq * sq ) / ( 2.0 * g );
        }

        cosT              = std::clamp( cosT, -1.0, 1.0 );
        const double sinT = std::sqrt( std::max( 0.0, 1.0 - cosT * cosT ) );
        const double phi  = 2.0 * kPi * u2;

        const dvec3 a = std::fabs( w.x ) < 0.9 ? dvec3( 1, 0, 0 ) : dvec3( 0, 1, 0 );
        const dvec3 t = glm::normalize( glm::cross( a, w ) );
        const dvec3 b = glm::cross( w, t );
        return glm::normalize( t * ( sinT * std::cos( phi ) ) + b * ( sinT * std::sin( phi ) ) + w * cosT );
    }

    // One backward path from the eye, with next-event estimation toward the sun at every vertex.
    //
    // THE SCATTERING COSINE IS dot(rayDirection, toSun), which is the SAME argument CloudRaymarch.shader
    // passes to its phase function: a photon reaching the eye travelled along -rayDirection and, before
    // its last scatter, along -toSun. Getting this backwards is a defect that survives every test of
    // either side alone, so it is written out rather than left to the reader.
    //
    // @p maxOrder folds every order past it away, which is how the per-order decomposition below is taken.
    // Pass a large number for the full answer.
    double TraceOnePath( const ReferenceLobe& lobe, dvec3 origin, dvec3 d, const dvec3& toSun, double g, Rng& rng,
                         int maxOrder )
    {
        double t0 = 0.0;
        double t1 = 0.0;
        if ( !IntersectLobe( lobe, origin, d, t0, t1 ) )
            return 0.0;

        dvec3  p          = origin + d * std::max( t0, 0.0 );
        double throughput = 1.0;
        double radiance   = 0.0;

        for ( int order = 1; order <= maxOrder; ++order )
        {
            // Free-path sampling in a homogeneous medium. Its pdf carries the sigma_t that would otherwise
            // multiply the estimator, which is why the weight below is the albedo and not sigma_s.
            const double s = -std::log( 1.0 - rng.Next() ) / lobe.SigmaPerKm;
            if ( s >= ExitDistanceKm( lobe, p, d ) )
                break; // it left without scattering

            p += d * s;

            const double tauSun = lobe.SigmaPerKm * ExitDistanceKm( lobe, p, toSun );
            radiance += throughput * lobe.Albedo * PhaseHG( glm::dot( d, toSun ), g ) * std::exp( -tauSun );

            throughput *= lobe.Albedo;

            // Russian roulette rather than a hard cut, so the estimator stays unbiased at an albedo near
            // one — where a cut would silently delete the orders that make a real cloud white.
            if ( throughput < 0.01 )
            {
                if ( rng.Next() > 0.5 )
                    break;
                throughput *= 2.0;
            }

            d = SampleHG( d, g, rng );
        }

        return radiance;
    }

    // The single-scattering march the estimator is validated against: the march's own arithmetic with the
    // octave series reduced to one term and the optical depth taken analytically.
    double AnalyticSingleScatter( const ReferenceLobe& lobe, const dvec3& origin, const dvec3& d,
                                  const dvec3& toSun, double g, int steps )
    {
        double t0 = 0.0;
        double t1 = 0.0;
        if ( !IntersectLobe( lobe, origin, d, t0, t1 ) )
            return 0.0;

        const double entry  = std::max( t0, 0.0 );
        const double stepKm = ( t1 - entry ) / steps;
        const double phase  = PhaseHG( glm::dot( d, toSun ), g );

        double radiance      = 0.0;
        double transmittance = 1.0;
        for ( int i = 0; i < steps; ++i )
        {
            const dvec3  x          = origin + d * ( entry + ( i + 0.5 ) * stepKm );
            const double od         = lobe.SigmaPerKm * ExitDistanceKm( lobe, x, toSun );
            const double sigma      = lobe.SigmaPerKm;
            const double scattering = std::exp( -od ) * phase * lobe.Albedo * sigma;

            radiance += transmittance * ( scattering - scattering * std::exp( -sigma * stepKm ) ) / sigma;
            transmittance *= std::exp( -sigma * stepKm );
        }
        return radiance;
    }

    // The march's answer for the same lobe: CloudMultiScatterStep, driven exactly as CloudRaymarch.shader
    // drives it, with the field replaced by a constant so the only variable is the scattering.
    double MarchTheSeries( const ReferenceLobe& lobe, const CloudScatterSeries& series, const dvec3& origin,
                           const dvec3& d, const dvec3& toSun, float phase, int steps )
    {
        double t0 = 0.0;
        double t1 = 0.0;
        if ( !IntersectLobe( lobe, origin, d, t0, t1 ) )
            return 0.0;

        const double entry  = std::max( t0, 0.0 );
        const double stepKm = ( t1 - entry ) / steps;
        const float  sigmaT = static_cast<float>( lobe.SigmaPerKm );

        double radiance      = 0.0;
        double transmittance = 1.0;
        for ( int i = 0; i < steps; ++i )
        {
            const dvec3 x  = origin + d * ( entry + ( i + 0.5 ) * stepKm );
            const float od = static_cast<float>( lobe.SigmaPerKm * ExitDistanceKm( lobe, x, toSun ) );

            // Unit sun radiance and no ambient: the series is being measured against a reference that has
            // no ambient either, and a term neither side carries can only blur the comparison.
            // A NEUTRAL vec3 albedo, so this arm measures exactly what it measured while the albedo was a
            // scalar: the reference below is a grey medium, and the comparison would say nothing about
            // the series if the two sides stopped describing the same one.
            const vec3 step =
                 CloudMultiScatterStep( series, vec3( 1.0f ), vec3( 0.0f ), od, phase, sigmaT,
                                        vec3( static_cast<float>( lobe.Albedo ) ), static_cast<float>( stepKm ) );

            radiance += transmittance * static_cast<double>( step.x );
            transmittance *= std::exp( -lobe.SigmaPerKm * stepKm );
        }
        return radiance;
    }

    // An orthographic camera basis for a view direction.
    void CameraBasis( const dvec3& view, dvec3& right, dvec3& up )
    {
        const dvec3 a = std::fabs( view.y ) < 0.9 ? dvec3( 0, 1, 0 ) : dvec3( 1, 0, 0 );
        right         = glm::normalize( glm::cross( a, view ) );
        up            = glm::cross( view, right );
    }

    struct LobeImage
    {
        std::vector<double> Pixels;

        double Mean() const
        {
            double s = 0.0;
            for ( double v : Pixels )
                s += v;
            return s / static_cast<double>( Pixels.size() );
        }

        // TONAL CONTRAST, NORMALISED: (p95 - p05) / (p95 + p05). Tools/ImageStat measures the frame's
        // contrast as the plain difference, which is the right ruler for two 8-bit images of the same
        // scene. Here the two sides can differ in absolute brightness by a factor, so a plain difference
        // would report the brighter one as the more structured one. The ratio is scale free and answers
        // the question actually being asked: does the lobe read as a modelled body or as a flat disc.
        double Contrast() const
        {
            std::vector<double> sorted = Pixels;
            std::sort( sorted.begin(), sorted.end() );
            auto q = [&]( double f )
            { return sorted[std::min( sorted.size() - 1, static_cast<size_t>( f * sorted.size() ) )]; };
            const double lo = q( 0.05 );
            const double hi = q( 0.95 );
            return ( hi + lo ) > 1e-12 ? ( hi - lo ) / ( hi + lo ) : 0.0;
        }
    };

    // The disc is sampled at 0.92 of the radius: at the very rim the chord vanishes and both sides
    // approach zero together, which would put a pile of agreeing near-zeros into a percentile that is
    // supposed to be measuring the shaded side of the body.
    constexpr double kDiscFraction = 0.92;

    bool DiscPoint( const ReferenceLobe& lobe, const dvec3& view, int i, int j, int n, dvec3& origin )
    {
        dvec3 right;
        dvec3 up;
        CameraBasis( view, right, up );

        const double half = kDiscFraction * lobe.RadiusKm;
        const double u    = ( ( i + 0.5 ) / n * 2.0 - 1.0 ) * half;
        const double v    = ( ( j + 0.5 ) / n * 2.0 - 1.0 ) * half;
        if ( u * u + v * v > half * half )
            return false;

        origin = right * u + up * v - view * ( 4.0 * lobe.RadiusKm );
        return true;
    }

    LobeImage RenderReference( const ReferenceLobe& lobe, const dvec3& view, const dvec3& toSun, double g, int n,
                               int paths, std::uint64_t seed )
    {
        LobeImage image;
        for ( int j = 0; j < n; ++j )
            for ( int i = 0; i < n; ++i )
            {
                dvec3 o;
                if ( !DiscPoint( lobe, view, i, j, n, o ) )
                    continue;

                Rng    rng( seed + static_cast<std::uint64_t>( j ) * 7919u +
                            static_cast<std::uint64_t>( i ) * 104729u );
                double acc = 0.0;
                for ( int s = 0; s < paths; ++s )
                    acc += TraceOnePath( lobe, o, view, toSun, g, rng, 1 << 20 );
                image.Pixels.push_back( acc / paths );
            }
        return image;
    }

    LobeImage RenderSeries( const ReferenceLobe& lobe, const CloudScatterSeries& series, const dvec3& view,
                            const dvec3& toSun, float phase, int n, int steps )
    {
        LobeImage image;
        for ( int j = 0; j < n; ++j )
            for ( int i = 0; i < n; ++i )
            {
                dvec3 o;
                if ( !DiscPoint( lobe, view, i, j, n, o ) )
                    continue;
                image.Pixels.push_back( MarchTheSeries( lobe, series, o, view, toSun, phase, steps ) );
            }
        return image;
    }

    // The MATERIAL's shipped series, so the test moves with the defaults instead of restating them —
    // the four numbers are schema parameters since O1, and CloudMaterialValues is their pinned mirror
    // (Desert/Tests/Engine/CloudMaterialSchema holds mirror == schema).
    CloudScatterSeries ShippedSeries()
    {
        const Desert::Graphic::CloudMaterialValues material;

        CloudScatterSeries series;
        series.Octaves     = static_cast<float>( material.MultiScatterOctaves );
        series.ScatterStep = material.MultiScatterContribution;
        series.ExtinctStep = material.MultiScatterOcclusion;
        series.PhaseStep   = material.MultiScatterEccentricity;
        return series;
    }

    // The sun 60 degrees up, and three views the owner's own frames contain: across the lit flank, within
    // 30 degrees of the sun, and straight up at the base.
    const dvec3 kSun60 = glm::normalize( dvec3( 0.5, 0.866, 0.0 ) );
} // namespace

TEST( CloudMultiScatterSeries, TheMonteCarloReferencesFirstOrderIsTheAnalyticSingleScattering )
{
    // THE INSTRUMENT IS PINNED BEFORE ANYTHING IS MEASURED WITH IT. Truncated at one scattering order the
    // path tracer must reproduce the closed-form single-scattering march, which shares no line of code
    // with it. Two independent evaluations of one quantity — the strongest shape of test this project
    // has (verify skill §4) — and without it every divergence reported below could be the estimator's.
    const double g = 0.8;

    for ( const double sigma : { 4.0, 8.0, 20.0 } )
    {
        ReferenceLobe lobe;
        lobe.SigmaPerKm = sigma;

        const dvec3 view( 0.0, 0.0, 1.0 );
        const dvec3 origin = -view * ( 4.0 * lobe.RadiusKm );

        Rng           rng( 4242 );
        constexpr int kPaths = 400000;
        double        first  = 0.0;
        for ( int s = 0; s < kPaths; ++s )
            first += TraceOnePath( lobe, origin, view, kSun60, g, rng, 1 );
        first /= kPaths;

        const double analytic = AnalyticSingleScatter( lobe, origin, view, kSun60, g, 4096 );

        EXPECT_NEAR( first, analytic, 0.05 * analytic )
             << "at sigma " << sigma << " the estimator's first order is " << first
             << " against the analytic march's " << analytic
             << " -- the reference itself is wrong, so nothing measured against it means anything";
    }
}

TEST( CloudMultiScatterSeries, AStepCannotScatterMoreLightThanTheSourceItIntegrates )
{
    // THE BOUND, and it is the one property that catches any spurious factor anywhere in the series.
    //
    // Each octave in-scatters `source_o = (sun * visibility * phase_o + ambient) * albedo * sigma * s_o`
    // and integrates it across the step, and CloudScatterIntegral is already known to return at most
    // `source * stepKm`. So the whole series is bounded by the sum of those products — which is a
    // statement about the arithmetic and not about any particular medium, and holds at every extinction,
    // every step and every optical depth. A stray multiply, a factor that failed to decay, an octave
    // counted twice: all of them break this and none of them breaks a spot value.
    const CloudScatterSeries series = ShippedSeries();

    // The scatter weights this series applies, derived the way the loop derives them so the bound cannot
    // drift from the implementation by being restated.
    double weights = 0.0;
    {
        double factor = 1.0;
        double step   = series.ScatterStep;
        for ( int octave = 0; octave < static_cast<int>( series.Octaves ); ++octave )
        {
            if ( octave > 0 )
            {
                factor *= step;
                step *= step;
            }
            weights += factor;
        }
    }

    const float phase    = CloudPhaseDualLobe( 0.9f, 0.8f, 0.1667f, 0.575f );
    const float maxPhase = std::max( phase, static_cast<float>( CLOUD_ISOTROPIC_PHASE ) );

    for ( const float sigma : { 0.5f, 8.0f, 45.0f, 120.0f } )
        for ( const float od : { 0.0f, 0.5f, 5.0f, 45.0f } )
            for ( const float stepKm : { 0.001f, 0.02f, 0.2f, 2.0f } )
            {
                const vec3 sun( 3.0f, 2.0f, 1.0f );
                const vec3 ambient( 0.4f, 0.5f, 0.6f );
                const vec3 albedo( 0.98f );

                const vec3 value = CloudMultiScatterStep( series, sun, ambient, od, phase, sigma, albedo, stepKm );

                // The most any octave can present to the integral, before its own weight: the sun at full
                // visibility through the most forward the phase ever gets, plus the whole ambient.
                const double perOctave = static_cast<double>( sun.x ) * maxPhase + ambient.x;
                const double bound     = perOctave * albedo.x * sigma * weights * stepKm;

                EXPECT_LE( static_cast<double>( value.x ), bound * ( 1.0 + 1e-5 ) + 1e-9 )
                     << "sigma " << sigma << ", optical depth " << od << ", step " << stepKm
                     << ": the series returned " << value.x << " against a source that can only supply " << bound;

                EXPECT_GE( value.x, 0.0f );
                EXPECT_TRUE( std::isfinite( value.x ) );
            }
}

// A COLOURED ALBEDO IS EXACTLY THREE INDEPENDENT GREY RUNS — the relation that makes the medium's colour
// a physical quantity rather than a tint applied to the answer.
//
// WHY THIS IS THE STRONGEST ASSERTION IN THE FILE ABOUT THE CHANGE. Scattering does not mix wavelengths:
// a photon that leaves as red arrived as red, so the red channel of the result may depend on the red
// channel of the source and of the albedo and on NOTHING else. That property is invisible in a frame — a
// coupled implementation produces a plausible tint — and it is exactly what a `dot()`, a `length()`, a
// luminance or an accidental `.x` broadcast inside the series would destroy. Asserted to the BIT rather
// than within a tolerance, because there is no arithmetic reason for the two paths to differ: the same
// operations happen in the same order either way, only three at a time instead of one.
//
// It also pins the OTHER direction, which is the one the migration exists for: a grey colour must be the
// same answer the scalar gave, so every scene raised from a scalar albedo renders the sky it rendered
// before. That half is what the six protocol points then confirm on a real frame.
TEST( CloudMultiScatterSeries, AColouredAlbedoIsThreeIndependentGreyRuns )
{
    const CloudScatterSeries series = ShippedSeries();
    const float              phase  = CloudPhaseDualLobe( 0.85f, 0.8f, 0.1667f, 0.575f );

    // A DELIBERATELY LOPSIDED SOURCE. With sun and ambient equal across the channels, a series that
    // collapsed the albedo to its luminance would still agree with the grey runs at a grey albedo, and
    // this test would pass over the very defect it is here for.
    const vec3 sun( 3.0f, 2.0f, 1.0f );
    const vec3 ambient( 0.4f, 0.5f, 0.6f );

    int compared = 0;
    for ( const vec3 albedo : { vec3( 0.98f, 0.98f, 0.98f ), vec3( 0.98f, 0.72f, 0.55f ),
                                vec3( 0.20f, 0.90f, 0.05f ), vec3( 0.0f, 0.5f, 1.0f ) } )
        for ( const float sigma : { 0.5f, 8.0f, 45.0f } )
            for ( const float od : { 0.0f, 0.5f, 5.0f, 45.0f } )
                for ( const float stepKm : { 0.001f, 0.02f, 0.2f, 2.0f } )
                {
                    const vec3 coloured =
                         CloudMultiScatterStep( series, sun, ambient, od, phase, sigma, albedo, stepKm );

                    // Each channel run on its own, through the same function, with every vector quantity
                    // reduced to that one channel. Reusing the function rather than restating its maths is
                    // the point: a reference written out here would be a second implementation, and the
                    // two agreeing would say nothing about the one that ships.
                    for ( int c = 0; c < 3; ++c )
                    {
                        const vec3 grey = CloudMultiScatterStep( series, vec3( sun[c] ), vec3( ambient[c] ), od,
                                                                 phase, sigma, vec3( albedo[c] ), stepKm );

                        EXPECT_FLOAT_EQ( coloured[c], grey[c] )
                             << "channel " << c << " of a coloured albedo " << albedo[c]
                             << " differs from the same channel run alone, at sigma " << sigma
                             << ", optical depth " << od << ", step " << stepKm
                             << ". Scattering does not mix wavelengths, so this is a cross-channel term "
                                "that has no physical meaning — look for a dot(), a length(), a luminance "
                                "or a broadcast of .x inside the series.";
                        ++compared;
                    }
                }

    // QUOTED, because the loops above are vacuously green over an empty parameter set and a census that
    // stopped counting is exactly how one silently becomes a no-op.
    std::printf( "[CloudMultiScatterSeries] %d channel comparisons across 4 albedos x 3 extinctions x 4 "
                 "optical depths x 4 steps\n",
                 compared );
    EXPECT_EQ( compared, 4 * 3 * 4 * 4 * 3 );
}

TEST( CloudMultiScatterSeries, MoreCloudBetweenTheSampleAndTheSunMeansLessLightArrives )
{
    // THE MONOTONICITY. Every octave reads the same shadow ray through its own extinction factor, so the
    // whole series must fall as that ray lengthens. It is the relation an "energy-conserving" rewrite is
    // most likely to break — a normalisation that divides by the series' own sum can easily make a deeper
    // sample brighter than a shallower one, which reads as a cloud lit from inside.
    //
    // The ambient is left out on purpose: it is a constant the series adds to the first octave and would
    // put a floor under the sequence, hiding an inversion above it.
    const CloudScatterSeries series = ShippedSeries();
    const float              phase  = CloudPhaseDualLobe( 0.7f, 0.8f, 0.1667f, 0.575f );

    for ( const float sigma : { 0.5f, 8.0f, 45.0f } )
        for ( const float stepKm : { 0.005f, 0.05f, 0.5f } )
        {
            float previous = std::numeric_limits<float>::max();
            for ( int i = 0; i <= 120; ++i )
            {
                const float od = 0.5f * static_cast<float>( i );
                const float value = CloudMultiScatterStep( series, vec3( 1.0f ), vec3( 0.0f ), od, phase, sigma,
                                                           vec3( 0.98f ), stepKm )
                                         .x;

                EXPECT_LE( value, previous ) << "sigma " << sigma << ", step " << stepKm << ": optical depth "
                                             << od << " delivers MORE light than " << od - 0.5f << " did";
                previous = value;
            }
        }
}

TEST( CloudMultiScatterSeries, TheSeriesTracksTheConvergedMarchAtTheSHIPPEDExtinctionAndNotAtAPHYSICALOne )
{
    // THE MEASUREMENT TASK Р18 WAS SET, kept as a test because it is the only thing in the repository that
    // can tell the next person whether the extinction is still tethered.
    //
    // WHAT IT FOUND. The series is not a bad approximation in general — at the shipped 8 /km it reproduces
    // the converged lobe's tonal contrast to within a few hundredths. At a cumulus' physical ~45 /km it
    // does not, and the direction is the one that matters: the reference gets MORE structured as the
    // medium thickens (a lit flank and a dark one, contrast about 0.95) while the series gets FLATTER.
    //
    // WHY, and this is the mechanism rather than the observation. The deepest octave's extinction factor
    // is `MultiScatterOcclusion^3` = 1/64, so the length over which its light is attenuated is 64/sigma —
    // 8 km at the shipped extinction and 1.42 km at 45. The medium's own diffusion length,
    // 1 / (sigma * sqrt(3 (1-a)(1-a g))), is 1.10 km and 195 m for the same two. At 8 /km the two agree
    // to within a factor of seven ON A BODY THAT IS ONLY 1 km ACROSS, so "a uniform glow inside the body"
    // is very nearly the truth and the approximation is accidentally right. At 45 /km the truth has
    // structure at 195 m and the approximation still spreads it over 1.42 km. The octaves were calibrated
    // at the one extinction where their own flat-glow assumption happens to hold.
    //
    // WHAT WOULD CHANGE THE ANSWER, and it needs no new code at all: MultiScatterOcclusion IS the base of
    // that power, so setting it to the cube root of the similarity factor — 0.4847 rather than 0.25 —
    // puts the deepest of three octaves exactly on the medium's diffusion scale. Measured on this lobe at
    // 45 /km the contrast error goes -0.228/-0.426/-0.432 to +0.016/+0.044/+0.032, at a cost of 36 % to
    // 71 % of the energy. At the SHIPPED 8 /km the same change makes it WORSE (-0.010 to +0.215), which
    // is the same finding read the other way: 0.25 is right for the medium the sky is actually made of.
    const double             g      = 0.8;
    const CloudScatterSeries series = ShippedSeries();

    struct View
    {
        const char* Name;
        dvec3       Direction;
    };
    const View views[] = { { "flank", glm::normalize( dvec3( 0, 0, 1 ) ) },
                           { "sunward", glm::normalize( dvec3( 0.5, 0.7, 0.5 ) ) },
                           { "base", glm::normalize( dvec3( 0, 1, 0 ) ) } };

    // Sized for Debug: 10 x 10 rays over the disc at 700 paths each is about 55 000 paths, which is a
    // couple of seconds and puts the mean's own noise near half a per cent (measured by re-seeding).
    constexpr int kGrid  = 10;
    constexpr int kPaths = 700;

    std::printf( "[CloudMultiScatterSeries] %-8s %-8s %10s %10s %8s %9s %9s %8s\n", "sigma", "view", "ref mean",
                 "our mean", "ratio", "ref contr", "our contr", "delta" );

    double shippedWorst  = 0.0;
    double physicalWorst = 0.0;

    for ( const double sigma : { 8.0, 45.0 } )
    {
        ReferenceLobe lobe;
        lobe.SigmaPerKm = sigma;

        for ( const View& view : views )
        {
            const LobeImage reference = RenderReference( lobe, view.Direction, kSun60, g, kGrid, kPaths, 12345 );
            const float phase = CloudPhaseDualLobe( static_cast<float>( glm::dot( view.Direction, kSun60 ) ), 0.8f,
                                                    0.1667f, 0.575f );
            const LobeImage ours = RenderSeries( lobe, series, view.Direction, kSun60, phase, kGrid, 256 );

            const double delta = ours.Contrast() - reference.Contrast();
            std::printf( "[CloudMultiScatterSeries] %-8.0f %-8s %10.5f %10.5f %7.2fx %9.4f %9.4f %+8.4f\n", sigma,
                         view.Name, reference.Mean(), ours.Mean(), ours.Mean() / reference.Mean(),
                         reference.Contrast(), ours.Contrast(), delta );

            if ( sigma < 20.0 )
                shippedWorst = std::max( shippedWorst, std::fabs( delta ) );
            else
                physicalWorst = std::max( physicalWorst, std::fabs( delta ) );
        }
    }

    // THE RELATION, and it is deliberately a comparison between the two extinctions rather than two
    // absolute bounds. What is being asserted is the TETHER: that the approximation is markedly better at
    // the extinction it was calibrated at than at a physical one. A change that improves the physical
    // case is supposed to come here and restate this with its own numbers, exactly as Р9's ratio in
    // Desert/Tests/Engine/CloudField is meant to be restated rather than silently passed.
    EXPECT_LT( shippedWorst, 0.10 )
         << "at the shipped extinction the series' worst tonal-contrast error against the converged march "
            "is now "
         << shippedWorst
         << "; it was 0.043 when this was written, and the whole argument for keeping the "
            "extinction at 8 rests on it being small there";

    EXPECT_GT( physicalWorst, 2.0 * shippedWorst )
         << "the series is no longer markedly worse at a physical extinction (" << physicalWorst << " against "
         << shippedWorst
         << "). If that is because the approximation was fixed, this test has done its job and wants "
            "rewriting around the new numbers -- do not delete it, restate it";
}

// ---------------------------------------------------------------------------------------------------
// WHY THE SKY-LIGHT TERM HAS A FLOOR — the measurement that stops the next person removing it
// ---------------------------------------------------------------------------------------------------
//
// CloudSkyOcclusion bottoms at CLOUD_SKY_LOWER_HEMISPHERE, so however much cloud stands over a sample it
// still receives half the sky's mean radiance. That reads like a modelling error, and Р19 was set to find
// out whether removing it is where the clouds' missing FORM comes from. It is not, and the reason is
// physical rather than a matter of taste.
//
// THE AMBIENT TERM IS THE ONLY PLACE ALL ORDERS OF SKY LIGHT ENTER. CloudMultiScatterStep adds @p ambient
// to octave 0 alone, so the number the march multiplies stands for every order of sky light at once — not
// for the first. The truth it should be judged against is therefore the ALL-ORDERS response of a body
// sitting in a surround of uniform radiance, and that quantity is nearly featureless: at a cloud's own
// scattering albedo the medium is close to radiative equilibrium with whatever surrounds it, and
// equilibrium has no shape by definition. The measurement below puts numbers on it.
//
// The estimator is Р18's, one line different: instead of next-event estimation toward the sun, a path
// that leaves the body collects the surround. Its albedo-zero limit is the single-scattering sphere
// integral, computed here independently, which is what pins it before anything is measured with it.
namespace
{
    // Equal-area directions on the sphere, deterministic. Used for the single-scattering integral so that
    // half the comparison carries no Monte Carlo noise at all.
    std::vector<dvec3> FibonacciSphere( int count )
    {
        std::vector<dvec3> directions;
        directions.reserve( count );

        const double golden = kPi * ( 3.0 - std::sqrt( 5.0 ) );
        for ( int i = 0; i < count; ++i )
        {
            const double y      = 1.0 - 2.0 * ( ( i + 0.5 ) / count );
            const double radius = std::sqrt( std::max( 0.0, 1.0 - y * y ) );
            directions.push_back( dvec3( radius * std::cos( golden * i ), y, radius * std::sin( golden * i ) ) );
        }
        return directions;
    }

    // The FIRST-ORDER sky light at @p p: the sphere-mean of the transmittance toward the surround. No
    // cosine weighting anywhere in it — a volume element has no normal, and the quantity the march wants
    // is a mean RADIANCE rather than an irradiance.
    double SingleScatterSkyResponse( const ReferenceLobe& lobe, const dvec3& p, const std::vector<dvec3>& dirs )
    {
        double sum = 0.0;
        for ( const dvec3& d : dirs )
            sum += std::exp( -lobe.SigmaPerKm * ExitDistanceKm( lobe, p, d ) );
        return sum / static_cast<double>( dirs.size() );
    }

    // ALL orders of sky light at @p p under a surround of uniform radiance 1. A uniform direction is drawn
    // — which IS the 1/4pi sphere integral — and the path walks until it either leaves the body, collecting
    // the surround, or scatters and carries on with the albedo as its throughput.
    //
    // The uniform draw is SampleHG at zero asymmetry, which draws cos(theta) uniformly on [-1, 1]: reusing
    // the sampler the suite already pins beats adding a second one that could be wrong on its own.
    double AllOrderSkyResponse( const ReferenceLobe& lobe, const dvec3& p, double g, int paths, Rng& rng )
    {
        double total = 0.0;

        for ( int path = 0; path < paths; ++path )
        {
            dvec3  d          = SampleHG( dvec3( 0.0, 0.0, 1.0 ), 0.0, rng );
            dvec3  x          = p;
            double throughput = 1.0;

            for ( int bounce = 0; bounce < 1024; ++bounce )
            {
                const double step = -std::log( 1.0 - rng.Next() ) / lobe.SigmaPerKm;
                if ( step >= ExitDistanceKm( lobe, x, d ) )
                {
                    total += throughput; // it reached the surround
                    break;
                }

                x += d * step;
                throughput *= lobe.Albedo;

                // Russian roulette rather than a hard cut, for the reason TraceOnePath gives: at an albedo
                // near one a cut deletes exactly the orders that make a cloud white.
                if ( throughput < 0.01 )
                {
                    if ( rng.Next() > 0.5 )
                        break;
                    throughput *= 2.0;
                }

                d = SampleHG( d, g, rng );
            }
        }

        return total / static_cast<double>( paths );
    }

    // Points on a lattice inside the lobe, pulled in to 0.9 of the radius for the reason kDiscFraction
    // gives: at the very rim every model agrees on "no cloud" and a pile of agreeing ones would sit in the
    // percentiles that are supposed to be measuring the body.
    std::vector<dvec3> LobeInteriorPoints( const ReferenceLobe& lobe, int perAxis )
    {
        std::vector<dvec3> points;
        const double       reach = 0.9 * lobe.RadiusKm;

        for ( int k = 0; k < perAxis; ++k )
            for ( int j = 0; j < perAxis; ++j )
                for ( int i = 0; i < perAxis; ++i )
                {
                    const dvec3 p( ( ( i + 0.5 ) / perAxis * 2.0 - 1.0 ) * reach,
                                   ( ( j + 0.5 ) / perAxis * 2.0 - 1.0 ) * reach,
                                   ( ( k + 0.5 ) / perAxis * 2.0 - 1.0 ) * reach );
                    if ( glm::dot( p, p ) <= reach * reach )
                        points.push_back( p );
                }
        return points;
    }

    double RelativeContrast( std::vector<double> values )
    {
        std::sort( values.begin(), values.end() );
        auto q = [&]( double f )
        { return values[std::min( values.size() - 1, static_cast<size_t>( f * values.size() ) )]; };
        const double lo = q( 0.05 );
        const double hi = q( 0.95 );
        return ( hi + lo ) > 1e-12 ? ( hi - lo ) / ( hi + lo ) : 0.0;
    }

    double MeanOf( const std::vector<double>& values )
    {
        double sum = 0.0;
        for ( double v : values )
            sum += v;
        return sum / static_cast<double>( values.size() );
    }

    double RmsDifference( const std::vector<double>& a, const std::vector<double>& b )
    {
        double sum = 0.0;
        for ( size_t i = 0; i < a.size(); ++i )
            sum += ( a[i] - b[i] ) * ( a[i] - b[i] );
        return std::sqrt( sum / static_cast<double>( a.size() ) );
    }

    // The three answers, evaluated over the same interior points so they can be differenced pointwise.
    struct SkyResponses
    {
        std::vector<double> Shipped;     // CloudSkyOcclusion driven exactly as the march drives it
        std::vector<double> SingleOrder; // the sphere-mean transmittance: exact, and first order only
        std::vector<double> AllOrders;   // the converged reference
    };

    SkyResponses MeasureSkyResponses( const ReferenceLobe& lobe, double g, int perAxis, int paths )
    {
        const std::vector<dvec3> points = LobeInteriorPoints( lobe, perAxis );
        const std::vector<dvec3> dirs   = FibonacciSphere( 512 );

        SkyResponses out;
        for ( const dvec3& p : points )
        {
            // THE SHIPPED PIPELINE, DRIVEN ON THIS LOBE and not restated: the column straight up is what
            // CloudSkyOcclusionVolume.shader integrates, CloudSkyDiffuseTransmittance is what it stores,
            // and CloudSkyOcclusion is what the march composes out of it.
            const double tauUp = lobe.SigmaPerKm * ExitDistanceKm( lobe, p, dvec3( 0.0, 1.0, 0.0 ) );
            out.Shipped.push_back(
                 CloudSkyOcclusion( CloudSkyDiffuseTransmittance( static_cast<float>( tauUp ) ), 1.0f ) );

            out.SingleOrder.push_back( SingleScatterSkyResponse( lobe, p, dirs ) );

            Rng rng( 7717u + static_cast<std::uint64_t>( out.AllOrders.size() ) * 104729u );
            out.AllOrders.push_back( AllOrderSkyResponse( lobe, p, g, paths, rng ) );
        }
        return out;
    }

    constexpr int kSkyGrid  = 7;
    constexpr int kSkyPaths = 2000;
} // namespace

TEST( CloudSkyOcclusion, TheEstimatorsAlbedoZeroLimitIsTheSingleScatteringSphereIntegral )
{
    // THE INSTRUMENT IS PINNED BEFORE ANYTHING IS MEASURED WITH IT, the same discipline
    // TheMonteCarloReferencesFirstOrderIsTheAnalyticSingleScattering applies to the sun's estimator. With
    // the albedo at zero a path can only escape or die, so the estimator must reproduce the deterministic
    // sphere-mean of exp(-tau) — which shares no line of code with it.
    ReferenceLobe lobe;
    lobe.Albedo = 0.0;

    const SkyResponses r = MeasureSkyResponses( lobe, 0.8, 5, kSkyPaths );

    const double rms = RmsDifference( r.AllOrders, r.SingleOrder );
    std::printf( "[CloudSkyOcclusion] albedo 0: path tracer mean %.5f, sphere integral mean %.5f, rms %.5f\n",
                 MeanOf( r.AllOrders ), MeanOf( r.SingleOrder ), rms );

    EXPECT_LT( rms, 0.02 ) << "the two evaluations of one quantity disagree by " << rms
                           << "; it was 0.005 when this was written, and every number in the test below "
                              "rests on this one";
}

TEST( CloudSkyOcclusion, TheSkyLightHasNoFormToGiveAtACloudsOwnAlbedoWhichIsWhyTheTermHasAFloor )
{
    // Р19'S RESULT, and it is a REFUSAL: the flat near-field body Р18 traced to the ambient term cannot be
    // fixed inside that term, because at a cloud's own albedo the sky light physically has no form in it.
    //
    // Three answers over the same interior points of one lobe:
    //
    //   all orders  the truth — what the march's ambient stands for, since CloudMultiScatterStep adds the
    //               ambient to octave 0 and nothing else carries sky light at all.
    //   one order   the sphere-mean transmittance. This is what "occlude the sky light correctly" means if
    //               you mean it literally, and it is what a richer occlusion volume would converge to.
    //   shipped     CloudSkyOcclusion on the vertical column, floor and all.
    //
    // The truth is nearly CONSTANT and the literal answer is not. That is the whole finding.
    ReferenceLobe lobe;
    const double  g = 0.8;

    const SkyResponses r = MeasureSkyResponses( lobe, g, kSkyGrid, kSkyPaths );

    const double allMean  = MeanOf( r.AllOrders );
    const double oneMean  = MeanOf( r.SingleOrder );
    const double shipMean = MeanOf( r.Shipped );

    const double allContrast  = RelativeContrast( r.AllOrders );
    const double oneContrast  = RelativeContrast( r.SingleOrder );
    const double shipContrast = RelativeContrast( r.Shipped );

    std::printf( "[CloudSkyOcclusion] lobe R %.2f km, sigma %.0f /km, albedo %.2f, %zu interior points\n",
                 lobe.RadiusKm, lobe.SigmaPerKm, lobe.Albedo, r.AllOrders.size() );
    std::printf( "[CloudSkyOcclusion] %-12s %10s %14s\n", "answer", "mean", "rel-contrast" );
    std::printf( "[CloudSkyOcclusion] %-12s %10.4f %14.4f\n", "all orders", allMean, allContrast );
    std::printf( "[CloudSkyOcclusion] %-12s %10.4f %14.4f\n", "one order", oneMean, oneContrast );
    std::printf( "[CloudSkyOcclusion] %-12s %10.4f %14.4f\n", "shipped", shipMean, shipContrast );
    std::printf( "[CloudSkyOcclusion] rms to truth: shipped %.4f, one order %.4f\n",
                 RmsDifference( r.Shipped, r.AllOrders ), RmsDifference( r.SingleOrder, r.AllOrders ) );

    // THE FINDING. At the component's own albedo the body is within a few per cent of being lit uniformly
    // by the sky, everywhere, including its core: it is close to radiative equilibrium with its surround.
    // Measured 0.017 when this was written, against 0.79 for the one-order answer.
    EXPECT_LT( allContrast, 0.05 ) << "the all-orders sky response over the body now varies by " << allContrast
                                   << "; it was 0.017. If this has genuinely risen, the sky term can carry "
                                      "form after all and Р19's refusal wants revisiting";

    // AND THE LITERAL ANSWER IS AN ORDER MORE STRUCTURED THAN THE TRUTH, which is the sentence that stops
    // the obvious fix. A volume that occluded the sky light exactly — both hemispheres, lateral neighbours
    // and all — would converge to `one order` and would therefore model the body's shading an order of
    // magnitude too strongly, as well as far too dark.
    EXPECT_GT( oneContrast, 10.0 * allContrast )
         << "single scattering is no longer markedly more structured than the truth (" << oneContrast
         << " against " << allContrast << ")";

    // THE RELATION THAT KEEPS THE FLOOR. Judged against what the term actually stands for, the shipped
    // form — floor included — is CLOSER to the truth than the physically exact first-order occlusion is.
    // Measured 0.398 against 0.814. Anything proposing to remove CLOUD_SKY_LOWER_HEMISPHERE has to come
    // here and beat this number rather than argue from the geometry.
    EXPECT_LT( RmsDifference( r.Shipped, r.AllOrders ), RmsDifference( r.SingleOrder, r.AllOrders ) )
         << "the shipped occlusion is no longer the better of the two approximations to the all-orders "
            "sky light; if a third one is now in the file, this comparison wants rewriting around it";

    // THE MECHANISM, NAMED RATHER THAN ASSERTED AS A NUMBER: it is the ALBEDO that flattens the response,
    // so lowering it must bring the form back. A body that is genuinely absorbing does have a dark core
    // under a uniform sky; a cloud, at 0.98, does not.
    ReferenceLobe absorbing = lobe;
    absorbing.Albedo        = 0.5;

    const SkyResponses dark         = MeasureSkyResponses( absorbing, g, 5, kSkyPaths );
    const double       darkContrast = RelativeContrast( dark.AllOrders );

    std::printf( "[CloudSkyOcclusion] the same body at albedo 0.50: rel-contrast %.4f\n", darkContrast );

    EXPECT_GT( darkContrast, 4.0 * allContrast )
         << "the flatness is supposed to be the albedo's doing, but halving the albedo moved the contrast "
            "from "
         << allContrast << " only to " << darkContrast;

    // THE ENERGY BOUND Р19 WAS ASKED FOR, on the truth itself rather than on the model: a point in a body
    // lit by a surround of radiance 1 can receive no more than 1 and no less than 0, at every point.
    for ( double v : r.AllOrders )
    {
        EXPECT_GE( v, 0.0 );
        EXPECT_LE( v, 1.0 );
    }
}

TEST( CloudSkyOcclusion, TheSkyLightGAINSFormAtAPhysicalExtinctionAndTheTermGoesTheOtherWay )
{
    // THE CONDITION ON Р19'S REFUSAL, and it is the same tether D-32 found on the octave series: both
    // halves of this subsystem's lighting are calibrated for the SHIPPED 8 /km and both fail at a cumulus'
    // physical ~45, in opposite directions.
    //
    // Thicken the medium and the sky light acquires form — a path from the core needs many more
    // scatterings to reach the surround, so the albedo's 0.98 is applied many more times and the core
    // genuinely goes dark. Measured on the same lobe: the all-orders response's relative contrast rises
    // from 0.017 at 8 /km to 0.273 at 45. And the shipped term does the OPPOSITE: its stored
    // transmittance is already numerically zero at 8 /km, so at 45 it is pinned flat on
    // CLOUD_SKY_LOWER_HEMISPHERE and delivers a constant 0.500 with a contrast of 0.002.
    //
    // So "the sky light cannot carry form" is a statement about THIS medium, not about sky light. If the
    // extinction ever moves, this term is the second thing that has to be rebuilt, and the first is the
    // octave series (TheSeriesTracksTheConvergedMarchAtTheSHIPPEDExtinctionAndNotAtAPHYSICALOne).
    ReferenceLobe shippedMedium;
    ReferenceLobe physicalMedium;
    physicalMedium.SigmaPerKm = 45.0;

    const SkyResponses atShipped  = MeasureSkyResponses( shippedMedium, 0.8, 5, kSkyPaths );
    const SkyResponses atPhysical = MeasureSkyResponses( physicalMedium, 0.8, 5, kSkyPaths );

    const double truthAt8  = RelativeContrast( atShipped.AllOrders );
    const double truthAt45 = RelativeContrast( atPhysical.AllOrders );
    const double termAt8   = RelativeContrast( atShipped.Shipped );
    const double termAt45  = RelativeContrast( atPhysical.Shipped );

    std::printf( "[CloudSkyOcclusion] %-8s %14s %14s\n", "sigma", "truth contr", "term contr" );
    std::printf( "[CloudSkyOcclusion] %-8.0f %14.4f %14.4f\n", shippedMedium.SigmaPerKm, truthAt8, termAt8 );
    std::printf( "[CloudSkyOcclusion] %-8.0f %14.4f %14.4f\n", physicalMedium.SigmaPerKm, truthAt45, termAt45 );

    EXPECT_GT( truthAt45, 10.0 * truthAt8 )
         << "the sky light no longer gains form as the medium thickens (" << truthAt45 << " against " << truthAt8
         << "), which is the mechanism Р19's refusal rests on";

    EXPECT_LT( termAt45, termAt8 ) << "the shipped term is supposed to FLATTEN as the medium thickens -- it "
                                      "runs out of range at its own floor -- and it no longer does";
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
