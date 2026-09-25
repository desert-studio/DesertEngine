#pragma once

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
     * @brief A tile's weight layers as the one RGBA8 texture UE packs a component's weightmap into: texel i,
     * channel c = WeightLayers()[c].Weights[i], 0 for a channel the tile has no layer in. Row-major, X
     * fastest, SamplesX * SamplesZ texels — the upload layout, so the bytes ARE the image.
     */
    inline std::vector<uint8_t> LandscapeWeightmapTexels( const LandscapeTileData& tile )
    {
        const std::vector<LandscapeWeightLayer>& layers = tile.WeightLayers();
        const size_t                             count  = static_cast<size_t>( tile.SamplesX() ) * tile.SamplesZ();
        std::vector<uint8_t>                     texels( count * 4u, 0u );
        for ( size_t c = 0; c < layers.size() && c < kLandscapeMaxWeightLayers; ++c )
            for ( size_t i = 0; i < count && i < layers[c].Weights.size(); ++i )
                texels[i * 4u + c] = layers[c].Weights[i];
        return texels;
    }

    /// What the surface shader needs per channel of a tile's weightmap (LandscapeWeights.glslh).
    struct LandscapeWeightChannels
    {
        /// rgb = the root layer's colour, a = 1; vec4(0) for a channel the root does not name (not drawn).
        std::array<glm::vec4, kLandscapeMaxWeightLayers> Colors{};
        /// 1 per channel whose root layer is NoWeightBlend (UE's LB_AlphaBlend).
        glm::vec4 AlphaBlend{ 0.0f };
        /// The tile's layer count (0 = no weightmap; the shader then keeps the rule-only path).
        uint32_t Count = 0u;
        /// Tile layers the root no longer names: kept on the tile (LandscapeWeightLayer), reported by the caller.
        std::vector<std::string> Unknown;
    };

    /**
     * @brief Resolves each channel of @p tile against the root's layer list by NAME — the key a tile layer
     * carries (UE's ULandscapeLayerInfoObject), so reordering the root never re-colours a tile.
     * @p RootLayer is ECS::LandscapeLayerInfo (Name, NoWeightBlend, Color); a template so this stays ECS-free.
     */
    template <typename RootLayers>
    LandscapeWeightChannels ResolveLandscapeWeightChannels( const LandscapeTileData& tile, const RootLayers& root )
    {
        LandscapeWeightChannels                  out;
        const std::vector<LandscapeWeightLayer>& layers = tile.WeightLayers();
        out.Count = static_cast<uint32_t>( std::min<size_t>( layers.size(), kLandscapeMaxWeightLayers ) );
        for ( uint32_t c = 0; c < out.Count; ++c )
        {
            const auto channel = static_cast<glm::length_t>( c ); // glm indexes vectors with a signed int
            bool       named   = false;
            for ( const auto& info : root )
            {
                if ( info.Name != layers[c].Name )
                    continue;
                out.Colors[c]           = glm::vec4( info.Color, 1.0f );
                out.AlphaBlend[channel] = info.NoWeightBlend ? 1.0f : 0.0f;
                named                   = true;
                break;
            }
            if ( !named )
                out.Unknown.push_back( layers[c].Name );
        }
        return out;
    }
} // namespace Desert::World::Landscape
