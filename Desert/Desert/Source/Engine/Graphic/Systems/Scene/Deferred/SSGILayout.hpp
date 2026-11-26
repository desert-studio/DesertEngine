#pragma once
// THE SSGI TRACE'S UNIFORMS, declared once: Tools/ShaderProtocolGen generates Generated/Post/SSGIUniforms.glslh
// (`struct SSGIUniforms` + DESERT_SSGIUniforms_MEMBERS) from it, and Programs/Deferred/SSGI.shader declares its
// SSGIUB (std140) with those members. Layout only: SSGIRenderer::AddPasses fills it from the ViewFrame and the
// scalability once per frame.
#include <Engine/Graphic/ShaderProtocols/ShaderLayout.hpp>
#include <Engine/Graphic/ShaderProtocols/ShaderProtocol.hpp>

#include <glm/glm.hpp>


// X( C++ type, member, "GLSL name", initialiser )
#define DESERT_SSGI_UNIFORMS_FIELDS( X )                                                                             \
    X( glm::mat4, InvJitteredViewProjection, "InvJitteredViewProjection", glm::mat4( 1.0f ) ) /* the inverse of the matrix the G-buffer depth was rasterised with */ \
    X( glm::mat4, JitteredViewProjection, "JitteredViewProjection", glm::mat4( 1.0f ) ) /* that matrix: rays are projected with it, in the depth's own space */ \
    X( glm::vec4, Trace, "Trace", glm::vec4( 0.0f ) ) /* x = rays, y = iterations, z = thickness (cm), w = seed */   \
    X( glm::vec4, History, "History", glm::vec4( 0.0f ) ) /* x = 1 when the scene colour history is last frame's, y = 1 / PrevPreExposure */

namespace Desert::Graphic::System
{
    DESERT_SHADER_STRUCT( SSGIUniforms, "SSGIUniforms", Std140, DESERT_SSGI_UNIFORMS_FIELDS );

    inline constexpr const ShaderProtocols::ShaderStructDecl* kSSGIUniformsStructs[] = { &kSSGIUniformsStruct };
    inline constexpr ShaderProtocols::ShaderIncludeDecl kSSGIUniformsShaderInclude{
         .Path         = "Generated/Post/SSGIUniforms.glslh",
         .SourceHeader = "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Deferred/SSGILayout.hpp",
         .Structs      = kSSGIUniformsStructs };
} // namespace Desert::Graphic::System
