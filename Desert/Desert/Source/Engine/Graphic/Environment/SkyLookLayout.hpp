#pragma once

// THE SKY LOOK BLOCK, declared once: Tools/ShaderProtocolGen writes its GLSL into
// Generated/Environment/SkyLook.glslh and checks the layout against this declaration at compile time. Every
// environment-sampling program owns its own slot of SkyLookUB (the forward and water surface passes, the deferred
// composite, the terrain, the skybox and the cubemap preview) and declares `SkyLookUB { DESERT_SkyLookGPU_MEMBERS
// } skyLook;`.

#include <Engine/Graphic/ShaderProtocols/ShaderLayout.hpp>
#include <Engine/Graphic/ShaderProtocols/ShaderProtocol.hpp>

#include <glm/glm.hpp>

namespace Desert::Graphic
{
    /// `SkyLookUB`: how a sky's cubes are read (two vec4s: std140 and std430 agree). Identity is (1, 0) and
    /// white — the value a procedural sky and "no sky" both bind, because a declared-but-unwritten uniform is
    /// whatever the last writer left there.
#define DESERT_SKY_LOOK_GPU_FIELDS( X )                                                                           \
    X( glm::vec4, YawCosSin, "YawCosSin", glm::vec4( 1.0f, 0.0f, 0.0f, 0.0f ) ) /* xy = (cos yaw, sin yaw) */     \
    X( glm::vec4, Gain, "Gain", glm::vec4( 1.0f ) )                             /* rgb = tint * intensity */
    DESERT_SHADER_STRUCT( SkyLookGPU, "SkyLookGPU", Both, DESERT_SKY_LOOK_GPU_FIELDS );
#undef DESERT_SKY_LOOK_GPU_FIELDS

    inline constexpr const ShaderProtocols::ShaderStructDecl* kSkyLookStructs[] = { &kSkyLookGPUStruct };
    inline constexpr ShaderProtocols::ShaderIncludeDecl       kSkyLookShaderInclude{
               .Path         = "Generated/Environment/SkyLook.glslh",
               .SourceHeader = "Desert/Desert/Source/Engine/Graphic/Environment/SkyLookLayout.hpp",
               .Structs      = kSkyLookStructs,
    };
} // namespace Desert::Graphic
