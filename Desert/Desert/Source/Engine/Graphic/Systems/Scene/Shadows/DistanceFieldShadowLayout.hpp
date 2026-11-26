#pragma once

// THE DISTANCE-FIELD SHADOW'S PARAMETER BLOCK, declared once: Tools/ShaderProtocolGen writes its GLSL twin into
// Generated/Shadows/DistanceFieldShadow.glslh (read by Common/DistanceFieldShadow.glslh) and checks the std430 layout
// against this declaration at compile time.

#include <Engine/Graphic/DistanceField/GlobalDistanceFieldCeiling.hpp>
#include <Engine/Graphic/ShaderProtocols/ShaderLayout.hpp>
#include <Engine/Graphic/ShaderProtocols/ShaderProtocol.hpp>

#include <glm/glm.hpp>

#include <array>
#include <cstdint>

namespace Desert::Graphic::System
{
    using Vec4ArrayGdfClipmaps = std::array<glm::vec4, DistanceField::kGlobalDistanceFieldClipmaps>;
#define DESERT_DISTANCE_FIELD_SHADOW_GPU_PARAMS_FIELDS( X ) \
    /* xyz = light-space x axis (unit), w = -dot(axis, camera) */ \
    X( glm::vec4, LightX, "lightX", glm::vec4( 1.0f, 0.0f, 0.0f, 0.0f ) ) \
    X( glm::vec4, LightY, "lightY", glm::vec4( 0.0f, 1.0f, 0.0f, 0.0f ) ) \
    X( glm::vec4, LightZ, "lightZ", glm::vec4( 0.0f, 0.0f, 1.0f, 0.0f ) ) /* toward the sun */ \
    /* xy = the grid's min corner (cm), zw = 1 / tile size (1/cm) */ \
    X( glm::vec4, Tiles, "tiles", glm::vec4( 0.0f ) ) \
    /* lowest light z, tan(angle / 2), max distance, band */ \
    X( glm::vec4, Receivers, "receivers", glm::vec4( 0.0f ) ) \
    /* objects (meshes + tiles), tiles a side, tiles, list room */ \
    X( glm::uvec4, Counts, "counts", glm::uvec4( 0u ) ) \
    /* the G-buffer's inverse jittered view-projection */ \
    X( glm::mat4, InvViewProjection, "invViewProjection", glm::mat4( 1.0f ) ) \
    X( glm::mat4, LastCascade, "lastCascade", glm::mat4( 1.0f ) ) /* the sun's last cascade's view-projection */ \
    /* xyz = camera (cm), w = 1 when LastCascade is the light's */ \
    X( glm::vec4, Camera, "camera", glm::vec4( 0.0f ) ) \
    X( Vec4ArrayGdfClipmaps, GdfLevels, "gdfLevels", {} ) /* centre.xyz, voxel cm */ \
    X( glm::vec4, Extent, "extent", glm::vec4( 0.0f ) ) /* full xy, half zw */
    DESERT_SHADER_STRUCT( DistanceFieldShadowGpuParams, "DistanceFieldShadowParams", Std430,
                          DESERT_DISTANCE_FIELD_SHADOW_GPU_PARAMS_FIELDS );
#undef DESERT_DISTANCE_FIELD_SHADOW_GPU_PARAMS_FIELDS

    inline constexpr const ShaderProtocols::ShaderStructDecl* kDistanceFieldShadowStructs[] = {
        &kDistanceFieldShadowGpuParamsStruct,
    };
    inline constexpr ShaderProtocols::ShaderIncludeDecl kDistanceFieldShadowShaderInclude{
        .Path         = "Generated/Shadows/DistanceFieldShadow.glslh",
        .SourceHeader = "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Shadows/DistanceFieldShadowLayout.hpp",
        .Structs      = kDistanceFieldShadowStructs,
    };
} // namespace Desert::Graphic::System
