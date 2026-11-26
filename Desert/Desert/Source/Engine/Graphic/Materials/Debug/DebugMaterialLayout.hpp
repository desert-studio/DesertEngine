#pragma once

// THE DEBUG MATERIALS' GPU RECORDS, declared once: a debug line list's vertex (MaterialDebugLine) and the editor
// grid's uniform block (MaterialGrid). Tools/ShaderProtocolGen writes them as
// Generated/Debug/DebugMaterials.glslh, which Programs/Debug/DebugLine.shader and Editor/Grid.shader include.

#include <Engine/Graphic/ShaderProtocols/ShaderLayout.hpp>
#include <Engine/Graphic/ShaderProtocols/ShaderProtocol.hpp>

#include <glm/glm.hpp>

namespace Desert::Graphic
{
    // One endpoint of a debug line (or a corner of a flat-coloured triangle), pulled by gl_VertexIndex.
#define DESERT_DEBUG_LINE_VERTEX_FIELDS( X )                                                                      \
    X( glm::vec4, PositionWS, "PositionWS", glm::vec4( 0.0f ) ) /* xyz = world position (w unused) */             \
    X( glm::vec4, Color, "Color", glm::vec4( 0.0f ) )           /* rgba */
    DESERT_SHADER_STRUCT( DebugLineVertex, "LineVertex", Std430, DESERT_DEBUG_LINE_VERTEX_FIELDS );
#undef DESERT_DEBUG_LINE_VERTEX_FIELDS

    // The infinite ground grid's block ("GridUB", MaterialGridShaderParameters::GridUB).
#define DESERT_GRID_UNIFORMS_FIELDS( X )                                                                          \
    X( glm::mat4, Projection, "Projection", glm::mat4( 1.0f ) )                                                   \
    X( glm::mat4, View, "View", glm::mat4( 1.0f ) )                                                               \
    X( glm::mat4, InvProjection, "InvProjection", glm::mat4( 1.0f ) )                                             \
    X( glm::mat4, InvView, "InvView", glm::mat4( 1.0f ) )                                                         \
    X( glm::vec4, CameraPos, "CameraPos", glm::vec4( 0.0f ) ) /* xyz */                                           \
    X( glm::vec4, ThinColor, "ThinColor", glm::vec4( 0.0f ) )                                                     \
    X( glm::vec4, ThickColor, "ThickColor", glm::vec4( 0.0f ) )                                                   \
    /* x = base cell size (cm), y = fade start, z = fade end (cm) */                                              \
    X( glm::vec4, Params, "Params", glm::vec4( 0.0f ) )
    DESERT_SHADER_STRUCT( GridUniforms, "GridParams", Std140, DESERT_GRID_UNIFORMS_FIELDS );
#undef DESERT_GRID_UNIFORMS_FIELDS

    inline constexpr const ShaderProtocols::ShaderStructDecl* kDebugMaterialStructs[] = {
         &kDebugLineVertexStruct,
         &kGridUniformsStruct,
    };
    inline constexpr ShaderProtocols::ShaderIncludeDecl kDebugMaterialShaderInclude{
         .Path         = "Generated/Debug/DebugMaterials.glslh",
         .SourceHeader = "Desert/Desert/Source/Engine/Graphic/Materials/Debug/DebugMaterialLayout.hpp",
         .Structs      = kDebugMaterialStructs,
    };
} // namespace Desert::Graphic
