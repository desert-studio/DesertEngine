#pragma once

#include "../RenderCommand.hpp"
// The renderer itself: every Execute below calls a method on it, and RenderCommand.hpp only
// forward-declares the type (see the note there on the include cycle that cost).
#include <Engine/Graphic/SceneRenderer.hpp>
#include <Engine/Graphic/Systems/Scene/Terrain/TerrainBatch.hpp>

#include <utility>

namespace Desert::Graphic
{
    class Image2D;
}

namespace Desert::Graphic::Render
{
    // Carries one landscape tile from the ECS (LandscapeECSSystem) to the TerrainRenderer for this frame.
    // The heightmap is the system's GPU copy of the tile's samples, owned by that system's cache, which
    // outlives the frame through the allocator's per-frame deletion queue.
    struct DrawLandscapeTileCommand : RenderCommand
    {
        Image2D*                  Heightmap = nullptr;
        System::LandscapeTileDraw Tile;
        glm::vec3                 LayerModes;
        MaterialOverrides         Overrides;
        System::LandscapeWeightDraw Weights;

        DrawLandscapeTileCommand( Image2D* heightmap, const System::LandscapeTileDraw& tile,
                                  const glm::vec3& layerModes, MaterialOverrides overrides,
                                  const System::LandscapeWeightDraw& weights )
             : Heightmap( heightmap ), Tile( tile ), LayerModes( layerModes ), Overrides( std::move( overrides ) ),
               Weights( weights )
        {
        }

        void Execute( SceneRenderer& renderer ) override
        {
            renderer.SubmitLandscapeTile( Heightmap, Tile, LayerModes, Overrides, Weights );
        }
    };
} // namespace Desert::Graphic::Render
