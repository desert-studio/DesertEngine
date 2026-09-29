#pragma once

// Compiles Engine/Content/Shaders/Mesh/IndirectBounce.glslh AS C++.
//
// Not a port and not a paraphrase — the same text, the same file, that
// Programs/Deferred/DeferredLighting.shader compiles as GLSL for its screen-space GI gather. That
// matters more here than usual: the quantity under test is the one that spent its whole life as four
// inline lines inside a helper, where no assertion could reach it, while a suite one directory over
// proved that every shader FILE reaches the shared BRDF. It did. Its GI gather did not.
//
// Mesh/PBRFunctions.glslh and Mesh/DirectLighting.glslh come first because IndirectBounce.glslh says so
// at the top of itself and because DeferredLighting.shader includes them in that order too.
//
// The arrangement is the house one for a shader-maths reference (see DirectLightingReference.hpp,
// AmbientIBLReference.hpp, SkyMediumReference.hpp):
//   * glm supplies vec2/vec3/vec4 and the maths built-ins with GLSL semantics;
//   * the include sits inside an ANONYMOUS namespace so each translation unit gets its own copy — GLSL
//     has no `inline`, so the shared text cannot carry one.

#include <glm/glm.hpp>

#include <cmath>
#include <Common/Core/GlslAsCpp.hpp>

namespace Desert::Tests::IndirectBounceRef
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
        using glm::normalize;
        using glm::smoothstep;

        // GLSL PROMOTES A LITERAL TO THE ARGUMENT'S TYPE; C++ MAKES IT A DOUBLE. Same shims, same
        // reason, as DirectLightingReference.hpp — editing the shipped text to please a compiler that
        // will never run it would make this header something other than what the GPU compiles, which is
        // the whole point of the arrangement.
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

        vec3 operator-( double scalar, const vec3& v )
        {
            return vec3( static_cast<float>( scalar ) ) - v;
        }

        vec3 operator*( const vec3& v, double scalar )
        {
            return v * static_cast<float>( scalar );
        }

#include <Mesh/PBRFunctions.glslh>
#include <Mesh/DirectLighting.glslh>
#include <Mesh/IndirectBounce.glslh>
        DESERT_GLSL_AS_CPP_END

    } // namespace
} // namespace Desert::Tests::IndirectBounceRef
