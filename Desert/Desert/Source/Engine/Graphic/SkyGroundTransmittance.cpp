#include "SkyGroundTransmittance.hpp"

#include <Engine/Graphic/SkyPayload.hpp>
#include <Common/Core/GlslAsCpp.hpp>

namespace Desert::Graphic
{
    namespace
    {
        // Engine/Content/Shaders/Common/SkyMedium.glslh, COMPILED AS C++ — the same text, the same
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
        using glm::clamp;
        using glm::cos;
        using glm::exp;
        using glm::max;
        using glm::min;
        using glm::sin;
        using glm::sqrt;

        DESERT_GLSL_AS_CPP_BEGIN // see the header: GLSL has no `inline`, so these are statics
#include <Common/SkyMedium.glslh>
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
} // namespace Desert::Graphic
