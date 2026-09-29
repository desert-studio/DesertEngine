#pragma once

// Compiles Engine/Content/Shaders/Mesh/PBRFunctions.glslh and Mesh/DirectLighting.glslh AS C++.
//
// Not a port and not a paraphrase — the same two files the deferred composite and every forward mesh
// shader compile as GLSL. The suite beside this header asks whether two surfaces standing symmetrically
// about a point light receive the same light, and the only honest way to ask it is to put the question
// to THE shading text rather than to a CPU model of it that could be right while the shader is wrong.
//
// PBRFunctions.glslh is under test here as well as included for its helpers: `LightFalloffFactor` is
// the point light's whole distance model, and the symmetry claim ("equidistant, therefore equally lit")
// is a statement about that function as much as about the BRDF.
//
// The arrangement is the house one for a shader-maths reference (see DirectLightingReference.hpp,
// IndirectBounceReference.hpp, AmbientIBLReference.hpp):
//   * glm supplies vec2/vec3/vec4 and the maths built-ins with GLSL semantics;
//   * the include sits inside an ANONYMOUS namespace so each translation unit gets its own copy — GLSL
//     has no `inline`, so the shared text cannot carry one.

#include <glm/glm.hpp>

#include <cmath>
#include <Common/Core/GlslAsCpp.hpp>

namespace Desert::Tests::CornellSymmetryRef
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

        // GLSL PROMOTES A LITERAL TO THE ARGUMENT'S TYPE; C++ MAKES IT A DOUBLE. `max(dot(N, L), 0.0)`
        // is unambiguous in GLSL and deduces float-against-double in C++, where glm's templates then
        // have no candidate. The mixed-type forms are supplied here, at the inner-most scope, rather
        // than by editing shipped shader text to please a compiler that will never run it.
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

        // `1.0 - F` and `(1.0 - F) * (1.0 - metalness)` in GLSL: a scalar against a vec3, and a vec3
        // against a scalar. Same reason as above — the literal is a double here and glm cannot deduce
        // one T for both operands.
        vec3 operator-( double scalar, const vec3& v )
        {
            return vec3( static_cast<float>( scalar ) ) - v;
        }

        vec3 operator*( const vec3& v, double scalar )
        {
            return v * static_cast<float>( scalar );
        }

// PBRFunctions first: DirectLighting.glslh names PI, DistributionGGX, VisibilitySmith and
// fresnelSchlick, exactly as it says at the top of itself and exactly as every shader includes them.
#include <Mesh/PBRFunctions.glslh>
#include <Mesh/DirectLighting.glslh>
        DESERT_GLSL_AS_CPP_END

    } // namespace
} // namespace Desert::Tests::CornellSymmetryRef
