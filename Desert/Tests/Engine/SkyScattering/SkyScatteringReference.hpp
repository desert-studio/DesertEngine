#pragma once

// Compiles Engine/Content/Shaders/Common/SkyScattering.glslh AS C++, on top of the
// Common/SkyMedium.glslh it builds on.
//
// Not a port and not a paraphrase — the same text, the same files, that SkyViewLut.shader,
// BakeProceduralSky.shader and ProceduralSky.shader compile as GLSL. Sharing the text means a passing
// test is a statement about the code the GPU runs (the SkyMediumReference.hpp arrangement, verbatim).
//
// The integrator's two LUT lookups are macros the includer must provide (a sampler cannot appear in
// text that compiles as C++). Here they expand to CALLBACK GLOBALS the tests swap per case: the
// default sun transmittance is SkyMedium's own march — an independent 40-step reference, not a texture
// — and the default multi-scatter term is zero, which is what lets a test isolate single scattering
// against a brute force and then pin the MS term's linearity separately.

#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <Common/Core/GlslAsCpp.hpp>

namespace Desert::Tests::SkyScatteringRef
{
    namespace
    {
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

             // The test-controllable LUT stand-ins. Assigned per test; the defaults make the integrator a
             // pure single-scattering march with an exact (marched) sun transmittance.
                  std::function<vec3( SkyAtmParams, float, float )>
                  g_SunTransmittance = []( SkyAtmParams p, float radiusKm, float sunZenithCos )
        { return SkyTransmittanceToTop( p, radiusKm, sunZenithCos, 40 ); };

        std::function<vec3( SkyAtmParams, float, float )> g_MultiScatter = []( SkyAtmParams, float, float )
        { return vec3( 0.0f, 0.0f, 0.0f ); };

#define SKY_SCATTERING_SUN_TRANSMITTANCE( atm, radiusKm, sunZenithCos )                                           \
    g_SunTransmittance( ( atm ), ( radiusKm ), ( sunZenithCos ) )
#define SKY_SCATTERING_MULTI_SCATTER( atm, radiusKm, sunZenithCos )                                               \
    g_MultiScatter( ( atm ), ( radiusKm ), ( sunZenithCos ) )

#include <Common/SkyScattering.glslh>
        DESERT_GLSL_AS_CPP_END

#undef SKY_SCATTERING_SUN_TRANSMITTANCE
#undef SKY_SCATTERING_MULTI_SCATTER

    } // namespace
} // namespace Desert::Tests::SkyScatteringRef
