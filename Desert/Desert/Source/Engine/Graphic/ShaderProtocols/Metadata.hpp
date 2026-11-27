#pragma once
// The light counts' binding parameter. The block's layout (struct LightsMetadata, binding 4) is declared once in
// LightLayout.hpp; Tools/ShaderProtocolGen generates Mesh/LightsMetadata.glslh from it.
#include <Engine/Graphic/RDG/RDGShaderParameters.hpp>
#include <Engine/Graphic/ShaderProtocols/LightLayout.hpp>

namespace Desert::Graphic::ShaderProtocols
{
    /// The LightsMetadata block as a binding parameter (RDGShaderParameters.hpp), named by its declaration.
    inline constexpr RDG::UniformBufferParameter kLightsMetadataParameter{ kLightsMetadataBlock.BlockName };
} // namespace Desert::Graphic::ShaderProtocols
