#pragma once

// Compiles Editor/Resources/Shaders/Common/ViewRay.glslh AS C++.
//
// Not a port and not a paraphrase — the same text, the same file, that the Skybox and ProceduralSky
// vertex stages compile as GLSL. The property under test is one a frame can only show by accident: the
// background ray must be a function of where the camera LOOKS and never of where it STANDS. The HDR
// skybox shipped violating it, and every screenshot taken from a camera near the origin looked correct.
//
// It lives in DepthConvention because the reconstruction is an agreement with the projection factory —
// the clip z of 1.0 the shaders feed it is the near plane only under reversed-Z, and this suite is where
// that convention's agreements are written down.
//
// The arrangement is the house one for a shader-maths reference (see CloudNoiseReference.hpp):
//   * glm supplies vec3/vec4/mat3/mat4 and the built-ins with GLSL semantics;
//   * the include sits inside an ANONYMOUS namespace so each translation unit gets its own copy — GLSL
//     has no `inline`, so the shared text cannot carry one.

#include <glm/glm.hpp>

#include <Common/Core/GlslAsCpp.hpp>

namespace Desert::Tests::ViewRayRef
{
    namespace
    {
        using vec3 = glm::vec3;
        using vec4 = glm::vec4;
        using mat3 = glm::mat3;
        using mat4 = glm::mat4;

        using glm::inverse;

        DESERT_GLSL_AS_CPP_BEGIN // see the header: GLSL has no `inline`, so these are statics
#include <Common/ViewRay.glslh>
             DESERT_GLSL_AS_CPP_END

    } // namespace
} // namespace Desert::Tests::ViewRayRef
