#pragma once

#include "System.hpp"

#include <entt/entt.hpp>

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace Desert::Graphic
{
    class Image2D;
}

namespace Desert::World::Landscape
{
    class LandscapeTileData;
}

namespace Desert::ECS
{
    struct LandscapeTileComponent;

    /**
     * @brief Draws every loaded landscape tile, and owns the tiles' GPU copies.
     *
     * The tile's uint16 samples (LandscapeTileComponent::Heights) are the one source of the landscape's
     * heights (analysis A2); the GPU holds an R16_UNORM COPY of them, one image per tile, made here:
     *   - uploaded when the tile first appears, and again whenever it reports dirty rectangles (a new tile
     *     is wholly dirty, LandscapeData.hpp) or changes size — and whenever a NEIGHBOUR is edited, appears
     *     or goes away, because the copy carries a one-sample ring of each neighbour's next row (the normal
     *     at a seam is a central difference through it, LandscapeBorderedSamples);
     *   - a re-upload makes a NEW image rather than writing into the old one, because the old one may be
     *     bound by a frame still in flight; dropping it hands it to the allocator's per-frame deletion
     *     queue, which is what makes the release safe;
     *   - released when the entity, its tile component or its loaded heights go away.
     * Nothing is created per frame: a tile that did not change is drawn with the image it already has.
     *
     * Not parallel: it takes the tile's dirty list (a write) and creates GPU images.
     */
    class LandscapeECSSystem : public System
    {
    public:
        using System::System;
        ~LandscapeECSSystem() override;

        void Update( entt::registry& registry, Graphic::Render::RenderCommandBuffer& renderCommandBuffer,
                     const Common::Timestep& ts ) override;

    private:
        struct TileGpu
        {
            // The R16 copy of the tile's samples plus the neighbour ring. Rewritten IN PLACE per edit and
            // recreated only when the sample count changes (LandscapeTileImage.hpp): its address keys the
            // tile's terrain material, so a new image per sculpt frame was a new material per frame (L8-leak).
            std::shared_ptr<Graphic::Image2D> Heightmap;
            uint32_t                          SamplesX = 0u;
            uint32_t                          SamplesZ = 0u;
            uint32_t                          NeighbourMask = 0u; // whose ring rows the copy carries
            // The RGBA8 copy of the tile's weight layers (LandscapeWeightmap.hpp); null while it has none.
            // Updated IN PLACE per stroke: its address keys the tile's terrain material (TerrainTextureKey),
            // so a new image per stroke would be a new material per stroke.
            std::shared_ptr<Graphic::Image2D> Weightmap;
            uint32_t                          WeightmapX = 0u;
            uint32_t                          WeightmapZ = 0u;
        };

        // Creates, rewrites in place, or drops the tile's weightmap copy to match its weight layers.
        void UpdateWeightmap( TileGpu& gpu, const World::Landscape::LandscapeTileData& heights,
                              entt::entity entity, const LandscapeTileComponent& tileComp, bool weightsDirty );

        std::unordered_map<entt::entity, TileGpu> m_Tiles;
        // Said once per tile, not once per frame: a tile that cannot be drawn stays that way until edited.
        std::unordered_set<entt::entity> m_Warned;
        // Weight-layer problems (a layer the root does not name, a weightmap that could not be created or
        // written), said once per tile until its weights are edited again.
        std::unordered_set<entt::entity> m_WarnedWeights;

        // Material handles already reported as unresolvable, so the warning is said once and not once a frame.
        std::unordered_set<uint64_t> m_WarnedMaterials;
    };
} // namespace Desert::ECS
