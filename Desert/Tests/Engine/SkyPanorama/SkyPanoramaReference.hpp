#pragma once

// Compiles Editor/Resources/Shaders/Common/SkyPanorama.glslh AS C++.
//
// Not a port and not a paraphrase — the same text, the same file, that PanoramaToCubemap and
// DiffuseIrradiance compile as GLSL. The arrangement is the house one for a shader-maths reference
// (see DepthConvention/ViewRayReference.hpp and CloudNoiseReference.hpp):
//   * glm supplies vec2/vec3 and the built-ins with GLSL semantics;
//   * the include sits inside an ANONYMOUS namespace so each translation unit gets its own copy — GLSL
//     has no `inline`, so the shared text cannot carry one.

#include <glm/glm.hpp>

#include <Common/Core/GlslAsCpp.hpp>

namespace Desert::Tests::SkyPanoramaRef
{
    namespace
    {
        using vec2 = glm::vec2;
        using vec3 = glm::vec3;

        using glm::acos;
        using glm::atan;
        using glm::clamp;

        DESERT_GLSL_AS_CPP_BEGIN // see the header: GLSL has no `inline`, so these are statics
#include <Common/SkyPanorama.glslh>
             DESERT_GLSL_AS_CPP_END

    } // namespace
} // namespace Desert::Tests::SkyPanoramaRef
