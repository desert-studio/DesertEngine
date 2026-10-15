#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace Desert::Graphic::ShaderProtocols
{
    // std430 layout matching `struct PointLight` in Mesh/LightSources.glslh (48 bytes). vec3 has 16-byte
    // alignment in std430, so each vec3 is followed by a float with no extra padding; two trailing pad
    // floats round the struct up to a multiple of 16.
    struct PointLightPayload
    {
        glm::vec3 Color;     // 0
        float     Intensity; // 12
        glm::vec3 Position;  // 16
        float     Radius;    // 28
        float     MinRadius; // 32
        int32_t   Falloff;   // 36  (LightFalloff)
        // The light's first row in the view's local shadow table (LocalShadowViewPayload; a point light's six
        // faces are that row and the five after it), or kNoLocalShadowView. Written by
        // MeshRenderer::UpdateLocalShadows, never by the light system.
        int32_t   ShadowView = -1;   // 40
        // 1 when the light casts shadows in this view (its CastShadows, and the view draws local shadows): a
        // receiver with no ShadowView then is one the shadow maps do not cover (the view budget refused the
        // light), and the surface cache traces its shadow through the global distance field instead. Written by
        // MeshRenderer::UpdateLocalShadows with ShadowView.
        int32_t ShadowCaster = 0; // 44 -> 48
    };

    struct PointLight
    {
        // Block name of the (storage) buffer in the shader — keys the material property lookup.
        inline const static std::string Name = "PointLightsUB";

        std::vector<PointLightPayload> PointLights;
    };

} // namespace Desert::Graphic::ShaderProtocols
