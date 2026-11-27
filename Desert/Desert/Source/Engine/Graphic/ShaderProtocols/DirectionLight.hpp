#pragma once
// The scene directional light's per-frame upload. Its GPU layout (struct DirectionLight, the DirectionLightsUB
// block at binding 14) is declared once in LightLayout.hpp; Tools/ShaderProtocolGen generates
// Common/DirectionLightsUB.glslh from it.
#include <Engine/Graphic/RDG/RDGShaderParameters.hpp>
#include <Engine/Graphic/ShaderProtocols/LightLayout.hpp>

#include <string>
#include <vector>

namespace Desert::Graphic::ShaderProtocols
{
    struct DirectionLight
    {
        // The block as a binding parameter (RDGShaderParameters.hpp), named by its declaration; Name is its string
        // form and keys the material property lookup.
        static constexpr RDG::UniformBufferParameter Parameter{ kDirectionLightsUBBlock.BlockName };
        inline const static std::string              Name = std::string( Parameter.Name );

        std::vector<DirectionLightPayload> DirectionLights;
    };
} // namespace Desert::Graphic::ShaderProtocols
