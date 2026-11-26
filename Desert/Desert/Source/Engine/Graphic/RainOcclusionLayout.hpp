#pragma once

// THE RAIN OCCLUSION BLOCK, declared once: Tools/ShaderProtocolGen writes its GLSL into
// Generated/Weather/RainOcclusion.glslh and checks the std140 layout against this declaration at compile time.
// Common/RainOcclusion.glslh owns the slot (Core::kRainOcclusionBlockBinding of the set-0 scene table) and
// declares `RainOcclusionUB { DESERT_RainOcclusionUniforms_MEMBERS };`.

#include <Engine/Graphic/ShaderProtocols/ShaderLayout.hpp>
#include <Engine/Graphic/ShaderProtocols/ShaderProtocol.hpp>

#include <glm/glm.hpp>

#include <array>

namespace Desert::Graphic
{
    // WHERE THE WATER LAST REACHED THE GROUND: the view's foam map (Water/WaterFoamField.glslh), whose B is the
    // sand the swash wetted. Shape.y = 0: no map.
    using Vec4ArrayShoreFoamOrigin = std::array<glm::vec4, 3>;
#define DESERT_SHORE_FOAM_UNIFORMS_FIELDS( X )                                                                    \
    /* xy = the level's lower grid texel, z = 1 / texel (1/cm) */                                                 \
    X( Vec4ArrayShoreFoamOrigin, Origin, "Origin", {} )                                                           \
    /* x = a level's side (texels), y = 1 with a map, z = its bindless slot */                                    \
    X( glm::vec4, Shape, "Shape", glm::vec4( 0.0f ) )
    DESERT_SHADER_STRUCT( ShoreFoamUniforms, "ShoreFoamUniforms", Std140, DESERT_SHORE_FOAM_UNIFORMS_FIELDS );
#undef DESERT_SHORE_FOAM_UNIFORMS_FIELDS

    // RainOcclusionUB: the view's rain occlusion map and the shore foam the wetted ground reads beside it.
#define DESERT_RAIN_OCCLUSION_UNIFORMS_FIELDS( X )                                                                \
    /* world -> the map's standard-Z clip (uv = (x, -y) * 0.5 + 0.5, z = depth) */                                \
    X( glm::mat4, WorldToMap, "RainOcclusionWorldToMap", glm::mat4( 1.0f ) )                                      \
    /* x = depth range (cm), y = valid (>0.5), z = bias (cm), w = texel (cm) */                                   \
    X( glm::vec4, Params, "RainOcclusionParams", glm::vec4( 0.0f ) )                                              \
    X( ShoreFoamUniforms, Shore, "ShoreFoam", {} )
    DESERT_SHADER_STRUCT( RainOcclusionUniforms, "RainOcclusionUniforms", Std140,
                          DESERT_RAIN_OCCLUSION_UNIFORMS_FIELDS );
#undef DESERT_RAIN_OCCLUSION_UNIFORMS_FIELDS

    inline constexpr const ShaderProtocols::ShaderStructDecl* kRainOcclusionStructs[] = {
         &kShoreFoamUniformsStruct, &kRainOcclusionUniformsStruct };
    inline constexpr ShaderProtocols::ShaderIncludeDecl kRainOcclusionShaderInclude{
         .Path         = "Generated/Weather/RainOcclusion.glslh",
         .SourceHeader = "Desert/Desert/Source/Engine/Graphic/RainOcclusionLayout.hpp",
         .Structs      = kRainOcclusionStructs,
    };
} // namespace Desert::Graphic
