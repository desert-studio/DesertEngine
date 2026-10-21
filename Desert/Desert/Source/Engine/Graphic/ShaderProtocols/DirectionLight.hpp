#pragma once

#include <Engine/Graphic/RDG/RDGShaderParameters.hpp>

#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace Desert::Graphic::ShaderProtocols
{
    // std140-friendly layout (two vec4s, 32 bytes) matching DirectionLightsUB in Lit.glsl.frag.
    struct DirectionLightPayload
    {
        glm::vec4 Direction;      // xyz = normalized direction
        glm::vec4 ColorIntensity; // rgb = color, a = intensity
    };

    struct DirectionLight
    {
        // The block as a binding parameter (RDGShaderParameters.hpp) - the one spelling; Name is its string form.
        static constexpr RDG::UniformBufferParameter Parameter{ "DirectionLightsUB" };
        inline const static std::string              Name = std::string( Parameter.Name );

        std::vector<DirectionLightPayload> DirectionLights;
    };

} // namespace Desert::Graphic::ShaderProtocols
