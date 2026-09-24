#pragma once

#include <Engine/Graphic/Materials/MaterialOverrides.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Desert::Graphic::System
{
    // ── The terrain batching contract ───────────────────────────────────────────────────────────────
    //
    // The terrain pass records EVERY draw of a frame before the GPU executes any of them, and a
    // material's descriptor sets are written at most once per frame BEFORE its first bind
    // (VulkanMaterialBackend's per-frame stamp — updating a set bound in a recording command buffer is
    // illegal without update-after-bind). So nothing that varies per terrain may travel through the
    // shared material's uniform block or its descriptors:
    //
    //   - PER-DRAW DATA (sizes, LODs, frame, layer modes) rides as a row of the
    //     `TerrainInstances[]` storage buffer below, named per draw by the same push-constant index
    //     that names the material param row. This is the transport the param fix already used
    //     (Engine/Core/Formats/MaterialParamRow.hpp); TerrainUB simply had not moved with it, and on a
    //     scene with two terrains the GPU read the LAST one's Model/Params for both.
    //
    //   - TEXTURES cannot ride in a row — a sampler is a descriptor and a descriptor set belongs to the
    //     material — so terrains wanting different textures need different materials, keyed by
    //     TerrainTextureKey below. Same rule, same key shape as MeshRenderer's GenericTextureKey.

    // One terrain's per-draw engine data, as the Terrain shader reads it. This struct and the GLSL
    // `TerrainInstance` in Shaders/Common/TerrainInstance.glslh are two statements of one layout; every
    // field is a mat4 or vec4, so the std430 offsets are trivially 16-byte multiples (the rule
    // MaterialParamRow.hpp applies to parameter rows for the same reason: a layout too simple to get wrong
    // instead of one checked).
    struct TerrainInstance
    {
        // x = tile extent (cm), y = the tile's continuous LOD (LandscapeLodFromScreenSize), z = height range
        // (cm), w = the LOD whose grid the tile is drawn with (floor of y; the vertex count is that grid's).
        glm::vec4 Params{ 0.0f };
        // x = 1 / LOD blend range (UE's InvLODBlendRange), y/z = std430 padding, w = the tile's
        // LandscapeNeighbourMask (which sides have their ring row in the heightmap).
        glm::vec4 Params2{ 0.0f };
        // x = grass, y = rock, z = snow (ECS::LandscapeLayerMode: 0=Auto, 1=Off). w is std430 padding: a
        // vec3 here would still occupy 16 bytes and a glm::vec3 member would occupy 12, which is how a
        // C++/GLSL mirror silently shears.
        glm::vec4 LayerModes{ 0.0f };
        // x/z = the ROOT's origin, y = its base height, w = spacing (cm between samples), see LandscapeTileDraw.
        glm::vec4 LandscapeFrame{ 0.0f };
        // x/y = this tile's first sample in the landscape's global sample grid, z = quads per tile side,
        // w = vertical scale (cm per local height unit).
        glm::vec4 LandscapeTile{ 0.0f };
        // The LOD a vertex on each border takes: max(tile, neighbour) for West, East, South, North.
        glm::vec4 LodEdges{ 0.0f };
        // ...and on each corner: the max of the four tiles sharing it, at (-x,-z), (+x,-z), (-x,+z), (+x,+z).
        glm::vec4 LodCorners{ 0.0f };
    };

    static_assert( sizeof( TerrainInstance ) == 7 * sizeof( glm::vec4 ),
                   "TerrainInstance must stay seven 16-byte slots - the GLSL mirror in TerrainInstance.glslh "
                   "reads these offsets" );
    static_assert( offsetof( TerrainInstance, Params2 ) == 16 && offsetof( TerrainInstance, LayerModes ) == 32 &&
                        offsetof( TerrainInstance, LandscapeFrame ) == 48 &&
                        offsetof( TerrainInstance, LandscapeTile ) == 64 &&
                        offsetof( TerrainInstance, LodEdges ) == 80 &&
                        offsetof( TerrainInstance, LodCorners ) == 96,
                   "TerrainInstance fields moved - the GLSL mirror in TerrainInstance.glslh no longer agrees" );

    // Where one landscape tile sits, in the form the seam needs. The ROOT's origin and the tile's first
    // GLOBAL sample, not the tile's own origin: a vertex on a shared edge is then the same integer sample
    // index seen from both tiles, turned into a position by the same operations on the same numbers, so
    // both tiles place it bit for bit identically. Per-tile origins would be two roundings of one point.
    struct LandscapeTileDraw
    {
        float    OriginX      = 0.0f;
        float    OriginZ      = 0.0f;
        float    BaseY        = 0.0f;
        float    SpacingCm    = 0.0f;
        float    ZScale       = 0.0f;
        int32_t  FirstSampleX = 0;
        int32_t  FirstSampleZ = 0;
        uint32_t QuadsPerTile = 0u;
        // World::Landscape::LandscapeNeighbourMask of the neighbours whose ring row the heightmap carries.
        uint32_t NeighbourMask = 0u;
    };

    // ── Landscape LOD ───────────────────────────────────────────────────────────────────────────────
    //
    // Ported from UE 5.8 Engine/Source/Runtime/Landscape/Private/LandscapeRender.cpp:575-589
    // (FLandscapeRenderSystem::ComputeLODFromScreenSize) and :1547-1593 (the per-LOD screen-size ratios of
    // FLandscapeComponentSceneProxy), and Engine/Source/Runtime/Engine/Public/SceneManagement.h
    // (ComputeBoundsScreenRadiusSquared), adapted: the UE proxy's LOD0ScreenSize / LOD0DistributionSetting /
    // LODDistributionSetting / LODBlendRange are UE's defaults as constants (no per-landscape override yet),
    // no r.StaticMeshLODDistanceScale, and MaxLOD is the last stride-2^L grid of the tile (LandscapeLod.glslh)
    // instead of the heightmap's last mip. The whole tile takes ONE continuous LOD, from the main camera,
    // for every pass — the cascades draw the camera's vertices, so a shadow cannot swim against its caster.
    inline constexpr float kLandscapeLod0ScreenSize   = 0.5f;
    inline constexpr float kLandscapeLod0Distribution = 1.25f;
    inline constexpr float kLandscapeLodDistribution  = 3.0f;
    inline constexpr float kLandscapeLodBlendRange    = 1.0f;

    // The coarsest grid LOD of a tile: its stride is the largest power of two below the tile's samples, so
    // the next grid (the morph target) is the tile's four corners.
    inline uint32_t LandscapeMaxLod( uint32_t quadsPerTile )
    {
        uint32_t lod = 0u;
        while ( ( 2u << lod ) < quadsPerTile + 1u )
            ++lod;
        return lod;
    }

    struct LandscapeLodSettings
    {
        float LOD0ScreenSizeSquared               = 0.0f;
        float LOD1ScreenSizeSquared               = 0.0f;
        float LODOnePlusDistributionScalarSquared = 0.0f;
        float LastLODScreenSizeSquared            = 0.0f;
        float LastLODIndex                        = 0.0f;
    };

    inline LandscapeLodSettings MakeLandscapeLodSettings( uint32_t quadsPerTile )
    {
        LandscapeLodSettings settings;
        const uint32_t       maxLod    = LandscapeMaxLod( quadsPerTile );
        float                divider   = std::max( kLandscapeLod0Distribution, 1.01f );
        float                ratio     = kLandscapeLod0ScreenSize;
        settings.LOD0ScreenSizeSquared = ratio * ratio;
        ratio /= divider;
        settings.LOD1ScreenSizeSquared               = ratio * ratio;
        divider                                      = std::max( kLandscapeLodDistribution, 1.01f );
        settings.LODOnePlusDistributionScalarSquared = divider * divider;
        float last                                   = settings.LOD0ScreenSizeSquared;
        for ( uint32_t lod = 1u; lod <= maxLod; ++lod )
        {
            last = ratio * ratio;
            ratio /= divider;
        }
        settings.LastLODScreenSizeSquared = last;
        settings.LastLODIndex             = static_cast<float>( maxLod );
        return settings;
    }

    inline float LandscapeLodFromScreenSize( const LandscapeLodSettings& settings, float screenSizeSquared )
    {
        if ( screenSizeSquared <= settings.LastLODScreenSizeSquared )
            return settings.LastLODIndex;
        if ( screenSizeSquared > settings.LOD1ScreenSizeSquared )
            return ( settings.LOD0ScreenSizeSquared -
                     std::min( screenSizeSquared, settings.LOD0ScreenSizeSquared ) ) /
                   ( settings.LOD0ScreenSizeSquared - settings.LOD1ScreenSizeSquared );
        // No longer a linear fraction (UE's words): log base (distribution^2) of the size ratio.
        return std::min( settings.LastLODIndex,
                         1.0f + std::log( settings.LOD1ScreenSizeSquared / screenSizeSquared ) /
                                     std::log( settings.LODOnePlusDistributionScalarSquared ) );
    }

    // The squared screen radius of a bounding sphere: a fraction of the screen, from the projection's focal
    // scale. abs(): a Vulkan projection flips Y, UE's does not.
    inline float LandscapeScreenRadiusSquared( const glm::vec3& center, float radius, const glm::vec3& viewOrigin,
                                               const glm::mat4& projection )
    {
        const glm::vec3 d      = center - viewOrigin;
        const float     distSq = glm::dot( d, d );
        const float     multiple =
             std::max( 0.5f * std::abs( projection[0][0] ), 0.5f * std::abs( projection[1][1] ) );
        return ( multiple * radius ) * ( multiple * radius ) / std::max( 1.0f, distSq );
    }

    // A tile's LOD and what its borders and corners take. `lodAt(dx, dz)` is the continuous LOD of the tile
    // at that offset from this one, or nullopt where there is none (the landscape's own edge: nothing to
    // agree with there, so the tile's own LOD). max() on both sides of a border is what makes it one value.
    struct LandscapeTileLod
    {
        float     Center = 0.0f;
        glm::vec4 Edges{ 0.0f };
        glm::vec4 Corners{ 0.0f };
    };

    template <class LodAt>
    LandscapeTileLod LandscapeTileLods( float center, LodAt&& lodAt )
    {
        const auto at = [&]( int dx, int dz )
        {
            const std::optional<float> lod = lodAt( dx, dz );
            return std::max( center, lod.value_or( center ) );
        };
        const auto corner = [&]( int dx, int dz )
        { return std::max( { at( dx, 0 ), at( 0, dz ), at( dx, dz ) } ); };
        LandscapeTileLod result;
        result.Center  = center;
        result.Edges   = glm::vec4( at( -1, 0 ), at( 1, 0 ), at( 0, -1 ), at( 0, 1 ) );
        result.Corners = glm::vec4( corner( -1, -1 ), corner( 1, -1 ), corner( -1, 1 ), corner( 1, 1 ) );
        return result;
    }

    // Vertices of a tile's draw at grid LOD `gridLod`: six per cell (LandscapeLod.glslh decodes them).
    inline uint32_t LandscapeLodVertexCount( uint32_t quadsPerTile, uint32_t gridLod )
    {
        const uint32_t stride = 1u << gridLod;
        const uint32_t cells  = ( quadsPerTile + stride - 1u ) / stride;
        return cells * cells * 6u;
    }

    // The texture half of a terrain material's identity. Two terrains may share one material only if
    // they bind the same textures: the sorted texture-override handles plus the address of the landscape
    // tile's heightmap (runtime-owned, no asset handle — same spelling MeshRenderer uses for the font
    // atlas). Every landscape tile is therefore its own material.
    // Sorted, because two terrains naming the same textures in a different order are the same texture
    // set and must share one material rather than allocate two.
    inline std::string TerrainTextureKey( const MaterialOverrides& overrides, const void* heightmap )
    {
        std::vector<std::string> parts;
        parts.reserve( overrides.Textures.size() + 1 );
        for ( const auto& [name, handle] : overrides.Textures )
            if ( handle != 0 )
                parts.push_back( name + "=" + std::to_string( handle ) );
        if ( heightmap )
            parts.push_back( "u_Heightmap=@" + std::to_string( reinterpret_cast<uintptr_t>( heightmap ) ) );
        std::sort( parts.begin(), parts.end() );

        std::string key;
        for ( const auto& part : parts )
            key += "|" + part;
        return key;
    }
} // namespace Desert::Graphic::System
