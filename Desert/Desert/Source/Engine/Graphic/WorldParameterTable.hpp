#pragma once

#include <Engine/Core/WorldParameterInstance.hpp>

#include <glm/glm.hpp>

#include <bit>
#include <cstdint>
#include <span>
#include <vector>

namespace Desert::Graphic
{
    // The C++ twin of Common/WorldParameters.glslh: a 16-byte header, then `capacity` 32-byte slots (std430).
    struct GpuWorldParameterHeader
    {
        uint32_t Capacity = 1;
        uint32_t Count    = 0;
        uint32_t Pad0     = 0;
        uint32_t Pad1     = 0;
    };
    struct GpuWorldParameterSlot
    {
        uint32_t  Key  = 0; // 0 = empty
        uint32_t  Pad0 = 0;
        uint32_t  Pad1 = 0;
        uint32_t  Pad2 = 0;
        glm::vec4 Value{ 0.0f };
    };
    static_assert( sizeof( GpuWorldParameterHeader ) == 16 && sizeof( GpuWorldParameterSlot ) == 32,
                   "the std430 layout of Common/WorldParameters.glslh" );

    // Common/WeatherSnow.glslh reads the snow layer by these keys, written there as literals (GLSL has no
    // strings).
    static_assert( Assets::Serialization::WorldParameterKey( "SnowCover" ) == 0x0b8bf081u &&
                        Assets::Serialization::WorldParameterKey( "SnowAlbedo" ) == 0xc0678c8bu &&
                        Assets::Serialization::WorldParameterKey( "SnowNormal" ) == 0x253367e1u &&
                        Assets::Serialization::WorldParameterKey( "SnowArm" ) == 0xc2d545c4u &&
                        Assets::Serialization::WorldParameterKey( "SnowTileSize" ) == 0x2fe67b17u,
                   "the snow layer's keys in Common/WeatherSnow.glslh" );

    struct WorldParameterTable
    {
        GpuWorldParameterHeader            Header;
        std::vector<GpuWorldParameterSlot> Slots;
    };

    /// The open-addressed table a shader probes by key (linear probing from key & (capacity - 1)). Capacity is
    /// the smallest power of two at least twice the count, so a probe ends at an empty slot within a few steps;
    /// an empty collection is one empty slot, which ends every probe at once. Keys are unique per collection
    /// (Serialization::ValidateWorldParametersData), so placement needs no collision rule beyond probing.
    [[nodiscard]] inline WorldParameterTable
    BuildWorldParameterTable( const std::span<const Core::WorldParameterValue> values )
    {
        WorldParameterTable table;
        const auto          count = static_cast<uint32_t>( values.size() );
        table.Header.Capacity     = std::bit_ceil( std::max<uint32_t>( 1u, count * 2u ) );
        table.Header.Count        = count;
        table.Slots.resize( table.Header.Capacity );
        const uint32_t mask = table.Header.Capacity - 1u;
        for ( const auto& value : values )
        {
            uint32_t slot = value.Key & mask;
            while ( table.Slots[slot].Key != 0u )
                slot = ( slot + 1u ) & mask;
            table.Slots[slot].Key   = value.Key;
            table.Slots[slot].Value = value.Value;
        }
        return table;
    }
} // namespace Desert::Graphic
