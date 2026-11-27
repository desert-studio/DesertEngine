#pragma once

// THE REFLECTION CAPTURES' GPU RECORDS, declared once: one capture as the lighting passes read it, and one capture
// point the diffuse measure reads. Tools/ShaderProtocolGen writes them as
// Generated/ReflectionCaptures/ReflectionCaptures.glslh, which Common/ReflectionEnvironment.glslh,
// Mesh/Surface/ViewIndirect.glslh and ReflectionCaptureDiffuse.shader include.

#include <Engine/Graphic/ShaderProtocols/ShaderLayout.hpp>
#include <Engine/Graphic/ShaderProtocols/ShaderProtocol.hpp>

#include <glm/glm.hpp>

namespace Desert::Graphic
{
    // ONE CAPTURE AS THE LIGHTING PASSES READ IT (Common/ReflectionEnvironment.glslh says who reads it and how).
#define DESERT_REFLECTION_CAPTURE_GPU_FIELDS( X )                                                                 \
    /* xyz = influence centre (entity position, world cm), w = sphere influence radius */                         \
    X( glm::vec4, PositionRadius, "PositionRadius", glm::vec4( 0.0f ) )                                           \
    /* xyz = capture point - centre (world cm), w = Brightness */                                                 \
    X( glm::vec4, CaptureOffsetBrightness, "CaptureOffsetBrightness", glm::vec4( 0.0f ) )                         \
    /* xyz = box half extent (cm), w = box transition distance (cm) */                                            \
    X( glm::vec4, BoxExtentTransition, "BoxExtentTransition", glm::vec4( 0.0f ) )                                 \
    /* x = 0 sphere / 1 box, y = cube index in ReflectionCaptureCubesTexture, z = the capture's presence (float   \
       bits, 0..1: PackReflectionCaptures): its first cube fading in */                                           \
    X( glm::uvec4, ShapeCube, "ShapeCube", glm::uvec4( 0u ) )                                                     \
    /* world -> box space, where the box is [-1, 1]^3 (box shape only) */                                         \
    X( glm::mat4, WorldToBox, "WorldToBox", glm::mat4( 1.0f ) )
    DESERT_SHADER_STRUCT( ReflectionCaptureGpu, "ReflectionCaptureGpu", Std430,
                          DESERT_REFLECTION_CAPTURE_GPU_FIELDS );
#undef DESERT_REFLECTION_CAPTURE_GPU_FIELDS

    // One capture point the diffuse measure reads (ReflectionCaptureDiffuse.shader DiffusePoints).
#define DESERT_REFLECTION_CAPTURE_DIFFUSE_POINT_FIELDS( X )                                                       \
    X( glm::vec4, Point, "Point", glm::vec4( 0.0f ) ) /* xyz = the capture point (world cm) */                    \
    /* x = cube index in the array (the staging cube for a recapture), y = 1 measure unconditionally (the cube    \
       being written) */                                                                                          \
    X( glm::uvec4, CubeForced, "CubeForced", glm::uvec4( 0u ) )
    DESERT_SHADER_STRUCT( ReflectionCaptureDiffusePoint, "DiffusePoint", Std430,
                          DESERT_REFLECTION_CAPTURE_DIFFUSE_POINT_FIELDS );
#undef DESERT_REFLECTION_CAPTURE_DIFFUSE_POINT_FIELDS

    inline constexpr const ShaderProtocols::ShaderStructDecl* kReflectionCaptureStructs[] = {
         &kReflectionCaptureGpuStruct,
         &kReflectionCaptureDiffusePointStruct,
    };
    inline constexpr ShaderProtocols::ShaderIncludeDecl kReflectionCaptureShaderInclude{
         .Path         = "Generated/ReflectionCaptures/ReflectionCaptures.glslh",
         .SourceHeader = "Desert/Desert/Source/Engine/Graphic/ReflectionCaptures/ReflectionCaptureLayout.hpp",
         .Structs      = kReflectionCaptureStructs,
    };
} // namespace Desert::Graphic
