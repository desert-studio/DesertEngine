#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace Desert::Graphic::ShaderProtocols
{
    // std430 layout matching `struct SpotLight` in Mesh/LightSources.glslh (96 bytes). CosInner/CosOuter are the
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
        // The light's first row in the view's local shadow table (LocalShadowViewPayload; a point light's six
        // faces are that row and the five after it), or kNoLocalShadowView. Written by
        // MeshRenderer::UpdateLocalShadows, never by the light system.
        int32_t   ShadowView = -1;   // 56
        // UE ULightComponent::ContactShadowLength / bContactShadowLengthInWS (PointLightPayload says how).
        float   ContactShadowLength  = 0.0f; // 60
        int32_t ContactShadowInWorld = 0;    // 64
        // UE ContactShadowCastingIntensity / ContactShadowNonCastingIntensity: the occlusion of a hit on a mesh
        // that casts contact shadows / on one that does not (the hit texel's NO_CONTACT_SHADOW bit).
        float ContactShadowCastingIntensity    = 1.0f; // 68
        float ContactShadowNonCastingIntensity = 0.0f; // 72
        // 1 when the light casts shadows in this view (its CastShadows, and the view draws local shadows): a
        // receiver with no ShadowView then is one the shadow maps do not cover (the view budget refused the
        // light), and the surface cache traces its shadow through the global distance field instead. Written by
        // MeshRenderer::UpdateLocalShadows with ShadowView.
        int32_t ShadowCaster = 0; // 76
        // UE SourceRadius (cm): the emitter sphere traced shadows aim at (MegaLights); 0 = a point.
        float SourceRadius = 0.0f; // 80
        // std430 rounds the array stride of a struct with a vec3 to 16 bytes: explicit, so sizeof matches.
        float Pad0 = 0.0f;
        float Pad1 = 0.0f;
        float Pad2 = 0.0f; // -> 96
    };
    static_assert( sizeof( SpotLightPayload ) == 96, "struct SpotLight (Mesh/LightSources.glslh) is 96 bytes" );

    struct SpotLight
    {
        // Block name of the storage buffer in the shader — keys the material property lookup.
        inline const static std::string Name = "SpotLightsUB";

        std::vector<SpotLightPayload> SpotLights;
    };

} // namespace Desert::Graphic::ShaderProtocols
