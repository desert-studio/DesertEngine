#pragma once

// Compiles Engine/Content/Shaders/Common/SkyMedium.glslh AS C++.
//
// Not a port and not a paraphrase — the same text, the same file, that SkyTransmittanceLut.shader and
// SkyMultiScatterLut.shader compile as GLSL. Vulkan does not run in every development environment, so a
// LUT texel cannot always be looked at; the only thing a hand-written CPU copy could prove is that the
// copy agrees with itself, and the first tuning pass on either side would separate them silently.
// Sharing the text means a passing test is a statement about the code the GPU runs.
//
// The arrangement is the house one for a shader-maths reference:
//   * glm supplies vec2/vec3/vec4 and the maths built-ins with GLSL semantics;
//   * the include sits inside an ANONYMOUS namespace so each translation unit gets its own copy — GLSL
//     has no `inline`, so the shared text cannot carry one.

#include <glm/glm.hpp>

#include <cstdint>
#include <Common/Core/GlslAsCpp.hpp>

namespace Desert::Tests::SkyMediumRef
{
    namespace
    {
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
} // namespace Desert::Tests::SkyMediumRef
