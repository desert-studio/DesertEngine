#pragma once

#include <Engine/Graphic/ShaderProtocols/Std140Block.hpp>

#include <glm/glm.hpp>

#include <array>
#include <string>
#include <string_view>
#include <vector>

// THE SCENE DIRECTIONAL LIGHT AS THE FORWARD PASSES READ IT, listed ONCE: the members expand into
// DirectionLightPayload and into kDirectionLightPayloadFields, from which the includer GENERATES
// Common/DirectionLightsUB.glslh (struct DirectionLight + the DirectionLightsUB block at binding 3; no file on
// disk). Engine-filled by reflection name (Graphic::SceneFrameBinding::ApplyTo -> SceneLightsBind).
//
// FIELD( C++ type, member, GLSL name, initialiser )
#define DESERT_DIRECTION_LIGHT_FIELDS( FIELD )                                                                    \
    FIELD( glm::vec4, Direction, Direction, {} ) /* offset  0: xyz = normalized direction (from light) */         \
    FIELD( glm::vec4, ColorIntensity, ColorIntensity, {} ) /* offset 16: rgb = color, a = intensity */

namespace Desert::Graphic::ShaderProtocols
{
    /// DirectionLightPayload and kDirectionLightPayloadFields (GLSL `struct DirectionLight`), Std140Block.hpp.
    DESERT_SHADER_STRUCT( DirectionLightPayload, DESERT_DIRECTION_LIGHT_FIELDS )
    static_assert( Std140LayoutMatches( kDirectionLightPayloadFields, sizeof( DirectionLightPayload ) ),
                   "DirectionLightPayload is not laid out as std140 places struct DirectionLight" );
    static_assert( sizeof( DirectionLightPayload ) == 32 );

    struct DirectionLight
    {
        inline const static std::string Name = "DirectionLightsUB";

        std::vector<DirectionLightPayload> DirectionLights;
    };

    /// The include path the shader includer answers with the generated struct + block.
    inline constexpr std::string_view kDirectionLightsUBInclude = "Common/DirectionLightsUB.glslh";

    /// The GLSL of the directional-light block: ONE light (the scene's main one) — generated once per process.
    [[nodiscard]] inline const std::string& DirectionLightsUBGlsl()
    {
        static const std::string glsl = GenerateGuardedGlsl(
             "DESERT_DIRECTION_LIGHTS_UB_GLSLH", "Engine/Graphic/ShaderProtocols/DirectionLight.hpp",
             GenerateGlslStruct( "DirectionLight", kDirectionLightPayloadFields ) + "\n" +
                  GenerateGlslBlock( "Uniform", 3, DirectionLight::Name, "    DirectionLight directionLights;\n",
                                     "directionLights" ) );
        return glsl;
    }

} // namespace Desert::Graphic::ShaderProtocols
