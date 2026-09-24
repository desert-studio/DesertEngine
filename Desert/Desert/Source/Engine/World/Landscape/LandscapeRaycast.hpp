#pragma once

// A ray against the landscape's surface, answered from the CPU heights alone (no GPU, no physics world).
//
// THE SURFACE IS BILINEAR, per cell — the one SampleLandscapeHeight answers and the terrain shader
// evaluates at every tessellated vertex (LandscapeHeight.glslh, LandscapeBilinear). The intersection is
// exact for that surface: along a ray the bilinear height is a quadratic in the ray parameter, so each
// cell is one quadratic, solved in double.
//
// It is NOT the surface Jolt collides with. Jolt's heightfield is two planar triangles per cell, split on
// the (x, z)-(x+1, z+1) diagonal (HeightFieldShape.cpp, GetTriangleVertices). The two agree on every
// sample and along every grid line (bilinear is linear there); inside a cell they differ by at most a
// quarter of the cell's twist |h00 - h10 - h01 + h11|. Picking follows what is drawn; physics follows
// Jolt. LandscapeCollision.hpp states the same bound from the other side.

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
        glm::vec3 Normal   = glm::vec3( 0.0f, 1.0f, 0.0f ); ///< The bilinear surface's own normal there.
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
