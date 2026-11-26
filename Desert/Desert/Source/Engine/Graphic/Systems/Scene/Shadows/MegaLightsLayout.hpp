#pragma once

// THE MEGALIGHTS VIEW BLOCKS, declared once: Tools/ShaderProtocolGen writes their GLSL twins into
// Generated/Shadows/MegaLights.glslh (read by Common/MegaLights.glslh, Common/MegaLightsVolume.glslh and
// Common/MegaLightsVolumeWrite.glslh) and checks the layouts against these declarations at compile time.

#include <Engine/Graphic/ShaderProtocols/ShaderLayout.hpp>
#include <Engine/Graphic/ShaderProtocols/ShaderProtocol.hpp>

#include <glm/glm.hpp>

#include <cstdint>

namespace Desert::Graphic::System
{
    // MegaLightsViewUB (Common/MegaLights.glslh), std140.
#define DESERT_MEGA_LIGHTS_VIEW_UNIFORMS_FIELDS( X ) \
    X( glm::mat4, ViewProjection, "ViewProjection", {} ) \
    X( glm::mat4, PrevViewProjection, "PreviousViewProjection", {} ) \
    X( glm::mat4, InvJitteredViewProjection, "InverseJitteredViewProjection", {} ) \
    /* the contact shadow's march (Common/ContactShadow.glslh) */ \
    X( glm::mat4, JitteredViewProjection, "JitteredWorldToClip", {} ) \
    X( glm::vec4, CameraPosition, "CameraPosition", {} ) /* xyz, w = PreExposure */ \
    X( glm::uvec4, Counts, "Counts", {} ) /* points, spots, samples per pixel, hash readable */ \
    /* max shading weight, of a hidden light, normal bias, hidden light share */ \
    X( glm::vec4, Params, "Params", {} ) \
    /* frame index, render width, render height, downsample factor */ \
    X( glm::uvec4, Frame, "MegaLightsFrame", {} ) \
    X( glm::vec4, ContactShadowView, "ContactShadowView", {} ) /* steps, tan(fov / 2), dither frame */ \
    /* EmissiveLightsFrame::StaticParams of MegaLightsInputs::Emissive */ \
    X( glm::uvec4, EmissiveCounts, "EmissiveCounts", {} ) \
    X( glm::uvec4, EmissiveDynamicCounts, "EmissiveDynamicCounts", {} ) /* and its DynamicParams */
    DESERT_SHADER_STRUCT( MegaLightsViewUniforms, "MegaLightsView", Std140,
                          DESERT_MEGA_LIGHTS_VIEW_UNIFORMS_FIELDS );
#undef DESERT_MEGA_LIGHTS_VIEW_UNIFORMS_FIELDS

    // MegaLightsVolumeViewUB (Programs/Shadows/MegaLightsVolume.shader), std140.
#define DESERT_MEGA_LIGHTS_VOLUME_UNIFORMS_FIELDS( X ) \
    X( glm::mat4, InvViewProjection, "InverseViewProjection", {} ) \
    X( glm::mat4, View, "View", {} ) \
    X( glm::mat4, PrevView, "PreviousView", {} ) \
    X( glm::mat4, PrevViewProjection, "PreviousViewProjection", {} ) \
    X( glm::vec4, ZParams, "ZParams", {} ) /* near, slices / log(far / near), far, froxel edge (px) */ \
    X( glm::vec4, PrevZParams, "PreviousZParams", {} ) /* the same, of the frame the history was written in */ \
    X( glm::uvec4, Size, "RenderSize", {} ) /* froxels xyz, frame index */ \
    X( glm::uvec4, Counts, "Counts", {} ) /* points, spots, samples per froxel, history readable */ \
    X( glm::vec4, Params, "Params", {} ) /* history weight, ray bias (cm), render extent (px) */ \
    /* the screen trace: the matrices the HZB's depth was drawn with */ \
    X( glm::mat4, JitteredViewProjection, "JitteredWorldToClip", {} ) \
    X( glm::mat4, InvJitteredViewProjection, "InverseJitteredViewProjection", {} ) \
    X( glm::vec4, CameraPosition, "CameraPosition", {} ) /* xyz */
    DESERT_SHADER_STRUCT( MegaLightsVolumeUniforms, "MegaLightsVolumeView", Std140,
                          DESERT_MEGA_LIGHTS_VOLUME_UNIFORMS_FIELDS );
#undef DESERT_MEGA_LIGHTS_VOLUME_UNIFORMS_FIELDS

    // MegaLightsVolumeUB (Common/MegaLightsVolume.glslh), std430: what a reader maps a fragment with.
#define DESERT_MEGA_LIGHTS_VOLUME_READ_PARAMS_FIELDS( X ) \
    X( glm::mat4, View, "View", {} ) \
    X( glm::vec4, ZParams, "ZParams", {} ) \
    X( glm::uvec4, Size, "Size", {} ) /* froxels xyz, on (1) / off (0) */
    DESERT_SHADER_STRUCT( MegaLightsVolumeReadParams, "MegaLightsVolumeReadParams", Std430,
                          DESERT_MEGA_LIGHTS_VOLUME_READ_PARAMS_FIELDS );
#undef DESERT_MEGA_LIGHTS_VOLUME_READ_PARAMS_FIELDS

    inline constexpr const ShaderProtocols::ShaderStructDecl* kMegaLightsStructs[] = {
        &kMegaLightsViewUniformsStruct,
        &kMegaLightsVolumeUniformsStruct,
        &kMegaLightsVolumeReadParamsStruct,
    };
    inline constexpr ShaderProtocols::ShaderIncludeDecl kMegaLightsShaderInclude{
        .Path         = "Generated/Shadows/MegaLights.glslh",
        .SourceHeader = "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Shadows/MegaLightsLayout.hpp",
        .Structs      = kMegaLightsStructs,
    };
} // namespace Desert::Graphic::System
