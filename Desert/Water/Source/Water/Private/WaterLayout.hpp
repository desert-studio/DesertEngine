#pragma once

// THE WATER PASSES' OWN BLOCKS, declared once: Tools/ShaderProtocolGen writes their GLSL into
// Generated/Water/*.glslh and checks each layout against these declarations at compile time. The programs own the
// bindings and write `Uniform(N) Block { DESERT_<GlslName>_MEMBERS };`. The rows public headers name are
// Water/Public/WaterLayout.hpp's.

#include <Engine/Graphic/ShaderProtocols/ShaderLayout.hpp>
#include <Engine/Graphic/ShaderProtocols/ShaderProtocol.hpp>
#include <Water/Public/WaterFoamClipmap.hpp>

#include <glm/glm.hpp>

#include <array>

namespace Desert::Graphic::System
{
    // Programs/Water/WaterUnderwater.shader UnderwaterUB (a uniform block, not a push block: 176 bytes is past
    // the 128 Vulkan guarantees).
#define DESERT_UNDERWATER_UB_DATA_FIELDS( X )                                                                     \
    /* the view's jittered inverse (the depth was rasterised jittered) */                                         \
    X( glm::mat4, InvViewProjection, "InvViewProjection", glm::mat4( 1.0f ) )                                     \
    /* xyz = the camera (cm), w = the surface height at the camera (cm) */                                        \
    X( glm::vec4, CameraSurface, "CameraSurface", glm::vec4( 0.0f ) )                                             \
    /* rgb = scattering 1/m, a = phase g */                                                                       \
    X( glm::vec4, Scattering, "Scattering", glm::vec4( 0.0f ) )                                                   \
    /* rgb = absorption 1/m, a = the view's pre-exposure */                                                       \
    X( glm::vec4, Absorption, "Absorption", glm::vec4( 0.0f ) )                                                   \
    /* xyz = toward the sun */                                                                                    \
    X( glm::vec4, ToSun, "ToSun", glm::vec4( 0.0f ) )                                                             \
    /* rgb = colour * intensity (not pre-exposed) */                                                              \
    X( glm::vec4, SunRadiance, "SunRadiance", glm::vec4( 0.0f ) )                                                 \
    /* xy = the sky look's (cos yaw, sin yaw) */                                                                  \
    X( glm::vec4, SkyYawCosSin, "SkyYawCosSin", glm::vec4( 0.0f ) )                                               \
    /* rgb = the sky look's tint * intensity */                                                                   \
    X( glm::vec4, SkyGain, "SkyGain", glm::vec4( 0.0f ) )
    DESERT_SHADER_STRUCT( UnderwaterUBData, "UnderwaterUniforms", Std140, DESERT_UNDERWATER_UB_DATA_FIELDS );
#undef DESERT_UNDERWATER_UB_DATA_FIELDS

    inline constexpr const ShaderProtocols::ShaderStructDecl* kWaterUnderwaterStructs[] = {
         &kUnderwaterUBDataStruct };
    inline constexpr ShaderProtocols::ShaderIncludeDecl kWaterUnderwaterShaderInclude{
         .Path         = "Generated/Water/WaterUnderwater.glslh",
         .SourceHeader = "Desert/Water/Source/Water/Private/WaterLayout.hpp",
         .Structs      = kWaterUnderwaterStructs,
    };

    // Programs/Water/WaterFoamUpdate.shader WaterFoamUB.
    using WaterFoamLevelOrigins = std::array<glm::vec4, Water::kFoamLevels>;
#define DESERT_WATER_FOAM_UB_DATA_FIELDS( X )                                                                     \
    /* xy = the history's window origins */                                                                       \
    X( WaterFoamLevelOrigins, Previous, "FoamPrevious", {} )                                                      \
    /* x = the time (s), y = since the history (s), z = 1 with a history, w = the bodies */                       \
    X( glm::vec4, Time, "FoamTime", glm::vec4( 0.0f ) )                                                           \
    /* the world rectangle the history does not stand in (min X, min Z, max X, max Z); min > max: none */         \
    X( glm::vec4, Changed, "FoamChanged", glm::vec4( 1.0f, 1.0f, 0.0f, 0.0f ) )                                   \
    /* x = the plunges in WaterFoamPlunges */                                                                     \
    X( glm::vec4, PlungeCount, "FoamPlungeCount", glm::vec4( 0.0f ) )
    DESERT_SHADER_STRUCT( WaterFoamUBData, "WaterFoamUniforms", Std140, DESERT_WATER_FOAM_UB_DATA_FIELDS );
#undef DESERT_WATER_FOAM_UB_DATA_FIELDS

    inline constexpr const ShaderProtocols::ShaderStructDecl* kWaterFoamUpdateStructs[] = {
         &kWaterFoamUBDataStruct };
    inline constexpr ShaderProtocols::ShaderIncludeDecl kWaterFoamUpdateShaderInclude{
         .Path         = "Generated/Water/WaterFoamUpdate.glslh",
         .SourceHeader = "Desert/Water/Source/Water/Private/WaterLayout.hpp",
         .Structs      = kWaterFoamUpdateStructs,
    };

    // Programs/Water/WaterShoreUpload.shader WaterShoreUploadParams.
#define DESERT_SHORE_UPLOAD_PARAMS_FIELDS( X )                                                                    \
    /* the rectangle [x0, x1) x [z0, z1) as (x0, z0, x1, z1) */                                                   \
    X( glm::ivec4, Rect, "ShoreRect", glm::ivec4( 0 ) )                                                           \
    /* x = the rows' width (the buffer starts at row z0, texel 0) */                                              \
    X( glm::ivec4, Rows, "ShoreRows", glm::ivec4( 0 ) )
    DESERT_SHADER_STRUCT( ShoreUploadParams, "ShoreUploadParams", Std430, DESERT_SHORE_UPLOAD_PARAMS_FIELDS );
#undef DESERT_SHORE_UPLOAD_PARAMS_FIELDS

    inline constexpr const ShaderProtocols::ShaderStructDecl* kWaterShoreUploadStructs[] = {
         &kShoreUploadParamsStruct };
    inline constexpr ShaderProtocols::ShaderIncludeDecl kWaterShoreUploadShaderInclude{
         .Path         = "Generated/Water/WaterShoreUpload.glslh",
         .SourceHeader = "Desert/Water/Source/Water/Private/WaterLayout.hpp",
         .Structs      = kWaterShoreUploadStructs,
    };

    // Water/WaterFFT.glslh WaterFFTParams: the per-frame block the FFT compute programs share.
#define DESERT_WATER_FFT_PARAMS_FIELDS( X )                                                                       \
    /* the patch sides of cascades 0..3 (cm) */                                                                   \
    X( glm::vec4, Cascade, "Cascade", glm::vec4( 0.0f ) )                                                         \
    /* x = choppiness (lambda), y = IFFT direction (0 = rows, 1 = columns), z = the view's time (s), w = what the \
       array holds: 0 nothing, 1 the previous frame, 2 this very frame's time (a second view) */                  \
    X( glm::vec4, Shape, "Shape", glm::vec4( 0.0f ) )
    DESERT_SHADER_STRUCT( WaterFFTParams, "WaterFFTParams", Std430, DESERT_WATER_FFT_PARAMS_FIELDS );
#undef DESERT_WATER_FFT_PARAMS_FIELDS

    inline constexpr const ShaderProtocols::ShaderStructDecl* kWaterFFTStructs[] = { &kWaterFFTParamsStruct };
    inline constexpr ShaderProtocols::ShaderIncludeDecl       kWaterFFTShaderInclude{
               .Path         = "Generated/Water/WaterFFT.glslh",
               .SourceHeader = "Desert/Water/Source/Water/Private/WaterLayout.hpp",
               .Structs      = kWaterFFTStructs,
    };
} // namespace Desert::Graphic::System
