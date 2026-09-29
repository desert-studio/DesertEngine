// The Phase 2 sky — the Sky-View LUT parameterisation, the single-scattering integrator and the sun
// disc — tested against the relations that make the physical sky trustworthy, not hand-picked spot
// values.
//
// The functions under test are Engine/Content/Shaders/Common/SkyScattering.glslh compiled as C++
// (SkyScatteringReference.hpp), i.e. the exact text the SkyViewLut / BakeProceduralSky /
// ProceduralSky shaders run on the GPU. What is pinned, and why:
//
//   * the horizon-warped UV mapping ROUND-TRIPS, in both directions, at several altitudes — the fill
//     writes through one side and the sky pass reads through the other, and a mismatch is a sky that
//     is subtly displaced everywhere and provably wrong nowhere;
//   * the split lands EXACTLY on the horizon — the one angle the warp exists to protect;
//   * the 32-sample integrator agrees with a brute-force fine march of the same medium — the
//     sampling cannot vouch for itself;
//   * the planet's shadow still answers what the hard `SkyIntersectsGround` step answered everywhere
//     outside its own fade band, rises monotonically through it, and is narrower than the sun's disc
//     — the fade exists to stop a term jumping the whole way inside one march segment, and it must
//     not become a second way to brighten twilight;
//   * energy sanity — non-negative everywhere, transmittance a survival probability, the Mie forward
//     lobe brighter toward the sun than away from it, the multi-scattering term linear in Psi;
//   * the sun disc's limb darkening is 1 at the centre, falls to the rim, and reddens there.
//
// The last section adds a second unit and a RELATION between them: Common/CloudAerial.glslh (the cloud
// march's aerial-perspective composition, through CloudAerialReference.hpp) against this file's own sky.
// A cloud the air has taken away must BE the sky beside it, and that is a statement neither pass can
// make about itself.

#include "CloudAerialReference.hpp"
#include "SkyScatteringReference.hpp"

#include <Engine/Graphic/SkyPayload.hpp>

#include <glm/gtx/component_wise.hpp>
#include <gtest/gtest.h>

#include <cmath>
#include <limits>

namespace Ref = Desert::Tests::SkyScatteringRef;

namespace
{
    // Earth, through the REAL packing path (the SkyMedium suite's arrangement): SkySettings' defaults
    // -> PackSky -> SkyMakeAtmParams, world-units planet radius converted inside the shader text.
    Ref::SkyAtmParams EarthParams()
    {
        const Desert::Graphic::SkySettings   sky{};
        const Desert::Graphic::SkyGpuPayload payload =
             Desert::Graphic::PackSky( glm::vec3( 0.0f, 1.0f, 0.0f ), sky );
        return Ref::SkyMakeAtmParams( payload.MediumRayleigh, payload.MediumMie, payload.MediumMieAbsorption,
                                      payload.MediumOzone, payload.MediumGround, payload.MediumTentPlanet );
    }

    // Every test that touches the integrator resets the LUT stand-ins first: the defaults are the
    // exact (marched) sun transmittance and NO multi-scattering, which is the configuration the
    // brute-force comparison needs.
    void ResetLutCallbacks()
    {
        Ref::g_SunTransmittance = []( Ref::SkyAtmParams p, float radiusKm, float sunZenithCos )
        { return Ref::SkyTransmittanceToTop( p, radiusKm, sunZenithCos, 40 ); };
        Ref::g_MultiScatter = []( Ref::SkyAtmParams, float, float ) { return glm::vec3( 0.0f, 0.0f, 0.0f ); };
    }

    // A brute-force single-scattering march: uniform steps, midpoint rule, running optical depth, a
    // finely-marched sun transmittance per step. Never calls the integrator under test — an
    // independent statement of the same physics.
    glm::vec3 BruteForceSingleScattering( const Ref::SkyAtmParams& p, const glm::vec3& originKm,
                                          const glm::vec3& rayDir, const glm::vec3& sunDir,
                                          const glm::vec3& sunIlluminance, int stepCount )
    {
        const float r0 = glm::length( originKm );
        const float mu = glm::dot( originKm, rayDir ) / r0;

        const bool  hitsGround = Ref::SkyIntersectsGround( r0, mu, p.BottomRadiusKm );
        const float tMax       = hitsGround ? Ref::SkyDistanceToBottom( r0, mu, p.BottomRadiusKm )
                                            : Ref::SkyDistanceToTop( r0, mu, p.TopRadiusKm );

        const float cosTheta = glm::dot( rayDir, sunDir );
        const float phaseR   = Ref::SkyPhaseRayleigh( cosTheta );
        const float phaseM   = Ref::SkyPhaseCornetteShanks( p.MiePhaseG, cosTheta );

        const float dt = tMax / static_cast<float>( stepCount );

        glm::vec3 luminance( 0.0f );
        glm::vec3 opticalDepth( 0.0f );
        for ( int i = 0; i < stepCount; ++i )
        {
            const float     t        = ( static_cast<float>( i ) + 0.5f ) * dt;
            const glm::vec3 position = originKm + rayDir * t;
            const float     radius   = glm::length( position );
            const float     altitude = radius - p.BottomRadiusKm;

            const float dRay = Ref::SkyDensityRayleigh( p, altitude );
            const float dMie = Ref::SkyDensityMie( p, altitude );
            const float dOzo = Ref::SkyDensityOzone( p, altitude );

            const glm::vec3 scatR = p.RayleighScattering * dRay;
            const glm::vec3 scatM = p.MieScattering * dMie;
            const glm::vec3 ext   = scatR + scatM + p.MieAbsorption * dMie + p.OzoneAbsorption * dOzo;

            const glm::vec3 viewT = glm::exp( -( opticalDepth + ext * ( dt * 0.5f ) ) );

            const glm::vec3 zenith       = position / radius;
            const float     sunZenithCos = glm::dot( zenith, sunDir );
            const float shadow = Ref::SkyIntersectsGround( radius, sunZenithCos, p.BottomRadiusKm ) ? 0.0f : 1.0f;
            const glm::vec3 sunT = Ref::SkyTransmittanceToTop( p, radius, sunZenithCos, 200 );

            luminance += sunIlluminance * shadow * viewT * sunT * ( phaseR * scatR + phaseM * scatM ) * dt;
            opticalDepth += ext * dt;
        }
        return luminance;
    }
} // namespace

// ---------------------------------------------------------------------------------------------------
// The Sky-View LUT mapping is an exactly invertible pair, at every altitude the camera can hold
// ---------------------------------------------------------------------------------------------------

TEST( SkyScattering, SkyViewUnitRoundTripsAcrossTheDomain )
{
    const Ref::SkyAtmParams p = EarthParams();

    for ( const float altitude : { 0.05f, 0.5f, 2.0f, 8.0f, 30.0f } )
    {
        const float r = p.BottomRadiusKm + altitude;

        // Every texel centre of the real 192x104 LUT plus the exact corners: unit -> params -> unit.
        for ( int y = 0; y <= 104; ++y )
        {
            for ( int x = 0; x <= 192; ++x )
            {
                const glm::vec2 unit( static_cast<float>( x ) / 192.0f, static_cast<float>( y ) / 104.0f );

                const Ref::SkyViewLutCoord c = Ref::SkyViewParamsFromUnit( p.BottomRadiusKm, r, unit );

                EXPECT_GE( c.ViewZenithCos, -1.0f );
                EXPECT_LE( c.ViewZenithCos, 1.0f );
                EXPECT_GE( c.LightViewCos, -1.0f );
                EXPECT_LE( c.LightViewCos, 1.0f );

                const glm::vec2 back =
                     Ref::SkyViewUnitFromParams( p.BottomRadiusKm, r, c.ViewZenithCos, c.LightViewCos );

                // 2e-3 of the unit domain is about a fifth of a texel at 104 rows: acos/cos round
                // trips through float, and the sqrt warp amplifies that near its flat ends.
                EXPECT_NEAR( back.x, unit.x, 2e-3f )
                     << "alt=" << altitude << " unit=(" << unit.x << "," << unit.y << ")";
                EXPECT_NEAR( back.y, unit.y, 2e-3f )
                     << "alt=" << altitude << " unit=(" << unit.x << "," << unit.y << ")";
            }
        }
    }
}

TEST( SkyScattering, SkyViewParamsRoundTripThroughUnit )
{
    const Ref::SkyAtmParams p = EarthParams();

    for ( const float altitude : { 0.05f, 1.0f, 10.0f } )
    {
        const float r = p.BottomRadiusKm + altitude;

        for ( int zi = 0; zi <= 64; ++zi )
        {
            const float viewZenithCos = -1.0f + 2.0f * static_cast<float>( zi ) / 64.0f;
            for ( int li = 0; li <= 16; ++li )
            {
                const float lightViewCos = -1.0f + 2.0f * static_cast<float>( li ) / 16.0f;

                const glm::vec2 unit =
                     Ref::SkyViewUnitFromParams( p.BottomRadiusKm, r, viewZenithCos, lightViewCos );
                const Ref::SkyViewLutCoord c = Ref::SkyViewParamsFromUnit( p.BottomRadiusKm, r, unit );

                // The warp compresses toward the horizon, so the honest bound is on the ANGLE the
                // mapping actually transports, not its cosine.
                EXPECT_NEAR( std::acos( glm::clamp( c.ViewZenithCos, -1.0f, 1.0f ) ),
                             std::acos( glm::clamp( viewZenithCos, -1.0f, 1.0f ) ), 3e-3f )
                     << "alt=" << altitude << " mu=" << viewZenithCos;
                EXPECT_NEAR( c.LightViewCos, lightViewCos, 3e-3f ) << "alt=" << altitude << " lv=" << lightViewCos;
            }
        }
    }
}

TEST( SkyScattering, TheSplitSitsExactlyOnTheHorizon )
{
    const Ref::SkyAtmParams p = EarthParams();
    const float             r = p.BottomRadiusKm + 2.0f;

    const float horizonCos = Ref::SkyViewHorizonCos( p.BottomRadiusKm, r );

    // unit.y = 0.5 decodes to the horizon itself; a hair to either side lands on the matching half.
    const Ref::SkyViewLutCoord atSplit =
         Ref::SkyViewParamsFromUnit( p.BottomRadiusKm, r, glm::vec2( 0.5f, 0.5f ) );
    EXPECT_NEAR( atSplit.ViewZenithCos, horizonCos, 1e-4f );

    const Ref::SkyViewLutCoord justSky =
         Ref::SkyViewParamsFromUnit( p.BottomRadiusKm, r, glm::vec2( 0.5f, 0.45f ) );
    const Ref::SkyViewLutCoord justGround =
         Ref::SkyViewParamsFromUnit( p.BottomRadiusKm, r, glm::vec2( 0.5f, 0.55f ) );
    EXPECT_FALSE( Ref::SkyViewHitsGround( p.BottomRadiusKm, r, justSky.ViewZenithCos ) );
    EXPECT_TRUE( Ref::SkyViewHitsGround( p.BottomRadiusKm, r, justGround.ViewZenithCos ) );

    // And the geometric truth the flag stands for: the analytic sphere test agrees with the angular
    // threshold on both sides of it.
    EXPECT_TRUE( Ref::SkyIntersectsGround( r, horizonCos - 1e-4f, p.BottomRadiusKm ) );
    EXPECT_FALSE( Ref::SkyIntersectsGround( r, horizonCos + 1e-4f, p.BottomRadiusKm ) );
}

TEST( SkyScattering, TexelRemapIsExactAtTheLutResolution )
{
    // The write side (SkyViewLut.shader) applies SkyTexelUvToUnit to texel centres, the read side
    // (ProceduralSky.shader) applies SkyUnitToTexelUv — pinned at the LUT's own 192x104 so the two
    // constants and the remap stay one system.
    EXPECT_FLOAT_EQ( Ref::SKY_VIEW_LUT_WIDTH, 192.0f );
    EXPECT_FLOAT_EQ( Ref::SKY_VIEW_LUT_HEIGHT, 104.0f );

    for ( int i = 0; i <= 32; ++i )
    {
        const float unit = static_cast<float>( i ) / 32.0f;
        EXPECT_NEAR( Ref::SkyTexelUvToUnit( Ref::SkyUnitToTexelUv( unit, Ref::SKY_VIEW_LUT_WIDTH ),
                                            Ref::SKY_VIEW_LUT_WIDTH ),
                     unit, 1e-6f );
        EXPECT_NEAR( Ref::SkyTexelUvToUnit( Ref::SkyUnitToTexelUv( unit, Ref::SKY_VIEW_LUT_HEIGHT ),
                                            Ref::SKY_VIEW_LUT_HEIGHT ),
                     unit, 1e-6f );
    }

    // Texel 0's centre holds unit 0, the last texel's centre unit 1 — the ends of the warp's domain.
    EXPECT_NEAR( Ref::SkyTexelUvToUnit( 0.5f / 104.0f, 104.0f ), 0.0f, 1e-6f );
    EXPECT_NEAR( Ref::SkyTexelUvToUnit( 103.5f / 104.0f, 104.0f ), 1.0f, 1e-6f );
}

// ---------------------------------------------------------------------------------------------------
// The camera-to-planet-frame helpers
// ---------------------------------------------------------------------------------------------------

TEST( SkyScattering, ViewHeightAndZenithRecoverTheCameraExactly )
{
    const Ref::SkyAtmParams p = EarthParams();

    // On the surface at the origin: r = Rb, zenith = +Y.
    EXPECT_NEAR( Ref::SkyViewHeightKm( glm::vec3( 0.0f ), p.BottomRadiusKm ), p.BottomRadiusKm, 1e-3f );
    const glm::vec3 zenithAtOrigin = Ref::SkyViewZenith( glm::vec3( 0.0f ), p.BottomRadiusKm );
    EXPECT_NEAR( zenithAtOrigin.y, 1.0f, 1e-6f );

    // Straight up 2 km: r = Rb + 2 exactly (the factored form does not cancel).
    EXPECT_NEAR( Ref::SkyViewHeightKm( glm::vec3( 0.0f, 2.0f, 0.0f ), p.BottomRadiusKm ), p.BottomRadiusKm + 2.0f,
                 1e-3f );

    // 30 km sideways at the surface: against the double-precision law of cosines. This is the case
    // the naive sqrt(r^2) formulation loses to float cancellation.
    const glm::vec3 sideways( 30.0f, 0.0f, 0.0f );
    const double    rd = std::sqrt( 30.0 * 30.0 + static_cast<double>( p.BottomRadiusKm ) *
                                                       static_cast<double>( p.BottomRadiusKm ) );
    EXPECT_NEAR( Ref::SkyViewHeightKm( sideways, p.BottomRadiusKm ), static_cast<float>( rd ), 1e-3f );

    // The zenith there tilts by exactly atan(30 / Rb).
    const glm::vec3 zenithSideways = Ref::SkyViewZenith( sideways, p.BottomRadiusKm );
    EXPECT_NEAR( zenithSideways.x, 30.0f / static_cast<float>( rd ), 1e-5f );
}

TEST( SkyScattering, LightViewCosAndRayDirectionAgree )
{
    // Build a ray FROM (viewZenithCos, lightViewCos), then measure its light-view cosine back with
    // the world-frame helper — the fill writes through one function and the sky pass reads through
    // the other, so they must be inverse views of the same geometry.
    const glm::vec3 zenith( 0.0f, 1.0f, 0.0f );

    for ( int zi = 1; zi < 16; ++zi )
    {
        const float viewZenithCos = -0.95f + 1.9f * static_cast<float>( zi ) / 16.0f;
        for ( int li = 0; li <= 8; ++li )
        {
            const float lightViewCos = -1.0f + 2.0f * static_cast<float>( li ) / 8.0f;

            const glm::vec3 dir = Ref::SkyViewRayDirection( viewZenithCos, lightViewCos );
            EXPECT_NEAR( glm::length( dir ), 1.0f, 1e-5f );
            EXPECT_NEAR( dir.y, viewZenithCos, 1e-5f );

            // The sun of the LUT's local frame lies toward +X.
            const glm::vec3 sunDir    = glm::normalize( glm::vec3( 0.8f, 0.6f, 0.0f ) );
            const float     roundTrip = Ref::SkyViewLightViewCos( zenith, dir, sunDir );
            EXPECT_NEAR( roundTrip, lightViewCos, 1e-4f ) << "mu=" << viewZenithCos << " lv=" << lightViewCos;
        }
    }
}

// ---------------------------------------------------------------------------------------------------
// The planet's shadow on the air
// ---------------------------------------------------------------------------------------------------

// The fade replaced a hard `SkyIntersectsGround` step, so the first thing to pin is that it still
// AGREES with that step everywhere except inside its own band — a fade that answered differently in
// broad daylight would be a new sky, not an artefact fix.
TEST( SkyScattering, PlanetShadowAgreesWithTheHardTestAwayFromTheTerminator )
{
    const Ref::SkyAtmParams p = EarthParams();

    // From the offset the whole medium is parameterised against upward — a sample never sits exactly
    // ON the surface, and the hard test's discriminant is degenerate there (see the case below).
    for ( const float altitudeKm : { Ref::SKY_PLANET_RADIUS_OFFSET_KM, 5.0f, 20.0f, 55.0f } )
    {
        const float radius = p.BottomRadiusKm + altitudeKm;

        for ( int i = 0; i <= 400; ++i )
        {
            const float sunZenithCos = -1.0f + 2.0f * static_cast<float>( i ) / 400.0f;
            const float faded        = Ref::SkyPlanetShadow( radius, sunZenithCos, p.BottomRadiusKm );

            // Distance from this sample's own horizon, in the same cosine units as the band.
            const float rho =
                 std::sqrt( std::max( ( radius - p.BottomRadiusKm ) * ( radius + p.BottomRadiusKm ), 0.0f ) );
            const float horizonCos = -rho / radius;

            if ( std::abs( sunZenithCos - horizonCos ) <= Ref::SKY_TERMINATOR_HALF_WIDTH_COS )
                continue; // inside the band the two are ALLOWED to differ — that is the whole point

            const float hard = Ref::SkyIntersectsGround( radius, sunZenithCos, p.BottomRadiusKm ) ? 0.0f : 1.0f;
            EXPECT_FLOAT_EQ( faded, hard ) << "altitude " << altitudeKm << " km, sun zenith cos " << sunZenithCos;
        }
    }
}

// EXACTLY on the surface the hard test does not merely differ, it is WRONG: rho is zero, so the ray
// to the sun grazes, the discriminant is (r*mu)^2 and sqrt() rounds a hair above |r*mu| — the
// distance comes out negative and a sample with the sun a degree UNDER its feet reports full sun.
// The fade has no discriminant and answers 0. Pinned because it is the direction of the difference
// that matters: replacing the step did not weaken this case, it repaired it.
TEST( SkyScattering, PlanetShadowIsCorrectWhereTheHardTestIsDegenerate )
{
    const Ref::SkyAtmParams p = EarthParams();

    for ( const float sunZenithCos : { -0.99f, -0.5f, -0.1f } )
    {
        EXPECT_FLOAT_EQ( Ref::SkyPlanetShadow( p.BottomRadiusKm, sunZenithCos, p.BottomRadiusKm ), 0.0f )
             << "the sun is below this sample's horizon at cos " << sunZenithCos;
    }
    for ( const float sunZenithCos : { 0.1f, 0.5f, 0.99f } )
    {
        EXPECT_FLOAT_EQ( Ref::SkyPlanetShadow( p.BottomRadiusKm, sunZenithCos, p.BottomRadiusKm ), 1.0f )
             << "the sun is above this sample's horizon at cos " << sunZenithCos;
    }
}

// A monotonic fade, bounded to [0, 1]: the artefact it exists to remove came from a term that could
// jump the whole way inside one march segment, so what must hold is that light never DECREASES as the
// sun rises, and that no sample is ever more than fully lit.
TEST( SkyScattering, PlanetShadowRisesMonotonicallyThroughTheTerminator )
{
    const Ref::SkyAtmParams p      = EarthParams();
    const float             radius = p.BottomRadiusKm + 10.0f;

    const float rho = std::sqrt( std::max( ( radius - p.BottomRadiusKm ) * ( radius + p.BottomRadiusKm ), 0.0f ) );
    const float horizonCos = -rho / radius;
    const float band       = Ref::SKY_TERMINATOR_HALF_WIDTH_COS;

    float previous = 0.0f;
    for ( int i = 0; i <= 200; ++i )
    {
        const float sunZenithCos = horizonCos - 2.0f * band + 4.0f * band * static_cast<float>( i ) / 200.0f;
        const float shadow       = Ref::SkyPlanetShadow( radius, sunZenithCos, p.BottomRadiusKm );

        EXPECT_GE( shadow, 0.0f );
        EXPECT_LE( shadow, 1.0f );
        EXPECT_GE( shadow, previous ) << "the fade must not dip as the sun rises";
        previous = shadow;
    }

    // Fully shadowed a band below the horizon, fully lit a band above it, and exactly half at it.
    EXPECT_FLOAT_EQ( Ref::SkyPlanetShadow( radius, horizonCos - band, p.BottomRadiusKm ), 0.0f );
    EXPECT_FLOAT_EQ( Ref::SkyPlanetShadow( radius, horizonCos + band, p.BottomRadiusKm ), 1.0f );
    EXPECT_NEAR( Ref::SkyPlanetShadow( radius, horizonCos, p.BottomRadiusKm ), 0.5f, 1e-5f );
}

// The band is narrower than the sun's own angular radius (~0.27 degrees, 0.0047 in cosine at the
// horizon). That direction matters: a band WIDER than the real penumbra would admit light the sky
// does not have and lift the whole twilight, which is the failure this fade must not become.
TEST( SkyScattering, TheTerminatorBandIsNoWiderThanTheSunItself )
{
    constexpr float kSunAngularRadiusCos = 0.00465f;
    EXPECT_LT( Ref::SKY_TERMINATOR_HALF_WIDTH_COS, kSunAngularRadiusCos );
    EXPECT_GT( Ref::SKY_TERMINATOR_HALF_WIDTH_COS, 0.0f );
}

// ---------------------------------------------------------------------------------------------------
// The integrator against a brute-force fine march
// ---------------------------------------------------------------------------------------------------

TEST( SkyScattering, IntegratorMatchesTheBruteForceMarch )
{
    ResetLutCallbacks();
    // Both sides read the SAME 200-step sun transmittance, so the comparison isolates what this test
    // is about — the integrator's 32-sample squared distribution and per-step analytic integral —
    // rather than re-testing the transmittance march (SkyMedium already pins 40 vs 20000 steps).
    Ref::g_SunTransmittance = []( Ref::SkyAtmParams p, float radiusKm, float sunZenithCos )
    { return Ref::SkyTransmittanceToTop( p, radiusKm, sunZenithCos, 200 ); };
    const Ref::SkyAtmParams p = EarthParams();

    const glm::vec3 illuminance( 1.0f );
    const glm::vec3 originKm( 0.0f, p.BottomRadiusKm + 0.5f, 0.0f );

    struct Case
    {
        glm::vec3   RayDir;
        glm::vec3   SunDir;
        const char* Name;
    };
    const Case cases[] = {
         { { 0.0f, 1.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, "zenith ray, noon sun" },
         { { 0.0f, 1.0f, 0.0f }, glm::normalize( glm::vec3( 0.9f, 0.3f, 0.0f ) ), "zenith ray, low sun" },
         { glm::normalize( glm::vec3( 0.9f, 0.2f, 0.0f ) ), glm::normalize( glm::vec3( 0.7f, 0.6f, 0.2f ) ),
           "oblique ray, day sun" },
    };

    for ( const Case& c : cases )
    {
        // 32 samples: the shader's own Sky-View budget. The reference is 4000 uniform steps with a
        // 200-step sun transmittance per step — different distribution, different transmittance
        // source, same physics.
        const Ref::SkyScatterResult fast =
             Ref::SkyIntegrateScatteredLuminance( p, originKm, c.RayDir, c.SunDir, illuminance, 32 );
        const glm::vec3 fine = BruteForceSingleScattering( p, originKm, c.RayDir, c.SunDir, illuminance, 4000 );

        // 4% relative (plus an absolute floor for near-zero channels): the squared distribution vs
        // the uniform one, and the 40- vs 200-step sun transmittance.
        for ( int ch = 0; ch < 3; ++ch )
        {
            const float tolerance = std::max( 0.04f * fine[ch], 2e-5f );
            EXPECT_NEAR( fast.Luminance[ch], fine[ch], tolerance ) << c.Name << " channel " << ch;
        }
    }
}

TEST( SkyScattering, TransmittanceLaneMatchesTheMediumMarch )
{
    ResetLutCallbacks();
    const Ref::SkyAtmParams p = EarthParams();

    // For a non-ground ray the integrator's throughput IS the view-path transmittance — the same
    // quantity SkyMedium's dedicated integrator computes. Two implementations of one number.
    const glm::vec3 originKm( 0.0f, p.BottomRadiusKm + 0.5f, 0.0f );
    for ( const float mu : { 1.0f, 0.6f, 0.2f } )
    {
        const glm::vec3             rayDir( std::sqrt( 1.0f - mu * mu ), mu, 0.0f );
        const Ref::SkyScatterResult result = Ref::SkyIntegrateScatteredLuminance(
             p, originKm, rayDir, glm::vec3( 0.0f, 1.0f, 0.0f ), glm::vec3( 1.0f ), 64 );

        const glm::vec3 expected = Ref::SkyTransmittanceToTop( p, originKm.y, mu, 20000 );
        EXPECT_NEAR( result.Transmittance.x, expected.x, 0.01f ) << "mu=" << mu;
        EXPECT_NEAR( result.Transmittance.y, expected.y, 0.01f ) << "mu=" << mu;
        EXPECT_NEAR( result.Transmittance.z, expected.z, 0.01f ) << "mu=" << mu;
    }
}

// ---------------------------------------------------------------------------------------------------
// Energy sanity
// ---------------------------------------------------------------------------------------------------

TEST( SkyScattering, LuminanceIsNonNegativeAndTransmittanceIsAProbability )
{
    ResetLutCallbacks();
    const Ref::SkyAtmParams p = EarthParams();

    const glm::vec3 originKm( 0.0f, p.BottomRadiusKm + 0.2f, 0.0f );

    // The whole view sphere x sun elevations from noon to well below the horizon (twilight and night
    // must be DARK, never negative).
    for ( int si = 0; si <= 6; ++si )
    {
        const float     sunY = 1.0f - 2.0f * static_cast<float>( si ) / 6.0f;
        const float     sunX = std::sqrt( std::max( 1.0f - sunY * sunY, 0.0f ) );
        const glm::vec3 sunDir( sunX, sunY, 0.0f );

        for ( int zi = 0; zi <= 12; ++zi )
        {
            const float     mu = 1.0f - 2.0f * static_cast<float>( zi ) / 12.0f;
            const float     s  = std::sqrt( std::max( 1.0f - mu * mu, 0.0f ) );
            const glm::vec3 rayDir( s, mu, 0.0f );

            const Ref::SkyScatterResult result =
                 Ref::SkyIntegrateScatteredLuminance( p, originKm, rayDir, sunDir, glm::vec3( 10.0f ), 32 );

            for ( int ch = 0; ch < 3; ++ch )
            {
                EXPECT_GE( result.Luminance[ch], 0.0f ) << "sunY=" << sunY << " mu=" << mu;
                EXPECT_GT( result.Transmittance[ch], 0.0f ) << "sunY=" << sunY << " mu=" << mu;
                EXPECT_LE( result.Transmittance[ch], 1.0f ) << "sunY=" << sunY << " mu=" << mu;
            }
        }
    }
}

TEST( SkyScattering, PositiveAnisotropyMakesTheSunwardSkyBrighter )
{
    ResetLutCallbacks();
    const Ref::SkyAtmParams p = EarthParams(); // g = 0.8

    // Same sun, same elevation, opposite azimuths: the Mie forward lobe must make the ray TOWARD the
    // sun's azimuth brighter than the one away from it. A flipped phase convention passes every
    // normalization test and fails exactly this.
    const glm::vec3 originKm( 0.0f, p.BottomRadiusKm + 0.2f, 0.0f );
    const glm::vec3 sunDir = glm::normalize( glm::vec3( 0.9f, 0.44f, 0.0f ) );

    const glm::vec3 toward = glm::normalize( glm::vec3( 0.98f, 0.2f, 0.0f ) );
    const glm::vec3 away   = glm::normalize( glm::vec3( -0.98f, 0.2f, 0.0f ) );

    const Ref::SkyScatterResult sunward =
         Ref::SkyIntegrateScatteredLuminance( p, originKm, toward, sunDir, glm::vec3( 1.0f ), 32 );
    const Ref::SkyScatterResult antisolar =
         Ref::SkyIntegrateScatteredLuminance( p, originKm, away, sunDir, glm::vec3( 1.0f ), 32 );

    EXPECT_GT( sunward.Luminance.x + sunward.Luminance.y + sunward.Luminance.z,
               antisolar.Luminance.x + antisolar.Luminance.y + antisolar.Luminance.z );
}

TEST( SkyScattering, MultiScatterContributionIsLinearInPsi )
{
    ResetLutCallbacks();
    const Ref::SkyAtmParams p = EarthParams();

    const glm::vec3 originKm( 0.0f, p.BottomRadiusKm + 0.2f, 0.0f );
    const glm::vec3 rayDir( 0.0f, 1.0f, 0.0f );
    const glm::vec3 sunDir = glm::normalize( glm::vec3( 0.7f, 0.7f, 0.0f ) );

    const Ref::SkyScatterResult base =
         Ref::SkyIntegrateScatteredLuminance( p, originKm, rayDir, sunDir, glm::vec3( 1.0f ), 32 );

    Ref::g_MultiScatter = []( Ref::SkyAtmParams, float, float ) { return glm::vec3( 0.01f ); };
    const Ref::SkyScatterResult withPsi =
         Ref::SkyIntegrateScatteredLuminance( p, originKm, rayDir, sunDir, glm::vec3( 1.0f ), 32 );

    Ref::g_MultiScatter = []( Ref::SkyAtmParams, float, float ) { return glm::vec3( 0.02f ); };
    const Ref::SkyScatterResult withTwicePsi =
         Ref::SkyIntegrateScatteredLuminance( p, originKm, rayDir, sunDir, glm::vec3( 1.0f ), 32 );

    ResetLutCallbacks();

    // Additive, and linear: doubling Psi doubles exactly the delta it contributed. This is the
    // property that lets the MS LUT store a transfer rather than a luminance.
    const glm::vec3 delta1 = withPsi.Luminance - base.Luminance;
    const glm::vec3 delta2 = withTwicePsi.Luminance - base.Luminance;
    for ( int ch = 0; ch < 3; ++ch )
    {
        EXPECT_GT( delta1[ch], 0.0f );
        EXPECT_NEAR( delta2[ch], 2.0f * delta1[ch], 1e-5f + 0.001f * delta2[ch] );
    }

    // And Psi must not move the transmittance lane: it adds light, it does not remove any.
    EXPECT_FLOAT_EQ( base.Transmittance.x, withPsi.Transmittance.x );
}

// ---------------------------------------------------------------------------------------------------
// The sun disc
// ---------------------------------------------------------------------------------------------------

TEST( SkyScattering, SunDiscSoftEdgeIsOneInsideZeroOutside )
{
    const float apex    = 0.005f; // ~half-degree diameter
    const float cosApex = std::cos( apex );

    EXPECT_FLOAT_EQ( Ref::SkySunDiscSoftEdge( 1.0f, cosApex ), 1.0f );                    // centre
    EXPECT_FLOAT_EQ( Ref::SkySunDiscSoftEdge( cosApex, cosApex ), 0.0f );                 // the rim
    EXPECT_FLOAT_EQ( Ref::SkySunDiscSoftEdge( std::cos( 2.0f * apex ), cosApex ), 0.0f ); // outside

    // Monotone across the rim band.
    const float inner = Ref::SkySunDiscSoftEdge( std::cos( apex * 0.3f ), cosApex );
    const float outer = Ref::SkySunDiscSoftEdge( std::cos( apex * 0.8f ), cosApex );
    EXPECT_GT( inner, outer );
}

TEST( SkyScattering, LimbDarkeningFallsToARedderRim )
{
    // Centre: no darkening at all.
    const glm::vec3 centre = Ref::SkySunLimbDarkening( 0.0f );
    EXPECT_FLOAT_EQ( centre.x, 1.0f );
    EXPECT_FLOAT_EQ( centre.y, 1.0f );
    EXPECT_FLOAT_EQ( centre.z, 1.0f );

    // Strictly darkening outward, every channel.
    glm::vec3 previous = centre;
    for ( int i = 1; i <= 10; ++i )
    {
        const glm::vec3 sample = Ref::SkySunLimbDarkening( static_cast<float>( i ) / 10.0f );
        EXPECT_LT( sample.x, previous.x + 1e-6f );
        EXPECT_LT( sample.y, previous.y + 1e-6f );
        EXPECT_LT( sample.z, previous.z + 1e-6f );
        previous = sample;
    }

    // The rim is WARM: blue darkens fastest, red least — the ordering the 3-coefficient law encodes.
    const glm::vec3 nearRim = Ref::SkySunLimbDarkening( 0.95f );
    EXPECT_GT( nearRim.x, nearRim.y );
    EXPECT_GT( nearRim.y, nearRim.z );
}

// ---------------------------------------------------------------------------------------------------
// The camera aerial-perspective volume: the slice mapping, the froxel walk, and the one value the
// whole thing has to get exactly right — nothing at zero distance
// ---------------------------------------------------------------------------------------------------

namespace
{
    // The froxel-centre distance of slice @p index, exactly as SkyAerialPerspectiveLut.shader computes
    // it: unit = index / (depth - 1), so slice 0 sits at distance 0 and the last at the volume's far
    // extent. Mirrored here rather than exported because the shader's loop is the definition.
    float SliceDistanceKm( int index, float apDepthKm )
    {
        const float unit = static_cast<float>( index ) / ( Ref::SKY_AP_VOLUME_DEPTH - 1.0f );
        return Ref::SkyApDistanceFromSliceUnit( unit, apDepthKm );
    }

    // Transmittance over [0, tKm] of the camera ray, by brute force: a fine uniform march of the
    // medium's extinction. Independent of the froxel walk under test — it calls neither the walk nor
    // the integrator, only the medium — which is what makes it a reference rather than a tautology.
    glm::vec3 BruteForceTransmittance( const Ref::SkyAtmParams& p, const glm::vec3& originKm,
                                       const glm::vec3& rayDir, float tKm, int steps )
    {
        const float dt = tKm / static_cast<float>( steps );

        glm::vec3 opticalDepth( 0.0f );
        for ( int i = 0; i < steps; ++i )
        {
            const float     t        = ( static_cast<float>( i ) + 0.5f ) * dt;
            const glm::vec3 position = originKm + rayDir * t;
            const float     radius   = glm::max( glm::length( position ), p.BottomRadiusKm );

            opticalDepth += Ref::SkySampleMedium( p, radius - p.BottomRadiusKm ).Extinction * dt;
        }
        return glm::exp( -opticalDepth );
    }
} // namespace

TEST( SkyScattering, AerialPerspectiveSliceMappingRoundTrips )
{
    // The extents are part of the parameterisation, not a quality dial — Graphic::kAerialPerspective*
    // states the same three numbers, and the remap below bakes them into every read.
    EXPECT_FLOAT_EQ( Ref::SKY_AP_VOLUME_WIDTH, 32.0f );
    EXPECT_FLOAT_EQ( Ref::SKY_AP_VOLUME_HEIGHT, 32.0f );
    EXPECT_FLOAT_EQ( Ref::SKY_AP_VOLUME_DEPTH, 16.0f );

    for ( const float depthKm : { 1.0f, 8.0f, 96.0f, 200.0f } )
    {
        // unit -> distance -> unit, over the whole domain including both ends.
        for ( int i = 0; i <= 64; ++i )
        {
            const float unit = static_cast<float>( i ) / 64.0f;
            const float back =
                 Ref::SkyApSliceUnitFromDistance( Ref::SkyApDistanceFromSliceUnit( unit, depthKm ), depthKm );
            EXPECT_NEAR( back, unit, 1e-6f ) << "depth " << depthKm << " unit " << unit;
        }

        // distance -> unit -> distance, at distances a scene actually produces.
        for ( int i = 0; i <= 64; ++i )
        {
            const float distanceKm = depthKm * static_cast<float>( i ) / 64.0f;
            const float back       = Ref::SkyApDistanceFromSliceUnit(
                 Ref::SkyApSliceUnitFromDistance( distanceKm, depthKm ), depthKm );
            EXPECT_NEAR( back, distanceKm, depthKm * 1e-5f );
        }

        // The ends are exact, and beyond the volume the mapping SATURATES rather than wrapping — the
        // read-side clamp that keeps a pixel past the far extent from sampling froxel 0.
        EXPECT_FLOAT_EQ( Ref::SkyApSliceUnitFromDistance( 0.0f, depthKm ), 0.0f );
        EXPECT_FLOAT_EQ( Ref::SkyApSliceUnitFromDistance( depthKm, depthKm ), 1.0f );
        EXPECT_FLOAT_EQ( Ref::SkyApSliceUnitFromDistance( depthKm * 10.0f, depthKm ), 1.0f );
        EXPECT_FLOAT_EQ( Ref::SkyApSliceUnitFromDistance( -5.0f, depthKm ), 0.0f );
    }

    // Slice 0 IS the camera, and the distribution is squared: the first half of the slices covers the
    // first quarter of the range. Both are what the fill relies on.
    EXPECT_FLOAT_EQ( SliceDistanceKm( 0, 96.0f ), 0.0f );
    EXPECT_FLOAT_EQ( SliceDistanceKm( 15, 96.0f ), 96.0f );
    EXPECT_LT( SliceDistanceKm( 8, 96.0f ), 96.0f * 0.3f );

    // Strictly increasing, so the froxel walk's segments are never inverted.
    for ( int i = 1; i < 16; ++i )
        EXPECT_GT( SliceDistanceKm( i, 96.0f ), SliceDistanceKm( i - 1, 96.0f ) );

    // Every slice's read coordinate lands on its own texel centre: the fill writes slice i, the apply
    // pass reads back through SkyUnitToTexelUv, and the two must be the same texel to a float.
    for ( int i = 0; i < 16; ++i )
    {
        const float unit = Ref::SkyApSliceUnitFromDistance( SliceDistanceKm( i, 96.0f ), 96.0f );
        EXPECT_NEAR( Ref::SkyUnitToTexelUv( unit, Ref::SKY_AP_VOLUME_DEPTH ),
                     ( static_cast<float>( i ) + 0.5f ) / 16.0f, 1e-6f );
    }
}

TEST( SkyScattering, AerialPerspectiveSegmentSampleCountFollowsTheSegmentLength )
{
    // One sample per target step, at least one however short the segment, capped however long it is —
    // the property that keeps the near slices cheap and the far ones resolved.
    EXPECT_EQ( Ref::SkyApSegmentSampleCount( 0.0f ), 1 );
    EXPECT_EQ( Ref::SkyApSegmentSampleCount( -1.0f ), 1 );
    EXPECT_EQ( Ref::SkyApSegmentSampleCount( 0.4f ), 1 );
    EXPECT_EQ( Ref::SkyApSegmentSampleCount( 1.5f ), 2 );
    EXPECT_EQ( Ref::SkyApSegmentSampleCount( 1000.0f ), Ref::SKY_AP_MAX_SEGMENT_SAMPLES );

    // Monotone, so a longer segment never gets fewer samples than a shorter one.
    int previous = 0;
    for ( int i = 0; i <= 40; ++i )
    {
        const int n = Ref::SkyApSegmentSampleCount( static_cast<float>( i ) * 0.5f );
        EXPECT_GE( n, previous );
        previous = n;
    }
}

TEST( SkyScattering, AerialPerspectiveIsTheIdentityAtZeroDistance )
{
    ResetLutCallbacks();
    const Ref::SkyAtmParams p = EarthParams();

    const glm::vec3 origin( 0.0f, p.BottomRadiusKm + 0.002f, 0.0f );
    const glm::vec3 rayDir = glm::normalize( glm::vec3( 0.6f, 0.05f, -0.8f ) );
    const glm::vec3 sunDir = glm::normalize( glm::vec3( 0.3f, 0.5f, 0.8f ) );
    const glm::vec3 sun( 22.0f );

    // A surface touching the camera must take NO colour shift at all: slice 0 of the volume is an empty
    // segment, and an empty segment returns the accumulators it was given. `Fog.rgb + 0 * Fog.a` and
    // `Fog.a * 1` are then exactly the pixel that was there — the whole reason the fill starts at 0.
    const Ref::SkyScatterResult zero = Ref::SkyApIntegrateSegment( p, origin, rayDir, sunDir, sun, 0.0f, 0.0f,
                                                                   glm::vec3( 0.0f ), glm::vec3( 1.0f ) );
    EXPECT_FLOAT_EQ( zero.Luminance.r, 0.0f );
    EXPECT_FLOAT_EQ( zero.Luminance.g, 0.0f );
    EXPECT_FLOAT_EQ( zero.Luminance.b, 0.0f );
    EXPECT_FLOAT_EQ( zero.Transmittance.r, 1.0f );
    EXPECT_FLOAT_EQ( zero.Transmittance.g, 1.0f );
    EXPECT_FLOAT_EQ( zero.Transmittance.b, 1.0f );

    // An INVERTED segment is the identity too, not a negative one — the start-depth clamp produces
    // exactly that for every froxel nearer than the authored start.
    const Ref::SkyScatterResult inverted = Ref::SkyApIntegrateSegment( p, origin, rayDir, sunDir, sun, 4.0f, 1.0f,
                                                                       glm::vec3( 0.25f ), glm::vec3( 0.5f ) );
    EXPECT_FLOAT_EQ( inverted.Luminance.r, 0.25f );
    EXPECT_FLOAT_EQ( inverted.Transmittance.r, 0.5f );

    // And the whole volume's first slice, walked as the fill walks it, is the identity for any start
    // depth: distance 0 lifted to the start depth is still an empty segment.
    for ( const float startKm : { 0.0f, 0.1f, 5.0f } )
    {
        const float                 tEnd = SliceDistanceKm( 0, 96.0f );
        const Ref::SkyScatterResult slice0 =
             Ref::SkyApIntegrateSegment( p, origin, rayDir, sunDir, sun, glm::max( 0.0f, startKm ),
                                         glm::max( tEnd, startKm ), glm::vec3( 0.0f ), glm::vec3( 1.0f ) );
        EXPECT_FLOAT_EQ( slice0.Luminance.b, 0.0f ) << "start " << startKm;
        EXPECT_FLOAT_EQ( slice0.Transmittance.b, 1.0f ) << "start " << startKm;
    }
}

TEST( SkyScattering, AerialPerspectiveWalkAgreesWithADirectMarchToTheSameDistance )
{
    ResetLutCallbacks();
    const Ref::SkyAtmParams p = EarthParams();

    const glm::vec3 origin( 0.0f, p.BottomRadiusKm + 0.002f, 0.0f );
    const glm::vec3 sunDir = glm::normalize( glm::vec3( 0.4f, 0.35f, -0.85f ) );
    const glm::vec3 sun( 22.0f );

    for ( const glm::vec3 rayDir :
          { glm::normalize( glm::vec3( 0.0f, 0.02f, -1.0f ) ), glm::normalize( glm::vec3( 0.7f, 0.30f, -0.65f ) ),
            glm::normalize( glm::vec3( -0.5f, 0.10f, 0.85f ) ) } )
    {
        // The fill's walk: 16 slices, accumulators carried, exactly the shader's loop.
        glm::vec3 luminance( 0.0f );
        glm::vec3 transmittance( 1.0f );
        float     tPrev = 0.0f;

        for ( int slice = 0; slice < 16; ++slice )
        {
            const float                 tNext = SliceDistanceKm( slice, 96.0f );
            const Ref::SkyScatterResult step  = Ref::SkyApIntegrateSegment( p, origin, rayDir, sunDir, sun, tPrev,
                                                                            tNext, luminance, transmittance );
            luminance                         = step.Luminance;
            transmittance                     = step.Transmittance;
            tPrev                             = tNext;

            // THE relation the volume's whole arrangement depends on: what slice N holds must be the
            // atmosphere between the CAMERA and slice N, not an accumulation artefact. Compared against
            // a 4000-step brute-force march of the same medium over [0, tNext] — an independent
            // statement of the same physics, so a carried-accumulator bug, a dropped segment or an
            // off-by-one in the slice distances shows here rather than as a shell in a screenshot.
            const glm::vec3 reference = BruteForceTransmittance( p, origin, rayDir, tNext, 4000 );

            EXPECT_NEAR( transmittance.r, reference.r, 4e-3f ) << "slice " << slice;
            EXPECT_NEAR( transmittance.g, reference.g, 4e-3f ) << "slice " << slice;
            EXPECT_NEAR( transmittance.b, reference.b, 4e-3f ) << "slice " << slice;

            // Transmittance is a survival probability and it only ever falls with distance.
            EXPECT_LE( transmittance.b, 1.0f );
            EXPECT_GE( transmittance.b, 0.0f );

            // In-scatter only ever accumulates: no segment may remove light already gathered.
            EXPECT_GE( luminance.r, 0.0f );
            EXPECT_GE( luminance.b, 0.0f );
        }

        // The walk's own final transmittance, against the same brute force over the whole 96 km.
        const glm::vec3 reference = BruteForceTransmittance( p, origin, rayDir, 96.0f, 4000 );
        EXPECT_NEAR( transmittance.r, reference.r, 4e-3f );
        EXPECT_NEAR( transmittance.b, reference.b, 4e-3f );
    }
}

TEST( SkyScattering, AerialPerspectiveGrowsWithDistanceAndAgreesWithTheDistantSky )
{
    ResetLutCallbacks();
    const Ref::SkyAtmParams p = EarthParams();

    const glm::vec3 origin( 0.0f, p.BottomRadiusKm + 0.002f, 0.0f );
    const glm::vec3 rayDir = glm::normalize( glm::vec3( 0.0f, 0.02f, -1.0f ) );
    const glm::vec3 sunDir = glm::normalize( glm::vec3( 0.0f, 0.35f, -0.94f ) );
    const glm::vec3 sun( 22.0f );

    // Monotone in distance, in BOTH lanes: more air between the eye and a surface can only add light
    // and only remove background.
    glm::vec3 previousLuminance( 0.0f );
    glm::vec3 previousTransmittance( 1.0f );
    for ( int slice = 1; slice < 16; ++slice )
    {
        const Ref::SkyScatterResult r =
             Ref::SkyApIntegrateSegment( p, origin, rayDir, sunDir, sun, 0.0f, SliceDistanceKm( slice, 96.0f ),
                                         glm::vec3( 0.0f ), glm::vec3( 1.0f ) );

        EXPECT_GT( r.Luminance.b, previousLuminance.b );
        EXPECT_LT( r.Transmittance.b, previousTransmittance.b );
        previousLuminance     = r.Luminance;
        previousTransmittance = r.Transmittance;
    }

    // THE POINT OF THE WHOLE PASS: the haze on a distant surface and the sky above the horizon are ONE
    // quantity. March the same ray to the shell exit with the distant-sky integrator and the volume's
    // 96 km must already be most of the way there — the same medium, the same LUT callbacks, the same
    // sun. A factor between them (a missing phase, a doubled tint, kilometres against centimetres)
    // shows here as a ratio nowhere near 1.
    const Ref::SkyScatterResult distant =
         Ref::SkyIntegrateScatteredLuminance( p, origin, rayDir, sunDir, sun, 64 );

    EXPECT_GT( previousLuminance.b, distant.Luminance.b * 0.5f );
    EXPECT_LT( previousLuminance.b, distant.Luminance.b * 1.05f );
}

// ---------------------------------------------------------------------------------------------------
// The distant sky light — the average sky the fog is lit by (UE's Distant Sky Light LUT)
//
// One value per frame, produced on the GPU by 64 threads and a groupshared sum. What is pinned here is
// the ARITHMETIC that sum performs, because the reduction itself is the one part a unit test cannot
// reach: if the mean of the 64 directions is right, a wrong reduction shows up as a fog that is 64x or
// 2x off, which is exactly what "bounded by the brightest direction" catches on a frame.
// ---------------------------------------------------------------------------------------------------

TEST( SkyDistantLight, DirectionSetIsTheUniformSphereGridExactlyOnce )
{
    // The 64 directions must be the 8x8 area-uniform grid, each exactly once: the mean of the sample
    // set IS the sphere integral only because the cells are equal solid angles, and the GPU takes
    // index gl_LocalInvocationIndex where this loop takes i.
    glm::vec3 mean( 0.0f );
    for ( int i = 0; i < Ref::SKY_DISTANT_LIGHT_DIRECTIONS; ++i )
    {
        const glm::vec3 dir = Ref::SkyDistantLightDirection( i );
        EXPECT_NEAR( glm::length( dir ), 1.0f, 1e-5f ) << "direction " << i;

        const int row = i / 8;
        const int col = i - row * 8;

        const glm::vec3 expected = Ref::SkyUniformSphereDirection( ( static_cast<float>( row ) + 0.5f ) / 8.0f,
                                                                   ( static_cast<float>( col ) + 0.5f ) / 8.0f );
        EXPECT_NEAR( dir.x, expected.x, 1e-6f ) << "direction " << i;
        EXPECT_NEAR( dir.y, expected.y, 1e-6f ) << "direction " << i;
        EXPECT_NEAR( dir.z, expected.z, 1e-6f ) << "direction " << i;

        mean += dir;
    }

    // A balanced set sums to the origin — the property that makes the plain average unbiased.
    mean /= static_cast<float>( Ref::SKY_DISTANT_LIGHT_DIRECTIONS );
    EXPECT_NEAR( mean.x, 0.0f, 1e-5f );
    EXPECT_NEAR( mean.y, 0.0f, 1e-5f );
    EXPECT_NEAR( mean.z, 0.0f, 1e-5f );
}

TEST( SkyDistantLight, IsPositiveAndBoundedByTheBrightestDirectionItSampled )
{
    ResetLutCallbacks();

    const Ref::SkyAtmParams p = EarthParams();

    const glm::vec3 sunDir         = glm::normalize( glm::vec3( 0.6f, 0.8f, 0.0f ) );
    const glm::vec3 sunIlluminance = glm::vec3( 22.0f );

    // The 64 samples the reduction averages, evaluated one at a time.
    glm::vec3 brightest( 0.0f );
    glm::vec3 dimmest( 1e9f );
    glm::vec3 sum( 0.0f );
    for ( int i = 0; i < Ref::SKY_DISTANT_LIGHT_DIRECTIONS; ++i )
    {
        const glm::vec3 radiance =
             Ref::SkyDistantLightRadiance( p, Ref::SkyDistantLightDirection( i ), sunDir, sunIlluminance,
                                           Ref::SKY_DISTANT_LIGHT_ALTITUDE_KM, Ref::SKY_DISTANT_LIGHT_SAMPLES );

        // Radiance is never negative — a sky that removes light from the fog is not a sky.
        EXPECT_GE( radiance.x, 0.0f ) << "direction " << i;
        EXPECT_GE( radiance.y, 0.0f ) << "direction " << i;
        EXPECT_GE( radiance.z, 0.0f ) << "direction " << i;

        brightest = glm::max( brightest, radiance );
        dimmest   = glm::min( dimmest, radiance );
        sum += radiance;
    }

    const glm::vec3 average = Ref::SkyDistantLight( p, sunDir, sunIlluminance );

    // THE BOUND. An average lies between the dimmest and the brightest of what it averaged — and this
    // one catches the whole class of reduction mistakes at once: a missing division, a 4pi that should
    // not be there, a stride that adds a direction twice.
    for ( int c = 0; c < 3; ++c )
    {
        EXPECT_GT( average[c], 0.0f ) << "channel " << c;
        EXPECT_LE( average[c], brightest[c] + 1e-5f ) << "channel " << c;
        EXPECT_GE( average[c], dimmest[c] - 1e-5f ) << "channel " << c;
    }

    // And it is exactly the mean of those same 64 samples — the GPU's groupshared sum divided by 64,
    // spelled out.
    const glm::vec3 expected = sum / static_cast<float>( Ref::SKY_DISTANT_LIGHT_DIRECTIONS );
    EXPECT_NEAR( average.x, expected.x, 1e-5f );
    EXPECT_NEAR( average.y, expected.y, 1e-5f );
    EXPECT_NEAR( average.z, expected.z, 1e-5f );

    // The daylit sky is BLUE: more short-wavelength light reaches the eye from every direction than
    // long. A distant sky light that came out neutral or warm at noon would tint every fogged frame.
    EXPECT_GT( average.z, average.x );
}

TEST( SkyDistantLight, TheTexelTheFillWritesIsTheOneTheEngineNames )
{
    // The direction set is a full sphere and the reduction is a single mean, so the fill writes exactly
    // one texel — and the column it writes has to be the column the height fog reads. Two constants,
    // one in GLSL and one in C++, and this is the assertion that stops them drifting.
    EXPECT_EQ( Ref::SKY_DISTANT_LIGHT_SPHERE_TEXEL, Desert::Graphic::kDistantLightSphereTexel );
    EXPECT_LT( static_cast<uint32_t>( Ref::SKY_DISTANT_LIGHT_SPHERE_TEXEL ), Desert::Graphic::kDistantLightWidth );

    // Nothing sits exactly on the horizon, so no cell of the grid is ambiguous about which half of the
    // sphere it belongs to — the property the area-uniform mapping is supposed to have.
    for ( int i = 0; i < Ref::SKY_DISTANT_LIGHT_DIRECTIONS; ++i )
        EXPECT_GT( glm::abs( Ref::SkyDistantLightDirection( i ).y ), 1e-3f ) << "direction " << i;
}

TEST( SkyDistantLight, FallsAsTheSunSets )
{
    ResetLutCallbacks();

    const Ref::SkyAtmParams p = EarthParams();

    // The ambient a fog receives must follow the sun down. It is a MONOTONE fall, not a spot value:
    // an ambient that brightened anywhere on the way to sunset would light the fog from a sky that
    // is not there.
    float previousLuminance = 1e9f;
    for ( const float elevationDeg : { 90.0f, 60.0f, 40.0f, 25.0f, 15.0f, 8.0f, 3.0f, 0.0f, -5.0f } )
    {
        const float     rad    = elevationDeg * 3.14159265358979323846f / 180.0f;
        const glm::vec3 sunDir = glm::vec3( std::cos( rad ), std::sin( rad ), 0.0f );

        const glm::vec3 average = Ref::SkyDistantLight( p, sunDir, glm::vec3( 22.0f ) );

        const float luminance = 0.2126f * average.x + 0.7152f * average.y + 0.0722f * average.z;
        EXPECT_GE( luminance, 0.0f ) << "elevation " << elevationDeg;
        EXPECT_LT( luminance, previousLuminance ) << "elevation " << elevationDeg;
        previousLuminance = luminance;
    }

    // Five degrees under the horizon the sky is all but out — with no multi-scattering term in this
    // configuration, what is left is the thin twilight the single-scattering march can still see.
    EXPECT_LT( previousLuminance, 0.05f );
}

// ---------------------------------------------------------------------------------------------------
// SkySunAtAltitude — the sun at a point INSIDE the atmosphere, minus the fetch (Р14)
//
// The two passes that march the volumetric cloud field read the transmittance LUT through this, and they
// read it through the SAME text so that a visible deck and the deck baked into the IBL panorama cannot
// drift apart. What is asserted here is the composition — that it is the sky's own terminator and the
// clamped uv, not a second opinion about either.
// ---------------------------------------------------------------------------------------------------

TEST( SkySunAtPointReader, ItIsTheSkysOwnTerminatorAndNotASecondOpinionAboutIt )
{
    // THE RELATION THAT KEEPS A CLOUD AND THE SKY BEHIND IT ON ONE TERMINATOR. A hard ray-sphere test
    // here would be defensible on its own and would still be a seam: the deck would switch off instantly
    // against a sky fading over SKY_TERMINATOR_HALF_WIDTH_COS. Asserting equality with SkyPlanetShadow —
    // rather than asserting "it is zero at night" — is what makes that structural.
    const Ref::SkyAtmParams p = EarthParams();

    const glm::vec2 lutSize( 256.0f, 64.0f );

    for ( const float altitudeKm : { 0.0f, 0.01f, 2.0f, 5.0f, 12.0f } )
    {
        const float radius = p.BottomRadiusKm + altitudeKm;

        for ( const float mu : { 1.0f, 0.5f, 0.05f, 0.0f, -0.001f, -0.002f, -0.05f, -0.5f, -1.0f } )
        {
            const Ref::SkySunAtPoint sun =
                 Ref::SkySunAtAltitude( p.BottomRadiusKm, p.TopRadiusKm, altitudeKm, mu, lutSize );

            EXPECT_FLOAT_EQ( sun.PlanetShadow, Ref::SkyPlanetShadow( radius, mu, p.BottomRadiusKm ) )
                 << "altitude " << altitudeKm << ", mu " << mu;

            // And the uv is the mapping's, clamped — not the mapping's, raw. The band is half a texel,
            // which is what CLAMP_TO_EDGE would give and what this engine's REPEAT samplers will not.
            EXPECT_FLOAT_EQ(
                 sun.Uv.x,
                 Ref::SkyTransmittanceLutUvClamped( p.BottomRadiusKm, p.TopRadiusKm, radius, mu, lutSize ).x )
                 << "altitude " << altitudeKm << ", mu " << mu;
        }
    }
}

TEST( SkySunAtPointReader, TheShadowAndTheFetchSwitchOffTogetherAtTheSamplesOwnHorizon )
{
    // A SAMPLE'S HORIZON IS NOT THE WORLD'S, and this is the half of the relation a ground-level test
    // cannot see: at eight kilometres the sun is still up at an elevation of -2.8 degrees. A reader that
    // used a fixed zero here would darken a high deck while the sky behind it was still lit.
    const Ref::SkyAtmParams p = EarthParams();

    const glm::vec2 lutSize( 256.0f, 64.0f );

    for ( const float altitudeKm : { 0.01f, 2.0f, 8.0f, 20.0f } )
    {
        const float radius = p.BottomRadiusKm + altitudeKm;
        const float horizonCos =
             -std::sqrt( std::max( 1.0f - ( p.BottomRadiusKm / radius ) * ( p.BottomRadiusKm / radius ), 0.0f ) );

        // A whole band below its own horizon is night, and the reader says so with an exact zero — which
        // is what lets both marches take an early return instead of a diverged fetch.
        const Ref::SkySunAtPoint night =
             Ref::SkySunAtAltitude( p.BottomRadiusKm, p.TopRadiusKm, altitudeKm, horizonCos - 0.01f, lutSize );
        EXPECT_FLOAT_EQ( night.PlanetShadow, 0.0f ) << "altitude " << altitudeKm;

        // And a band above it is full sun, not a fraction of one.
        const Ref::SkySunAtPoint day =
             Ref::SkySunAtAltitude( p.BottomRadiusKm, p.TopRadiusKm, altitudeKm, horizonCos + 0.01f, lutSize );
        EXPECT_FLOAT_EQ( day.PlanetShadow, 1.0f ) << "altitude " << altitudeKm;

        // The horizon rises with altitude in the only sense that matters: a HIGHER sample still sees the
        // sun at an elevation where a lower one no longer does. Without this the whole per-sample
        // treatment would be indistinguishable from evaluating it once at the ground.
        if ( altitudeKm > 1.0f )
        {
            const float groundRadius  = p.BottomRadiusKm + 0.01f;
            const float groundHorizon = -std::sqrt( std::max(
                 1.0f - ( p.BottomRadiusKm / groundRadius ) * ( p.BottomRadiusKm / groundRadius ), 0.0f ) );
            EXPECT_LT( horizonCos, groundHorizon )
                 << "the sample's horizon does not dip further with altitude, so the reader has stopped "
                    "distinguishing a high deck from the ground";
        }
    }
}

// ---------------------------------------------------------------------------------------------------
// The cloud's aerial perspective — a RELATION between two passes, not a value in one of them
//
// A cloud and the sky beside it are seen through the same air. The sky pixel is the full integral of
// the medium along the ray; the cloud pixel is that integral truncated at the cloud, composed with what
// the cloud itself scattered. What must be true is that the two agree — a cloud the atmosphere has taken
// away has to BECOME the sky, not merely fade toward something. A composition that is off by a factor,
// or that hazes toward a colour the sky does not have, passes every test of either pass alone and is
// visible in a frame as a distant band that is too dark, too pale or the wrong hue.
//
// Both sides come from the shaders' own text: Common/CloudAerial.glslh through CloudAerialReference.hpp
// (the composition and the ramp, shared by the screen march and the panorama bake) and
// Common/SkyScattering.glslh through SkyScatteringReference.hpp (the froxel walk and the distant sky).
// ---------------------------------------------------------------------------------------------------

namespace
{
    // The froxel column's walk, exactly as SkyAerialPerspectiveLut.shader performs it, stopped at
    // @p distanceKm. That is the quantity the volume holds and the cloud march fetches, so it is the
    // right stand-in for the texture read — the interpolation between slices is the only thing left out.
    Ref::SkyScatterResult ApTo( const Ref::SkyAtmParams& p, const glm::vec3& originKm, const glm::vec3& rayDir,
                                const glm::vec3& sunDir, const glm::vec3& sun, float distanceKm,
                                float startDepthKm )
    {
        glm::vec3 luminance( 0.0f );
        glm::vec3 transmittance( 1.0f );
        float     tPrev = 0.0f;

        for ( int slice = 0; slice < 16; ++slice )
        {
            const float unit  = static_cast<float>( slice ) / 15.0f;
            const float tNext = glm::min( Ref::SkyApDistanceFromSliceUnit( unit, 96.0f ), distanceKm );

            const Ref::SkyScatterResult step =
                 Ref::SkyApIntegrateSegment( p, originKm, rayDir, sunDir, sun, glm::max( tPrev, startDepthKm ),
                                             glm::max( tNext, startDepthKm ), luminance, transmittance );
            luminance     = step.Luminance;
            transmittance = step.Transmittance;
            tPrev         = tNext;

            if ( tPrev >= distanceKm )
                break;
        }

        Ref::SkyScatterResult r;
        r.Luminance     = luminance;
        r.Transmittance = transmittance;
        return r;
    }

    float MeanChannel( const glm::vec3& v )
    {
        return ( v.r + v.g + v.b ) / 3.0f;
    }
} // namespace

TEST( CloudAerialPerspective, AmountIsUnrealsRampAndFullByDefault )
{
    namespace Cloud = Desert::Tests::CloudAerialRef;

    // A fade distance of zero is "apply it in full, immediately" — the ramp degenerating, not a branch
    // that skips the term. Both engines default to this, and it is what the component's defaults give.
    for ( const float d : { 0.0f, 1.0f, 30.0f, 500.0f } )
    {
        EXPECT_FLOAT_EQ( Cloud::CloudAerialAmount( d, 0.0f, 0.0f ), 1.0f ) << "distance " << d;
        EXPECT_FLOAT_EQ( Cloud::CloudAerialAmount( d, 30.0f, 0.0f ), 1.0f ) << "distance " << d;
    }

    // With a ramp it is saturate( ( distance - start ) / fade ), the same expression Unreal evaluates as
    // saturate( tDepth * FadeDistanceKmInv ).
    EXPECT_FLOAT_EQ( Cloud::CloudAerialAmount( 10.0f, 30.0f, 90.0f ), 0.0f );
    EXPECT_FLOAT_EQ( Cloud::CloudAerialAmount( 30.0f, 30.0f, 90.0f ), 0.0f );
    EXPECT_FLOAT_EQ( Cloud::CloudAerialAmount( 60.0f, 30.0f, 90.0f ), 1.0f / 3.0f );
    EXPECT_FLOAT_EQ( Cloud::CloudAerialAmount( 120.0f, 30.0f, 90.0f ), 1.0f );
    EXPECT_FLOAT_EQ( Cloud::CloudAerialAmount( 500.0f, 30.0f, 90.0f ), 1.0f );

    // Monotone and bounded, so the dial can only ever remove atmosphere and never add it.
    float previous = -1.0f;
    for ( int i = 0; i <= 40; ++i )
    {
        const float a = Cloud::CloudAerialAmount( static_cast<float>( i ) * 5.0f, 30.0f, 90.0f );
        EXPECT_GE( a, previous );
        EXPECT_GE( a, 0.0f );
        EXPECT_LE( a, 1.0f );
        previous = a;
    }
}

TEST( CloudAerialPerspective, MeanDistanceIsTheTransmittanceWeightedOne )
{
    namespace Cloud = Desert::Tests::CloudAerialRef;

    // Two samples, the near one twice as visible as the far one: the term is evaluated where the cloud
    // the eye can SEE is, not at the middle of the marched span.
    const float weighted = 4.0f * 0.8f + 20.0f * 0.4f;
    const float sum      = 0.8f + 0.4f;
    EXPECT_FLOAT_EQ( Cloud::CloudAerialMeanDistanceKm( weighted, sum, 1.0f ), weighted / sum );

    // The atmosphere's view-distance scale is a multiplier on the read side, and a negative one is
    // clamped away rather than folding the cloud to the camera.
    EXPECT_FLOAT_EQ( Cloud::CloudAerialMeanDistanceKm( weighted, sum, 2.0f ), 2.0f * weighted / sum );
    EXPECT_FLOAT_EQ( Cloud::CloudAerialMeanDistanceKm( weighted, sum, -1.0f ), 0.0f );
}

TEST( CloudAerialPerspective, ACloudThatIsTheSkyBehindItCompositesToTheSky )
{
    namespace Cloud = Desert::Tests::CloudAerialRef;

    ResetLutCallbacks();
    const Ref::SkyAtmParams p = EarthParams();

    const glm::vec3 origin( 0.0f, p.BottomRadiusKm + 0.002f, 0.0f );
    const glm::vec3 sunDir = glm::normalize( glm::vec3( 0.0f, 0.35f, -0.94f ) );
    const glm::vec3 sun( 22.0f );

    for ( const glm::vec3 rayDir :
          { glm::normalize( glm::vec3( 0.0f, 0.0349f, -1.0f ) ), glm::normalize( glm::vec3( 0.4f, 0.20f, -0.9f ) ),
            glm::normalize( glm::vec3( -0.3f, 0.70f, 0.6f ) ) } )
    {
        // The pixel the frame already holds where there is no cloud: the whole medium, to the shell.
        const Ref::SkyScatterResult sky =
             Ref::SkyIntegrateScatteredLuminance( p, origin, rayDir, sunDir, sun, 64 );

        for ( const float distanceKm : { 5.0f, 20.0f, 60.0f, 96.0f } )
        {
            const Ref::SkyScatterResult ap = ApTo( p, origin, rayDir, sunDir, sun, distanceKm, 0.1f );

            // The sky the cloud stands in front of, marched from the cloud's own position. The medium is
            // additive along the ray, so this and the volume must reconstruct the sky pixel exactly —
            // the premise everything below rests on, asserted rather than assumed.
            const glm::vec3             beyondOrigin = origin + rayDir * distanceKm;
            const Ref::SkyScatterResult beyond =
                 Ref::SkyIntegrateScatteredLuminance( p, beyondOrigin, rayDir, sunDir, sun, 64 );

            // The only slack anywhere below: two quadratures of one integral (16 froxel segments against
            // a 64-sample march) never agree to the last bit. Everything else is asserted exactly.
            const float quadrature = 0.02f;

            const glm::vec3 reconstructed = ap.Luminance + ap.Transmittance * beyond.Luminance;
            EXPECT_NEAR( reconstructed.r, sky.Luminance.r, quadrature ) << "d=" << distanceKm;
            EXPECT_NEAR( reconstructed.g, sky.Luminance.g, quadrature ) << "d=" << distanceKm;
            EXPECT_NEAR( reconstructed.b, sky.Luminance.b, quadrature ) << "d=" << distanceKm;

            // THE TEST. A "cloud" that emits exactly the sky behind it, over any coverage, is not a
            // cloud at all — the composite has to give the sky pixel back untouched. It is the strongest
            // available statement that the cloud is hazed toward the SKY and not toward some other
            // colour: a wrong factor on either term, or an in-scatter added twice, leaves a residue here
            // that no picture of a real cloud would separate from "the distant band looks a bit off".
            for ( const float cloudTransmittance : { 0.0f, 0.25f, 0.6f, 0.95f } )
            {
                const glm::vec3 asIfAir = ( 1.0f - cloudTransmittance ) * beyond.Luminance;

                const glm::vec3 hazed = Cloud::CloudApplyAerialPerspective(
                     asIfAir, cloudTransmittance, ap.Luminance, MeanChannel( ap.Transmittance ), 1.0f );
                const glm::vec3 pixel = hazed + cloudTransmittance * sky.Luminance;

                // EXACTLY the sky, up to ONE named term: the volume's scalar alpha standing in for a
                // spectral transmittance, which Unreal collapses the same way and for the same reason.
                // Written out rather than absorbed into a tolerance — the residue is
                // ( meanT - transmittance ) times the cloud's own light, and stating it that way means
                // the test measures the approximation instead of hiding a wrong factor behind it.
                const glm::vec3 collapse = ( MeanChannel( ap.Transmittance ) - ap.Transmittance ) * asIfAir;

                EXPECT_NEAR( pixel.r, sky.Luminance.r + collapse.r, quadrature )
                     << "d=" << distanceKm << " Tc=" << cloudTransmittance;
                EXPECT_NEAR( pixel.g, sky.Luminance.g + collapse.g, quadrature )
                     << "d=" << distanceKm << " Tc=" << cloudTransmittance;
                EXPECT_NEAR( pixel.b, sky.Luminance.b + collapse.b, quadrature )
                     << "d=" << distanceKm << " Tc=" << cloudTransmittance;
            }
        }
    }
}

TEST( CloudAerialPerspective, ContrastAgainstTheSkyIsAttenuatedByTheAirAndNothingElse )
{
    namespace Cloud = Desert::Tests::CloudAerialRef;

    ResetLutCallbacks();
    const Ref::SkyAtmParams p = EarthParams();

    const glm::vec3 origin( 0.0f, p.BottomRadiusKm + 0.002f, 0.0f );
    const glm::vec3 rayDir = glm::normalize( glm::vec3( 0.0f, 0.0349f, -1.0f ) );
    const glm::vec3 sunDir = glm::normalize( glm::vec3( 0.0f, 1.0f, 0.0f ) );
    const glm::vec3 sun( 22.0f );

    const Ref::SkyScatterResult sky = Ref::SkyIntegrateScatteredLuminance( p, origin, rayDir, sunDir, sun, 64 );

    const float cloudTransmittance = 0.15f;
    // A bright cumulus face: several times the horizon sky, which is what makes the band read as cloud.
    const glm::vec3 cloudLuminance = ( 1.0f - cloudTransmittance ) * glm::vec3( 3.0f, 3.0f, 3.0f );

    float previousResidual = std::numeric_limits<float>::max();

    for ( const float distanceKm : { 5.0f, 10.0f, 20.0f, 40.0f, 60.0f, 96.0f } )
    {
        const Ref::SkyScatterResult ap    = ApTo( p, origin, rayDir, sunDir, sun, distanceKm, 0.1f );
        const float                 meanT = MeanChannel( ap.Transmittance );

        const glm::vec3             beyondOrigin = origin + rayDir * distanceKm;
        const Ref::SkyScatterResult beyond =
             Ref::SkyIntegrateScatteredLuminance( p, beyondOrigin, rayDir, sunDir, sun, 64 );

        const glm::vec3 hazed =
             Cloud::CloudApplyAerialPerspective( cloudLuminance, cloudTransmittance, ap.Luminance, meanT, 1.0f );
        const glm::vec3 pixel = hazed + cloudTransmittance * sky.Luminance;

        // The whole point, in one line: what separates the cloud from the sky beside it is the cloud's
        // own light times the air's transmittance, LESS the sky it displaced times the same air — and
        // nothing else. No extra fade, no second attenuation, no term that depends on how bright the
        // cloud happens to be. The cloud's side takes the scalar alpha and the sky's side the spectral
        // one, which is the collapse named in the test above and is written into the prediction rather
        // than tolerated.
        const glm::vec3 predicted =
             meanT * cloudLuminance - ( 1.0f - cloudTransmittance ) * ap.Transmittance * beyond.Luminance;
        const glm::vec3 measured = pixel - sky.Luminance;

        EXPECT_NEAR( measured.r, predicted.r, 0.02f ) << "d=" << distanceKm;
        EXPECT_NEAR( measured.g, predicted.g, 0.02f ) << "d=" << distanceKm;
        EXPECT_NEAR( measured.b, predicted.b, 0.02f ) << "d=" << distanceKm;

        // And it only ever shrinks with distance: more air can only take the cloud further toward the
        // sky, never bring it back out of it.
        const float residual = glm::length( measured );
        EXPECT_LT( residual, previousResidual ) << "d=" << distanceKm;
        previousResidual = residual;
    }
}

TEST( CloudAerialPerspective, TheRampFadesTheATMOSPHERE_NotTheResultTowardNothing )
{
    namespace Cloud = Desert::Tests::CloudAerialRef;

    ResetLutCallbacks();
    const Ref::SkyAtmParams p = EarthParams();

    const glm::vec3 origin( 0.0f, p.BottomRadiusKm + 0.002f, 0.0f );
    const glm::vec3 rayDir = glm::normalize( glm::vec3( 0.0f, 0.0349f, -1.0f ) );
    const glm::vec3 sunDir = glm::normalize( glm::vec3( 0.0f, 0.5f, -0.87f ) );
    const glm::vec3 sun( 22.0f );

    const Ref::SkyScatterResult ap    = ApTo( p, origin, rayDir, sunDir, sun, 60.0f, 0.1f );
    const float                 meanT = MeanChannel( ap.Transmittance );

    const float     cloudTransmittance = 0.3f;
    const glm::vec3 cloudLuminance( 2.2f, 2.1f, 1.9f );

    for ( const float amount : { 0.0f, 0.25f, 0.5f, 1.0f } )
    {
        const glm::vec3 ours =
             Cloud::CloudApplyAerialPerspective( cloudLuminance, cloudTransmittance, ap.Luminance, meanT, amount );

        // Unreal ramps the SAMPLE toward "no atmosphere" before composing with it
        // (SkyAtmosphereCommon.ush: `AP.rgb *= Weight; AP.a = 1 - Weight * ( 1 - AP.a )`) and then runs
        // the composition at full strength. Our cheaper mix of the RESULT has to be the same value, or
        // the dial means something different here than it does there for anyone porting a setting.
        const glm::vec3 fadedInScatter = ap.Luminance * amount;
        const float     fadedT         = 1.0f - amount * ( 1.0f - meanT );
        const glm::vec3 unreal         = Cloud::CloudApplyAerialPerspective( cloudLuminance, cloudTransmittance,
                                                                             fadedInScatter, fadedT, 1.0f );

        EXPECT_NEAR( ours.r, unreal.r, 1e-5f ) << "amount " << amount;
        EXPECT_NEAR( ours.g, unreal.g, 1e-5f ) << "amount " << amount;
        EXPECT_NEAR( ours.b, unreal.b, 1e-5f ) << "amount " << amount;
    }

    // Amount 0 is the cloud untouched — the setting the scenes shipped, stated so its meaning is not in
    // doubt: below the start distance the atmosphere is not attenuated, it is ABSENT.
    const glm::vec3 none =
         Cloud::CloudApplyAerialPerspective( cloudLuminance, cloudTransmittance, ap.Luminance, meanT, 0.0f );
    EXPECT_FLOAT_EQ( none.r, cloudLuminance.r );
    EXPECT_FLOAT_EQ( none.g, cloudLuminance.g );
    EXPECT_FLOAT_EQ( none.b, cloudLuminance.b );
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
