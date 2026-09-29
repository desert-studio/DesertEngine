#pragma once

// Compiles Engine/Content/Shaders/Mesh/AmbientIBL.glslh AS C++.
//
// Not a port and not a paraphrase — the same text, the same file, that StaticMeshPBR.shader,
// StaticMeshPBR_Instanced.shader, SkinnedMeshPBR.shader and Deferred/DeferredLighting.shader compile as
// GLSL. A hand-written CPU copy could only ever prove that the copy agrees with itself, and this is
// precisely the quantity where that failed once already: the deferred composite had its own ambient, it
// disagreed with the forward one by two orders of magnitude, and nothing said so for months.
//
// AMBIENT_IBL_NO_SAMPLERS takes the algebra half only — the sampling half names three samplerCube /
// sampler2D uniforms, which have no C++ meaning. What that leaves out of the test's reach is WHICH
// texel each shader fetches; what it puts in is every line that turns three fetched values into light.
//
// The arrangement is the house one for a shader-maths reference (see SkyMediumReference.hpp):
//   * glm supplies vec2/vec3/vec4 and the maths built-ins with GLSL semantics;
//   * the include sits inside an ANONYMOUS namespace so each translation unit gets its own copy — GLSL
//     has no `inline`, so the shared text cannot carry one.

#include <glm/glm.hpp>

#include <cmath>
#include <Common/Core/GlslAsCpp.hpp>

namespace Desert::Tests::AmbientIBLRef
{
    namespace
    {
        using vec2 = glm::vec2;
        using vec3 = glm::vec3;
        using vec4 = glm::vec4;

        using glm::clamp;
        using glm::max;
        using glm::min;
        using glm::mix;
        using glm::smoothstep;

        // GLSL PROMOTES A LITERAL TO THE ARGUMENT'S TYPE; C++ MAKES IT A DOUBLE. That one difference is
        // the whole reason these five lines exist. `max(minRadius, 0.05)` is unambiguous in GLSL and
        // deduces float-against-double in C++, and glm's templates then have no candidate. Rather than
        // edit the shipped shader text to please a compiler that will never run it — which would make
        // the header something other than what the GPU compiles, defeating the point of the suite —
        // the mixed-type forms are supplied here, at the inner-most scope, and simply narrow first.
        //
        // Nothing below changes a value: float arithmetic on inputs the GPU also holds as float.
        float pow( float base, float exponent )
        {
            return std::pow( base, exponent );
        }

        float max( float a, double b )
        {
            return glm::max( a, static_cast<float>( b ) );
        }

        DESERT_GLSL_AS_CPP_BEGIN // see the header: GLSL has no `inline`, so these are statics
             float
             clamp( float x, double lo, double hi )
        {
            return glm::clamp( x, static_cast<float>( lo ), static_cast<float>( hi ) );
        }

        float smoothstep( double edge0, double edge1, float x )
        {
            return glm::smoothstep( static_cast<float>( edge0 ), static_cast<float>( edge1 ), x );
        }

#define AMBIENT_IBL_NO_SAMPLERS
#include <Mesh/PBRFunctions.glslh>
#include <Mesh/AmbientIBL.glslh>
        DESERT_GLSL_AS_CPP_END
#undef AMBIENT_IBL_NO_SAMPLERS

    } // namespace
} // namespace Desert::Tests::AmbientIBLRef
