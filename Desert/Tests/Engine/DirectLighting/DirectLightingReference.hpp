#pragma once

// Compiles Editor/Resources/Shaders/Mesh/DirectLighting.glslh AS C++.
//
// Not a port and not a paraphrase — the same text, the same file, that StaticMeshPBR.shader,
// StaticMeshPBR_Instanced.shader, SkinnedMeshPBR.shader, Deferred/DeferredLighting.shader,
// ShadingModels/DefaultLit.shadingmodel compile as GLSL. A hand-written CPU copy could only
// ever prove that the copy agrees with itself, and this is precisely the quantity where that failed:
// there were FOUR copies of this BRDF in the shaders and the forward mesh shaders' copy had lost the
// /PI of the Lambert diffuse, so the sun was PI times too bright there — both against the deferred
// composite and against a point light standing beside it in the same shader.
//
// DirectLighting.glslh declares no uniform block and no sampler, so unlike AmbientIBLReference this
// needs no define to strip a sampling half: the whole file is algebra and the whole file is under test.
//
// The arrangement is the house one for a shader-maths reference (see AmbientIBLReference.hpp,
// SkyMediumReference.hpp):
//   * glm supplies vec2/vec3/vec4 and the maths built-ins with GLSL semantics;
//   * the include sits inside an ANONYMOUS namespace so each translation unit gets its own copy — GLSL
//     has no `inline`, so the shared text cannot carry one.

#include <glm/glm.hpp>

#include <cmath>
#include <Common/Core/GlslAsCpp.hpp>

namespace Desert::Tests::DirectLightingRef
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

        // GLSL PROMOTES A LITERAL TO THE ARGUMENT'S TYPE; C++ MAKES IT A DOUBLE. That one difference is
        // the whole reason these shims exist — `max(dot(N, L), 0.0)` is unambiguous in GLSL and deduces
        // float-against-double in C++, and glm's templates then have no candidate. Rather than edit the
        // shipped shader text to please a compiler that will never run it — which would make the header
        // something other than what the GPU compiles, defeating the point of the suite — the mixed-type
        // forms are supplied here, at the inner-most scope, and simply narrow first.
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
} // namespace Desert::Tests::DirectLightingRef
