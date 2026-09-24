#pragma once

#include <Engine/Graphic/Materials/MaterialOverrides.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
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
    //   - PER-DRAW DATA (Model, sizes, heights, seed, layer modes) rides as a row of the
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
        glm::mat4 Model{ 1.0f };
        glm::vec4 Params{ 0.0f }; // x = tile extent (cm), y = gridDim, z = height range (cm), w = tessLevel
        // w = the tile's LandscapeNeighbourMask (which sides have their ring row in the heightmap). x/y/z are
        // std430 padding: they held the procedural terrain's frequency, seed and height-source switch until
        // v23 baked that terrain into landscapes (LS-6), and the slot stays so the layout below stays.
        glm::vec4 Params2{ 0.0f };
        // x = grass, y = rock, z = snow (ECS::LandscapeLayerMode: 0=Auto, 1=Off). w is std430 padding: a
        // vec3 here would still occupy 16 bytes and a glm::vec3 member would occupy 12, which is how a
        // C++/GLSL mirror silently shears. .w carried the grass ENABLE flag until Г25 cut the generator.
        glm::vec4 LayerModes{ 0.0f };
        // Landscape tiles only (height source = heightmap), see LandscapeTileDraw: x/z = the ROOT's origin,
        // y = its base height, w = spacing (cm between samples).
        glm::vec4 LandscapeFrame{ 0.0f };
        // x/y = this tile's first sample in the landscape's global sample grid, z = quads per tile side,
        // w = vertical scale (cm per local height unit).
        glm::vec4 LandscapeTile{ 0.0f };
    };

    static_assert( sizeof( TerrainInstance ) == 9 * sizeof( glm::vec4 ),
                   "TerrainInstance must stay nine 16-byte slots - the GLSL mirror in TerrainInstance.glslh "
                   "reads these offsets" );
    static_assert( offsetof( TerrainInstance, Params ) == 64 && offsetof( TerrainInstance, Params2 ) == 80 &&
                        offsetof( TerrainInstance, LayerModes ) == 96 &&
                        offsetof( TerrainInstance, LandscapeFrame ) == 112 &&
                        offsetof( TerrainInstance, LandscapeTile ) == 128,
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

    // The most heightmap quads one tessellated patch spans. A patch's near tessellation level is its own
    // quads per side (TerrainRenderer, LandscapeInstance), so a near patch places a vertex on every sample
    // and draws the cells' own triangles; this bounds that level to what the GPU tessellates cheaply.
    inline constexpr uint32_t kLandscapeMaxQuadsPerPatch = 16u;

    // Patches per tile side: the fewest that divide the tile's quads evenly with at most
    // kLandscapeMaxQuadsPerPatch quads each. EVENLY, because a patch corner must sit on a sample — a corner
    // between samples would be an edge the neighbouring tile does not have. 63 quads -> 7 patches of 9;
    // a prime count degrades to one quad per patch, which is still exact, only more patches.
    inline uint32_t LandscapePatchesPerSide( uint32_t quadsPerTile )
    {
        for ( uint32_t patches = 1u; patches <= quadsPerTile; ++patches )
            if ( quadsPerTile % patches == 0u && quadsPerTile / patches <= kLandscapeMaxQuadsPerPatch )
                return patches;
        return quadsPerTile; // quadsPerTile == 0: no patches, and no draw
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
