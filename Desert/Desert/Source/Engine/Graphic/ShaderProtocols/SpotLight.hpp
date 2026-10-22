#pragma once

#include <Engine/Graphic/RDG/RDGShaderParameters.hpp>
#include <Engine/Graphic/ShaderProtocols/Std140Block.hpp>

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// ONE SPOT LIGHT AS THE SHADERS READ IT, listed ONCE: the members expand into SpotLightPayload and into
// kSpotLightPayloadFields, from which the includer GENERATES `struct SpotLight` (Mesh/LightSourceLayouts.glslh)
// and the SpotLightsUB storage buffer (Mesh/SpotLightsUB.glslh, binding 16, included by Mesh/Spotlight.glslh);
// neither is a file on disk. std430, 96 bytes. CosInner/CosOuter are the COSINES of the inner/outer cone
// half-angles (precomputed on the CPU so the shader avoids acos).
//
// FIELD( C++ type, member, GLSL name, initialiser )
#define DESERT_SPOT_LIGHT_FIELDS( FIELD )                                                                         \
    FIELD( glm::vec3, Color, color, {} )         /* 0 */                                                          \
    FIELD( float, Intensity, intensity, {} )     /* 12 */                                                         \
    FIELD( glm::vec3, Position, position, {} )   /* 16 */                                                         \
    FIELD( float, Range, range, {} )             /* 28 */                                                         \
    FIELD( glm::vec3, Direction, direction, {} ) /* 32, normalized, pointing OUT of the cone apex */              \
    FIELD( float, CosInner, cosInner, {} )       /* 44, full intensity inside this */                             \
    FIELD( float, CosOuter, cosOuter, {} )       /* 48, zero intensity outside this */                            \
    FIELD( int32_t, Falloff, falloff, {} )       /* 52, LightFalloff: 0=Linear, 1=Quadratic, 2=InverseSquare */   \
    /* The light's first row in the view's local shadow table (LocalShadowViewPayload), or kNoLocalShadowView. */ \
    /* Written by MeshRenderer::UpdateLocalShadows, never by the light system. */                                 \
    FIELD( int32_t, ShadowView, shadowView, { -1 } ) /* 56 */                                                     \
    /* UE ULightComponent::ContactShadowLength / bContactShadowLengthInWS (PointLight.hpp says how). */           \
    FIELD( float, ContactShadowLength, contactShadowLength, { 0.0f } )  /* 60 */                                  \
    FIELD( int32_t, ContactShadowInWorld, contactShadowInWorld, { 0 } ) /* 64 */                                  \
    /* UE ContactShadowCastingIntensity / ContactShadowNonCastingIntensity: the occlusion of a hit on a mesh that \
     */                                                                                                           \
    /* casts contact shadows / on one that does not (the hit texel's NO_CONTACT_SHADOW bit). */                   \
    FIELD( float, ContactShadowCastingIntensity, contactShadowCastingIntensity, { 1.0f } )       /* 68 */         \
    FIELD( float, ContactShadowNonCastingIntensity, contactShadowNonCastingIntensity, { 0.0f } ) /* 72 */         \
    /* 1 when the light casts shadows in this view (PointLight.hpp says what a -1 ShadowView then means). */      \
    FIELD( int32_t, ShadowCaster, shadowCaster, { 0 } ) /* 76 */                                                  \
    /* UE SourceRadius (cm): the emitter sphere traced shadows aim at (MegaLights); 0 = a point. */               \
    FIELD( float, SourceRadius, sourceRadius, { 0.0f } ) /* 80 */                                                 \
    /* Full intensity within this distance (cm), the falloff measured from it (BISTRO-LIGHTS). */                 \
    FIELD( float, MinRadius, minRadius, { 0.0f } ) /* 84 */                                                       \
    /* std430 rounds the array stride of a struct with a vec3 to 16 bytes: explicit, so sizeof matches. */        \
    /* The light's layer in the view's ray-traced local shadow masks, or -1 (PointLight.hpp says how). */          \
    FIELD( int32_t, ShadowMaskSlot, shadowMaskSlot, { -1 } ) /* 88 */                                              \
    FIELD( float, Pad2, pad2, { 0.0f } ) /* -> 96 */

namespace Desert::Graphic::ShaderProtocols
{
    /// SpotLightPayload and kSpotLightPayloadFields (GLSL `struct SpotLight`), Std140Block.hpp.
    DESERT_SHADER_STRUCT( SpotLightPayload, DESERT_SPOT_LIGHT_FIELDS )
    static_assert(
         Std430StructLayoutMatches( kSpotLightPayloadFields, sizeof( SpotLightPayload ) ),
         "SpotLightPayload is not laid out (or strided) as std430 places the generated struct SpotLight: "
         "reorder or pad DESERT_SPOT_LIGHT_FIELDS" );
    static_assert( sizeof( SpotLightPayload ) == 96 );

    struct SpotLight
    {
        // Block name of the storage buffer in the shader — keys the material property lookup.
        // The block as a binding parameter (RDGShaderParameters.hpp) - the one spelling; Name is its string form.
        static constexpr RDG::StorageBufferParameter Parameter{ "SpotLightsUB" };
        inline const static std::string              Name = std::string( Parameter.Name );

        std::vector<SpotLightPayload> SpotLights;
    };

    /// The include path the shader includer answers with the generated storage buffer.
    inline constexpr std::string_view kSpotLightsUBInclude = "Mesh/SpotLightsUB.glslh";

    /// The scene's spot lights: an unbounded storage buffer (no cap), count = LightsMetadata.SpotLightCount.
    [[nodiscard]] inline const std::string& SpotLightsUBGlsl()
    {
        static const std::string glsl = GenerateGuardedGlsl(
             "DESERT_SPOT_LIGHTS_UB_GLSLH", "Engine/Graphic/ShaderProtocols/SpotLight.hpp",
             GenerateGlslBlock( "ReadBuffer", 16, SpotLight::Name, "    SpotLight spotLights[];\n", "" ) );
        return glsl;
    }

} // namespace Desert::Graphic::ShaderProtocols
