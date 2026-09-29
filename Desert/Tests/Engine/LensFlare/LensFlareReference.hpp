#pragma once

// Compiles Engine/Content/Shaders/Common/LensFlare.glslh AS C++.
//
// Not a port and not a paraphrase — the same text, the same file, that
// Programs/LensFlare/LensFlareFeatures.shader compiles as GLSL. A rendered frame says the flare LOOKS
// placed; only this says the ghosts walk the sun->centre axis in order, that the halo reads the ring it
// draws, and that the streak's taps overlap — across sun positions, counts and aspect ratios no single
// frame visits.
//
// The arrangement mirrors Tests/Engine/HeightFog/HeightFogReference.hpp exactly:
//   * glm supplies vec2/vec3 and the maths built-ins with GLSL semantics;
//   * the include sits inside an ANONYMOUS namespace so each translation unit gets its own copy — GLSL
//     has no `inline`, so the shared text cannot carry one.

#include <glm/glm.hpp>
#include <Common/Core/GlslAsCpp.hpp>

namespace Desert::Tests::LensFlareRef
{
    namespace
    {
        using vec2 = glm::vec2;
        using vec3 = glm::vec3;

        using glm::abs;
        using glm::clamp;
        using glm::length;
        using glm::max;
        using glm::min;
        using glm::mix;
        using glm::smoothstep;
        using glm::step;

        DESERT_GLSL_AS_CPP_BEGIN // see the header: GLSL has no `inline`, so these are statics
#include <Common/LensFlare.glslh>
             DESERT_GLSL_AS_CPP_END

    } // namespace
} // namespace Desert::Tests::LensFlareRef
