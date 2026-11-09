#pragma once

#include <Engine/Graphic/RDG/RDGShaderParameters.hpp>
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
// each vec3 is completed by a scalar, and the explicit pads round the struct to its 16-byte array stride (128
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
    /* The light's layer in the view's ray-traced local shadow masks (Shadows HardwareTraced), or -1: then */     \
    /* ShadowView is the light's shadow. Written by SceneRenderer::AssignLocalShadowMaskSlots. */                 \
    FIELD( int32_t, ShadowMaskSlot, shadowMaskSlot, { -1 } ) /* 68 */                                             \
    /* Under Shadows Stochastic, the source MegaLights' trace tests this light's samples with */                  \
    /* (Common::Scalability::StochasticVisibility: 0 ray query, 1 vsm+screen, 2 screen; the component's Shadow */ \
    /* Method clamped, Graphic::ResolveLightVisibility). Written by SceneRenderer::AssignLocalShadowMaskSlots. */ \
    FIELD( int32_t, ShadowVisibility, shadowVisibility, { 0 } ) /* 72 */                                          \
    /* UE ULightComponent::VolumetricScatteringIntensity: the light's share in the froxel fog's in-scattering */  \
    /* (VolumetricFogLightScattering); 1 = as bright as on surfaces, 0 = the fog does not see the light. */       \
    FIELD( float, VolumetricScatteringIntensity, volumetricScatteringIntensity, { 1.0f } ) /* 76 */               \
    /* THE LIGHT'S FRAME (unit, world): AxisX and AxisY, their cross the light's forward - a rect's emitting */   \
    /* face normal, a point light's IES forward (the entity's -Z, AxisX its X; IesProfile.glslh). */              \
    /* A RECT source (ECS::RectLightComponent) when RectHalfWidth > 0, else a point: the rect's half-extents */   \
    /* along AxisX / AxisY (cm), and its barn doors' free edge from the rect's border - RectBarnReach outward */  \
    /* in its plane, RectBarnDepth along its normal (cm). Intensity is then the emitted luminance. */             \
    FIELD( glm::vec3, AxisX, axisX, {} )                     /* 80 */                                             \
    FIELD( float, RectHalfWidth, rectHalfWidth, { 0.0f } )   /* 92 */                                             \
    FIELD( glm::vec3, AxisY, axisY, {} )                     /* 96 */                                             \
    FIELD( float, RectHalfHeight, rectHalfHeight, { 0.0f } ) /* 108 */                                            \
    FIELD( float, RectBarnReach, rectBarnReach, { 0.0f } )   /* 112 */                                            \
    FIELD( float, RectBarnDepth, rectBarnDepth, { 0.0f } )   /* 116 */                                            \
    /* UE ULocalLightComponent::IESTexture as Runtime::IesProfileService packs it: the atlas image's bindless */  \
    /* slot (stamped by SceneRenderer::AddPointLight) and the profile's first atlas row, or -1 for none. */       \
    FIELD( int32_t, IesAtlasTexture, iesAtlasTexture, { 0 } ) /* 120 */                                           \
    FIELD( int32_t, IesProfileRow, iesProfileRow, { -1 } )    /* 124 -> 128 */

namespace Desert::Graphic::ShaderProtocols
{
    /// PointLightPayload and kPointLightPayloadFields (GLSL `struct PointLight`), Std140Block.hpp.
    DESERT_SHADER_STRUCT( PointLightPayload, DESERT_POINT_LIGHT_FIELDS )
    static_assert(
         Std430StructLayoutMatches( kPointLightPayloadFields, sizeof( PointLightPayload ) ),
         "PointLightPayload is not laid out (or strided) as std430 places the generated struct PointLight: "
         "reorder or pad DESERT_POINT_LIGHT_FIELDS" );
    static_assert( sizeof( PointLightPayload ) == 128 );

    struct PointLight
    {
        // Block name of the (storage) buffer in the shader — keys the material property lookup.
        // The block as a binding parameter (RDGShaderParameters.hpp) - the one spelling; Name is its string form.
        static constexpr RDG::StorageBufferParameter Parameter{ "PointLightsUB" };
        inline const static std::string              Name = std::string( Parameter.Name );

        std::vector<PointLightPayload> PointLights;
    };

    /// The include path the shader includer answers with the generated storage buffer.
    inline constexpr std::string_view kPointLightsUBInclude = "Mesh/PointLightsUB.glslh";

    /// The buffer's set-0 slot (GeneratedProtocolIncludes.hpp holds the protocols' slots apart).
    inline constexpr uint32_t kPointLightsUBBinding = 6u;

    /// The scene's point lights: an unbounded storage buffer (no cap), count = LightsMetadata.PointLightCount.
    [[nodiscard]] inline const std::string& PointLightsUBGlsl()
    {
        static const std::string glsl =
             GenerateGuardedGlsl( "DESERT_POINT_LIGHTS_UB_GLSLH", "Engine/Graphic/ShaderProtocols/PointLight.hpp",
                                  GenerateGlslBlock( "ReadBuffer", kPointLightsUBBinding, PointLight::Name,
                                                     "    PointLight pointLights[];\n", "" ) );
        return glsl;
    }

} // namespace Desert::Graphic::ShaderProtocols
