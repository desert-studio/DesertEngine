#pragma once

// Compiles Engine/Content/Shaders/Common/CloudGeometry.glslh AS C++.
//
// Same arrangement, and for the same reason, as Desert/Tests/Engine/CloudNoise/CloudNoiseReference.hpp:
// the text under test is the text the cloud passes compile, not a CPU paraphrase of it. A paraphrase
// would prove only that the paraphrase agrees with itself, and the first time somebody retuned one side
// the two would part company in silence.
//
// What only this can say, and a frame cannot: that a ray pointing away from the shell yields NEGATIVE
// roots rather than a hit behind the camera, that the shell boundary is resolved to metres at a radius of
// 6360 km, and that the height fraction follows the curvature rather than the world Y. Every one of those
// is invisible in a still frame taken from the ground looking up, which is where they were last checked.
//
// Dialect shim, as in CloudNoiseReference.hpp:
//   * glm supplies vec2/vec3 and the maths built-ins with GLSL semantics;
//   * the include sits inside an ANONYMOUS namespace so each translation unit gets its own copy — GLSL
//     has no `inline`, so the shared text cannot carry one.

#include <glm/glm.hpp>
#include <Common/Core/GlslAsCpp.hpp>

namespace Desert::Tests::CloudGeometryRef
{
    namespace
    {
        using vec2 = glm::vec2;
        using vec3 = glm::vec3;

        using glm::clamp;
        using glm::dot;
        using glm::length;
        using glm::max;
        using glm::min;
        using glm::normalize;
        using glm::sqrt;

        DESERT_GLSL_AS_CPP_BEGIN // see the header: GLSL has no `inline`, so these are statics
#include <Common/CloudGeometry.glslh>
             DESERT_GLSL_AS_CPP_END

    } // namespace
} // namespace Desert::Tests::CloudGeometryRef
