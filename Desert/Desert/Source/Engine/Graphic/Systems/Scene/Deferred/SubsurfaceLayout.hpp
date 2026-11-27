#pragma once
// THE SUBSURFACE SCATTER'S VIEW BLOCK, declared once: Tools/ShaderProtocolGen generates
// Generated/Post/SubsurfaceViewGpu.glslh (`struct SubsurfaceViewGpu` + DESERT_SubsurfaceViewGpu_MEMBERS) from
// it, and Programs/Subsurface/SubsurfaceBurley.shader declares its SubsurfaceViewUB (std430) with those
// members. Layout only: SubsurfaceRenderer fills it once per frame.
#include <Engine/Graphic/ShaderProtocols/ShaderLayout.hpp>
#include <Engine/Graphic/ShaderProtocols/ShaderProtocol.hpp>

#include <glm/glm.hpp>


// X( C++ type, member, "GLSL name", initialiser )
#define DESERT_SUBSURFACE_VIEW_FIELDS( X )                                                                           \
    X( glm::mat4, WorldToClip, "WorldToClip", glm::mat4( 1.0f ) ) /* the jittered view-projection the G-buffer was rasterised with */ \
    X( glm::mat4, ClipToWorld, "ClipToWorld", glm::mat4( 1.0f ) ) /* its inverse */                                  \
    X( glm::vec4, Params, "ViewParams", glm::vec4( 0.0f ) ) /* x scale, y most samples, z frame index, w fewest samples */ \
    X( glm::mat4, PrevWorldToClip, "PrevWorldToClip", glm::mat4( 1.0f ) ) /* the previous frame's jittered view-projection */ \
    X( glm::mat4, PrevClipToWorld, "PrevClipToWorld", glm::mat4( 1.0f ) ) /* its inverse */                          \
    X( glm::vec4, HistoryParams, "HistoryParams", glm::vec4( 0.0f ) ) /* x history readable, y PreExposure / PrevPreExposure, z most frames, w the stamp the read history carries */ \
    X( glm::vec4, StampParams, "StampParams", glm::vec4( 0.0f ) ) /* x this scatter's stamp, written into every moments texel it writes */

namespace Desert::Graphic::System
{
    DESERT_SHADER_STRUCT( SubsurfaceViewGpu, "SubsurfaceViewGpu", Std430, DESERT_SUBSURFACE_VIEW_FIELDS );

    inline constexpr const ShaderProtocols::ShaderStructDecl* kSubsurfaceViewGpuStructs[] = { &kSubsurfaceViewGpuStruct };
    inline constexpr ShaderProtocols::ShaderIncludeDecl kSubsurfaceViewGpuShaderInclude{
         .Path         = "Generated/Post/SubsurfaceViewGpu.glslh",
         .SourceHeader = "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Deferred/SubsurfaceLayout.hpp",
         .Structs      = kSubsurfaceViewGpuStructs };
} // namespace Desert::Graphic::System
