#pragma once

// THE FALLING RAIN'S BLOCK, declared once: Tools/ShaderProtocolGen writes its GLSL into
// Generated/Weather/RainStreaks.glslh and checks the std140 layout against this declaration at compile time.
// Programs/Weather/RainStreaks.shader owns the slot in both stages: `Uniform(0) RainStreaksUB
// { DESERT_RainStreaksUniforms_MEMBERS };`.

#include <Engine/Graphic/ShaderProtocols/ShaderLayout.hpp>
#include <Engine/Graphic/ShaderProtocols/ShaderProtocol.hpp>

#include <glm/glm.hpp>

namespace Desert::Graphic::System
{
    // What RainStreakRenderer writes into RainStreaksUB each frame.
#define DESERT_RAIN_STREAKS_UNIFORMS_FIELDS( X )                                                                  \
    X( glm::mat4, ViewProjection, "RainViewProjection", glm::mat4( 1.0f ) ) /* the view's jittered view-proj */   \
    X( glm::vec4, Camera, "RainCamera", glm::vec4( 0.0f ) ) /* xyz camera (cm), w = scene time (s) */             \
    /* xz = SceneWind directional velocity (cm/s), y = 0, w = PreExposure */                                      \
    X( glm::vec4, Wind, "RainWind", glm::vec4( 0.0f ) )                                                           \
    /* SceneWind gust fronts (ViewFrame::WindGust, Common/WindGust.glslh) */                                      \
    X( glm::vec4, Gust, "RainGust", glm::vec4( 0.0f ) )                                                           \
    X( glm::vec4, Light, "RainLight", glm::vec4( 0.0f ) ) /* x = the drop's radiance (RainDropShape.z) */
    DESERT_SHADER_STRUCT( RainStreaksUniforms, "RainStreaksUniforms", Std140,
                          DESERT_RAIN_STREAKS_UNIFORMS_FIELDS );
#undef DESERT_RAIN_STREAKS_UNIFORMS_FIELDS

    inline constexpr const ShaderProtocols::ShaderStructDecl* kRainStreaksStructs[] = {
         &kRainStreaksUniformsStruct };
    inline constexpr ShaderProtocols::ShaderIncludeDecl kRainStreaksShaderInclude{
         .Path         = "Generated/Weather/RainStreaks.glslh",
         .SourceHeader = "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Weather/WeatherLayout.hpp",
         .Structs      = kRainStreaksStructs,
    };
} // namespace Desert::Graphic::System
