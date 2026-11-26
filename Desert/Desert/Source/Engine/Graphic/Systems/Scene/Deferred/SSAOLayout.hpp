#pragma once
// THE GTAO PASS'S PARAMETER BLOCK, declared once: Tools/ShaderProtocolGen generates
// Generated/Post/SSAOUBData.glslh (`struct SSAOUBData` + DESERT_SSAOUBData_MEMBERS) from it, and
// Programs/Deferred/SSAO.shader declares its SSAOUB (std140) with those members. Layout only:
// SSAOUniforms::Make (SSAOUniforms.hpp) builds it once per frame.
#include <Engine/Graphic/ShaderProtocols/ShaderLayout.hpp>
#include <Engine/Graphic/ShaderProtocols/ShaderProtocol.hpp>

#include <glm/glm.hpp>


// X( C++ type, member, "GLSL name", initialiser )
#define DESERT_SSAO_UB_FIELDS( X )                                                                                   \
    X( glm::mat4, InvJitteredViewProjection, "InvJitteredViewProjection", glm::mat4( 1.0f ) ) /* world position from the G-buffer depth */ \
    X( glm::mat4, WorldToView, "WorldToView", glm::mat4( 1.0f ) ) /* the camera's View matrix (right-handed, -z forward) */ \
    X( glm::vec4, Target, "Target", glm::vec4( 0.0f ) ) /* xy = AO target size (px), zw = 1 / size */                \
    X( glm::vec4, Horizon, "Horizon", glm::vec4( 0.0f ) ) /* x = world radius (cm), y = thickness blend, z = taps per direction, w = px per cm at depth 1 */ \
    X( glm::vec4, Temporal, "Temporal", glm::vec4( 0.0f ) ) /* xy = cos/sin of the slice rotation, z = step offset, w = slice angles */

namespace Desert::Graphic::System
{
    DESERT_SHADER_STRUCT( SSAOUBData, "SSAOUBData", Std140, DESERT_SSAO_UB_FIELDS );

    inline constexpr const ShaderProtocols::ShaderStructDecl* kSSAOUBDataStructs[] = { &kSSAOUBDataStruct };
    inline constexpr ShaderProtocols::ShaderIncludeDecl kSSAOUBDataShaderInclude{
         .Path         = "Generated/Post/SSAOUBData.glslh",
         .SourceHeader = "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Deferred/SSAOLayout.hpp",
         .Structs      = kSSAOUBDataStructs };
} // namespace Desert::Graphic::System
