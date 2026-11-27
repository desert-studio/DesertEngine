#pragma once
// The scene's point lights as uploaded each frame. Their GPU layout (struct PointLight, the PointLightsUB storage
// buffer at binding 6, the ShadowFlags bits) is declared once in LightLayout.hpp; Tools/ShaderProtocolGen
// generates Mesh/LightSourceLayouts.glslh and Mesh/PointLightsUB.glslh from it.
#include <Engine/Graphic/RDG/RDGShaderParameters.hpp>
#include <Engine/Graphic/ShaderProtocols/LightLayout.hpp>

#include <string>
#include <vector>

namespace Desert::Graphic::ShaderProtocols
{
    struct PointLight
    {
        // The block as a binding parameter (RDGShaderParameters.hpp), named by its declaration; Name is its string
        // form and keys the material property lookup.
        static constexpr RDG::StorageBufferParameter Parameter{ kPointLightsUBBlock.BlockName };
        inline const static std::string              Name = std::string( Parameter.Name );

        std::vector<PointLightPayload> PointLights;
    };
} // namespace Desert::Graphic::ShaderProtocols
