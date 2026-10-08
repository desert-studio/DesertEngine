#include "SkyGroundTransmittance.hpp"

#include <Engine/Graphic/SkyPayload.hpp>
#include <Common/Core/GlslAsCpp.hpp>

#include <glm/gtc/constants.hpp>

namespace Desert::Graphic
{
    namespace
    {
        // Editor/Resources/Shaders/Common/SkyMedium.glslh, COMPILED AS C++ — the same text, the same
        // file, that SkyTransmittanceLut.shader compiles as GLSL. This is the arrangement the
        // SkyMedium test reference established, used here in the engine for the first
        // time and for the same reason: the sun light's colour and the transmittance LUT's texels are
        // one quantity, and a hand-written CPU copy of the march would agree with the GPU exactly until
        // the first tuning pass on either side.
        //
        // The dialect rules are the .glslh's own: glm supplies vec2/vec3/vec4 and the maths built-ins
        // with GLSL semantics, and the include sits in an ANONYMOUS namespace because GLSL has no
        // `inline` — this translation unit gets its own copy and exports none of it.
        //
        // The shader root is on this project's include path for exactly this include
        // (Desert/Desert/premake5.lua says so).
        using vec2 = glm::vec2;
        using vec3 = glm::vec3;
        using vec4 = glm::vec4;

        using glm::abs;
        using glm::acos;
        using glm::clamp;
        using glm::cos;
        using glm::exp;
        using glm::max;
        using glm::min;
        using glm::pow;
        using glm::sin;
        using glm::sqrt;

        DESERT_GLSL_AS_CPP_BEGIN // see the header: GLSL has no `inline`, so these are statics
#include <Common/SkyMedium.glslh>
// For SkyPlanetShadow — the sky's own terminator, reused for the sun light (SunLightFactorAtGround).
// No SKY_SCATTERING_* macros are defined, so the integrator block is compiled out, as for the screen pass.
#include <Common/SkyScattering.glslh>
             DESERT_GLSL_AS_CPP_END

    } // namespace

    glm::vec3 SunTransmittanceAtGround( const SkySettings& sky, const glm::vec3& towardSun )
    {
        // Through the REAL packing path, not a second unpacking of SkySettings: PackSky is what the GPU
        // reads, so a lane that moved would move for both sides at once. The sun direction it packs is
        // irrelevant here — only the medium block is used.
        const SkyGpuPayload payload = PackSky( towardSun, sky );

        const SkyAtmParams p =
             SkyMakeAtmParams( payload.MediumRayleigh, payload.MediumMie, payload.MediumMieAbsorption,
                               payload.MediumOzone, payload.MediumGround, payload.MediumTentPlanet );

        // The world origin lies ON the planet surface with +Y up (SkyScattering.glslh's convention), so
        // the ground's zenith is +Y and the sun's zenith cosine is the direction's y component.
        const float sunZenithCos = glm::clamp( towardSun.y, -1.0f, 1.0f );

        // Mathematically already in (0, 1] — the optical depth of a non-negative density can only be
        // non-negative — so the clamp is a domain guard on the authored medium, not a correction: it is
        // what keeps one hand-edited negative coefficient in a scene file from multiplying the sun light
        // by more than the sun.
        const vec3 t = SkyTransmittanceAtGroundToSun( p, sunZenithCos );
        return glm::clamp( t, glm::vec3( 0.0f ), glm::vec3( 1.0f ) );
    }

    namespace
    {
    float SunDiskFractionAboveHorizon( float sunElevationRad, float sunAngularRadiusRad )
    {
        // The planet's shadow cut through the solar DISK: the area fraction of a circle of radius r whose
        // centre sits h = elevation / r radii above a straight horizon (a circular segment). Exactly 0
        // once the disk's top limb has set, exactly 1 once its bottom limb has risen, 0.5 at the centre.
        // A zero-size disk is a point: a step at the horizon.
        if ( sunAngularRadiusRad <= 0.0f )
            return sunElevationRad > 0.0f ? 1.0f : 0.0f;
        const float h = glm::clamp( sunElevationRad / sunAngularRadiusRad, -1.0f, 1.0f );
        const float hiddenSegment = glm::acos( h ) - h * glm::sqrt( glm::max( 1.0f - h * h, 0.0f ) );
        return glm::clamp( 1.0f - hiddenSegment / glm::pi<float>(), 0.0f, 1.0f );
    }

    float RelativeAirMass( float sunElevationRad )
    {
        // Kasten & Young (1989), Applied Optics 28(22): air mass relative to the zenith column, finite
        // (~38) at the horizon. Elevation is clamped at the horizon: a partly set disk is lit through the
        // horizontal path, the part below is already taken by the disk shadow.
        const float elevationDeg = glm::degrees( glm::max( sunElevationRad, 0.0f ) );
        const float zenithDeg    = 90.0f - elevationDeg;
        return 1.0f / ( glm::sin( glm::radians( elevationDeg ) ) +
                        0.50572f * glm::pow( 96.07995f - zenithDeg, -1.6364f ) );
    }

    glm::vec3 SunTransmittanceByAirMass( const SkySettings& sky, const glm::vec3& towardSun )
    {
        // The zenith column of THE SAME medium (Rayleigh, Mie extinction, ozone — the component's
        // coefficients through the shared SkyMedium.glslh integral), tilted to the sun's elevation by the
        // analytic air mass: T = T_zenith ^ m. Per channel, so the light reddens by the same Rayleigh
        // spectrum that colours the physical sky. Above ~15 degrees m is within a few percent of 1/sin.
        const glm::vec3 zenith = SunTransmittanceAtGround( sky, glm::vec3( 0.0f, 1.0f, 0.0f ) );
        const float     elevation = glm::asin( glm::clamp( towardSun.y, -1.0f, 1.0f ) );
        const glm::vec3 opticalDepth = -glm::log( glm::max( zenith, glm::vec3( 1e-30f ) ) );
        return glm::clamp( glm::exp( -opticalDepth * RelativeAirMass( elevation ) ), glm::vec3( 0.0f ),
                           glm::vec3( 1.0f ) );
    }
    } // namespace

    glm::vec3 SunLightFactorAtGround( const SkySettings& sky, const glm::vec3& towardSun,
                                      bool affectedByAtmosphereTransmittance )
    {
        if ( sky.Model == ECS::SkyModel::PhysicalAtmosphere )
        {
            const SkyGpuPayload payload = PackSky( towardSun, sky );
            const SkyAtmParams  p =
                 SkyMakeAtmParams( payload.MediumRayleigh, payload.MediumMie, payload.MediumMieAbsorption,
                                   payload.MediumOzone, payload.MediumGround, payload.MediumTentPlanet );

            // The ground sample sits where the transmittance march starts, so the shadow's horizon and
            // the physical model's horizon are the same circle — the sky's own terminator band.
            const float sunZenithCos = glm::clamp( towardSun.y, -1.0f, 1.0f );
            const float planetShadow =
                 SkyPlanetShadow( p.BottomRadiusKm + SKY_PLANET_RADIUS_OFFSET_KM, sunZenithCos, p.BottomRadiusKm );
            const glm::vec3 atmosphere =
                 affectedByAtmosphereTransmittance ? SunTransmittanceAtGround( sky, towardSun ) : glm::vec3( 1.0f );
            return planetShadow * atmosphere;
        }

        // ArtisticGradient: no LUTs exist, so the same two terms analytically — the horizon cuts the
        // solar disk (a fade over the disk's angular diameter, not a switch), and the atmosphere dims and
        // reddens the light by air mass with the component's own coefficients.
        const float     elevation = glm::asin( glm::clamp( towardSun.y, -1.0f, 1.0f ) );
        const float     disk      = SunDiskFractionAboveHorizon( elevation, sky.SunAngularRadius );
        const glm::vec3 atmosphere =
             affectedByAtmosphereTransmittance ? SunTransmittanceByAirMass( sky, towardSun ) : glm::vec3( 1.0f );
        return disk * atmosphere;
    }
} // namespace Desert::Graphic
