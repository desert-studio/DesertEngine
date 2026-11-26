#pragma once
// THE SCENE LIGHTS' GPU LAYOUTS, declared once: Tools/ShaderProtocolGen generates Mesh/LightSourceLayouts.glslh
// (struct PointLight, struct SpotLight, the ShadowFlags bits), Mesh/PointLightsUB.glslh, Mesh/SpotLightsUB.glslh,
// Common/DirectionLightsUB.glslh and Mesh/LightsMetadata.glslh from them at build time. Layout only: the RDG
// parameters and the per-frame light lists live in PointLight.hpp, SpotLight.hpp, DirectionLight.hpp and
// Metadata.hpp. Every block's binding and name is written here once, in its ShaderBlockDecl.
#include <Engine/Graphic/ShaderProtocols/ShaderLayout.hpp>
#include <Engine/Graphic/ShaderProtocols/ShaderProtocol.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <string_view>

// THE SCENE DIRECTIONAL LIGHT AS THE FORWARD PASSES READ IT: struct DirectionLight, std140, 32 bytes, held by the
// DirectionLightsUB block at binding 14. Engine-filled by reflection name (Graphic::SceneFrameBinding::ApplyTo ->
// SceneLightsBind).
//
// X( C++ type, member, "GLSL name", initialiser )
#define DESERT_DIRECTION_LIGHT_FIELDS( X )                                                                    \
    /* offset 0: xyz = normalized direction (from light); w = sin(SourceAngle / 2), the disc's radius */          \
    X( glm::vec4, Direction, "Direction", {} )                                                                  \
    X( glm::vec4, ColorIntensity, "ColorIntensity", {} ) /* offset 16: rgb = color, a = intensity */

// THE LIGHT COUNTS: the LightsMetadata uniform block at binding 4. The explicit tail pad makes the C++ object the
// std140 block's full 16 bytes, uploaded as one instance.
#define DESERT_LIGHTS_METADATA_FIELDS( X )                                                                    \
    X( uint32_t, DirectionLightsCount, "DirectionLightCount", { 0U } ) /* offset 0 */                           \
    X( uint32_t, PointLightsCount, "PointLightCount", { 0U } )         /* offset 4 */                           \
    X( uint32_t, SpotLightsCount, "SpotLightCount", { 0U } )           /* offset 8 */                           \
    X( uint32_t, LightsMetadataPad, "LightsMetadataPad", { 0U } )      /* offset 12; block size 16 */

// ONE POINT LIGHT AS THE SHADERS READ IT: `struct PointLight` (Mesh/LightSourceLayouts.glslh, included by the
// binding-free Mesh/LightSources.glslh) and the PointLightsUB storage buffer (Mesh/PointLightsUB.glslh, binding 6,
// included by Mesh/PointLight.glslh). std430: each vec3 is completed by a scalar, and the explicit pads round the
// struct to its 16-byte array stride (144 bytes); DESERT_SHADER_STRUCT's static_assert makes any other layout a
// build error.
#define DESERT_POINT_LIGHT_FIELDS( X )                                                                        \
    X( glm::vec3, Color, "color", {} )       /* 0 */                                                            \
    X( float, Intensity, "intensity", {} )   /* 12 */                                                           \
    X( glm::vec3, Position, "position", {} ) /* 16 */                                                           \
    X( float, Radius, "radius", {} )         /* 28 */                                                           \
    X( float, MinRadius, "minRadius", {} )   /* 32 */                                                           \
    X( int32_t, Falloff, "falloff", {} )     /* 36, LightFalloff: 0=Linear, 1=Quadratic, 2=InverseSquare */     \
    /* The light's first row in the view's local shadow table (LocalShadowViewPayload; a point light's six faces  \
     */                                                                                                           \
    /* are that row and the five after it: +X -X +Y -Y +Z -Z), or kNoLocalShadowView. Written by */               \
    /* MeshRenderer::UpdateLocalShadows, never by the light system. */                                            \
    X( int32_t, ShadowView, "shadowView", { -1 } ) /* 40 */                                                     \
    /* UE ULightComponent::ContactShadowLength / bContactShadowLengthInWS: the screen-space ray toward the light  \
     */                                                                                                           \
    /* the deferred composite marches (Common/ContactShadow.glslh). A fraction of the screen height at the */     \
    /* receiver, or centimetres when ContactShadowInWorld; 0 = none (also when the light casts no shadow). */     \
    X( float, ContactShadowLength, "contactShadowLength", { 0.0f } )  /* 44 */                                  \
    X( int32_t, ContactShadowInWorld, "contactShadowInWorld", { 0 } ) /* 48 */                                  \
    /* UE ContactShadowCastingIntensity / ContactShadowNonCastingIntensity: the occlusion of a hit on a mesh that \
     */                                                                                                           \
    /* casts contact shadows / on one that does not (the hit texel's NO_CONTACT_SHADOW bit). */                   \
    X( float, ContactShadowCastingIntensity, "contactShadowCastingIntensity", { 1.0f } )       /* 52 */         \
    X( float, ContactShadowNonCastingIntensity, "contactShadowNonCastingIntensity", { 0.0f } ) /* 56 */         \
    /* THE LIGHT'S SHADOW BITS (kLightShadow*). kLightShadowInView: the light casts in this view (its CastShadows, */ \
    /* and the view draws local shadows) - a receiver with no ShadowView is then one the maps do not cover, and the */ \
    /* surface cache traces it instead. kLightShadowCasts: the light's own CastShadows, whatever the view draws - */  \
    /* the path tracer and the GI hits trace its visibility only when it is set (UE PATHTRACER_FLAG_CAST_SHADOW_MASK). */ \
    /* Written by MeshRenderer::UpdateLocalShadows with ShadowView. */                                              \
    X( int32_t, ShadowFlags, "shadowFlags", { 0 } ) /* 60 */                                                      \
    /* UE SourceRadius (cm): the emitter sphere traced shadows aim at (MegaLights); 0 = a point. */               \
    X( float, SourceRadius, "sourceRadius", { 0.0f } ) /* 64 */                                                 \
    /* std430 rounds the array stride of a struct with a vec3 to 16 bytes: explicit, so sizeof matches. */        \
    /* The light's layer in the view's ray-traced local shadow masks (Shadows HardwareTraced), or -1: then */     \
    /* ShadowView is the light's shadow. Written by SceneRenderer::AssignLocalShadowMaskSlots. */                 \
    X( int32_t, ShadowMaskSlot, "shadowMaskSlot", { -1 } ) /* 68 */                                             \
    /* Under Shadows Stochastic, the source MegaLights' trace tests this light's samples with */                  \
    /* (Common::Scalability::StochasticVisibility: 0 ray query, 1 vsm+screen, 2 screen; the component's Shadow */ \
    /* Method clamped, Graphic::ResolveLightVisibility). Written by SceneRenderer::AssignLocalShadowMaskSlots. */ \
    X( int32_t, ShadowVisibility, "shadowVisibility", { 0 } ) /* 72 */                                          \
    /* UE ULightComponent::VolumetricScatteringIntensity: the light's share in the froxel fog's in-scattering */  \
    /* (VolumetricFogLightScattering); 1 = as bright as on surfaces, 0 = the fog does not see the light. */       \
    X( float, VolumetricScatteringIntensity, "volumetricScatteringIntensity", { 1.0f } ) /* 76 */               \
    /* THE LIGHT'S FRAME (unit, world): AxisX and AxisY, their cross the light's forward - a rect's emitting */   \
    /* face normal, a point light's IES forward (the entity's -Z, AxisX its X; IesProfile.glslh). */              \
    /* A RECT source (ECS::RectLightComponent) when RectHalfWidth > 0, else a point: the rect's half-extents */   \
    /* along AxisX / AxisY (cm), and its barn doors' free edge from the rect's border - RectBarnReach outward */  \
    /* in its plane, RectBarnDepth along its normal (cm). Intensity is then the emitted luminance. */             \
    X( glm::vec3, AxisX, "axisX", {} )                     /* 80 */                                             \
    X( float, RectHalfWidth, "rectHalfWidth", { 0.0f } )   /* 92 */                                             \
    X( glm::vec3, AxisY, "axisY", {} )                     /* 96 */                                             \
    X( float, RectHalfHeight, "rectHalfHeight", { 0.0f } ) /* 108 */                                            \
    X( float, RectBarnReach, "rectBarnReach", { 0.0f } )   /* 112 */                                            \
    X( float, RectBarnDepth, "rectBarnDepth", { 0.0f } )   /* 116 */                                            \
    /* UE ULocalLightComponent::IESTexture as Runtime::IesProfileService packs it: the atlas image's bindless */  \
    /* slot (stamped by SceneRenderer::AddPointLight) and the profile's first atlas row, or -1 for none. */       \
    X( int32_t, IesAtlasTexture, "iesAtlasTexture", { 0 } ) /* 120 */                                           \
    X( int32_t, IesProfileRow, "iesProfileRow", { -1 } )    /* 124 */                                           \
    /* A rect source's GGX LTC tables (UE LTCMat / LTCAmp, Runtime::RectLightLtcService): their bindless   */ \
    /* slot, stamped by SceneRenderer::AddPointLight for a rect; 0 for a point (Mesh/RectLight.glslh).      */ \
    X( int32_t, RectLtcTexture, "rectLtcTexture", { 0 } )   /* 128 */                                           \
    /* UE light function (LIGHT-FUNCTIONS): the view's light function atlas (System::LightFunctionAtlas) as its */ \
    /* bindless slot and this light's tile in it, or -1 for none; stamped by SceneRenderer::AddPointLight. The */    \
    /* tile holds the function over the octahedral map of the direction in the light's frame */                     \
    /* (Mesh/LightFunctionAtlas.glslh DesertOmniLightFunction). */                                                   \
    X( int32_t, LightFunctionAtlasTexture, "lightFunctionAtlasTexture", { 0 } ) /* 132 */                       \
    X( int32_t, LightFunctionTile, "lightFunctionTile", { -1 } )                /* 136 */                       \
    /* UE ULocalLightComponent::SourceLength (cm): a capsule ("tube") emitter of SourceRadius around a */       \
    /* segment this long along the light's forward, cross( AxisX, AxisY ), centred on Position; 0 = a sphere */ \
    /* (Mesh/LightSources.glslh DesertPointLightAt). A rect states 0. */                                        \
    X( float, SourceLength, "sourceLength", { 0.0f } )      /* 140 -> 144 */

// ONE SPOT LIGHT AS THE SHADERS READ IT: `struct SpotLight` (Mesh/LightSourceLayouts.glslh) and the SpotLightsUB
// storage buffer (Mesh/SpotLightsUB.glslh, binding 16, included by Mesh/Spotlight.glslh). std430, 144 bytes.
// CosInner/CosOuter are the COSINES of the inner/outer cone half-angles (precomputed on the CPU so the shader
// avoids acos).
#define DESERT_SPOT_LIGHT_FIELDS( X )                                                                         \
    X( glm::vec3, Color, "color", {} )         /* 0 */                                                          \
    X( float, Intensity, "intensity", {} )     /* 12 */                                                         \
    X( glm::vec3, Position, "position", {} )   /* 16 */                                                         \
    X( float, Range, "range", {} )             /* 28 */                                                         \
    X( glm::vec3, Direction, "direction", {} ) /* 32, normalized, pointing OUT of the cone apex */              \
    X( float, CosInner, "cosInner", {} )       /* 44, full intensity inside this */                             \
    X( float, CosOuter, "cosOuter", {} )       /* 48, zero intensity outside this */                            \
    X( int32_t, Falloff, "falloff", {} )       /* 52, LightFalloff: 0=Linear, 1=Quadratic, 2=InverseSquare */   \
    /* The light's first row in the view's local shadow table (LocalShadowViewPayload), or kNoLocalShadowView. */ \
    /* Written by MeshRenderer::UpdateLocalShadows, never by the light system. */                                 \
    X( int32_t, ShadowView, "shadowView", { -1 } ) /* 56 */                                                     \
    /* UE ULightComponent::ContactShadowLength / bContactShadowLengthInWS (PointLight.hpp says how). */           \
    X( float, ContactShadowLength, "contactShadowLength", { 0.0f } )  /* 60 */                                  \
    X( int32_t, ContactShadowInWorld, "contactShadowInWorld", { 0 } ) /* 64 */                                  \
    /* UE ContactShadowCastingIntensity / ContactShadowNonCastingIntensity: the occlusion of a hit on a mesh that \
     */                                                                                                           \
    /* casts contact shadows / on one that does not (the hit texel's NO_CONTACT_SHADOW bit). */                   \
    X( float, ContactShadowCastingIntensity, "contactShadowCastingIntensity", { 1.0f } )       /* 68 */         \
    X( float, ContactShadowNonCastingIntensity, "contactShadowNonCastingIntensity", { 0.0f } ) /* 72 */         \
    /* The light's shadow bits kLightShadowInView / kLightShadowCasts (declared below).                */       \
    X( int32_t, ShadowFlags, "shadowFlags", { 0 } ) /* 76 */                                                    \
    /* UE SourceRadius (cm): the emitter sphere traced shadows aim at (MegaLights); 0 = a point. */               \
    X( float, SourceRadius, "sourceRadius", { 0.0f } ) /* 80 */                                                 \
    /* Full intensity within this distance (cm), the falloff measured from it (BISTRO-LIGHTS). */                 \
    X( float, MinRadius, "minRadius", { 0.0f } ) /* 84 */                                                       \
    /* std430 rounds the array stride of a struct with a vec3 to 16 bytes: explicit, so sizeof matches. */        \
    /* The light's layer in the view's ray-traced local shadow masks, or -1 (PointLight.hpp says how). */         \
    X( int32_t, ShadowMaskSlot, "shadowMaskSlot", { -1 } ) /* 88 */                                             \
    /* Under Shadows Stochastic, MegaLights' visibility source for the light (PointLight.hpp says how). */        \
    X( int32_t, ShadowVisibility, "shadowVisibility", { 0 } ) /* 92 */                                          \
    /* UE ULightComponent::VolumetricScatteringIntensity (PointLight.hpp says how). */                            \
    X( float, VolumetricScatteringIntensity, "volumetricScatteringIntensity", { 1.0f } ) /* 96 */               \
    /* The IES profile (PointLight.hpp says how): atlas slot, stamped by SceneRenderer::AddSpotLight, and row. */ \
    X( int32_t, IesAtlasTexture, "iesAtlasTexture", { 0 } ) /* 100 */                                           \
    X( int32_t, IesProfileRow, "iesProfileRow", { -1 } )    /* 104 */                                           \
    /* UE light function (LIGHT-FUNCTIONS): the view's atlas slot, stamped by SceneRenderer::AddSpotLight; the */ \
    /* tile (LightFunctionTile below) holds the function over the cone's perspective footprint. */                 \
    X( int32_t, LightFunctionAtlasTexture, "lightFunctionAtlasTexture", { 0 } ) /* 108 */                                           \
    /* The light's tangent (unit, world; the entity's X): with Direction, the frame IES horizontal angles */      \
    /* are measured in (IesProfile.glslh). */                                                                     \
    X( glm::vec3, Tangent, "tangent", {} ) /* 112 */                                                            \
    X( int32_t, LightFunctionTile, "lightFunctionTile", { -1 } ) /* 124, -1 = no light function */          \
    /* UE SourceLength (cm): a capsule ("tube") emitter along Direction, centred on Position (PointLight.hpp */ \
    /* says how); 0 = a sphere. The pads round the struct to its 16-byte array stride. */                       \
    X( float, SourceLength, "sourceLength", { 0.0f } ) /* 128 */                                              \
    X( float, Pad0, "pad0", { 0.0f } )                 /* 132 */                                              \
    X( float, Pad1, "pad1", { 0.0f } )                 /* 136 */                                              \
    X( float, Pad2, "pad2", { 0.0f } )                 /* 140 -> 144 */

namespace Desert::Graphic::ShaderProtocols
{
    /// PointLightPayload::ShadowFlags / SpotLightPayload::ShadowFlags bits; the GLSL constants of the same names
    /// are generated into Mesh/LightSourceLayouts.glslh from kLightShadowFlagConstants below.
    inline constexpr int32_t kLightShadowInView = 1;
    inline constexpr int32_t kLightShadowCasts  = 2;

    DESERT_SHADER_STRUCT( DirectionLightPayload, "DirectionLight", Std140, DESERT_DIRECTION_LIGHT_FIELDS );
    static_assert( sizeof( DirectionLightPayload ) == 32 );

    // The DirectionLightsUB block's members: ONE light (the scene's main one), as `DirectionLight directionLights`.
    // A block layout only, never a C++ object of its own: DirectionLight.hpp uploads the payload itself.
#define DESERT_DIRECTION_LIGHTS_UB_FIELDS( X ) X( DirectionLightPayload, Light, "directionLights", {} )
    DESERT_SHADER_STRUCT( DirectionLightsUBLayout, "DirectionLightsUB", Std140, DESERT_DIRECTION_LIGHTS_UB_FIELDS );

    DESERT_SHADER_STRUCT( LightsMetadata, "LightsMetadata", Std140, DESERT_LIGHTS_METADATA_FIELDS );
    static_assert( sizeof( LightsMetadata ) == 16 );

    DESERT_SHADER_STRUCT( PointLightPayload, "PointLight", Std430, DESERT_POINT_LIGHT_FIELDS );
    static_assert( sizeof( PointLightPayload ) == 144 );

    DESERT_SHADER_STRUCT( SpotLightPayload, "SpotLight", Std430, DESERT_SPOT_LIGHT_FIELDS );
    static_assert( sizeof( SpotLightPayload ) == 144 );

    // Mesh/LightSourceLayouts.glslh: the two analytic source structs with no binding, so the binding-free
    // Mesh/LightSources.glslh includes them and the storage buffers below hold arrays of them.
    inline constexpr const ShaderStructDecl* kLightSourceStructs[] = { &kPointLightPayloadStruct,
                                                                       &kSpotLightPayloadStruct };
    inline constexpr ShaderConstantDecl kLightShadowFlagConstants[] = {
         { .Kind = ShaderConstantDecl::Type::Int, .Name = "kLightShadowInView", .Value = kLightShadowInView },
         { .Kind = ShaderConstantDecl::Type::Int, .Name = "kLightShadowCasts", .Value = kLightShadowCasts } };
    inline constexpr ShaderIncludeDecl kLightSourceLayoutsShaderInclude{ .Path         = "Mesh/LightSourceLayouts.glslh",
                                                                          .SourceHeader = "Desert/Desert/Source/Engine/Graphic/ShaderProtocols/LightLayout.hpp",
                                                                          .Structs      = kLightSourceStructs,
                                                                          .Constants    = kLightShadowFlagConstants };

    inline constexpr std::string_view kLightSourceLayoutsRequires[] = { kLightSourceLayoutsShaderInclude.Path };

    inline constexpr ShaderBlockDecl kPointLightsUBBlocks[] = { { .Kind             = ShaderBlockKind::ReadBuffer,
                                                                    .Binding          = { ShaderBindingKind::Overridable, 6u, "POINT_LIGHTS_UB_BINDING" },
                                                                    .BlockName        = "PointLightsUB",
                                                                    .RuntimeArrayOf   = &kPointLightPayloadStruct,
                                                                    .RuntimeArrayName = "pointLights" } };
    inline constexpr const ShaderBlockDecl& kPointLightsUBBlock = kPointLightsUBBlocks[0];
    /// The scene's point lights: an unbounded storage buffer (no cap), count = LightsMetadata.PointLightCount.
    inline constexpr ShaderIncludeDecl kPointLightsUBShaderInclude{ .Path         = "Mesh/PointLightsUB.glslh",
                                                                     .SourceHeader = "Desert/Desert/Source/Engine/Graphic/ShaderProtocols/LightLayout.hpp",
                                                                     .Requires     = kLightSourceLayoutsRequires,
                                                                     .Blocks       = kPointLightsUBBlocks };

    inline constexpr ShaderBlockDecl kSpotLightsUBBlocks[] = { { .Kind             = ShaderBlockKind::ReadBuffer,
                                                                   .Binding          = { ShaderBindingKind::Overridable, 16u, "SPOT_LIGHTS_UB_BINDING" },
                                                                   .BlockName        = "SpotLightsUB",
                                                                   .RuntimeArrayOf   = &kSpotLightPayloadStruct,
                                                                   .RuntimeArrayName = "spotLights" } };
    inline constexpr const ShaderBlockDecl& kSpotLightsUBBlock = kSpotLightsUBBlocks[0];
    /// The scene's spot lights: an unbounded storage buffer (no cap), count = LightsMetadata.SpotLightCount.
    inline constexpr ShaderIncludeDecl kSpotLightsUBShaderInclude{ .Path         = "Mesh/SpotLightsUB.glslh",
                                                                    .SourceHeader = "Desert/Desert/Source/Engine/Graphic/ShaderProtocols/LightLayout.hpp",
                                                                    .Requires     = kLightSourceLayoutsRequires,
                                                                    .Blocks       = kSpotLightsUBBlocks };

    // Binding 14, not 3: Programs/Terrain/TerrainSurface.glslh holds 3 (LayerNormalTexture), and a block at 3
    // failed the Terrain program (two resources on one slot, VulkanShader::Reflect).
    inline constexpr const ShaderStructDecl* kDirectionLightStructs[] = { &kDirectionLightPayloadStruct };
    inline constexpr ShaderBlockDecl kDirectionLightsUBBlocks[] = { { .Kind         = ShaderBlockKind::Uniform,
                                                                        .Binding      = { ShaderBindingKind::Fixed, 14u, {} },
                                                                        .BlockName    = "DirectionLightsUB",
                                                                        .InstanceName = "directionLights",
                                                                        .Members      = &kDirectionLightsUBLayoutStruct } };
    inline constexpr const ShaderBlockDecl& kDirectionLightsUBBlock = kDirectionLightsUBBlocks[0];
    inline constexpr ShaderIncludeDecl kDirectionLightsUBShaderInclude{ .Path         = "Common/DirectionLightsUB.glslh",
                                                                         .SourceHeader = "Desert/Desert/Source/Engine/Graphic/ShaderProtocols/LightLayout.hpp",
                                                                         .Structs      = kDirectionLightStructs,
                                                                         .Blocks       = kDirectionLightsUBBlocks };

    inline constexpr ShaderBlockDecl kLightsMetadataBlocks[] = { { .Kind         = ShaderBlockKind::Uniform,
                                                                     .Binding      = { ShaderBindingKind::Fixed, 4u, {} },
                                                                     .BlockName    = "LightsMetadata",
                                                                     .InstanceName = "lightsMetadata",
                                                                     .Members      = &kLightsMetadataStruct } };
    inline constexpr const ShaderBlockDecl& kLightsMetadataBlock = kLightsMetadataBlocks[0];
    inline constexpr ShaderIncludeDecl kLightsMetadataShaderInclude{ .Path         = "Mesh/LightsMetadata.glslh",
                                                                      .SourceHeader = "Desert/Desert/Source/Engine/Graphic/ShaderProtocols/LightLayout.hpp",
                                                                      .Blocks       = kLightsMetadataBlocks };
} // namespace Desert::Graphic::ShaderProtocols
