#pragma once

// Compiles Engine/Content/Shaders/Common/CloudLighting.glslh AS C++.
//
// Same arrangement as Desert/Tests/Engine/CloudNoise/CloudNoiseReference.hpp, and for the same reason:
// what is asserted below is asserted about the text the march compiles, not about a CPU restatement of
// it that would agree with itself no matter what either side did next.
//
// The properties here are the ones a frame cannot show. That a phase function integrates to one over the
// sphere is the difference between a cloud that is lit and a cloud that has been multiplied by a number;
// that one step of the scatter integral equals ten steps covering the same distance is what stops a
// quality tier from changing how bright the sky is. Both are exactly true or quietly wrong, and neither
// is visible in a screenshot.

#include <glm/glm.hpp>
#include <Common/Core/GlslAsCpp.hpp>

namespace Desert::Tests::CloudLightingRef
{
    namespace
    {
        using vec2 = glm::vec2;
        using vec3 = glm::vec3;

        using glm::clamp;
        using glm::exp;
        using glm::log;
        using glm::max;
        using glm::mix;
        using glm::pow;

        DESERT_GLSL_AS_CPP_BEGIN // see the header: GLSL has no `inline`, so these are statics
#include <Common/CloudLighting.glslh>
             DESERT_GLSL_AS_CPP_END

    } // namespace
} // namespace Desert::Tests::CloudLightingRef
