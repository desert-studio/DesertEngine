#pragma once

#include <Common/Core/Core.hpp>
#include <Engine/World/Landscape/LandscapeData.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace Desert::World::Landscape
{
    /**
     * @brief Page @p page of a tile's weightmap: the RGBA8 texture UE packs four of a component's layers into
     * (FWeightmapLayerAllocationInfo: layer i sits in texture i / 4, channel i % 4). Texel t, channel c =
     * WeightLayers()[page * 4 + c].Weights[t], 0 for a channel past the tile's last layer. Row-major, X
     * fastest, SamplesX * SamplesZ texels — the upload layout, so the bytes ARE the image. A page at or past
     * LandscapeWeightmapPageCount is a caller defect and is asserted.
     */
    inline std::vector<uint8_t> LandscapeWeightmapTexels( const LandscapeTileData& tile, uint32_t page )
    {
        const std::vector<LandscapeWeightLayer>& layers = tile.WeightLayers();
        DESERT_VERIFY( page < LandscapeWeightmapPageCount( layers.size() ),
                       "weightmap page {} of a tile with {} layers", page, layers.size() );
        const size_t         count = static_cast<size_t>( tile.SamplesX() ) * tile.SamplesZ();
        const size_t         first = static_cast<size_t>( page ) * kLandscapeWeightmapChannels;
        std::vector<uint8_t> texels( count * kLandscapeWeightmapChannels, 0u );
        for ( size_t c = 0; c < kLandscapeWeightmapChannels && first + c < layers.size(); ++c )
            for ( size_t i = 0; i < count && i < layers[first + c].Weights.size(); ++i )
                texels[i * kLandscapeWeightmapChannels + c] = layers[first + c].Weights[i];
        return texels;
    }

    /// What the surface shader needs per channel of a tile's weightmap (LandscapeWeights.glslh).
    struct LandscapeWeightChannels
    {
        /// Per tile layer, in the tile's order (page i / 4, channel i % 4 of LandscapeWeightmapTexels):
        /// rgb = the root layer's colour, a = 1; vec4(0) for a layer the root does not name (not drawn).
        std::array<glm::vec4, kLandscapeMaxWeightLayers> Colors{};
        /// 1 per tile layer whose root layer is NoWeightBlend (UE's LB_AlphaBlend).
        std::array<float, kLandscapeMaxWeightLayers> AlphaBlend{};
        /// The tile's layer count (0 = no weightmap; the shader then keeps the rule-only path).
        uint32_t Count = 0u;
        /// Tile layers the root no longer names: kept on the tile (LandscapeWeightLayer), reported by the caller.
        std::vector<std::string> Unknown;
    };

    /**
     * @brief Resolves each channel of @p tile against the root's layer list by NAME — the key a tile layer
     * carries (UE's ULandscapeLayerInfoObject), so reordering the root never re-colours a tile.
     * @p RootLayer is Assets::Serialization::LandscapeLayerInfoData (LayerName, NoWeightBlend,
     * LayerUsageDebugColor); a template so this stays asset-free.
     */
    template <typename RootLayers>
    LandscapeWeightChannels ResolveLandscapeWeightChannels( const LandscapeTileData& tile, const RootLayers& root )
    {
        LandscapeWeightChannels                  out;
        const std::vector<LandscapeWeightLayer>& layers = tile.WeightLayers();
        out.Count = static_cast<uint32_t>( std::min<size_t>( layers.size(), kLandscapeMaxWeightLayers ) );
        for ( uint32_t c = 0; c < out.Count; ++c )
        {
            bool named = false;
            for ( const auto& info : root )
            {
                if ( info.LayerName != layers[c].Name )
                    continue;
                out.Colors[c]           = glm::vec4( info.LayerUsageDebugColor, 1.0f );
                out.AlphaBlend[c]       = info.NoWeightBlend ? 1.0f : 0.0f;
                named                   = true;
                break;
            }
            if ( !named )
                out.Unknown.push_back( layers[c].Name );
        }
        return out;
    }
} // namespace Desert::World::Landscape
