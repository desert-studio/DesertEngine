#pragma once
// THE TRANSLUCENCY LIGHTING VOLUME FILL'S PARAMETER BLOCK, declared once: Tools/ShaderProtocolGen generates
// Generated/Post/TranslucencyLightingVolumeParams.glslh (the TranslucencyLightingVolumeParams storage buffer,
// binding included) from it, included by Programs/Translucency/TranslucencyLightingVolume.shader. Layout only:
// TranslucencyLightingVolume.cpp fills and binds it.
#include <Engine/Graphic/ShaderProtocols/ShaderLayout.hpp>
#include <Engine/Graphic/ShaderProtocols/ShaderProtocol.hpp>

#include <glm/glm.hpp>

#include <array>

// X( C++ type, member, "GLSL name", initialiser )
namespace Desert::Graphic
{
    // A field type with a template comma cannot pass through X( ... ): named here.
    using TlvCascadeMatrices = std::array<glm::mat4, 4>;
} // namespace Desert::Graphic

#define DESERT_TRANSLUCENCY_LIGHTING_VOLUME_FIELDS( X )                                                              \
    X( glm::mat4, ViewProjection, "TlvViewProjection", glm::mat4( 1.0f ) ) /* world -> clip, this frame's unjittered camera (the light grid's) */ \
    X( TlvCascadeMatrices, CascadeViewProj, "TlvCascadeViewProj", {} ) /* the sun's cascades, standard Z (Graphic::CascadeFit::ViewProj) */ \
    X( glm::vec4, Camera, "TlvCamera", glm::vec4( 0.0f ) ) /* xyz = camera position (cm), w = cells per cascade edge */ \
    X( glm::vec4, RenderSize, "TlvRenderSize", glm::vec4( 0.0f ) ) /* xy = render size in pixels (the light grid's) */ \
    X( glm::vec4, SunDirection, "TlvSunDirection", glm::vec4( 0.0f ) ) /* xyz = TOWARD the sun (unit), w = 1 when a sun lights the volume */ \
    X( glm::vec4, SunRadiance, "TlvSunRadiance", glm::vec4( 0.0f ) ) /* rgb = the sun's irradiance as the surfaces receive it, w = cascades */ \
    X( glm::vec4, CascadeBias, "TlvCascadeBias", glm::vec4( 0.0f ) ) /* per cascade: depth bias in map units */      \
    X( glm::mat4, CloudShadowWorldToMap, "TlvCloudShadowWorldToMap", glm::mat4( 1.0f ) ) /* the cloud layer's shadow map (Graphic::CloudShadowUniforms) */ \
    X( glm::vec4, CloudShadowParams, "TlvCloudShadowParams", glm::vec4( 0.0f ) ) /* y = 0 when no layer casts */     \
    X( glm::mat4, RainOcclusionWorldToMap, "TlvRainOcclusionWorldToMap", glm::mat4( 1.0f ) ) /* the view's rain occlusion (Graphic::RainOcclusionUniforms) */ \
    X( glm::vec4, RainOcclusionParams, "TlvRainOcclusionParams", glm::vec4( 0.0f ) ) /* y = 0 when the view has no map */

namespace Desert::Graphic
{
    DESERT_SHADER_STRUCT( TranslucencyLightingVolumeGpuPayload, "TranslucencyLightingVolumeGpuPayload", Std430,
                          DESERT_TRANSLUCENCY_LIGHTING_VOLUME_FIELDS );

    // The fill's own block (the pipeline's SetStorageBuffer, not a graph binding): its binding is written here
    // once, and the shader includes the whole generated block.
    inline constexpr ShaderProtocols::ShaderBlockDecl kTranslucencyLightingVolumeParamsBlocks[] = {
         { .Kind      = ShaderProtocols::ShaderBlockKind::ReadBuffer,
           .Binding   = { ShaderProtocols::ShaderBindingKind::Fixed, 1u, {} },
           .BlockName = "TranslucencyLightingVolumeParams",
           .Members   = &kTranslucencyLightingVolumeGpuPayloadStruct } };
    inline constexpr const ShaderProtocols::ShaderBlockDecl& kTranslucencyLightingVolumeParamsBlock =
         kTranslucencyLightingVolumeParamsBlocks[0];
    inline constexpr ShaderProtocols::ShaderIncludeDecl kTranslucencyLightingVolumeShaderInclude{
         .Path         = "Generated/Post/TranslucencyLightingVolumeParams.glslh",
         .SourceHeader = "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Translucency/TranslucencyLightingVolumeLayout.hpp",
         .Blocks       = kTranslucencyLightingVolumeParamsBlocks };
} // namespace Desert::Graphic
