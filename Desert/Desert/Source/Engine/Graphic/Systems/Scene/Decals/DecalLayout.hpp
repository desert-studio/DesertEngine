#pragma once

// THE DECAL ROW, declared once: Tools/ShaderProtocolGen writes its GLSL twin into Generated/Decals/Decal.glslh (read by
// Programs/Decals/DefaultDecal.shader) and checks the std430 layout against this
// declaration at compile time.

#include <Engine/Graphic/ShaderProtocols/ShaderLayout.hpp>
#include <Engine/Graphic/ShaderProtocols/ShaderProtocol.hpp>

#include <glm/glm.hpp>

#include <cstdint>

namespace Desert::Graphic::System
{
    // One row of "Decals.Rows" (std430, GLSL DecalRow): what a decal's vertex and
    // fragment stages need, baked per view on the CPU so the push block stays one row index.
#define DESERT_DECAL_ROW_GPU_FIELDS( X ) \
    /* view-projection x DecalToWorld: the box's corners to clip space */ \
    X( glm::mat4, DecalToClip, "DecalToClip", {} ) \
    /* inverse( DecalToWorld ) x inverse view-projection: a depth texel into the box */ \
    X( glm::mat4, ClipToDecal, "ClipToDecal", {} ) \
    /* the box's world x / y / z axes, normalised (the decal's tangent frame) */ \
    X( glm::vec4, AxisX, "AxisX", {} ) \
    X( glm::vec4, AxisY, "AxisY", {} ) \
    X( glm::vec4, AxisZ, "AxisZ", {} ) \
    /* x opacity (the CPU fade), y edge fade, z channel mask (as a float), w unused */ \
    X( glm::vec4, Params, "Params", {} )
    DESERT_SHADER_STRUCT( DecalRowGpu, "DecalRow", Std430, DESERT_DECAL_ROW_GPU_FIELDS );
#undef DESERT_DECAL_ROW_GPU_FIELDS

    inline constexpr const ShaderProtocols::ShaderStructDecl* kDecalStructs[] = { &kDecalRowGpuStruct };
    inline constexpr ShaderProtocols::ShaderIncludeDecl kDecalShaderInclude{
        .Path         = "Generated/Decals/Decal.glslh",
        .SourceHeader = "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Decals/DecalLayout.hpp",
        .Structs      = kDecalStructs,
    };
} // namespace Desert::Graphic::System
