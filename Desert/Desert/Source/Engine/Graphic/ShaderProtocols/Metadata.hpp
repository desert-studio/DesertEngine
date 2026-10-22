#pragma once

#include <Engine/Graphic/RDG/RDGShaderParameters.hpp>
#include <Engine/Graphic/ShaderProtocols/Std140Block.hpp>

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

// THE LIGHT COUNTS, listed ONCE: the members expand into LightsMetadata and into kLightsMetadataFields, from which
// the includer GENERATES Mesh/LightsMetadata.glslh (the LightsMetadata uniform block at binding 4; no file on
// disk). The explicit tail pad makes the C++ object the std140 block's full 16 bytes, uploaded as one instance.
//
// FIELD( C++ type, member, GLSL name, initialiser )
#define DESERT_LIGHTS_METADATA_FIELDS( FIELD )                                                                    \
    FIELD( uint32_t, DirectionLightsCount, DirectionLightCount, { 0U } ) /* offset 0 */                           \
    FIELD( uint32_t, PointLightsCount, PointLightCount, { 0U } )         /* offset 4 */                           \
    FIELD( uint32_t, SpotLightsCount, SpotLightCount, { 0U } )           /* offset 8 */                           \
    FIELD( uint32_t, LightsMetadataPad, LightsMetadataPad, { 0U } )      /* offset 12; block size 16 */

namespace Desert::Graphic::ShaderProtocols
{
    /// LightsMetadata and kLightsMetadataFields (the block's std140 fields), Std140Block.hpp.
    DESERT_SHADER_STRUCT( LightsMetadata, DESERT_LIGHTS_METADATA_FIELDS,
                          static constexpr RDG::UniformBufferParameter Parameter{ "LightsMetadata" }; inline const static std::string Name = std::string( Parameter.Name ); )
    // `Name` is static, so the object is exactly the block — uploaded as one instance.
    static_assert( Std140LayoutMatches( kLightsMetadataFields, sizeof( LightsMetadata ) ),
                   "LightsMetadata is not laid out as std140 places the generated LightsMetadata block" );
    static_assert( sizeof( LightsMetadata ) == 16 );

    /// The include path the shader includer answers with the generated block.
    inline constexpr std::string_view kLightsMetadataInclude = "Mesh/LightsMetadata.glslh";

    [[nodiscard]] inline const std::string& LightsMetadataGlsl()
    {
        static const std::string glsl = GenerateUniformBlockGlsl(
             "DESERT_LIGHTS_METADATA_GLSLH", "Engine/Graphic/ShaderProtocols/Metadata.hpp", 4,
             LightsMetadata::Name, "lightsMetadata", kLightsMetadataFields );
        return glsl;
    }
} // namespace Desert::Graphic::ShaderProtocols
