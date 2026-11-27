#pragma once

#include <Engine/Graphic/WorldParameterLayout.hpp>
#include <Engine/Core/WorldParameterInstance.hpp>

#include <glm/glm.hpp>

#include <bit>
#include <cstdint>
#include <span>
#include <vector>

namespace Desert::Graphic
{
    // Common/SurfaceLayer.glslh reads the world's surface layer by this key, written there as a literal (GLSL has
    // no strings); the layer's own values are read by keys the graph compiler writes into
    // Generated/SurfaceLayers.glslh.
    static_assert( Assets::Serialization::WorldParameterKey( "SurfaceLayer" ) == 0xf2f26379u,
                   "the surface layer's key in Common/SurfaceLayer.glslh" );

    // The rain's values (Common/WeatherRain.glslh: the film, puddles, ripples, streaks on every lit surface and
    // the landscape), written there as literals for the same reason.
    static_assert( Assets::Serialization::WorldParameterKey( "Wetness" ) == 0x60957b78u &&
                        Assets::Serialization::WorldParameterKey( "PuddleLevel" ) == 0xfc667a91u &&
                        Assets::Serialization::WorldParameterKey( "PuddleShape" ) == 0x46b78b8au &&
                        Assets::Serialization::WorldParameterKey( "RainIntensity" ) == 0x22384490u &&
                        Assets::Serialization::WorldParameterKey( "RainRipple" ) == 0x67bc9dc9u &&
                        Assets::Serialization::WorldParameterKey( "RainRippleNormal" ) == 0x9836b6b6u &&
                        Assets::Serialization::WorldParameterKey( "RainStreak" ) == 0x30a7fbadu &&
                        Assets::Serialization::WorldParameterKey( "RainStreakNormal" ) == 0x23bc16dau,
                   "the rain's keys in Common/WeatherRain.glslh" );
    static_assert( Assets::Serialization::WorldParameterKey( "RainDrops" ) == 0xe5bb4d7bu &&
                        Assets::Serialization::WorldParameterKey( "RainDropShape" ) == 0x71b62487u,
                   "the falling rain's keys in Programs/Weather/RainStreaks.shader" );

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
