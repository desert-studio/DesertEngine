#pragma once

// A ray against the landscape's surface, answered from the CPU heights alone (no GPU, no physics world).
//
// THE SURFACE is the landscape's one surface: two planar triangles per cell on Jolt's (x, z)-(x+1, z+1)
// diagonal (LandscapeHeight.glslh, LandscapeTriangle) — what the terrain shader draws, what
// SampleLandscapeHeight answers and what the heightfield body collides with, so a click lands where a
// body would rest. Along a ray each triangle's height is linear in the ray parameter, so the ray is cut
// at the grid lines and at each cell's diagonal and every piece is one linear equation, solved in double.

#include <Engine/World/Landscape/LandscapeData.hpp>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <cstddef>
#include <optional>
#include <span>

namespace Desert::World::Landscape
{
    /// One tile the ray may hit: its heights and where they sit. The heights are borrowed, not owned.
    struct LandscapeRayTile
    {
        const LandscapeTileData* Heights = nullptr;
        LandscapeFrame           Frame;
    };

    struct LandscapeRayHit
    {
        float     Distance = 0.0f;                          ///< Along the ray, in units of |direction|.
        glm::vec3 Point    = glm::vec3( 0.0f );             ///< World, centimetres.
        glm::vec3 Normal   = glm::vec3( 0.0f, 1.0f, 0.0f ); ///< The hit triangle's own (face) normal.
        size_t    Tile     = 0u;                            ///< Index into the span that was searched.
        glm::vec2 Uv       = glm::vec2( 0.0f );             ///< Across that tile, [0, 1] edge to edge.
    };

    /**
     * @brief The nearest point where the ray meets any of @p tiles, or nullopt.
     *
     * Every tile covers its rectangle with BOTH edges inclusive (neighbours share their edge samples), so a
     * ray that lands exactly on a seam or a corner is answered by every tile that owns it, with the same
     * point; the nearest wins. A ray that passes beyond every tile's rectangle, or above every surface
     * within @p maxDistance, misses. A ray that starts below the surface hits it from underneath at the
     * first crossing — picking from inside a hill is still a pick. A tile with no samples is skipped.
     */
    std::optional<LandscapeRayHit> RaycastLandscape( std::span<const LandscapeRayTile> tiles,
                                                     const glm::vec3& origin, const glm::vec3& direction,
                                                     float maxDistance );
} // namespace Desert::World::Landscape
