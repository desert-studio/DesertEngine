// The exponential height fog's closed form, tested against the medium it claims to integrate.
//
// The closed form (Common/HeightFog.glslh, compiled here AS C++ through HeightFogReference.hpp) states:
// transmittance = exp2(-OpticalDepth) with OpticalDepth the exact line integral of the exp2-exponential
// density sigma(y) = ln2 * Density * 2^(-Falloff * (y - FogHeight)) along the camera-to-point segment.
// Every test below is a RELATION, not a spot value: the closed form against a brute-force Riemann sum of
// that same sigma; transmittance against its own multiplicativity across a split (which is what makes
// StartDistance an exclusion, not a fade); two layers against the sum of each alone; the clamp against
// the floor it promises. A single wrong factor anywhere — a lost ln2, a falloff applied to the wrong
// axis, a start distance that fades instead of excluding — breaks one of them.

#include "HeightFogReference.hpp"

#include <Engine/Graphic/Fog/FogPayload.hpp>

#include <glm/glm.hpp>

#include <gtest/gtest.h>

#include <bit>
#include <cmath>
#include <cstdint>

using namespace Desert::Tests::HeightFogRef;

namespace
{
    constexpr float kLn2 = 0.6931471805599453f;

    // The natural-log line integral of the medium the closed form integrates, by midpoint Riemann sum:
    // sigma(y) = ln2 * density * 2^(-falloff * (y - fogHeight)), summed along camera -> camera + dir*L.
    // 4096 steps puts the midpoint rule's error orders below the tolerances used against it.
    double NumericOpticalDepth( float density, float falloff, float fogHeightKm, glm::vec3 cameraKm, glm::vec3 dir,
                                float lengthKm )
    {
        constexpr int n   = 4096;
        const double  dt  = static_cast<double>( lengthKm ) / n;
        double        sum = 0.0;
        for ( int i = 0; i < n; ++i )
        {
            const double t = ( i + 0.5 ) * dt;
            const double y = cameraKm.y + dir.y * t;
            sum += kLn2 * density * std::exp2( -falloff * ( y - fogHeightKm ) ) * dt;
        }
        return sum;
    }

    // The closed form for one layer, assembled from the same primitives HeightFogEvaluate uses.
    float ClosedOpticalDepth( float density, float falloff, float fogHeightKm, glm::vec3 cameraKm, glm::vec3 dir,
                              float lengthKm )
    {
        const float collapsed = HeightFogCollapsedDensity( density, falloff, cameraKm.y, fogHeightKm );
        return HeightFogLineIntegral( collapsed, falloff, dir.y * lengthKm ) * lengthKm;
    }

    // A parameter block with everything colour-related neutral, so transmittance is the whole story.
    HeightFogParams PlainParams( float density0, float falloff0, float height0Km )
    {
        HeightFogParams p;
        p.Density0            = density0;
        p.Falloff0            = falloff0;
        p.Height0Km           = height0Km;
        p.Density1            = 0.0f;
        p.Falloff1            = 0.0f;
        p.Height1Km           = 0.0f;
        p.StartDistanceKm     = 0.0f;
        p.CutoffDistanceKm    = 0.0f;
        p.MinTransmittance    = 0.0f;
        p.Inscattering        = glm::vec3( 1.0f, 1.0f, 1.0f );
        p.DirectionalColour   = glm::vec3( 0.0f, 0.0f, 0.0f );
        p.DirectionalExponent = 4.0f;
        p.DirectionalStartKm  = 0.0f;
        p.SunDirection        = glm::vec3( 0.0f, 1.0f, 0.0f );
        return p;
    }
} // namespace

// ---------------------------------------------------------------------------------------------------
// The closed form IS the integral of the medium it names
// ---------------------------------------------------------------------------------------------------

TEST( HeightFogClosedForm, MatchesNumericIntegrationAcrossHeightsAnglesAndFalloffs )
{
    // The grid stays inside the clamp-free domain: |falloff * (y - fogHeight)| < 110 everywhere along
    // every ray, so neither float-safety saturation (the -125..126 collapse clamp, the -127 integral
    // clamp) engages — those saturate DELIBERATELY where the true integral overflows float, and the
    // extreme test below covers them. Falloff 20/km is already UE's default (0.2) x100.
    const float cameraHeights[] = { -0.1f, 0.0f, 0.05f, 0.5f, 2.0f }; // km, below and above the floor
    const float falloffs[]      = { 0.0f, 0.005f, 1.0f, 5.0f, 20.0f };
    const float lengths[]       = { 0.05f, 1.0f, 5.0f };

    const glm::vec3 directions[] = {
         { 0.0f, 1.0f, 0.0f },                            // straight up
         { 0.0f, -1.0f, 0.0f },                           // straight down
         { 1.0f, 0.0f, 0.0f },                            // horizontal: the Taylor branch's home
         glm::normalize( glm::vec3( 1.0f, 0.4f, 0.3f ) ), // oblique up
         glm::normalize( glm::vec3( 0.6f, -0.3f, 0.8f ) ) // oblique down
    };

    const float density     = 2.0f; // UE's default 0.02, in the per-km form PackFogParams produces
    const float fogHeightKm = 0.0f;

    for ( const float cameraY : cameraHeights )
        for ( const float falloff : falloffs )
            for ( const glm::vec3& dir : directions )
                for ( const float length : lengths )
                {
                    const glm::vec3 camera( 0.3f, cameraY, -0.1f );
                    const double    numeric =
                         NumericOpticalDepth( density, falloff, fogHeightKm, camera, dir, length );
                    const float closed = ClosedOpticalDepth( density, falloff, fogHeightKm, camera, dir, length );

                    // 2e-3 relative covers the Riemann residue at the steepest falloff; the absolute
                    // term covers the cases where both sides have decayed to (the same) nothing.
                    EXPECT_NEAR( closed, numeric, 2e-3 * numeric + 1e-5 )
                         << "cameraY " << cameraY << " falloff " << falloff << " dir.y " << dir.y << " length "
                         << length;
                }
}

TEST( HeightFogClosedForm, FalloffZeroIsTheUniformMedium )
{
    // With no height dependence the integral collapses to density * ln2 * length, whatever the ray does.
    const float density = 2.0f;
    const float length  = 3.0f;
    for ( const glm::vec3& dir : { glm::vec3( 0.0f, 1.0f, 0.0f ), glm::vec3( 1.0f, 0.0f, 0.0f ) } )
    {
        const float od = ClosedOpticalDepth( density, 0.0f, 0.0f, glm::vec3( 0.0f, 5.0f, 0.0f ), dir, length );
        EXPECT_NEAR( od, density * kLn2 * length, 1e-5f * od );
    }
}

// Outside the comparison grid above, the closed form's clamps take over: a camera hundreds of
// e-foldings under the fog floor must SATURATE to full fog (the true integral overflows float there),
// never produce a NaN or an out-of-range transmittance the composite would smear across the screen.
TEST( HeightFogClosedForm, ExtremeExponentsSaturateInsteadOfMisbehaving )
{
    HeightFogParams p  = PlainParams( 2.0f, 80.0f, 0.0f );
    p.MinTransmittance = 0.25f;

    const glm::vec3 positions[][2] = {
         { { 0.0f, -0.2f, 0.0f }, { 0.0f, -5.2f, 0.0f } }, // deep below the floor, looking down
         { { 0.0f, -0.2f, 0.0f }, { 5.0f, -0.2f, 0.0f } }, // deep below, horizontal
         { { 0.0f, 20.0f, 0.0f }, { 0.0f, 25.0f, 0.0f } }, // far above, looking up
    };

    for ( const auto& ray : positions )
    {
        const HeightFogResult fog = HeightFogEvaluate( p, ray[0], ray[1] );
        EXPECT_FALSE( std::isnan( fog.Transmittance ) );
        EXPECT_GE( fog.Transmittance, p.MinTransmittance );
        EXPECT_LE( fog.Transmittance, 1.0f );
        EXPECT_FALSE( std::isnan( fog.Inscattering.x ) );
    }

    // The two under-the-floor rays are optically bottomless: the floor is exactly what survives.
    EXPECT_FLOAT_EQ( HeightFogEvaluate( p, positions[0][0], positions[0][1] ).Transmittance, 0.25f );
    EXPECT_FLOAT_EQ( HeightFogEvaluate( p, positions[1][0], positions[1][1] ).Transmittance, 0.25f );
}

// The exact branch and its Taylor replacement meet at |a| = 0.01. A discontinuity there would be a
// visible ring in the fog wherever a ray's slope crosses the switch.
TEST( HeightFogClosedForm, TaylorBranchIsContinuousAtItsSwitchPoint )
{
    for ( const float sign : { 1.0f, -1.0f } )
    {
        const float below = HeightFogLineIntegral( 1.0f, 1.0f, sign * 0.009999f );
        const float above = HeightFogLineIntegral( 1.0f, 1.0f, sign * 0.010001f );
        EXPECT_NEAR( below, above, 1e-4f * std::abs( below ) ) << "sign " << sign;
    }
}

// ---------------------------------------------------------------------------------------------------
// Monotonicities — the properties that catch inversions no spot value would
// ---------------------------------------------------------------------------------------------------

TEST( HeightFogClosedForm, TransmittanceNeverIncreasesWithDistance )
{
    const HeightFogParams p = PlainParams( 2.0f, 20.0f, 0.0f );
    const glm::vec3       camera( 0.0f, 0.1f, 0.0f );

    // Including looking straight up, where the integral saturates: it may flatten, never turn back.
    for ( const glm::vec3& dir : { glm::normalize( glm::vec3( 1.0f, 0.2f, 0.0f ) ), glm::vec3( 0.0f, 1.0f, 0.0f ),
                                   glm::vec3( 0.0f, -1.0f, 0.0f ) } )
    {
        float previous = 1.0f;
        for ( float length = 0.05f; length <= 8.0f; length += 0.05f )
        {
            const HeightFogResult fog = HeightFogEvaluate( p, camera, camera + dir * length );
            EXPECT_LE( fog.Transmittance, previous + 1e-6f ) << "dir.y " << dir.y << " at " << length;
            previous = fog.Transmittance;
        }
    }
}

TEST( HeightFogClosedForm, AHigherCameraSeesLessFogUnderPositiveFalloff )
{
    const glm::vec3 dir( 1.0f, 0.0f, 0.0f );
    const float     lowOd  = ClosedOpticalDepth( 2.0f, 20.0f, 0.0f, glm::vec3( 0.0f, 0.0f, 0.0f ), dir, 2.0f );
    const float     highOd = ClosedOpticalDepth( 2.0f, 20.0f, 0.0f, glm::vec3( 0.0f, 1.0f, 0.0f ), dir, 2.0f );
    EXPECT_LT( highOd, lowOd );
    // And by exactly the collapse factor: the same horizontal ray a kilometre up is 2^-20 as dense.
    EXPECT_NEAR( highOd, lowOd * std::exp2( -20.0f ), 1e-4f * lowOd );
}

// ---------------------------------------------------------------------------------------------------
// StartDistance is an exclusion: transmittance multiplies across any split of the ray
// ---------------------------------------------------------------------------------------------------

TEST( HeightFogClosedForm, TransmittanceIsMultiplicativeAcrossAStartDistanceSplit )
{
    HeightFogParams p = PlainParams( 2.0f, 20.0f, 0.0f );
    p.Density1        = 0.8f;
    p.Falloff1        = 5.0f;
    p.Height1Km       = -0.05f;

    const glm::vec3 camera( 0.2f, 0.3f, -0.4f );
    const glm::vec3 dir   = glm::normalize( glm::vec3( 0.7f, -0.25f, 0.4f ) );
    const float     total = 2.0f;
    const float     split = 0.7f;

    const float wholeT = HeightFogEvaluate( p, camera, camera + dir * total ).Transmittance;
    const float nearT  = HeightFogEvaluate( p, camera, camera + dir * split ).Transmittance;

    HeightFogParams clipped = p;
    clipped.StartDistanceKm = split;
    const float farT        = HeightFogEvaluate( clipped, camera, camera + dir * total ).Transmittance;

    EXPECT_NEAR( wholeT, nearT * farT, 1e-4f );
}

TEST( HeightFogClosedForm, InsideTheStartDistanceThereIsNoFogAtAll )
{
    HeightFogParams p = PlainParams( 50.0f, 0.0f, 0.0f );
    p.StartDistanceKm = 1.0f;

    const glm::vec3       camera( 0.0f, 0.0f, 0.0f );
    const HeightFogResult fog = HeightFogEvaluate( p, camera, camera + glm::vec3( 0.9f, 0.0f, 0.0f ) );
    EXPECT_FLOAT_EQ( fog.Transmittance, 1.0f );
    EXPECT_EQ( fog.Inscattering, glm::vec3( 0.0f ) );
}

TEST( HeightFogClosedForm, CutoffRemovesTheFogEntirelyBeyondIt )
{
    HeightFogParams p  = PlainParams( 50.0f, 0.0f, 0.0f );
    p.CutoffDistanceKm = 2.0f;

    const glm::vec3 camera( 0.0f, 0.0f, 0.0f );
    const glm::vec3 dir( 1.0f, 0.0f, 0.0f );

    const HeightFogResult beyond = HeightFogEvaluate( p, camera, camera + dir * 2.5f );
    EXPECT_FLOAT_EQ( beyond.Transmittance, 1.0f );
    EXPECT_EQ( beyond.Inscattering, glm::vec3( 0.0f ) );

    const HeightFogResult inside = HeightFogEvaluate( p, camera, camera + dir * 1.5f );
    EXPECT_LT( inside.Transmittance, 0.5f );
}

// ---------------------------------------------------------------------------------------------------
// Max opacity, two layers, the directional lobe
// ---------------------------------------------------------------------------------------------------

TEST( HeightFogClosedForm, MaxOpacityFloorsTheTransmittanceAndScalesTheColour )
{
    HeightFogParams p  = PlainParams( 1e6f, 0.0f, 0.0f ); // an opaque wall of fog
    p.MinTransmittance = 0.35f;                           // FogMaxOpacity = 0.65

    const glm::vec3       camera( 0.0f, 0.0f, 0.0f );
    const HeightFogResult fog = HeightFogEvaluate( p, camera, camera + glm::vec3( 5.0f, 0.0f, 0.0f ) );

    EXPECT_FLOAT_EQ( fog.Transmittance, 0.35f );
    // The inscattering scales by the SAME clamped factor: the fog colour cannot exceed the opacity it
    // was allowed, however thick the medium.
    EXPECT_NEAR( fog.Inscattering.x, 0.65f, 1e-6f );
}

TEST( HeightFogClosedForm, TwoLayersSumTheirOpticalDepths )
{
    const glm::vec3 camera( 0.0f, 0.2f, 0.0f );
    const glm::vec3 dir   = glm::normalize( glm::vec3( 1.0f, -0.1f, 0.2f ) );
    const glm::vec3 world = camera + dir * 1.5f;

    HeightFogParams both = PlainParams( 2.0f, 20.0f, 0.0f );
    both.Density1        = 0.8f;
    both.Falloff1        = 5.0f;
    both.Height1Km       = 0.1f;

    const HeightFogParams only0 = PlainParams( 2.0f, 20.0f, 0.0f );
    HeightFogParams       only1 = PlainParams( 0.0f, 0.0f, 0.0f );
    only1.Density1              = 0.8f;
    only1.Falloff1              = 5.0f;
    only1.Height1Km             = 0.1f;

    const float odBoth = -std::log2( HeightFogEvaluate( both, camera, world ).Transmittance );
    const float od0    = -std::log2( HeightFogEvaluate( only0, camera, world ).Transmittance );
    const float od1    = -std::log2( HeightFogEvaluate( only1, camera, world ).Transmittance );

    EXPECT_NEAR( odBoth, od0 + od1, 1e-4f * odBoth );
}

TEST( HeightFogClosedForm, AnEmptySecondLayerChangesNothing )
{
    const glm::vec3       camera( 0.0f, 0.1f, 0.0f );
    const glm::vec3       world( 2.0f, 0.3f, 1.0f );
    const HeightFogParams single = PlainParams( 2.0f, 20.0f, 0.0f );

    HeightFogParams withEmpty = single;
    withEmpty.Falloff1        = 7.0f;  // a falloff and a height with
    withEmpty.Height1Km       = -3.0f; // zero density behind them
    const HeightFogResult a   = HeightFogEvaluate( single, camera, world );
    const HeightFogResult b   = HeightFogEvaluate( withEmpty, camera, world );

    EXPECT_FLOAT_EQ( a.Transmittance, b.Transmittance );
    EXPECT_EQ( a.Inscattering, b.Inscattering );
}

TEST( HeightFogClosedForm, DirectionalInscatteringBrightensTheSunSideOnly )
{
    HeightFogParams p   = PlainParams( 2.0f, 0.0f, 0.0f );
    p.DirectionalColour = glm::vec3( 1.0f, 0.5f, 0.2f );
    p.SunDirection      = glm::vec3( 1.0f, 0.0f, 0.0f );

    const glm::vec3 camera( 0.0f, 0.0f, 0.0f );
    const float     length = 2.0f;

    const HeightFogResult towardSun = HeightFogEvaluate( p, camera, camera + glm::vec3( length, 0.0f, 0.0f ) );
    const HeightFogResult awaySun   = HeightFogEvaluate( p, camera, camera - glm::vec3( length, 0.0f, 0.0f ) );

    EXPECT_GT( towardSun.Inscattering.x, awaySun.Inscattering.x );
    // Away from the sun the lobe is pow(0, e) = 0: exactly the base fog colour remains.
    EXPECT_NEAR( awaySun.Inscattering.x, 1.0f - awaySun.Transmittance, 1e-6f );
}

TEST( HeightFogClosedForm, DirectionalInscatteringWaitsForItsStartDistance )
{
    HeightFogParams p    = PlainParams( 2.0f, 0.0f, 0.0f );
    p.DirectionalColour  = glm::vec3( 1.0f, 1.0f, 1.0f );
    p.SunDirection       = glm::vec3( 1.0f, 0.0f, 0.0f );
    p.DirectionalStartKm = 1.0f;

    const glm::vec3 camera( 0.0f, 0.0f, 0.0f );

    // Short of the start distance the lobe is exactly absent... (`near`/`far` are Windows macros,
    // hence the names)
    const HeightFogResult nearFog = HeightFogEvaluate( p, camera, camera + glm::vec3( 0.8f, 0.0f, 0.0f ) );
    EXPECT_NEAR( nearFog.Inscattering.x, 1.0f - nearFog.Transmittance, 1e-6f );

    // ...and beyond it, present.
    const HeightFogResult farFog = HeightFogEvaluate( p, camera, camera + glm::vec3( 2.0f, 0.0f, 0.0f ) );
    EXPECT_GT( farFog.Inscattering.x, 1.0f - farFog.Transmittance + 1e-4f );
}

// ---------------------------------------------------------------------------------------------------
// The packer: component units in, kilometre payload out, each conversion exactly once
// ---------------------------------------------------------------------------------------------------

TEST( FogPayload, PackerConvertsUEUnitsAndWorldUnitsToKilometresOnce )
{
    Desert::ECS::ExponentialHeightFogData data;
    data.FogDensity                           = 0.02f;
    data.FogHeightFalloff                     = 0.2f;
    data.SecondFogDensity                     = 0.01f;
    data.SecondFogHeightFalloff               = 0.5f;
    data.SecondFogHeightOffset                = 50000.0f;  // 500 m, world units
    data.StartDistance                        = 200000.0f; // 2 km
    data.FogCutoffDistance                    = 900000.0f; // 9 km
    data.FogMaxOpacity                        = 0.8f;
    data.DirectionalInscatteringStartDistance = 10000.0f; // 100 m

    Desert::Graphic::AtmosphereEnv noAtmosphere; // Valid = false

    const auto p = Desert::Graphic::PackFogParams( data, noAtmosphere, /*fogHeightWorldY*/ 30000.0f );

    // UE authors density and falloff per 1000 cm; per kilometre that is x100. Distances are world
    // centimetres; per kilometre that is /100000.
    EXPECT_FLOAT_EQ( p.Layer0.x, 2.0f );
    EXPECT_FLOAT_EQ( p.Layer0.y, 20.0f );
    EXPECT_FLOAT_EQ( p.Layer0.z, 0.3f );
    EXPECT_FLOAT_EQ( p.Layer0.w, 2.0f );
    EXPECT_FLOAT_EQ( p.Layer1.x, 1.0f );
    EXPECT_FLOAT_EQ( p.Layer1.y, 50.0f );
    EXPECT_FLOAT_EQ( p.Layer1.z, 0.8f ); // the second layer's height is the first's plus its offset
    EXPECT_FLOAT_EQ( p.Layer1.w, 9.0f );
    EXPECT_FLOAT_EQ( p.Inscattering.w, 1.0f - 0.8f );
    EXPECT_FLOAT_EQ( p.Directional.w, 0.1f );

    // Without an atmosphere there is no sun to build the lobe around: the whole term is dropped, and
    // the fog colour is the authored one alone.
    EXPECT_EQ( glm::vec3( p.Directional ), glm::vec3( 0.0f ) );
    EXPECT_EQ( glm::vec3( p.Inscattering ), data.FogInscatteringLuminance );
}

TEST( FogPayload, PackerFoldsTheAtmosphereIntoSunLobeAndTheGradientsAmbient )
{
    Desert::ECS::ExponentialHeightFogData data;
    data.DirectionalInscatteringLuminance           = { 0.1f, 0.2f, 0.3f };
    data.SkyAtmosphereAmbientContributionColorScale = { 0.5f, 0.5f, 0.5f };

    // No DistantSkyLight handle: this is a SkyModel::ArtisticGradient frame, where the sky's ambient is
    // the dome the gradient itself defines.
    Desert::Graphic::AtmosphereEnv atmosphere;
    atmosphere.Valid          = true;
    atmosphere.SunDirection   = glm::normalize( glm::vec3( 0.3f, 0.8f, 0.5f ) );
    atmosphere.SunIrradiance  = { 10.0f, 9.0f, 8.0f };
    atmosphere.ZenithRadiance = { 0.2f, 0.4f, 0.8f };
    atmosphere.GroundRadiance = { 0.4f, 0.2f, 0.0f };

    const auto p = Desert::Graphic::PackFogParams( data, atmosphere, 0.0f );

    // The lobe: authored colour + the SKY's sun (AtmosphereEnv::SunIrradiance), which is the gradient's
    // half of the model-scoped choice — FogPayload.hpp says why the physical model takes the other.
    EXPECT_EQ( glm::vec3( p.Directional ), data.DirectionalInscatteringLuminance + atmosphere.SunIrradiance );
    EXPECT_EQ( glm::vec3( p.SunDirection ), atmosphere.SunDirection );

    // The ambient: the authored colour plus the scaled mean of dome and ground bounce, folded in on the
    // CPU because on this model the value exists there.
    const glm::vec3 expectedAmbient =
         glm::vec3( 0.5f ) * 0.5f * ( atmosphere.ZenithRadiance + atmosphere.GroundRadiance );
    EXPECT_EQ( glm::vec3( p.Inscattering ), data.FogInscatteringLuminance + expectedAmbient );

    // ... and the shader is told NOT to add a distant sky light on top of it. This gate is the whole
    // reason the two ambients cannot be counted twice.
    EXPECT_FLOAT_EQ( p.Ambient.w, 0.0f );
}

TEST( FogPayload, PhysicalAtmosphereMovesTheAmbientOntoTheDistantSkyLight )
{
    // The physical model's ambient is a GPU texel (AtmosphereEnv::DistantSkyLight), so the packer's job
    // changes: it must hand the shader the artist's SCALE and a gate, and it must NOT fold the
    // gradient's dome in as well — that would be two ambients on one fog.
    Desert::ECS::ExponentialHeightFogData data;
    data.SkyAtmosphereAmbientContributionColorScale = { 0.25f, 0.5f, 0.75f };

    // A non-null handle is the model switch; its target is never dereferenced on the CPU, which is why
    // a stand-in address is a legitimate test fixture here.
    // An empty-owner alias: non-null, owns nothing, so the stand-in address is never deleted.
    const int                                       standInToken = 0;
    const std::shared_ptr<Desert::Graphic::Image2D> distantSkyLight(
         std::shared_ptr<Desert::Graphic::Image2D>{}, std::bit_cast<Desert::Graphic::Image2D*>( &standInToken ) );

    Desert::Graphic::AtmosphereEnv atmosphere;
    atmosphere.Valid                  = true;
    atmosphere.SunDirection           = glm::normalize( glm::vec3( 0.0f, 1.0f, 0.0f ) );
    atmosphere.SunIrradiance          = { 10.0f, 9.0f, 8.0f };
    atmosphere.SunIlluminanceOnGround = { 0.8f, 0.7f, 0.6f };
    atmosphere.ZenithRadiance         = { 0.2f, 0.4f, 0.8f };
    atmosphere.GroundRadiance         = { 0.4f, 0.2f, 0.0f };
    atmosphere.DistantSkyLight        = distantSkyLight;

    const auto p = Desert::Graphic::PackFogParams( data, atmosphere, 0.0f );

    // The authored fog colour ALONE — the dome is not folded in.
    EXPECT_EQ( glm::vec3( p.Inscattering ), data.FogInscatteringLuminance );

    // The scale travels, and the gate says the texel is real.
    EXPECT_EQ( glm::vec3( p.Ambient ), data.SkyAtmosphereAmbientContributionColorScale );
    EXPECT_FLOAT_EQ( p.Ambient.w, 1.0f );
}

TEST( FogPayload, PhysicalAtmosphereLobeIsTheLightOnTheGroundNotTheSkysSun )
{
    // UE's AtmosphereLightIlluminanceOnGroundPostTransmittance, and the whole reason it is a separate
    // field: the sky's sun and the light's sun are authored on two different components and are not the
    // same number. The fixture uses the shipped magnitudes — a sky SunIntensity of 22 against a light
    // Intensity of 1 — because "more than an order of magnitude" is the property under test, not a
    // rounding difference.
    Desert::ECS::ExponentialHeightFogData data;
    data.DirectionalInscatteringLuminance = { 0.05f, 0.05f, 0.05f };

    // An empty-owner alias: non-null, owns nothing, so the stand-in address is never deleted.
    const int                                       standInToken = 0;
    const std::shared_ptr<Desert::Graphic::Image2D> distantSkyLight(
         std::shared_ptr<Desert::Graphic::Image2D>{}, std::bit_cast<Desert::Graphic::Image2D*>( &standInToken ) );

    Desert::Graphic::AtmosphereEnv physical;
    physical.Valid                  = true;
    physical.SunDirection           = glm::normalize( glm::vec3( 0.0f, 1.0f, 0.0f ) );
    physical.SunIrradiance          = { 22.0f, 21.1f, 19.4f }; // the SKY's sun: SunColor x SunIntensity
    physical.SunIlluminanceOnGround = { 0.84f, 0.80f, 0.73f }; // the LIGHT's, after transmittance
    physical.DistantSkyLight        = distantSkyLight;

    const auto lit = Desert::Graphic::PackFogParams( data, physical, 0.0f );
    EXPECT_EQ( glm::vec3( lit.Directional ),
               data.DirectionalInscatteringLuminance + physical.SunIlluminanceOnGround );

    // The same scene on the artistic gradient — no distant-sky-light handle — keeps the sky's sun.
    Desert::Graphic::AtmosphereEnv gradient = physical;
    gradient.DistantSkyLight                = nullptr;

    const auto unlit = Desert::Graphic::PackFogParams( data, gradient, 0.0f );
    EXPECT_EQ( glm::vec3( unlit.Directional ), data.DirectionalInscatteringLuminance + gradient.SunIrradiance );

    // THE RELATION, not the two values: switching the model must move the lobe by more than a factor of
    // ten, which is what makes the retune of every physical fog scene a requirement rather than taste.
    EXPECT_GT( unlit.Directional.x, 10.0f * lit.Directional.x );
}
