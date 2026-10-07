#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace Desert::Graphic::ShaderProtocols
{
    // std430 layout matching `struct SpotLight` in Mesh/LightSources.glslh (64 bytes). CosInner/CosOuter are the
    // COSINES of the inner/outer cone half-angles (precomputed on the CPU so the shader avoids acos).
    struct SpotLightPayload
    {
        glm::vec3 Color;     // 0
        float     Intensity; // 12
        glm::vec3 Position;  // 16
        float     Range;     // 28
        glm::vec3 Direction; // 32  (normalized, pointing OUT of the cone apex)
        float     CosInner;  // 44
        float     CosOuter;  // 48
        int32_t   Falloff;   // 52  (LightFalloff)
        float     _pad0 = 0.0f;
        float     _pad1 = 0.0f; // -> 64
    };

    struct SpotLight
    {
        // Block name of the storage buffer in the shader — keys the material property lookup.
        inline const static std::string Name = "SpotLightsUB";

        std::vector<SpotLightPayload> SpotLights;
    };

} // namespace Desert::Graphic::ShaderProtocols
