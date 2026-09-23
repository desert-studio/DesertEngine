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

namespace Desert::ECS
{
    /**
     * @brief Draws every loaded landscape tile, and owns the tiles' GPU copies.
     *
     * The tile's uint16 samples (LandscapeTileComponent::Heights) are the one source of the landscape's
     * heights (analysis A2); the GPU holds an R16_UNORM COPY of them, one image per tile, made here:
     *   - uploaded when the tile first appears, and again whenever it reports dirty rectangles (a new tile
     *     is wholly dirty, LandscapeData.hpp) or changes size;
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
            std::shared_ptr<Graphic::Image2D> Heightmap;
            uint32_t                          SamplesX = 0u;
            uint32_t                          SamplesZ = 0u;
        };

        std::unordered_map<entt::entity, TileGpu> m_Tiles;
        // Said once per tile, not once per frame: a tile that cannot be drawn stays that way until edited.
        std::unordered_set<entt::entity> m_Warned;
    };
} // namespace Desert::ECS
