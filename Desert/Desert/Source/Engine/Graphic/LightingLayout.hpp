#pragma once

// THE LIGHTING ROWS THE SHADERS READ: the light grid's parameters (LightGrid.hpp), a local shadow view
// (LocalShadows.hpp) and a light function atlas row (Systems/Scene/Lights/LightFunctionAtlas.hpp). Each is declared
// once here; the GLSL structs and the LIGHT_GRID_* constants are generated at build time from it (the
// k*ShaderInclude below) into Generated/Lighting/, which Mesh/LightGrid.glslh, Mesh/LocalLightShadowView.glslh and
// Common/LightFunction.glslh include.

#include <Engine/Graphic/GlobalIllumination/RadianceCacheCeiling.hpp>
#include <Engine/Graphic/ShaderProtocols/ShaderLayout.hpp>
#include <Engine/Graphic/ShaderProtocols/ShaderProtocol.hpp>

#include <glm/glm.hpp>

#include <array>
#include <cstdint>

namespace Desert::Graphic
{
    // Which cells a grid lists its lights in (LIGHT_GRID_LAYOUT_* in Mesh/LightGrid.glslh).
    enum class LightGridLayout : uint32_t
    {
        Frustum       = 0,
        WorldClipmaps = 1,
    };

    // The light grid's parameters (one per grid, a uniform of every lit pass that reads the grid).
    using Vec4ArrayMaxClipmaps = std::array<glm::vec4, RadianceCache::kMaxClipmaps>;
#define DESERT_LIGHT_GRID_PARAMS_PAYLOAD_FIELDS( X ) \
    /* world -> the space cells are boxed in (frustum: view; world: identity) */ \
    X( glm::mat4, View, "view", glm::mat4( 1.0f ) ) \
    /* frustum: clip -> view, of the jittered projection; world: unused */ \
    X( glm::mat4, InvProjection, "invProjection", glm::mat4( 1.0f ) ) \
    /* frustum: tiles x, tiles y, slices, list capacity per cell; world: resolution, resolution, resolution x \
       clipmaps, pool capacity (indices) */ \
    X( glm::uvec4, Size, "size", glm::uvec4( 1u ) ) \
    /* frustum: near, slices / log(far / near), far, tile edge in pixels */ \
    X( glm::vec4, ZParams, "zParams", glm::vec4( 0.0f ) ) \
    /* point lights, spot lights, layout (LightGridLayout), world: clipmaps */ \
    X( glm::uvec4, Counts, "counts", glm::uvec4( 0u ) ) \
    X( glm::vec4, Extent, "extent", glm::vec4( 0.0f ) ) /* frustum: render target width, height in pixels */ \
    /* world: clipmap i's min corner (xyz, world cm) and cell size (w, cm) — RadianceCache::MakeClipmaps */ \
    X( Vec4ArrayMaxClipmaps, Clipmaps, "clipmaps", {} )
    DESERT_SHADER_STRUCT( LightGridParamsPayload, "LightGridParams", Std430,
                          DESERT_LIGHT_GRID_PARAMS_PAYLOAD_FIELDS );
#undef DESERT_LIGHT_GRID_PARAMS_PAYLOAD_FIELDS

    // ONE ROW OF THE VIEW TABLE THE RECEIVER READS (Mesh/LocalLightShadow.glslh). No depth bias here: the casters
    // carry it (LocalCasterBias), the receiver only its normal offset.
#define DESERT_LOCAL_SHADOW_VIEW_PAYLOAD_FIELDS( X ) \
    X( glm::mat4, ViewProj, "viewProj", glm::mat4( 1.0f ) ) /* world -> clip of this view (standard Z, [0,1]) */ \
    /* xy = the tile's top-left in atlas UV, zw = its size in atlas UV */ \
    X( glm::vec4, AtlasRect, "atlasRect", glm::vec4( 0.0f ) ) \
    /* x = near, y = far, z = world size of one texel at unit distance, w = unused */ \
    X( glm::vec4, Params, "params", glm::vec4( 0.0f ) ) \
    X( glm::vec4, Params2, "params2", glm::vec4( 0.0f ) ) /* x = unused, y = 1 / atlas side, zw = unused */
    DESERT_SHADER_STRUCT( LocalShadowViewPayload, "LocalShadowView", Std430,
                          DESERT_LOCAL_SHADOW_VIEW_PAYLOAD_FIELDS );
#undef DESERT_LOCAL_SHADOW_VIEW_PAYLOAD_FIELDS

    inline constexpr const ShaderProtocols::ShaderStructDecl* kLightGridStructs[] = { &kLightGridParamsPayloadStruct };
    inline constexpr ShaderProtocols::ShaderConstantDecl kLightGridConstants[] = {
        { ShaderProtocols::ShaderConstantDecl::Type::Uint, "LIGHT_GRID_LAYOUT_FRUSTUM",
          static_cast<uint32_t>( LightGridLayout::Frustum ) },
        { ShaderProtocols::ShaderConstantDecl::Type::Uint, "LIGHT_GRID_LAYOUT_WORLD",
          static_cast<uint32_t>( LightGridLayout::WorldClipmaps ) },
        { ShaderProtocols::ShaderConstantDecl::Type::Int, "LIGHT_GRID_MAX_CLIPMAPS", RadianceCache::kMaxClipmaps },
    };
    inline constexpr ShaderProtocols::ShaderIncludeDecl kLightGridShaderInclude{
        .Path         = "Generated/Lighting/LightGrid.glslh",
        .SourceHeader = "Desert/Desert/Source/Engine/Graphic/LightingLayout.hpp",
        .Structs      = kLightGridStructs,
        .Constants    = kLightGridConstants,
    };

    inline constexpr const ShaderProtocols::ShaderStructDecl* kLocalShadowViewStructs[] = {
        &kLocalShadowViewPayloadStruct
    };
    inline constexpr ShaderProtocols::ShaderIncludeDecl kLocalShadowViewShaderInclude{
        .Path         = "Generated/Lighting/LocalShadowView.glslh",
        .SourceHeader = "Desert/Desert/Source/Engine/Graphic/LightingLayout.hpp",
        .Structs      = kLocalShadowViewStructs,
    };
} // namespace Desert::Graphic

namespace Desert::Graphic::System
{
    // One row of "LightFunctions.Rows" (Common/LightFunction.glslh).
#define DESERT_LIGHT_FUNCTION_ROW_GPU_FIELDS( X ) \
    X( glm::vec4, Forward, "Forward", {} ) /* w kind: 0 spot, 1 point / rect */ \
    X( glm::vec4, Tangent, "Tangent", {} ) /* w tan(outer cone) for a spot */ \
    X( glm::vec4, Bitangent, "Bitangent", {} ) /* cross( forward, tangent ) */ \
    X( glm::vec4, Position, "Position", {} ) /* w range (cm) */ \
    X( glm::vec4, InvScale, "InvScale", {} ) /* xyz 1 / LightFunctionScale, w scene time (s) */ \
    X( glm::vec4, TileUV, "TileUV", {} ) /* the tile in atlas texture coordinates: x0, y0, x1, y1 */
    DESERT_SHADER_STRUCT( LightFunctionRowGpu, "LightFunctionRow", Std430, DESERT_LIGHT_FUNCTION_ROW_GPU_FIELDS );
#undef DESERT_LIGHT_FUNCTION_ROW_GPU_FIELDS

    inline constexpr const ShaderProtocols::ShaderStructDecl* kLightFunctionStructs[] = { &kLightFunctionRowGpuStruct };
    inline constexpr ShaderProtocols::ShaderIncludeDecl kLightFunctionShaderInclude{
        .Path         = "Generated/Lighting/LightFunction.glslh",
        .SourceHeader = "Desert/Desert/Source/Engine/Graphic/LightingLayout.hpp",
        .Structs      = kLightFunctionStructs,
    };
} // namespace Desert::Graphic::System
