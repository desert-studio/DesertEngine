#pragma once

#include <Engine/Graphic/RDG/RDGShaderParameters.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <string>

namespace Desert::Graphic::ShaderProtocols
{
    struct LightsMetadata
    {
        // The block as a binding parameter (RDGShaderParameters.hpp) - the one spelling; Name is its string form.
        static constexpr RDG::UniformBufferParameter Parameter{ "LightsMetadata" };
        inline const static std::string              Name = std::string( Parameter.Name );

        // Field ORDER must match the LightsMetadata UBO in LightsMetadata.glslh (uploaded as raw bytes).
        uint32_t DirectionLightsCount = 0U;
        uint32_t PointLightsCount     = 0U;
        uint32_t SpotLightsCount      = 0U;
    };
    // `Name` is static, so the object is exactly the UBO's three counts — uploaded as one instance.
    static_assert( sizeof( LightsMetadata ) == 3 * sizeof( uint32_t ) );
} // namespace Desert::Graphic::ShaderProtocols
