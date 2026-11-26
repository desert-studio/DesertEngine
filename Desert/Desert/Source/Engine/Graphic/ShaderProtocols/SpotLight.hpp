#pragma once
// The scene's spot lights as uploaded each frame. Their GPU layout (struct SpotLight, the SpotLightsUB storage
// buffer at binding 16) is declared once in LightLayout.hpp; Tools/ShaderProtocolGen generates
// Mesh/LightSourceLayouts.glslh and Mesh/SpotLightsUB.glslh from it.
#include <Engine/Graphic/RDG/RDGShaderParameters.hpp>
#include <Engine/Graphic/ShaderProtocols/LightLayout.hpp>

#include <string>
#include <vector>

namespace Desert::Graphic::ShaderProtocols
{
    struct SpotLight
    {
        // The block as a binding parameter (RDGShaderParameters.hpp), named by its declaration; Name is its string
        // form and keys the material property lookup.
        static constexpr RDG::StorageBufferParameter Parameter{ kSpotLightsUBBlock.BlockName };
        inline const static std::string              Name = std::string( Parameter.Name );

        std::vector<SpotLightPayload> SpotLights;
    };
} // namespace Desert::Graphic::ShaderProtocols
