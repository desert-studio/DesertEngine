#pragma once

#include <Engine/Graphic/RDG/RDGShaderParameters.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace Desert::Graphic::ShaderProtocols
{
    // std430 layout matching `struct PointLight` in Mesh/LightSources.glslh (80 bytes). vec3 has 16-byte
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
        // 1 when the light casts shadows in this view (its CastShadows, and the view draws local shadows): a
        // receiver with no ShadowView then is one the shadow maps do not cover (the view budget refused the
        // light), and the surface cache traces its shadow through the global distance field instead. Written by
        // MeshRenderer::UpdateLocalShadows with ShadowView.
        int32_t ShadowCaster = 0; // 60
        // UE SourceRadius (cm): the emitter sphere traced shadows aim at (MegaLights); 0 = a point.
        float SourceRadius = 0.0f; // 64
        // std430 rounds the array stride of a struct with a vec3 to 16 bytes: explicit, so sizeof matches.
        float Pad0 = 0.0f;
        float Pad1 = 0.0f;
        float Pad2 = 0.0f; // -> 80
    };
    static_assert( sizeof( PointLightPayload ) == 80, "struct PointLight (Mesh/LightSources.glslh) is 80 bytes" );

    struct PointLight
    {
        // Block name of the (storage) buffer in the shader — keys the material property lookup.
        // The block as a binding parameter (RDGShaderParameters.hpp) - the one spelling; Name is its string form.
        static constexpr RDG::StorageBufferParameter Parameter{ "PointLightsUB" };
        inline const static std::string              Name = std::string( Parameter.Name );

        std::vector<PointLightPayload> PointLights;
    };

} // namespace Desert::Graphic::ShaderProtocols
