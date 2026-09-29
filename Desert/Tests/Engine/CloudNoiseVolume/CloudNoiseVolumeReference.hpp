#pragma once

// Compiles Engine/Content/Shaders/Common/CloudNoise.glslh AS C++, the same arrangement
// Desert/Tests/Engine/CloudNoise uses and for the same reason.
//
// WHY THIS SUITE NEEDS IT AT ALL, given the CloudNoise suite already drives these functions. Because the
// question here is a RELATION and not a function: does what the GENERATOR writes into the file equal what
// the SHADER reads out of the texture? Answering it needs both ends in one translation unit — the shared
// noise text on one side and Engine/Assets/CloudNoiseVolumeGenerator.cpp's output on the other — and a
// suite that had only one of them could only ever check that it agreed with itself.

#include <glm/glm.hpp>

#include <cstdint>
#include <Common/Core/GlslAsCpp.hpp>

namespace Desert::Tests::CloudNoiseVolumeRef
{
    namespace
    {
        using vec3 = glm::vec3;
        using vec4 = glm::vec4;

        using uint = std::uint32_t;

        using glm::clamp;
        using glm::floor;
        using glm::max;
        using glm::min;
        using glm::mix;
        using glm::mod;
        using glm::pow;

        DESERT_GLSL_AS_CPP_BEGIN // see the header: GLSL has no `inline`, so these are statics
#include <Common/CloudNoise.glslh>
             DESERT_GLSL_AS_CPP_END

    } // namespace
} // namespace Desert::Tests::CloudNoiseVolumeRef
