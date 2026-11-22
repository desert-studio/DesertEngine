#pragma once

// How many paint layers a landscape tile carries and how they pack into weightmap pages: what a consumer
// that lays out GPU tables per layer (TerrainBatch, TerrainLayerInputs) names WITHOUT the tile data
// (LandscapeData.hpp owns LandscapeTileData).

#include <cstddef>
#include <cstdint>

namespace Desert::World::Landscape
{
    /// How many weightmap layers one tile carries — owner decision O3 of the landscape programme (up to eight
    /// paint layers). A ninth is refused by name (AddWeightLayer, SetWeightLayers, DecodeLandscapeTile), never
    /// dropped.
    inline constexpr uint32_t kLandscapeMaxWeightLayers = 8u;

    /// Layers per weightmap texture: UE packs a component's layers four to an RGBA8 texture and adds a
    /// texture as a fifth layer is allocated (FWeightmapLayerAllocationInfo::WeightmapTextureIndex/Channel).
    /// Layer i of a tile lives in page i / 4, channel i % 4 (LandscapeWeightmapTexels).
    inline constexpr uint32_t kLandscapeWeightmapChannels = 4u;

    /// Weightmap textures a tile of @p layers layers needs: 0 for none, 1 for 1..4, 2 for 5..8.
    constexpr uint32_t LandscapeWeightmapPageCount( size_t layers )
    {
        return static_cast<uint32_t>( ( layers + kLandscapeWeightmapChannels - 1u ) /
                                      kLandscapeWeightmapChannels );
    }
} // namespace Desert::World::Landscape
