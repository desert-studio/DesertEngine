#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace Desert::Graphic::ShaderProtocols
{
    // std430 layout matching `struct PointLight` in Mesh/LightSources.glslh (64 bytes). vec3 has 16-byte
    // alignment in std430, so each vec3 is followed by a float with no extra padding; three trailing pad
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
        // UE ULightComponent::ContactShadowLength / bContactShadowLengthInWS: the screen-space ray toward the
        // light the deferred composite marches (Common/ContactShadow.glslh). A fraction of the screen height at
        // the receiver, or centimetres when ContactShadowInWorld; 0 = none (also when the light casts no shadow).
        float   ContactShadowLength  = 0.0f; // 44
        int32_t ContactShadowInWorld = 0;    // 48
        // UE ContactShadowCastingIntensity / ContactShadowNonCastingIntensity: the occlusion of a hit on a mesh
        // that casts contact shadows / on one that does not (the hit texel's NO_CONTACT_SHADOW bit).
        float ContactShadowCastingIntensity    = 1.0f; // 52
        float ContactShadowNonCastingIntensity = 0.0f; // 56
        float VolumetricScatteringIntensity    = 1.0f; // 60 -> 64
    };

    struct PointLight
    {
        // Block name of the (storage) buffer in the shader — keys the material property lookup.
        inline const static std::string Name = "PointLightsUB";

        std::vector<PointLightPayload> PointLights;
    };

} // namespace Desert::Graphic::ShaderProtocols
