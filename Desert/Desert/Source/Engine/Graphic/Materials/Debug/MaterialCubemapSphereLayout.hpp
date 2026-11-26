#pragma once

// THE CUBEMAP PREVIEW BALL'S BLOCK, declared once: Tools/ShaderProtocolGen writes its GLSL into
// Generated/Preview/CubemapSphere.glslh and checks the std140 layout against this declaration at compile time.
// Programs/Preview/CubemapSphere.shader owns the slot in both stages: `Uniform(0) CubemapSphereUB
// { DESERT_CubemapSphereGpu_MEMBERS } u;`.

#include <Engine/Graphic/ShaderProtocols/ShaderLayout.hpp>
#include <Engine/Graphic/ShaderProtocols/ShaderProtocol.hpp>

#include <glm/glm.hpp>

namespace Desert::Graphic
{
    /// What MaterialCubemapSphere writes into CubemapSphereUB each frame.
#define DESERT_CUBEMAP_SPHERE_GPU_FIELDS( X )                                                                     \
    X( glm::mat4, Projection, "Projection", glm::mat4( 1.0f ) )                                                   \
    X( glm::mat4, View, "View", glm::mat4( 1.0f ) )                                                               \
    X( glm::mat4, InvProjection, "InvProjection", glm::mat4( 1.0f ) )                                             \
    X( glm::mat4, InvView, "InvView", glm::mat4( 1.0f ) )                                                         \
    X( glm::vec4, CameraPos, "CameraPos", glm::vec4( 0.0f ) ) /* xyz; w unused */                                 \
    /* x = sphere radius (world units); y > 0.5 = the cube is the backdrop too; z = lod; w > 0.5 = long-lat */    \
    X( glm::vec4, Params, "Params", glm::vec4( 0.0f ) )
    DESERT_SHADER_STRUCT( CubemapSphereGpu, "CubemapSphereGpu", Std140, DESERT_CUBEMAP_SPHERE_GPU_FIELDS );
#undef DESERT_CUBEMAP_SPHERE_GPU_FIELDS

    inline constexpr const ShaderProtocols::ShaderStructDecl* kCubemapSphereStructs[] = {
         &kCubemapSphereGpuStruct };
    inline constexpr ShaderProtocols::ShaderIncludeDecl kCubemapSphereShaderInclude{
         .Path         = "Generated/Preview/CubemapSphere.glslh",
         .SourceHeader = "Desert/Desert/Source/Engine/Graphic/Materials/Debug/MaterialCubemapSphereLayout.hpp",
         .Structs      = kCubemapSphereStructs,
    };
} // namespace Desert::Graphic
