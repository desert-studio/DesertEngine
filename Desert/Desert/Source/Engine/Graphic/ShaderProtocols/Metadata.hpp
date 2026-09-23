#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <string>

namespace Desert::Graphic::ShaderProtocols
{
    struct LightsMetadata
    {
        inline const static std::string Name = "LightsMetadata";

        // Field ORDER must match the LightsMetadata UBO in LightsMetadata.glslh (uploaded as raw bytes).
        uint32_t DirectionLightsCount = 0U;
        uint32_t PointLightsCount     = 0U;
        uint32_t SpotLightsCount      = 0U;
    };
} // namespace Desert::Graphic::ShaderProtocols
