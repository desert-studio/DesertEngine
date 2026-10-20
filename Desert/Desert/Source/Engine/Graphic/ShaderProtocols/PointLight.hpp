#pragma once

#include <Engine/Graphic/ShaderProtocols/Std140Block.hpp>

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// ONE POINT LIGHT AS THE SHADERS READ IT, listed ONCE: the members expand into PointLightPayload and into
// kPointLightPayloadFields, from which the includer GENERATES `struct PointLight` (Mesh/LightSourceLayouts.glslh,
// included by the binding-free Mesh/LightSources.glslh) and the PointLightsUB storage buffer
// (Mesh/PointLightsUB.glslh, binding 6, included by Mesh/PointLight.glslh); neither is a file on disk. std430:
// each vec3 is completed by a scalar, and the explicit pads round the struct to its 16-byte array stride (80
// bytes); the static_asserts below make any other layout a build error.
//
// FIELD( C++ type, member, GLSL name, initialiser )
#define DESERT_POINT_LIGHT_FIELDS( FIELD )                                                                        \
    FIELD( glm::vec3, Color, color, {} )       /* 0 */                                                            \
    FIELD( float, Intensity, intensity, {} )   /* 12 */                                                           \
    FIELD( glm::vec3, Position, position, {} ) /* 16 */                                                           \
    FIELD( float, Radius, radius, {} )         /* 28 */                                                           \
    FIELD( float, MinRadius, minRadius, {} )   /* 32 */                                                           \
    FIELD( int32_t, Falloff, falloff, {} )     /* 36, LightFalloff: 0=Linear, 1=Quadratic, 2=InverseSquare */     \
    /* The light's first row in the view's local shadow table (LocalShadowViewPayload; a point light's six faces  \
     */                                                                                                           \
    /* are that row and the five after it: +X -X +Y -Y +Z -Z), or kNoLocalShadowView. Written by */               \
    /* MeshRenderer::UpdateLocalShadows, never by the light system. */                                            \
    FIELD( int32_t, ShadowView, shadowView, { -1 } ) /* 40 */                                                     \
    /* UE ULightComponent::ContactShadowLength / bContactShadowLengthInWS: the screen-space ray toward the light  \
     */                                                                                                           \
    /* the deferred composite marches (Common/ContactShadow.glslh). A fraction of the screen height at the */     \
    /* receiver, or centimetres when ContactShadowInWorld; 0 = none (also when the light casts no shadow). */     \
    FIELD( float, ContactShadowLength, contactShadowLength, { 0.0f } )  /* 44 */                                  \
    FIELD( int32_t, ContactShadowInWorld, contactShadowInWorld, { 0 } ) /* 48 */                                  \
    /* UE ContactShadowCastingIntensity / ContactShadowNonCastingIntensity: the occlusion of a hit on a mesh that \
     */                                                                                                           \
    /* casts contact shadows / on one that does not (the hit texel's NO_CONTACT_SHADOW bit). */                   \
    FIELD( float, ContactShadowCastingIntensity, contactShadowCastingIntensity, { 1.0f } )       /* 52 */         \
    FIELD( float, ContactShadowNonCastingIntensity, contactShadowNonCastingIntensity, { 0.0f } ) /* 56 */         \
    /* 1 when the light casts shadows in this view (its CastShadows, and the view draws local shadows): a */      \
    /* receiver with no ShadowView then is one the shadow maps do not cover (the view budget refused the light),  \
     */                                                                                                           \
    /* and the surface cache traces its shadow through the global distance field instead. Written by */           \
    /* MeshRenderer::UpdateLocalShadows with ShadowView. */                                                       \
    FIELD( int32_t, ShadowCaster, shadowCaster, { 0 } ) /* 60 */                                                  \
    /* UE SourceRadius (cm): the emitter sphere traced shadows aim at (MegaLights); 0 = a point. */               \
    FIELD( float, SourceRadius, sourceRadius, { 0.0f } ) /* 64 */                                                 \
    /* std430 rounds the array stride of a struct with a vec3 to 16 bytes: explicit, so sizeof matches. */        \
    FIELD( float, Pad0, pad0, { 0.0f } )                                                                          \
    FIELD( float, Pad1, pad1, { 0.0f } )                                                                          \
    FIELD( float, Pad2, pad2, { 0.0f } ) /* -> 80 */

namespace Desert::Graphic::ShaderProtocols
{
    /// PointLightPayload and kPointLightPayloadFields (GLSL `struct PointLight`), Std140Block.hpp.
    DESERT_SHADER_STRUCT( PointLightPayload, DESERT_POINT_LIGHT_FIELDS )
    static_assert(
         Std430StructLayoutMatches( kPointLightPayloadFields, sizeof( PointLightPayload ) ),
         "PointLightPayload is not laid out (or strided) as std430 places the generated struct PointLight: "
         "reorder or pad DESERT_POINT_LIGHT_FIELDS" );
    static_assert( sizeof( PointLightPayload ) == 80 );

    struct PointLight
    {
        // Block name of the (storage) buffer in the shader — keys the material property lookup.
        inline const static std::string Name = "PointLightsUB";

        std::vector<PointLightPayload> PointLights;
    };

    /// The include path the shader includer answers with the generated storage buffer.
    inline constexpr std::string_view kPointLightsUBInclude = "Mesh/PointLightsUB.glslh";

    /// The scene's point lights: an unbounded storage buffer (no cap), count = LightsMetadata.PointLightCount.
    [[nodiscard]] inline const std::string& PointLightsUBGlsl()
    {
        static const std::string glsl = GenerateGuardedGlsl(
             "DESERT_POINT_LIGHTS_UB_GLSLH", "Engine/Graphic/ShaderProtocols/PointLight.hpp",
             GenerateGlslBlock( "ReadBuffer", 6, PointLight::Name, "    PointLight pointLights[];\n", "" ) );
        return glsl;
    }

} // namespace Desert::Graphic::ShaderProtocols
