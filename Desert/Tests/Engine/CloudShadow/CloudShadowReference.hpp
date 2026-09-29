#pragma once

// Compiles Engine/Content/Shaders/Common/CloudShadowMap.glslh AS C++ — both halves of it.
//
// Same arrangement, and for the same reason, as CloudGeometryReference.hpp: the text under test is the
// text the two cloud passes compile, not a CPU paraphrase of it. Here it buys something extra, because
// this header is where the ENCODE and the DECODE meet. The producer
// (Programs/Clouds/CloudShadowMap.shader) writes a triple and the consumer
// (Programs/Deferred/DeferredLighting.shader) reads it back at a depth the producer never knew about;
// they are two halves of one relation, and the only place that relation can be asserted is here, where
// both halves are ordinary functions.
//
// THE MEDIUM IS SYNTHETIC AND THAT IS THE POINT. The march's real density field is a noise volume and a
// profile table, neither of which has a closed form to check against. A slab of known extinction does:
// its Beer-Lambert transmittance at any depth is a number this test can compute exactly and compare the
// reconstruction with. Every claim the encoding makes — that it is correct BELOW the layer, INSIDE it and
// ABOVE its front — becomes an equality rather than a picture.
//
// Dialect shim, as in the other cloud references:
//   * glm supplies vec2/vec3 and the maths built-ins with GLSL semantics;
//   * the include sits inside an ANONYMOUS namespace so each translation unit gets its own copy — GLSL
//     has no `inline`, so the shared text cannot carry one;
//   * CLOUD_SHADOW_SAMPLE_EXTINCTION is defined BEFORE the include, exactly as the producing shader
//     defines it, which is what makes the march half of the header compile at all.

#include <glm/glm.hpp>

#include <cmath>
#include <Common/Core/GlslAsCpp.hpp>

namespace Desert::Tests::CloudShadowRef
{
    namespace
    {
        using vec2 = glm::vec2;
        using vec3 = glm::vec3;

        using glm::abs;
        using glm::clamp;
        using glm::dot;
        using glm::exp;
        using glm::floor;
        using glm::length;
        using glm::max;
        using glm::min;
        using glm::mix;
        using glm::sqrt;

        DESERT_GLSL_AS_CPP_BEGIN // see the header: GLSL has no `inline`, so these are statics
#include <Common/CloudGeometry.glslh>

             // The slab the tests drive: constant extinction between two radii, optionally modulated
             // horizontally so that the "heterogeneous" cases are not the homogeneous one in disguise. Written
             // as a mutable global because the callback macro's signature is a position and nothing else —
             // which is exactly the constraint the real shader works under, where the field lives in globals
             // for the same reason.
             struct SyntheticMedium
        {
            float BottomRadiusKm  = 0.0f;
            float TopRadiusKm     = 0.0f;
            float ExtinctionPerKm = 0.0f;
            // 0 = uniform. Above 0 the extinction is scaled by (1 + amount * sin(x / periodKm)), which
            // gives the ray a genuinely varying medium without giving up a closed-form integral: the
            // reference below integrates the SAME function numerically at a far finer step.
            float ModulationAmount   = 0.0f;
            float ModulationPeriodKm = 1.0f;
        };

        SyntheticMedium g_Medium;

        float CloudShadowTestExtinction( vec3 posKm )
        {
            const float radiusKm = length( posKm );
            if ( radiusKm < g_Medium.BottomRadiusKm || radiusKm > g_Medium.TopRadiusKm )
                return 0.0f;

            const float modulation =
                 1.0f + g_Medium.ModulationAmount * std::sin( posKm.x / g_Medium.ModulationPeriodKm );
            return g_Medium.ExtinctionPerKm * max( modulation, 0.0f );
        }

#define CLOUD_SHADOW_SAMPLE_EXTINCTION( p ) CloudShadowTestExtinction( p )

#include <Common/CloudShadowMap.glslh>
        DESERT_GLSL_AS_CPP_END

    } // namespace
} // namespace Desert::Tests::CloudShadowRef
