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
    // `TerrainInstance` in Terrain.shader are two statements of one layout; every field is a mat4 or
    // vec4, so the std430 offsets are trivially 16-byte multiples (the rule MaterialParamRow.hpp applies
    // to parameter rows for the same reason: a layout too simple to get wrong instead of one checked).
    struct TerrainInstance
    {
        glm::mat4 Model{ 1.0f };
        glm::vec4 Params{ 0.0f };     // x = size, y = gridDim, z = heightScale, w = tessLevel
        glm::vec4 Params2{ 0.0f };    // x = noiseFrequency, y = seed, z/w = spare
        // x = grass, y = rock, z = snow (0=Auto,1=Manual,2=Off). w is std430 padding, not a field: a
        // vec3 here would still occupy 16 bytes and a glm::vec3 member would occupy 12, which is how a
        // C++/GLSL mirror silently shears. .w carried the grass ENABLE flag until Г25 cut the generator.
        glm::vec4 LayerModes{ 0.0f };
    };

    static_assert( sizeof( TerrainInstance ) == 7 * sizeof( glm::vec4 ),
                   "TerrainInstance must stay seven 16-byte slots - the GLSL mirror in Terrain.shader "
                   "reads these offsets" );
    static_assert( offsetof( TerrainInstance, Params ) == 64 && offsetof( TerrainInstance, Params2 ) == 80 &&
                        offsetof( TerrainInstance, LayerModes ) == 96,
                   "TerrainInstance fields moved - the GLSL mirror in Terrain.shader no longer agrees" );

    // The texture half of a terrain material's identity. Two terrains may share one material only if
    // they bind the same textures: the sorted texture-override handles plus the address of the painted
    // splat map (runtime-owned, no asset handle — same spelling MeshRenderer uses for the font atlas).
    // Sorted, because two terrains naming the same textures in a different order are the same texture
    // set and must share one material rather than allocate two.
    inline std::string TerrainTextureKey( const MaterialOverrides& overrides, const void* splatMap )
    {
        std::vector<std::string> parts;
        parts.reserve( overrides.Textures.size() + 1 );
        for ( const auto& [name, handle] : overrides.Textures )
            if ( handle != 0 )
                parts.push_back( name + "=" + std::to_string( handle ) );
        if ( splatMap )
            parts.push_back( "u_SplatMap=@" + std::to_string( reinterpret_cast<uintptr_t>( splatMap ) ) );
        std::sort( parts.begin(), parts.end() );

        std::string key;
        for ( const auto& part : parts )
            key += "|" + part;
        return key;
    }
} // namespace Desert::Graphic::System
