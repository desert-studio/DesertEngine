#pragma once

// The scene's shadow block as the lit shaders read it. Declared once here; Generated/Materials/ShadowUB.glslh is
// generated from it at build time (kShadowUBShaderInclude below).

#include <Engine/Graphic/ShaderProtocols/ShaderLayout.hpp>
#include <Engine/Graphic/ShaderProtocols/ShaderProtocol.hpp>

#include <glm/glm.hpp>

#include <array>
#include <cstdint>

namespace Desert::Graphic::SceneResources
{
    // How many shadow cascades the ShadowUB block carries PER DIRECTIONAL LIGHT, and therefore how many layers of
    // the cascade array one light may hold. ONE number for the whole chain: SceneFrameBinding's views and the
    // renderer's cascade count are defined from it.
    inline constexpr uint32_t kMaxCascades = 4;

    // HOW MANY DIRECTIONAL LIGHTS HAVE CASCADES OF THEIR OWN (UE: every directional light with a whole-scene
    // shadow gets its own CSM; the cap is the shadow budget, not "the one lamp"). The slot IS the light's
    // Atmosphere Sun Light Index (ECS::Rules::kAtmosphereLightCount, asserted in Core/Scene.cpp): 0 the sun,
    // 1 the moon - the only directional lights the frame shades. Each slot is fitted, cached, drawn and sampled
    // by the same code; nothing below names "the moon".
    inline constexpr uint32_t kMaxShadowedDirectionalLights = 2;
    inline constexpr uint32_t kDirectionalShadowSun         = 0; // the slot the sun's occluders are sampled from
    // Every cascade of every slot: ShadowUBData::LightViewProj is indexed [slot * kMaxCascades + cascade].
    inline constexpr uint32_t kMaxCascadeViews = kMaxShadowedDirectionalLights * kMaxCascades;

    // THE VIEW'S SHADOW BLOCK: Mesh/ShadowUB.glslh's uniform ShadowUB (binding 7 of every lit program) and the
    // storage copies SSRTrace.glslh SsrShadowUB / SurfaceCacheDirectLightingTexel.glslh SurfaceCacheShadowUB, all
    // three declared from DESERT_ShadowUBData_MEMBERS (std140 and std430 lay it out the same).
    using Mat4ArrayCascadeViews = std::array<glm::mat4, kMaxCascadeViews>;
    using Vec4ArrayDirectionalSlots = std::array<glm::vec4, kMaxShadowedDirectionalLights>;
    using Mat4ArrayDirectionalSlots = std::array<glm::mat4, kMaxShadowedDirectionalLights>;
#define DESERT_SHADOW_UBDATA_FIELDS( X ) \
    X( Mat4ArrayCascadeViews, LightViewProj, "LightViewProjection", {} ) /* [slot * kMaxCascades + cascade] */ \
    /* Per slot: x = tan(source angle / 2), y = casts shadows (1/0), z = cascades fitted, w = first array \
       layer. */ \
    X( Vec4ArrayDirectionalSlots, DirectionalShadow, "DirectionalShadow", {} ) \
    /* per cascade: world size of one map texel */ \
    X( Vec4ArrayDirectionalSlots, CascadeTexelWorld, "CascadeTexelWorld", {} ) \
    /* per cascade: world length of [0,1] map depth */ \
    X( Vec4ArrayDirectionalSlots, CascadeDepthRange, "CascadeDepthRange", {} ) \
    /* x = show normals, y = lighting debug, z = cascade debug mode (0/1/2), w = 0 */ \
    X( glm::vec4, DebugParams, "DebugParams", {} ) \
    /* The contact shadows' march (Common/ContactShadow.glslh): x = ray steps (0 = none), y = tan(fovY / 2), z \
       = dither frame 0..7, w unused. Packed by PackContactShadow. */ \
    X( glm::vec4, ContactShadowView, "ContactShadowView", {} ) \
    /* x = the sun's ContactShadowLength, y = 1 when it is in world centimetres, z = its Contact Shadow \
       Casting Intensity, w = its Non Casting Intensity (Common/ContactShadow.glslh's `light`). */ \
    X( glm::vec4, SunContactShadow, "SunContactShadow", {} ) \
    /* The PCSS cost (Mesh/CascadedShadow.glslh): x = blocker search taps, y = filter taps; z = 1 when the \
       directional lights' cascades hand over to this frame's distance-field shadow \
       (ShadowFilterSettings::DistanceField), w = the local lights' screen-trace iterations. Packed by \
       PackShadowFilter. */ \
    X( glm::vec4, ShadowFilter, "ShadowFilter", {} ) \
    /* THE FAR SHADOW per slot (DirectionalShadowView::Far*), appended so every receiver's block keeps its \
       offsets above: its matrix, and x = world size of one texel, y = world length of [0,1] depth, z = its \
       array layer, w = 1 when fitted this frame (0: lit past the cascades). */ \
    X( Mat4ArrayDirectionalSlots, FarShadowViewProj, "FarShadowViewProj", {} ) \
    X( Vec4ArrayDirectionalSlots, FarShadow, "FarShadow", {} )
    DESERT_SHADER_STRUCT( ShadowUBData, "ShadowUBData", Both, DESERT_SHADOW_UBDATA_FIELDS );
#undef DESERT_SHADOW_UBDATA_FIELDS

    inline constexpr const ShaderProtocols::ShaderStructDecl* kShadowUBStructs[] = { &kShadowUBDataStruct };
    inline constexpr ShaderProtocols::ShaderIncludeDecl kShadowUBShaderInclude{
        .Path         = "Generated/Materials/ShadowUB.glslh",
        .SourceHeader = "Desert/Desert/Source/Engine/Graphic/Materials/SceneResourcesLayout.hpp",
        .Structs      = kShadowUBStructs,
    };
} // namespace Desert::Graphic::SceneResources
