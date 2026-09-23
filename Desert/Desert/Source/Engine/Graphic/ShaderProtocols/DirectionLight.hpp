#pragma once

#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace Desert::Graphic::ShaderProtocols
{
    // std140-friendly layout (two vec4s, 32 bytes) matching DirectionLightsUB in PBR.glsl.frag.
    struct DirectionLightPayload
    {
        glm::vec4 Direction;      // xyz = normalized direction
        glm::vec4 ColorIntensity; // rgb = color, a = intensity
    };

    struct DirectionLight
    {
        inline const static std::string Name = "DirectionLightsUB";

        std::vector<DirectionLightPayload> DirectionLights;
    };

} // namespace Desert::Graphic::ShaderProtocols
