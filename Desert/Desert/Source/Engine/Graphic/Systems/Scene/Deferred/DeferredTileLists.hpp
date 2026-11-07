#pragma once

#include <Engine/Graphic/RDG/RDGResources.hpp>

#include <glm/glm.hpp>

#include <cstdint>

namespace Desert::Graphic::System
{
    // GPU-TILE-CLASSIFY (UE Substrate ClassificationTiles): THE VIEW'S ONE TILE LIST. "Deferred: Tile Classify"
    // (DeferredTileClassify.shader, async compute, right after the G-buffer) fills it once per frame
    // (SceneRenderer::AddFrameTileClassify, FrameTransients::DeferredTiles); every screen pass that writes only
    // geometry texels reads it instead of covering the view: the deferred lighting's two passes (DeferredLighting
    // Renderer, a draw per list) and SSAO (SSAORenderer, a dispatch over every listed tile). Invalid Buffer:
    // RenderPipeline.TileClassify 0, a G-buffer view, or no classify pipeline - every pass covers the view.
    // The layout is Common/DeferredTiles.glslh's; these are its numbers, spelled once for C++.
    struct DeferredTileLists
    {
        RDG::BufferRef Buffer;
        glm::uvec2     Tiles{ 0u };  // the tile grid "Deferred: Tile Classify" is dispatched over
        glm::uvec2     Extent{ 0u }; // the classified view, in pixels: a pass at another extent covers its view

        static constexpr uint32_t kHeaderWords = 20u;
        static constexpr uint32_t kTileSize    = 8u;
        static constexpr uint32_t kVertices    = 6u; // per listed tile, in either draw record
        // Byte offsets of the four indirect records.
        static constexpr uint64_t kSimpleDrawOffset       = 0u * sizeof( uint32_t );
        static constexpr uint64_t kComplexDrawOffset      = 4u * sizeof( uint32_t );
        static constexpr uint64_t kGeometryDispatchOffset = 12u * sizeof( uint32_t );
        static constexpr uint64_t kComplexDispatchOffset  = 16u * sizeof( uint32_t );

        // True when a pass of @p width x @p height may cover only the listed tiles.
        [[nodiscard]] bool Covers( const uint32_t width, const uint32_t height ) const noexcept
        {
            return Buffer.IsValid() && Extent.x == width && Extent.y == height;
        }
    };
} // namespace Desert::Graphic::System
