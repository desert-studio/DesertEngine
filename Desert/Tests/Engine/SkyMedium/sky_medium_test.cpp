// The physical atmosphere's medium, phases and LUT parameterisations, tested against the relations
// that make the Phase 2 sky possible — not against hand-picked spot values.
//
// The functions under test are Engine/Content/Shaders/Common/SkyMedium.glslh compiled as C++
// (SkyMediumReference.hpp), i.e. the exact text the SkyTransmittanceLut / SkyMultiScatterLut compute
// passes run on the GPU. What is pinned, and why:
//
//   * the UV mappings ROUND-TRIP — a write-side/read-side mapping mismatch is a sky that is subtly
//     wrong everywhere and provably wrong nowhere;
//   * the densities are positive and fall where they must — an inverted exponential renders "something";
//   * the 40-step integrator agrees with a brute-force fine march AND with the closed form the
//     exponential profile admits — two independent references, so an error in the shared sampling
//     cannot vouch for itself;
//   * every phase function integrates to 1 over the sphere — a spurious 4pi is invisible in a picture
//     that is merely "too bright".

#include "SkyMediumReference.hpp"

// AtmosphereEnv for the two radii it publishes to consumers that do not bind the sky parameter buffer —
// asserted below against the shader's own SkyMakeAtmParams rather than trusted.
#include <Engine/Graphic/AtmosphereEnv.hpp>
#include <Engine/Graphic/SkyGroundTransmittance.hpp>
#include <Engine/Graphic/SkyPayload.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace Ref = Desert::Tests::SkyMediumRef;

namespace
{
    // Earth, through the REAL packing path: SkySettings' defaults -> PackSky -> SkyMakeAtmParams. The
    // tests therefore exercise the same bytes-in-lanes agreement the GPU depends on, including the
    // world-units-to-kilometres conversion of the planet radius.
    Ref::SkyAtmParams EarthParams()
    {
        const Desert::Graphic::SkySettings   sky{};
        const Desert::Graphic::SkyGpuPayload payload =
             Desert::Graphic::PackSky( glm::vec3( 0.0f, 1.0f, 0.0f ), sky );
        return Ref::SkyMakeAtmParams( payload.MediumRayleigh, payload.MediumMie, payload.MediumMieAbsorption,
                                      payload.MediumOzone, payload.MediumGround, payload.MediumTentPlanet );
    }
} // namespace

// ---------------------------------------------------------------------------------------------------
// The payload's medium block and the shader's params describe the same Earth
// ---------------------------------------------------------------------------------------------------

TEST( SkyMedium, PayloadMediumBlockUnpacksToTheAuthoredEarth )
{
    const Ref::SkyAtmParams p = EarthParams();

    // The one unit conversion: 636 000 000 world units (centimetres) -> 6360 km, inside the shader text.
    EXPECT_FLOAT_EQ( p.BottomRadiusKm, 6360.0f );
    EXPECT_FLOAT_EQ( p.TopRadiusKm, 6420.0f ); // bottom + the 60 km atmosphere height

    // UE's Earth coefficients, already collapsed from scale x colour by MakeSkySettings' convention
    // (the SkySettings defaults hold the products).
    EXPECT_NEAR( p.RayleighScattering.x, 0.005802f, 1e-6f );
    EXPECT_NEAR( p.RayleighScattering.y, 0.013558f, 1e-6f );
    EXPECT_NEAR( p.RayleighScattering.z, 0.033100f, 1e-6f );
    EXPECT_FLOAT_EQ( p.RayleighScaleKm, 8.0f );
    EXPECT_NEAR( p.MieScattering.x, 0.003996f, 1e-6f );
    EXPECT_NEAR( p.MieAbsorption.x, 0.000444f, 1e-6f );
    EXPECT_FLOAT_EQ( p.MieScaleKm, 1.2f );
    EXPECT_FLOAT_EQ( p.MiePhaseG, 0.8f );
    EXPECT_NEAR( p.OzoneAbsorption.y, 0.001881f, 1e-6f );
    EXPECT_FLOAT_EQ( p.OzoneTipAltitudeKm, 25.0f );
    EXPECT_FLOAT_EQ( p.OzoneTipValue, 1.0f );
    EXPECT_FLOAT_EQ( p.OzoneTentWidthKm, 15.0f );
    EXPECT_NEAR( p.GroundAlbedo.x, 0.401978f, 1e-6f );
    EXPECT_FLOAT_EQ( p.MultiScatteringFactor, 1.0f );
}

// ---------------------------------------------------------------------------------------------------
// The transmittance LUT mapping is an exactly invertible pair
// ---------------------------------------------------------------------------------------------------

TEST( SkyMedium, TransmittanceUvRoundTripsAcrossTheDomain )
{
    const Ref::SkyAtmParams p = EarthParams();

    // Every texel centre of the real 256x64 LUT plus the exact corners: uv -> (r, mu) -> uv.
    for ( int y = 0; y <= 64; ++y )
    {
        for ( int x = 0; x <= 256; ++x )
        {
            const glm::vec2 uv( static_cast<float>( x ) / 256.0f, static_cast<float>( y ) / 64.0f );

            const Ref::SkyTransmittanceLutCoord c =
                 Ref::SkyTransmittanceLutParamsFromUv( p.BottomRadiusKm, p.TopRadiusKm, uv );

            EXPECT_GE( c.ViewHeightKm, p.BottomRadiusKm - 1e-2f );
            EXPECT_LE( c.ViewHeightKm, p.TopRadiusKm + 1e-2f );
            EXPECT_GE( c.ViewZenithCos, -1.0f );
            EXPECT_LE( c.ViewZenithCos, 1.0f );

            const glm::vec2 back = Ref::SkyTransmittanceLutUvFromParams( p.BottomRadiusKm, p.TopRadiusKm,
                                                                         c.ViewHeightKm, c.ViewZenithCos );

            EXPECT_NEAR( back.x, uv.x, 2e-3f ) << "uv=(" << uv.x << "," << uv.y << ")";
            EXPECT_NEAR( back.y, uv.y, 2e-3f ) << "uv=(" << uv.x << "," << uv.y << ")";
        }
    }
}

TEST( SkyMedium, TransmittanceParamsRoundTripThroughUv )
{
    const Ref::SkyAtmParams p = EarthParams();

    // (r, mu) -> uv -> (r, mu), over the mapping's actual domain: mu from the HORIZON cosine
    // (mu_h = -rho/r, where the ray grazes the planet) up to 1. Below the horizon the ray enters the
    // planet, the transmittance is identically zero, and Bruneton's distance parameterisation
    // deliberately folds that half onto uv.x = 1 — it is not part of the bijection and no consumer
    // reads it. r stops a kilometre short of the top on purpose: as r -> top the whole upward mu range
    // collapses into a vanishing distance interval (d -> 0 for every mu), so mu is numerically
    // unrecoverable there — and physically irrelevant, the transmittance of that last sliver being ~1
    // in every direction.
    for ( int ri = 0; ri <= 32; ++ri )
    {
        const float r =
             p.BottomRadiusKm + ( static_cast<float>( ri ) / 32.0f ) * ( p.TopRadiusKm - 1.0f - p.BottomRadiusKm );

        const float rho       = std::sqrt( std::max( ( r - p.BottomRadiusKm ) * ( r + p.BottomRadiusKm ), 0.0f ) );
        const float muHorizon = -rho / r;

        for ( int mi = 0; mi <= 64; ++mi )
        {
            const float mu = muHorizon + ( 1.0f - muHorizon ) * static_cast<float>( mi ) / 64.0f;

            const glm::vec2 uv = Ref::SkyTransmittanceLutUvFromParams( p.BottomRadiusKm, p.TopRadiusKm, r, mu );
            const Ref::SkyTransmittanceLutCoord c =
                 Ref::SkyTransmittanceLutParamsFromUv( p.BottomRadiusKm, p.TopRadiusKm, uv );

            EXPECT_NEAR( c.ViewHeightKm, r, 0.05f ) << "r=" << r << " mu=" << mu;
            // 6e-3: float reconstruction error at planetary magnitudes (the mu formula divides
            // ~1e5-sized cancellations by 2*r*d). The LUT's own uv.x texel is 1/256 = 3.9e-3 of the
            // distance domain, so this bound is about one texel — the mapping cannot be more exact
            // than the texture it addresses.
            EXPECT_NEAR( c.ViewZenithCos, mu, 6e-3f ) << "r=" << r << " mu=" << mu;
        }
    }
}

TEST( SkyMedium, MultiScatterMappingAndTexelRemapAreExactInverses )
{
    const Ref::SkyAtmParams p = EarthParams();

    for ( int i = 0; i <= 32; ++i )
    {
        const float unit = static_cast<float>( i ) / 32.0f;

        // The texel-centre remap: unit -> uv -> unit, and texel centres land on exact unit values.
        const float uv = Ref::SkyUnitToTexelUv( unit, 32.0f );
        EXPECT_NEAR( Ref::SkyTexelUvToUnit( uv, 32.0f ), unit, 1e-6f );

        for ( int j = 0; j <= 32; ++j )
        {
            const float unitY = static_cast<float>( j ) / 32.0f;

            const Ref::SkyMultiScatterLutCoord c =
                 Ref::SkyMultiScatterParamsFromUnit( p.BottomRadiusKm, p.TopRadiusKm, unit, unitY );
            const glm::vec2 back = Ref::SkyMultiScatterUnitFromParams( p.BottomRadiusKm, p.TopRadiusKm,
                                                                       c.ViewHeightKm, c.SunZenithCos );

            EXPECT_NEAR( back.x, unit, 1e-5f );
            EXPECT_NEAR( back.y, unitY, 1e-5f );

            // The lift off the exact surface that keeps the ground march non-degenerate.
            EXPECT_GE( c.ViewHeightKm, p.BottomRadiusKm + Ref::SKY_PLANET_RADIUS_OFFSET_KM - 1e-4f );
            EXPECT_LE( c.ViewHeightKm, p.TopRadiusKm + 1e-4f );
        }
    }

    // Texel 0's centre reads unit 0 and the last texel's centre reads unit 1 — the property that makes
    // the write side (SkyTexelUvToUnit of pixel centres) and a future linear-sampling read side agree.
    EXPECT_NEAR( Ref::SkyTexelUvToUnit( 0.5f / 32.0f, 32.0f ), 0.0f, 1e-6f );
    EXPECT_NEAR( Ref::SkyTexelUvToUnit( 31.5f / 32.0f, 32.0f ), 1.0f, 1e-6f );
}

// ---------------------------------------------------------------------------------------------------
// The medium behaves like an atmosphere
// ---------------------------------------------------------------------------------------------------

TEST( SkyMedium, DensitiesArePositiveAndFallWhereTheyMust )
{
    const Ref::SkyAtmParams p = EarthParams();

    float previousRayleigh = 2.0f;
    float previousMie      = 2.0f;
    for ( int i = 0; i <= 60; ++i )
    {
        const float altitude = static_cast<float>( i );

        const float rayleigh = Ref::SkyDensityRayleigh( p, altitude );
        const float mie      = Ref::SkyDensityMie( p, altitude );
        const float ozone    = Ref::SkyDensityOzone( p, altitude );

        EXPECT_GT( rayleigh, 0.0f );
        EXPECT_GT( mie, 0.0f );
        EXPECT_GE( ozone, 0.0f );

        // The exponential profiles fall strictly with altitude — an inverted sign here still renders a
        // sky, just one whose air thickens toward space.
        EXPECT_LT( rayleigh, previousRayleigh ) << "altitude " << altitude;
        EXPECT_LT( mie, previousMie ) << "altitude " << altitude;
        previousRayleigh = rayleigh;
        previousMie      = mie;

        // Extinction can never be below scattering: absorption only ever adds.
        const Ref::SkyMediumSample m = Ref::SkySampleMedium( p, altitude );
        EXPECT_GE( m.Extinction.x, m.Scattering.x );
        EXPECT_GE( m.Extinction.y, m.Scattering.y );
        EXPECT_GE( m.Extinction.z, m.Scattering.z );
    }

    // The ozone tent: peak value at the tip, zero at and beyond tip +/- width, symmetric flanks.
    EXPECT_FLOAT_EQ( Ref::SkyDensityOzone( p, p.OzoneTipAltitudeKm ), p.OzoneTipValue );
    EXPECT_FLOAT_EQ( Ref::SkyDensityOzone( p, p.OzoneTipAltitudeKm - p.OzoneTentWidthKm ), 0.0f );
    EXPECT_FLOAT_EQ( Ref::SkyDensityOzone( p, p.OzoneTipAltitudeKm + p.OzoneTentWidthKm ), 0.0f );
    EXPECT_FLOAT_EQ( Ref::SkyDensityOzone( p, 0.0f ), 0.0f );
    EXPECT_NEAR( Ref::SkyDensityOzone( p, p.OzoneTipAltitudeKm - 5.0f ),
                 Ref::SkyDensityOzone( p, p.OzoneTipAltitudeKm + 5.0f ), 1e-6f );
}

// ---------------------------------------------------------------------------------------------------
// The 40-step integrator against two independent references
// ---------------------------------------------------------------------------------------------------

TEST( SkyMedium, TransmittanceMatchesABruteForceFineMarch )
{
    const Ref::SkyAtmParams p = EarthParams();

    // The LUT's own budget (40 steps, the shader's kTransmittanceSampleCount) against a 20000-step
    // march of the SAME text. Compared as TRANSMITTANCE, the quantity the LUT stores: near the horizon
    // the optical depth grows large while its exp shrinks to nothing, so an absolute bound on T is the
    // honest statement of "the picture cannot tell them apart".
    const float testMus[]       = { 1.0f, 0.7f, 0.3f, 0.1f, 0.0f, -0.05f };
    const float testAltitudes[] = { 0.0f, 0.5f, 2.0f, 8.0f, 30.0f, 59.0f };

    for ( const float altitude : testAltitudes )
    {
        for ( const float mu : testMus )
        {
            const float r = p.BottomRadiusKm + altitude;

            const glm::vec3 coarse = Ref::SkyTransmittanceToTop( p, r, mu, 40 );
            const glm::vec3 fine   = Ref::SkyTransmittanceToTop( p, r, mu, 20000 );

            EXPECT_NEAR( coarse.x, fine.x, 0.015f ) << "alt=" << altitude << " mu=" << mu;
            EXPECT_NEAR( coarse.y, fine.y, 0.015f ) << "alt=" << altitude << " mu=" << mu;
            EXPECT_NEAR( coarse.z, fine.z, 0.015f ) << "alt=" << altitude << " mu=" << mu;
        }
    }
}

TEST( SkyMedium, VerticalOpticalDepthMatchesTheClosedForm )
{
    // A second, INDEPENDENT reference: for a vertical ray the exponential profile integrates in closed
    // form, integral of sigma0 * exp(-h/H) from h0 to hTop = sigma0 * H * (exp(-h0/H) - exp(-hTop/H)).
    // The march never sees this formula, so agreement here is not the sampling vouching for itself.
    const Ref::SkyAtmParams p = EarthParams();

    for ( const float h0 : { 0.0f, 1.0f, 4.0f, 16.0f, 40.0f } )
    {
        const float hTop = p.TopRadiusKm - p.BottomRadiusKm;

        const float rayleighClosed =
             p.RayleighScaleKm * ( std::exp( -h0 / p.RayleighScaleKm ) - std::exp( -hTop / p.RayleighScaleKm ) );
        const float mieClosed =
             p.MieScaleKm * ( std::exp( -h0 / p.MieScaleKm ) - std::exp( -hTop / p.MieScaleKm ) );

        // The ozone tent's closed form: the area of the clipped triangle above h0. Piecewise, so it is
        // evaluated by fine summation of the DENSITY FORMULA only (not the march) — still independent
        // of SkyOpticalDepthToTop's sampling.
        double       ozoneArea = 0.0;
        const double ozoneStep = 0.001;
        for ( double h = h0; h < hTop; h += ozoneStep )
            ozoneArea +=
                 static_cast<double>( Ref::SkyDensityOzone( p, static_cast<float>( h + ozoneStep * 0.5 ) ) ) *
                 ozoneStep;

        const glm::vec3 expected = p.RayleighScattering * rayleighClosed +
                                   ( p.MieScattering + p.MieAbsorption ) * mieClosed +
                                   p.OzoneAbsorption * static_cast<float>( ozoneArea );

        // A vertical ray's radius grows exactly linearly with t, so even the curved-geometry march
        // reduces to the flat integral; 4000 steps of the 60 km column pins it tightly.
        const glm::vec3 marched = Ref::SkyOpticalDepthToTop( p, p.BottomRadiusKm + h0, 1.0f, 4000 );

        EXPECT_NEAR( marched.x, expected.x, 2e-4f ) << "h0=" << h0;
        EXPECT_NEAR( marched.y, expected.y, 2e-4f ) << "h0=" << h0;
        EXPECT_NEAR( marched.z, expected.z, 2e-4f ) << "h0=" << h0;
    }
}

TEST( SkyMedium, TransmittanceIsBoundedAndGrowsWithAltitude )
{
    const Ref::SkyAtmParams p = EarthParams();

    glm::vec3 previous( 0.0f );
    for ( int i = 0; i <= 20; ++i )
    {
        const float     altitude = 3.0f * static_cast<float>( i );
        const glm::vec3 t        = Ref::SkyTransmittanceToTop( p, p.BottomRadiusKm + altitude, 1.0f, 40 );

        // A transmittance is a probability of survival: (0, 1], never amplifying.
        EXPECT_GT( t.x, 0.0f );
        EXPECT_LE( t.x, 1.0f );
        EXPECT_LE( t.y, 1.0f );
        EXPECT_LE( t.z, 1.0f );

        // Less air above a higher start: T must not shrink as the ray starts higher. The monotonicity
        // catches a flipped altitude sign that a spot value never would.
        EXPECT_GE( t.x, previous.x ) << "altitude " << altitude;
        EXPECT_GE( t.y, previous.y ) << "altitude " << altitude;
        EXPECT_GE( t.z, previous.z ) << "altitude " << altitude;
        previous = t;
    }

    // Red outlives blue through Rayleigh air — the physical ordering that makes sunsets red. From the
    // ground toward the horizon the effect is strongest.
    const glm::vec3 horizon = Ref::SkyTransmittanceToTop( p, p.BottomRadiusKm + 0.2f, 0.0f, 40 );
    EXPECT_GT( horizon.x, horizon.y );
    EXPECT_GT( horizon.y, horizon.z );
}

// ---------------------------------------------------------------------------------------------------
// The sun-light coupling: transmittance from the ground toward the sun (UE's PrepareSunLightProxy)
//
// This is the quantity the directional light's colour is multiplied by in SkyModel::PhysicalAtmosphere,
// so the relations below are the relations a viewer sees on lit geometry: a neutral, barely dimmed sun
// overhead, a red and much darker one at the horizon, and a smooth walk between the two.
//
// Graphic::SunTransmittanceAtGround is the ENGINE's function, compiled into this suite (premake5.lua
// says why). It is tested here rather than mirrored because the whole point of the arrangement is that
// there is one implementation: it packs the SkySettings exactly as the GPU reads them and marches the
// same text as the LUT.
// ---------------------------------------------------------------------------------------------------

namespace
{
    glm::vec3 SunDirectionAtElevation( float degrees )
    {
        const float rad = degrees * 3.14159265358979323846f / 180.0f;
        return glm::vec3( std::cos( rad ), std::sin( rad ), 0.0f );
    }
} // namespace

TEST( SkyGroundTransmittance, IsBoundedAndDimsAsTheSunDescends )
{
    const Desert::Graphic::SkySettings sky{};

    glm::vec3 previous( 2.0f );
    for ( const float elevation : { 90.0f, 60.0f, 45.0f, 30.0f, 20.0f, 10.0f, 5.0f, 2.0f, 1.0f, 0.0f } )
    {
        const glm::vec3 t = Desert::Graphic::SunTransmittanceAtGround( sky, SunDirectionAtElevation( elevation ) );

        // A transmittance is a probability of survival — never negative, never amplifying. An
        // amplifying one here would BRIGHTEN the sun at sunset, which is the exact opposite of the
        // effect this coupling exists for.
        for ( int c = 0; c < 3; ++c )
        {
            EXPECT_GT( t[c], 0.0f ) << "elevation " << elevation << " channel " << c;
            EXPECT_LE( t[c], 1.0f ) << "elevation " << elevation << " channel " << c;
        }

        // More air on a slanted path than on a vertical one: strictly monotone in elevation. A flipped
        // sign somewhere in the mapping still produces "a tint", just the wrong one at the wrong hour.
        EXPECT_LT( t.x, previous.x ) << "elevation " << elevation;
        EXPECT_LT( t.y, previous.y ) << "elevation " << elevation;
        EXPECT_LT( t.z, previous.z ) << "elevation " << elevation;
        previous = t;
    }

    // The zenith sun is barely touched and the horizon sun is nearly gone — the two ends the shots are
    // judged at (noon stays neutral, golden hour reddens).
    const glm::vec3 zenith = Desert::Graphic::SunTransmittanceAtGround( sky, SunDirectionAtElevation( 90.0f ) );
    EXPECT_GT( zenith.x, 0.9f );
    EXPECT_GT( zenith.z, 0.7f );

    const glm::vec3 horizon = Desert::Graphic::SunTransmittanceAtGround( sky, SunDirectionAtElevation( 0.0f ) );
    EXPECT_LT( horizon.x, zenith.x );
    EXPECT_LT( horizon.z, 0.01f );
}

TEST( SkyGroundTransmittance, RedensMonotonicallyAsTheSunDescends )
{
    const Desert::Graphic::SkySettings sky{};

    // R/B is the reddening, in one number. It must GROW without exception as the sun goes down: red
    // survives the long path that blue does not, which is the whole reason a sunset is a sunset.
    float previousRatio = 0.0f;
    for ( const float elevation : { 90.0f, 60.0f, 40.0f, 25.0f, 15.0f, 10.0f, 6.0f, 3.0f, 1.0f } )
    {
        const glm::vec3 t = Desert::Graphic::SunTransmittanceAtGround( sky, SunDirectionAtElevation( elevation ) );

        const float ratio = t.x / t.z;
        EXPECT_GT( ratio, previousRatio ) << "elevation " << elevation << " ratio " << ratio;
        previousRatio = ratio;

        // Red always outlives green, which always outlives blue: the ordering Rayleigh's 1/lambda^4
        // imposes, and the one that decides WHICH way the light shifts.
        EXPECT_GT( t.x, t.y ) << "elevation " << elevation;
        EXPECT_GT( t.y, t.z ) << "elevation " << elevation;
    }

    // Overhead the sun is close to neutral (a few per cent of warm cast), at golden hour it is
    // unmistakably warm. Numbers, not adjectives, so "noon must stay neutral" is a pinned claim.
    const glm::vec3 noon = Desert::Graphic::SunTransmittanceAtGround( sky, SunDirectionAtElevation( 90.0f ) );
    EXPECT_LT( noon.x / noon.z, 1.35f );

    const glm::vec3 golden = Desert::Graphic::SunTransmittanceAtGround( sky, SunDirectionAtElevation( 5.0f ) );
    EXPECT_GT( golden.x / golden.z, 3.0f );
}

TEST( SkyGroundTransmittance, AgreesWithTheTransmittanceLutItShares )
{
    // THE TWO-IMPLEMENTATIONS PROPERTY. The same quantity reaches a frame twice: through this CPU
    // evaluation (the light's colour) and through the 256x64 LUT the GPU samples (the sky's own
    // scattering, the sun disc, the aerial perspective). They are the same text, but not the same path:
    // one marches (r, mu) directly, the other marches Bruneton's uv grid and then interpolates. If those
    // disagree, the sun on the geometry and the sun in the sky part ways — and neither is provably
    // wrong on its own.
    const Desert::Graphic::SkySettings sky{};

    const Ref::SkyAtmParams p = EarthParams();

    // The LUT exactly as SkyTransmittanceLut.shader writes it: one texel per (uv) centre, the shared
    // sample budget, no texel-centre remap (Bruneton's mapping already puts the domain ends on the
    // edge texels).
    constexpr int kWidth  = 256;
    constexpr int kHeight = 64;

    std::vector<glm::vec3> lut( static_cast<size_t>( kWidth * kHeight ) );
    for ( int y = 0; y < kHeight; ++y )
    {
        for ( int x = 0; x < kWidth; ++x )
        {
            const glm::vec2 uv( ( static_cast<float>( x ) + 0.5f ) / kWidth,
                                ( static_cast<float>( y ) + 0.5f ) / kHeight );

            const Ref::SkyTransmittanceLutCoord c =
                 Ref::SkyTransmittanceLutParamsFromUv( p.BottomRadiusKm, p.TopRadiusKm, uv );
            lut[static_cast<size_t>( y * kWidth + x )] = Ref::SkyTransmittanceToTop(
                 p, c.ViewHeightKm, c.ViewZenithCos, Ref::SKY_TRANSMITTANCE_SAMPLE_COUNT );
        }
    }

    // ... and read exactly as a linear sampler would: bilinear on the texel grid, clamped at the edges.
    const auto sample = [&lut]( glm::vec2 uv ) -> glm::vec3
    {
        const float fx = glm::clamp( uv.x * kWidth - 0.5f, 0.0f, static_cast<float>( kWidth - 1 ) );
        const float fy = glm::clamp( uv.y * kHeight - 0.5f, 0.0f, static_cast<float>( kHeight - 1 ) );

        const int x0 = static_cast<int>( fx );
        const int y0 = static_cast<int>( fy );
        const int x1 = std::min( x0 + 1, kWidth - 1 );
        const int y1 = std::min( y0 + 1, kHeight - 1 );

        const float tx = fx - static_cast<float>( x0 );
        const float ty = fy - static_cast<float>( y0 );

        const glm::vec3 a = lut[static_cast<size_t>( y0 * kWidth + x0 )];
        const glm::vec3 b = lut[static_cast<size_t>( y0 * kWidth + x1 )];
        const glm::vec3 c = lut[static_cast<size_t>( y1 * kWidth + x0 )];
        const glm::vec3 d = lut[static_cast<size_t>( y1 * kWidth + x1 )];

        return glm::mix( glm::mix( a, b, tx ), glm::mix( c, d, tx ), ty );
    };

    for ( const float elevation : { 90.0f, 60.0f, 45.0f, 30.0f, 20.0f, 12.0f, 8.0f, 5.0f, 3.0f, 1.0f } )
    {
        const glm::vec3 towardSun = SunDirectionAtElevation( elevation );

        const glm::vec3 cpu = Desert::Graphic::SunTransmittanceAtGround( sky, towardSun );

        const glm::vec2 uv = Ref::SkyTransmittanceLutUvFromParams(
             p.BottomRadiusKm, p.TopRadiusKm, p.BottomRadiusKm + Ref::SKY_PLANET_RADIUS_OFFSET_KM, towardSun.y );
        const glm::vec3 gpu = sample( uv );

        // 0.01 of a transmittance is well under one display level on an 8-bit output, and it is the
        // honest bound: the LUT is 256x64 texels of a function that falls off a cliff at the horizon,
        // so the interpolation error IS the disagreement, and it must stay smaller than the effect.
        EXPECT_NEAR( cpu.x, gpu.x, 0.01f ) << "elevation " << elevation;
        EXPECT_NEAR( cpu.y, gpu.y, 0.01f ) << "elevation " << elevation;
        EXPECT_NEAR( cpu.z, gpu.z, 0.01f ) << "elevation " << elevation;
    }
}

// ---------------------------------------------------------------------------------------------------
// Phase functions: normalized over the sphere, forward-peaked where g says so
// ---------------------------------------------------------------------------------------------------

namespace
{
    // Integral over the sphere = 2pi * integral over cosTheta in [-1, 1] (azimuthal symmetry),
    // midpoint rule. 200000 cells resolves even the g = 0.9 forward spike.
    template <typename Phase>
    double IntegrateOverSphere( Phase&& phase )
    {
        const int    cells = 200000;
        const double dc    = 2.0 / cells;
        double       sum   = 0.0;
        for ( int i = 0; i < cells; ++i )
        {
            const double c = -1.0 + ( i + 0.5 ) * dc;
            sum += static_cast<double>( phase( static_cast<float>( c ) ) ) * dc;
        }
        return 2.0 * 3.14159265358979323846 * sum;
    }
} // namespace

TEST( SkyMedium, PhaseFunctionsIntegrateToOneOverTheSphere )
{
    EXPECT_NEAR( IntegrateOverSphere( []( float ) { return Ref::SkyPhaseUniform(); } ), 1.0, 1e-3 );
    EXPECT_NEAR( IntegrateOverSphere( []( float c ) { return Ref::SkyPhaseRayleigh( c ); } ), 1.0, 1e-3 );

    for ( const float g : { 0.0f, 0.3f, 0.8f, 0.9f } )
    {
        EXPECT_NEAR( IntegrateOverSphere( [g]( float c ) { return Ref::SkyPhaseHenyeyGreenstein( g, c ); } ), 1.0,
                     2e-3 )
             << "HG g=" << g;
        EXPECT_NEAR( IntegrateOverSphere( [g]( float c ) { return Ref::SkyPhaseCornetteShanks( g, c ); } ), 1.0,
                     2e-3 )
             << "CS g=" << g;
    }
}

TEST( SkyMedium, PositiveAnisotropyScattersForward )
{
    // The convention pin: cosTheta = +1 is forward. A flipped sign here puts the sun's halo at the
    // antisolar point — a bug a normalization test cannot see, because the integral is symmetric in it.
    EXPECT_GT( Ref::SkyPhaseHenyeyGreenstein( 0.8f, 1.0f ), Ref::SkyPhaseHenyeyGreenstein( 0.8f, -1.0f ) );
    EXPECT_GT( Ref::SkyPhaseCornetteShanks( 0.8f, 1.0f ), Ref::SkyPhaseCornetteShanks( 0.8f, -1.0f ) );

    // g = 0 collapses both lobes to isotropy-like symmetry.
    EXPECT_NEAR( Ref::SkyPhaseHenyeyGreenstein( 0.0f, 0.5f ), Ref::SkyPhaseUniform(), 1e-6f );
}

// ---------------------------------------------------------------------------------------------------
// Sphere sampling and ray/sphere geometry
// ---------------------------------------------------------------------------------------------------

TEST( SkyMedium, UniformSphereDirectionsAreUnitAndBalanced )
{
    glm::vec3 mean( 0.0f );
    for ( int i = 0; i < 8; ++i )
    {
        for ( int j = 0; j < 8; ++j )
        {
            const float u = ( static_cast<float>( i ) + 0.5f ) / 8.0f;
            const float v = ( static_cast<float>( j ) + 0.5f ) / 8.0f;

            const glm::vec3 dir = Ref::SkyUniformSphereDirection( u, v );
            EXPECT_NEAR( glm::length( dir ), 1.0f, 1e-5f );
            mean += dir;
        }
    }

    // The 8x8 grid is symmetric in azimuth and in zenith cosine, so a uniform distribution sums to the
    // origin. A biased mapping (the classic non-area-uniform acos-less mistake) does not.
    mean /= 64.0f;
    EXPECT_NEAR( mean.x, 0.0f, 1e-5f );
    EXPECT_NEAR( mean.y, 0.0f, 1e-5f );
    EXPECT_NEAR( mean.z, 0.0f, 1e-5f );
}

TEST( SkyMedium, RaySphereDistancesAgreeWithTheGeometry )
{
    const Ref::SkyAtmParams p = EarthParams();

    // Straight up from the ground: exactly the atmosphere height. Straight down from 1 km: exactly 1 km.
    EXPECT_NEAR( Ref::SkyDistanceToTop( p.BottomRadiusKm, 1.0f, p.TopRadiusKm ), p.TopRadiusKm - p.BottomRadiusKm,
                 1e-2f );
    EXPECT_NEAR( Ref::SkyDistanceToBottom( p.BottomRadiusKm + 1.0f, -1.0f, p.BottomRadiusKm ), 1.0f, 1e-2f );

    // From the top looking up there is nothing left to cross.
    EXPECT_NEAR( Ref::SkyDistanceToTop( p.TopRadiusKm, 1.0f, p.TopRadiusKm ), 0.0f, 1e-2f );

    // A horizontal ray from above the surface never hits the planet; a downward one does, and the
    // ground is always nearer than the top beyond it.
    EXPECT_FALSE( Ref::SkyIntersectsGround( p.BottomRadiusKm + 1.0f, 0.0f, p.BottomRadiusKm ) );
    EXPECT_TRUE( Ref::SkyIntersectsGround( p.BottomRadiusKm + 1.0f, -0.5f, p.BottomRadiusKm ) );
    EXPECT_LT( Ref::SkyDistanceToBottom( p.BottomRadiusKm + 1.0f, -0.5f, p.BottomRadiusKm ),
               Ref::SkyDistanceToTop( p.BottomRadiusKm + 1.0f, -0.5f, p.TopRadiusKm ) );

    // The radius along a vertical ray is linear in t — the anchor for the closed-form test above.
    EXPECT_NEAR( Ref::SkyRadiusAlongRay( p.BottomRadiusKm, 1.0f, 12.5f ), p.BottomRadiusKm + 12.5f, 1e-2f );
}

// ---------------------------------------------------------------------------------------------------
// READING THE TRANSMITTANCE LUT AT A SAMPLE'S OWN ALTITUDE (Р14)
//
// The volumetric cloud march's default sun colour is `OuterSpaceIlluminance x T(ground)` — one colour for
// a shell that is kilometres tall. With VolumetricCloudData::PerSampleAtmosphereTransmittance on, the
// packer sends the OUTER-SPACE illuminance and both marches of the field multiply by T at the sample's
// own altitude instead, through SkyRadiusAtAltitude and SkyTransmittanceLutUvClamped below.
//
// EVERYTHING HERE IS A RELATION AND NOT A SPOT VALUE, because every failure this reader can have is a
// relation failure: an altitude taken from the wrong shell's floor, a zenith taken from the world's +Y
// instead of the sample's own, a uv that saturates into a REPEAT sampler's wrap. None of those changes a
// single value into an obviously wrong one — they change the SHAPE of the answer over altitude.
// ---------------------------------------------------------------------------------------------------

namespace
{
    // SkyboxRenderer::kTransmittanceLutWidth / kTransmittanceLutHeight, which is what the march's own
    // textureSize() reports to it.
    constexpr int kLutWidth  = 256;
    constexpr int kLutHeight = 64;

    // THE LUT ITSELF, marched by the same text the GPU marches. Built once and reused, because a table of
    // 16 384 forty-step integrals is the expensive part of this file and every test below wants the same
    // one.
    const std::vector<glm::vec3>& TransmittanceTable( const Ref::SkyAtmParams& p )
    {
        static const std::vector<glm::vec3> table = [&p]
        {
            std::vector<glm::vec3> t( static_cast<size_t>( kLutWidth ) * kLutHeight );
            for ( int y = 0; y < kLutHeight; ++y )
            {
                for ( int x = 0; x < kLutWidth; ++x )
                {
                    // The fill's own texel-centre convention: Programs/Sky/SkyTransmittanceLut.shader
                    // writes texel (x, y) for the parameters at uv = (x + 0.5) / size.
                    const glm::vec2 uv( ( static_cast<float>( x ) + 0.5f ) / static_cast<float>( kLutWidth ),
                                        ( static_cast<float>( y ) + 0.5f ) / static_cast<float>( kLutHeight ) );
                    const Ref::SkyTransmittanceLutCoord c =
                         Ref::SkyTransmittanceLutParamsFromUv( p.BottomRadiusKm, p.TopRadiusKm, uv );
                    t[static_cast<size_t>( y ) * kLutWidth + x] = Ref::SkyTransmittanceToTop(
                         p, c.ViewHeightKm, c.ViewZenithCos, Ref::SKY_TRANSMITTANCE_SAMPLE_COUNT );
                }
            }
            return t;
        }();
        return table;
    }

    // A BILINEAR FETCH WITH REPEAT WRAPPING, which is the only kind this engine has: VulkanImage.cpp
    // creates every sampler REPEAT on all three axes and there is no CLAMP_TO_EDGE to fall back on. That
    // is why this helper wraps rather than clamps — modelling the sampler honestly is the whole point,
    // and a helper that clamped would make the texel-centre guard untestable by hiding its absence.
    glm::vec3 SampleLutRepeat( const Ref::SkyAtmParams& p, glm::vec2 uv )
    {
        const std::vector<glm::vec3>& table = TransmittanceTable( p );

        auto wrap = []( int i, int n ) { return ( ( i % n ) + n ) % n; };

        const float fx = uv.x * static_cast<float>( kLutWidth ) - 0.5f;
        const float fy = uv.y * static_cast<float>( kLutHeight ) - 0.5f;

        const int   x0 = static_cast<int>( std::floor( fx ) );
        const int   y0 = static_cast<int>( std::floor( fy ) );
        const float tx = fx - static_cast<float>( x0 );
        const float ty = fy - static_cast<float>( y0 );

        auto texel = [&]( int x, int y )
        { return table[static_cast<size_t>( wrap( y, kLutHeight ) ) * kLutWidth + wrap( x, kLutWidth )]; };

        const glm::vec3 a = glm::mix( texel( x0, y0 ), texel( x0 + 1, y0 ), tx );
        const glm::vec3 b = glm::mix( texel( x0, y0 + 1 ), texel( x0 + 1, y0 + 1 ), tx );
        return glm::mix( a, b, ty );
    }

    // What the marches do, in one place: the sample's radius, the reader's uv, the fetch.
    glm::vec3 PerSampleTransmittance( const Ref::SkyAtmParams& p, float altitudeKm, float sunZenithCos )
    {
        const float     viewHeightKm = Ref::SkyRadiusAtAltitude( p.BottomRadiusKm, p.TopRadiusKm, altitudeKm );
        const glm::vec2 uv           = Ref::SkyTransmittanceLutUvClamped(
             p.BottomRadiusKm, p.TopRadiusKm, viewHeightKm, sunZenithCos,
             glm::vec2( static_cast<float>( kLutWidth ), static_cast<float>( kLutHeight ) ) );
        return SampleLutRepeat( p, uv );
    }
} // namespace

TEST( SkyPerSampleSunTransmittance, TheSampleRadiusStaysInsideTheShellTheMappingIsDefinedOver )
{
    // A DOMAIN GUARD, not a correction. The mapping's rho is sqrt((r - Rb)(r + Rb)) and its d is a
    // distance to the top shell: a radius below the ground or above the atmosphere makes both imaginary,
    // and a cloud layer authored at 70 km under a 60 km atmosphere is an AUTHORABLE state rather than a
    // bug in the reader. The shadow ray can also step below its own start when that start is on the
    // layer's base, which is where a negative altitude comes from.
    const Ref::SkyAtmParams p = EarthParams();

    for ( const float altitudeKm : { -5.0f, -0.001f, 0.0f, 2.0f, 12.0f, 59.0f, 60.0f, 1000.0f } )
    {
        const float r = Ref::SkyRadiusAtAltitude( p.BottomRadiusKm, p.TopRadiusKm, altitudeKm );
        EXPECT_GE( r, p.BottomRadiusKm ) << "altitude " << altitudeKm;
        EXPECT_LE( r, p.TopRadiusKm ) << "altitude " << altitudeKm;
    }

    // And inside the shell it is the altitude, exactly — the guard must not be a clamp that also rounds.
    for ( const float altitudeKm : { 0.0f, 0.5f, 4.0f, 20.0f } )
        EXPECT_FLOAT_EQ( Ref::SkyRadiusAtAltitude( p.BottomRadiusKm, p.TopRadiusKm, altitudeKm ),
                         p.BottomRadiusKm + altitudeKm );
}

TEST( SkyPerSampleSunTransmittance, TheReaderNeverHandsARepeatSamplerAUvThatWraps )
{
    // THE RELATION BETWEEN THE READER AND THE SAMPLER IT FEEDS. Every sampler this engine creates is
    // REPEAT (VulkanImage.cpp asserts it), and the mapping saturates at BOTH ends under conditions an
    // ordinary scene reaches: uv.x = 1 for every sun below the sample's horizon, uv.x = 0 for a sun at
    // the sample's own zenith. A bilinear fetch at exactly 0 or 1 blends the two ENDS of the table, which
    // are the brightest and the darkest transmittance it holds.
    //
    // Half a texel in is what CLAMP_TO_EDGE would give, and asserting the band rather than the fetch is
    // what makes this a statement about the reader instead of about one sun.
    const Ref::SkyAtmParams p = EarthParams();

    const glm::vec2 size( static_cast<float>( kLutWidth ), static_cast<float>( kLutHeight ) );
    const glm::vec2 texel = 0.5f / size;

    for ( const float altitudeKm : { 0.0f, 0.01f, 2.0f, 5.0f, 12.0f, 59.9f } )
    {
        const float viewHeightKm = Ref::SkyRadiusAtAltitude( p.BottomRadiusKm, p.TopRadiusKm, altitudeKm );

        for ( const float elevation : { 90.0f, 60.0f, 30.0f, 5.0f, 0.5f, 0.0f, -0.5f, -10.0f, -90.0f } )
        {
            const float     mu = SunDirectionAtElevation( elevation ).y;
            const glm::vec2 uv =
                 Ref::SkyTransmittanceLutUvClamped( p.BottomRadiusKm, p.TopRadiusKm, viewHeightKm, mu, size );

            EXPECT_GE( uv.x, texel.x ) << "altitude " << altitudeKm << ", elevation " << elevation;
            EXPECT_LE( uv.x, 1.0f - texel.x ) << "altitude " << altitudeKm << ", elevation " << elevation;
            EXPECT_GE( uv.y, texel.y ) << "altitude " << altitudeKm << ", elevation " << elevation;
            EXPECT_LE( uv.y, 1.0f - texel.y ) << "altitude " << altitudeKm << ", elevation " << elevation;
        }
    }
}

TEST( SkyPerSampleSunTransmittance, MoreAirBetweenTheSampleAndSpaceMeansLessSunReachesIt )
{
    // THE RELATION THE WHOLE FEATURE IS, and it is a MONOTONICITY rather than a value: a sample that
    // stands higher has strictly less air above it, so the sun that reaches it cannot be dimmer. Read
    // through the FULL reader — the radius guard, the mapping, the texel-centre clamp and a REPEAT
    // bilinear fetch of a real LUT — so that any of those four getting it wrong shows here.
    //
    // Checked against a saboteur: dropping the texel clamp, inverting the altitude, or basing it on the
    // top radius instead of the bottom each turn this red, and none of them changes a single spot value
    // into anything that looks wrong on its own.
    const Ref::SkyAtmParams p = EarthParams();

    for ( const float elevation : { 90.0f, 45.0f, 20.0f, 10.0f, 5.0f, 2.0f, 0.5f } )
    {
        const float mu = SunDirectionAtElevation( elevation ).y;

        glm::vec3 previous( -1.0f );
        for ( const float altitudeKm : { 0.0f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f, 12.0f, 20.0f, 40.0f } )
        {
            const glm::vec3 t = PerSampleTransmittance( p, altitudeKm, mu );

            for ( int c = 0; c < 3; ++c )
            {
                // A transmittance is a probability of survival: it may not amplify, and it may not fall
                // as the sample climbs out of the air that was absorbing it. The tolerance is the LUT's
                // own quantisation, not a fudge — the table is 256 columns wide.
                EXPECT_GE( t[c], 0.0f ) << "elevation " << elevation << ", altitude " << altitudeKm;
                EXPECT_LE( t[c], 1.0f ) << "elevation " << elevation << ", altitude " << altitudeKm;
                EXPECT_GE( t[c], previous[c] - 1e-4f ) << "channel " << c << " falls with altitude at elevation "
                                                       << elevation << ", altitude " << altitudeKm;
            }
            previous = t;
        }
    }
}

TEST( SkyPerSampleSunTransmittance, AtTheGroundItIsTheColourTheOneColourPathAlreadyApplies )
{
    // THE RELATION THAT MAKES THE FLAG A GENERALISATION RATHER THAN A RECALIBRATION. At altitude zero the
    // per-sample form must be the value the shipped path already multiplies the sun by. If it were not,
    // turning the flag on would not be "the atmosphere, taken where the cloud is" — it would be a
    // different sun, and every number in Docs/Clouds/CALIBRATION.md would be re-based by an amount
    // nobody chose.
    //
    // The tolerance is the LUT's, not the integrator's: the shipped path evaluates the march directly
    // while the per-sample path reads a 256x64 table of it, so they agree to the table's resolution.
    const Ref::SkyAtmParams p = EarthParams();

    for ( const float elevation : { 90.0f, 60.0f, 30.0f, 10.0f, 5.0f, 1.0f } )
    {
        const float     mu      = SunDirectionAtElevation( elevation ).y;
        const glm::vec3 shipped = Ref::SkyTransmittanceAtGroundToSun( p, mu );
        const glm::vec3 here    = PerSampleTransmittance( p, Ref::SKY_PLANET_RADIUS_OFFSET_KM, mu );

        for ( int c = 0; c < 3; ++c )
            EXPECT_NEAR( here[c], shipped[c], 0.02f ) << "elevation " << elevation << " channel " << c;
    }
}

TEST( SkyPerSampleSunTransmittance, TheGainIsSmallAtNoonAndLargeAndBlueAtALowSun )
{
    // THE SIZE OF THE THING, pinned so that a later change which quietly neuters it fails here rather
    // than in a frame nobody re-shot. These are the numbers that decide whether the feature is worth its
    // fetch, and they are what the report quotes.
    const Ref::SkyAtmParams p = EarthParams();

    auto gain = [&]( float elevationDeg, float altitudeKm )
    {
        const float     mu     = SunDirectionAtElevation( elevationDeg ).y;
        const glm::vec3 ground = Ref::SkyTransmittanceAtGroundToSun( p, mu );
        const glm::vec3 up     = PerSampleTransmittance( p, altitudeKm, mu );
        return glm::vec3( up.x / ground.x, up.y / ground.y, up.z / ground.z );
    };

    // At noon over a deck at two kilometres the sun is a few per cent brighter and slightly bluer — a
    // change of one or two 8-bit levels on a lit cloud top, which is around the noise floor of a frame.
    const glm::vec3 noon = gain( 90.0f, 2.0f );
    EXPECT_GT( noon.x, 1.0f );
    EXPECT_LT( noon.x, 1.05f );
    EXPECT_GT( noon.z, noon.x ) << "the gain is not bluer than it is redder, so Rayleigh is not driving it";
    EXPECT_LT( noon.z, 1.15f );

    // At five degrees — the golden hour, where the whole point of the feature is — the same deck is
    // TENS of per cent brighter and much bluer, because the slant path through the low, dense air it is
    // standing above is many times longer.
    const glm::vec3 low = gain( 5.0f, 2.0f );
    EXPECT_GT( low.x, 1.15f );
    EXPECT_GT( low.z, 1.7f );
    EXPECT_GT( low.z / low.x, noon.z / noon.x )
         << "the colour shift does not grow as the sun descends, which is the entire mechanism";

    // And it grows with altitude at a fixed sun: a cirrus at eight kilometres sees more of the change
    // than a cumulus base at one.
    EXPECT_GT( gain( 5.0f, 8.0f ).z, gain( 5.0f, 1.0f ).z );
}

TEST( SkyPerSampleSunTransmittance, TheLutHasNoPlanetShadowSoItsReaderMustApplyOneItself )
{
    // FOUND BY A FRAME, NOT BY READING THE CODE, and recorded here so the next reader of this LUT does
    // not have to find it again. The mapping stores the transmittance from a radius to SPACE along a
    // zenith angle. It has NO state for "the ray is blocked by the planet": SkyDistanceToTop keeps
    // returning the top-shell root through the ground, so a sun below the sample's horizon simply
    // saturates the uv at its last column — the grazing ray.
    //
    // Read that way, a night sky lights its clouds with the GRAZING transmittance instead of nothing. It
    // is small in absolute terms and almost entirely red, so it renders as a blood-red deck rather than a
    // dark one. Both marches therefore multiply by Common/SkyScattering.glslh's SkyPlanetShadow — the
    // SAME term the sky multiplies its own sun by, so a cloud and the sky behind it cross the terminator
    // at one rate instead of two.
    const Ref::SkyAtmParams p = EarthParams();

    for ( const float altitudeKm : { 0.01f, 2.0f, 5.0f, 8.0f } )
    {
        const float viewHeightKm = p.BottomRadiusKm + altitudeKm;

        // The sample's OWN horizon, which dips further below the horizontal the higher it stands.
        const float sinHorizon = -std::sqrt(
             std::max( 1.0f - ( p.BottomRadiusKm / viewHeightKm ) * ( p.BottomRadiusKm / viewHeightKm ), 0.0f ) );

        // Above it the ray reaches space; below it the planet is in the way. Both halves asserted,
        // because a test that only checked the blocked side would pass on a function that always says yes.
        EXPECT_FALSE( Ref::SkyIntersectsGround( viewHeightKm, sinHorizon + 0.01f, p.BottomRadiusKm ) )
             << "altitude " << altitudeKm;
        EXPECT_TRUE( Ref::SkyIntersectsGround( viewHeightKm, sinHorizon - 0.01f, p.BottomRadiusKm ) )
             << "altitude " << altitudeKm;

        // AND WHAT THE FETCH RETURNS INSTEAD OF NOTHING. Every below-horizon sun saturates the mapping,
        // so the clamped read lands on the LAST COLUMN whatever the elevation. Its value is the leak, and
        // the two assertions are its size and its colour: per cents of the sun in RED against essentially
        // nothing in blue, which is why the symptom in the frame is a deck that glows dark red at night
        // rather than one that is merely a little too bright.
        const glm::vec3 leak = PerSampleTransmittance( p, altitudeKm, sinHorizon - 0.2f );

        EXPECT_GT( leak.x, 0.015f ) << "altitude " << altitudeKm;
        EXPECT_GT( leak.x, leak.y * 5.0f )
             << "the leak is no longer overwhelmingly RED at altitude " << altitudeKm
             << ", so the symptom this test describes has changed and the frame should be re-shot";
    }
}

TEST( SkyPerSampleSunTransmittance, AWrappingFetchAtTheSaturatedEndWouldReturnMostOfTheZenithSun )
{
    // THE HAZARD THE TEXEL-CENTRE CLAMP PREVENTS, measured rather than asserted. The two ends of the
    // table are the two EXTREMES of the quantity, so a bilinear blend of them is the worst possible
    // answer — and at uv.x exactly 1 a REPEAT sampler returns exactly that blend.
    const Ref::SkyAtmParams p = EarthParams();

    const glm::vec3 wrapped = SampleLutRepeat( p, glm::vec2( 1.0f, 0.18f ) );
    const glm::vec3 honest  = SampleLutRepeat(
         p, glm::vec2( ( static_cast<float>( kLutWidth ) - 0.5f ) / static_cast<float>( kLutWidth ), 0.18f ) );

    EXPECT_LT( honest.y, 0.05f ) << "the last column is no longer the grazing ray";
    EXPECT_GT( wrapped.y - honest.y, 0.35f )
         << "a wrapping fetch at the saturated end no longer differs materially from the correct one, so "
            "either the LUT's ends have changed meaning or this test has stopped measuring the hazard";
}

TEST( SkyPerSampleSunTransmittance, TheAtmosphereEnvPublishesTheShellTheLutWasWrittenOver )
{
    // THE MIRROR. AtmosphereEnv publishes the two radii for consumers that do not bind the sky parameter
    // buffer — the cloud march is the first — and it derives them in C++ while the LUT was written from
    // SkyMakeAtmParams inside the shader. Two derivations of one shell is exactly the pair this project
    // keeps finding, and a disagreement here is not an error anywhere: it is a transmittance sampled at
    // the wrong uv, which looks like a tuning problem.
    for ( const float planetRadiusWorld : { 636000000.0f, 100000000.0f, 1200000000.0f } )
    {
        for ( const float atmosphereHeightKm : { 60.0f, 100.0f, 8.0f, 0.0f } )
        {
            Desert::Graphic::SkySettings sky{};
            sky.PlanetRadius       = planetRadiusWorld;
            sky.AtmosphereHeightKm = atmosphereHeightKm;

            const Desert::Graphic::SkyGpuPayload payload =
                 Desert::Graphic::PackSky( glm::vec3( 0.0f, 1.0f, 0.0f ), sky );
            const Ref::SkyAtmParams shader =
                 Ref::SkyMakeAtmParams( payload.MediumRayleigh, payload.MediumMie, payload.MediumMieAbsorption,
                                        payload.MediumOzone, payload.MediumGround, payload.MediumTentPlanet );

            EXPECT_FLOAT_EQ( Desert::Graphic::AtmosphereBottomRadiusKm( sky ), shader.BottomRadiusKm )
                 << "planet " << planetRadiusWorld << ", height " << atmosphereHeightKm;
            EXPECT_FLOAT_EQ( Desert::Graphic::AtmosphereTopRadiusKm( sky ), shader.TopRadiusKm )
                 << "planet " << planetRadiusWorld << ", height " << atmosphereHeightKm;
        }
    }
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
