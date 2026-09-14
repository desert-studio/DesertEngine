#pragma once

// Compiles Editor/Resources/Shaders/Common/SdfText.glslh AS C++ — the exact text both text shaders
// compile as GLSL (Programs/UI/UIText.shader and Programs/Text/TextSDF.shader).
//
// This is what makes the corner and minification claims testable at all. The interesting half of this
// change is not in the baker: it is the three lines the fragment shader runs, and a test that
// re-implemented them in C++ would prove only that two listings agree with each other. Here the median,
// the screen-space range and the coverage ramp under test are the ones on the GPU.
//
// The mutation that must redden this suite is "go back to sampling one channel": replace the median in
// SdfText.glslh with `msd.r` and MsdfCorner.MedianKeepsTheCornerThatOneChannelRounds fails, because a
// single channel's bilinear reconstruction cannot hold a corner.

#include <glm/glm.hpp>

#include <Common/Core/GlslAsCpp.hpp>

namespace Desert::Tests::SdfTextRef
{
    namespace
    {
        using vec2 = glm::vec2;
        using vec3 = glm::vec3;

        using glm::abs;
        using glm::clamp;
        using glm::max;
        using glm::min;

        DESERT_GLSL_AS_CPP_BEGIN // see the header: GLSL has no `inline`, so these are statics
#include <Common/SdfText.glslh>
             DESERT_GLSL_AS_CPP_END

    } // namespace
} // namespace Desert::Tests::SdfTextRef
