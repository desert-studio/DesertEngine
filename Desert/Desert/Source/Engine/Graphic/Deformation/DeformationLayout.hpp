#pragma once

// THE DEFORMATION FIELD'S READ BLOCK, declared once: Tools/ShaderProtocolGen writes its GLSL into
// Generated/Deformation/DeformationFieldParams.glslh (included by Deformation/DeformationFieldRead.glslh, which
// owns the slot: Core::kDeformationFieldParamsBinding of the set-0 scene table) and checks the std430 layout
// against this declaration at compile time.

#include <Engine/Graphic/ShaderProtocols/ShaderLayout.hpp>
#include <Engine/Graphic/ShaderProtocols/ShaderProtocol.hpp>

#include <glm/glm.hpp>

namespace Desert::Graphic::Deformation
{
    // The block the field's surface readers bind (DEFORM-GRASS, DeformationFieldParams), std430.
#define DESERT_DEFORMATION_FIELD_PARAMS_GPU_FIELDS( X )                                                           \
    X( glm::ivec4, Windows, "DeformWindows", glm::ivec4( 0 ) ) /* xy: level 0's minimum cell, zw: level 1's */    \
    /* x, y: texel (cm) of level 0, 1; z, w: their resolution (0: no field this frame) */                         \
    X( glm::vec4, Levels, "DeformLevels", glm::vec4( 0.0f ) )
    DESERT_SHADER_STRUCT( DeformationFieldParamsGpu, "DeformationFieldParamsGpu", Std430,
                          DESERT_DEFORMATION_FIELD_PARAMS_GPU_FIELDS );
#undef DESERT_DEFORMATION_FIELD_PARAMS_GPU_FIELDS

    inline constexpr const ShaderProtocols::ShaderStructDecl* kDeformationFieldParamsStructs[] = {
         &kDeformationFieldParamsGpuStruct };
    inline constexpr ShaderProtocols::ShaderIncludeDecl kDeformationFieldParamsShaderInclude{
         .Path         = "Generated/Deformation/DeformationFieldParams.glslh",
         .SourceHeader = "Desert/Desert/Source/Engine/Graphic/Deformation/DeformationLayout.hpp",
         .Structs      = kDeformationFieldParamsStructs,
    };
} // namespace Desert::Graphic::Deformation
