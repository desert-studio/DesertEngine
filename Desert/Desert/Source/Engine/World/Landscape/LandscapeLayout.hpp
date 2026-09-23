#pragma once

#include <Engine/World/Landscape/LandscapeData.hpp>

#include <Common/Core/ResultStr.hpp>

#include <glm/vec3.hpp>

#include <cstdint>

namespace Desert::World::Landscape
{
    /**
     * @file
     * @brief How a landscape is cut into tiles and where each tile lies — the half of the landscape that
     *        LandscapeData.hpp deliberately does not know.
     *
     * THE SHAPE, AS UE HAS IT (landscape analysis, decision A1). UE's ALandscape owns the frame — its actor
     * transform carries the origin and the X/Y/Z scale — and every ALandscapeStreamingProxy holds a square
     * of components at a fixed offset from it. Here the ROOT is an entity with a LandscapeComponent (the
     * frame: its own world position, the sample spacing, the vertical scale and the tile size) and each
     * TILE is an entity with a LandscapeTileComponent (which square it is, and the file its heights live
     * in). The tile names its root by UUID, as an OBSERVATION and not as a child — a parent link would
     * make the whole landscape one composite for WorldPartition, and one cell would grow to hold all of
     * it (WorldPartitionRules.hpp, "THE UNIT IS THE COMPOSITE").
     *
     * WHAT IS NOT STORED ANYWHERE, ON PURPOSE:
     *   * how many tiles there are — that is the number of tile entities naming the root; a count on the
     *     root would be a second answer that can disagree with the first;
     *   * where a tile is in the world — it is derived from its coordinate and the root's frame, so moving
     *     the root moves every tile and no tile can drift from its neighbours;
     *   * how big a tile's blob is — the root's QuadsPerTile says it, and a blob that disagrees is refused
     *     at load by CheckTileMatchesRoot rather than drawn at another size.
     *
     * Pure: no entities, no scene, no renderer, no files — the partitioner computes a tile's rectangle
     * with these functions, and it must stay a pure function of the parsed records. The tile FILES are in
     * LandscapeTileFiles.hpp.
     */

    /// Quads along one side of a tile. UE:Runtime/Landscape/Private/LandscapeConfigHelper.cpp:25
    ///     FLandscapeConfig::SubsectionSizeQuadsValues[6] = { 7, 15, 31, 63, 127, 255 };
    /// One tile is one UE component of one section. The list is UE's and not "any size": each value is
    /// 2^n - 1 quads, i.e. 2^n samples less the shared edge, which is what gives a tile a full mip chain
    /// once it is a texture (LS-4). Any other size is refused by ValidateLandscapeRoot.
    inline constexpr uint32_t kLandscapeTileQuadsValues[] = { 7u, 15u, 31u, 63u, 127u, 255u };

    /// What UE's New Landscape tool proposes: 63 quads a section.
    inline constexpr uint32_t kLandscapeDefaultTileQuads = 63u;

    /**
     * @brief The root's frame, as the partitioner and the loader both need it.
     *
     * `Origin` is the root entity's WORLD position: sample (0, 0) of tile (0, 0) sits there. The other three
     * are the LandscapeComponent's fields, one for one.
     */
    struct LandscapeRoot
    {
        glm::vec3 Origin       = glm::vec3( 0.0f );
        uint32_t  QuadsPerTile = kLandscapeDefaultTileQuads;
        float     SpacingCm    = kLandscapeDefaultSpacingCm;
        float     ZScale       = kLandscapeDefaultZScale;
    };

    /// Refuses a root the tiling cannot honour: a tile size UE does not offer, or a spacing / vertical scale
    /// that is not positive and finite. Names the number.
    Common::BoolResultStr ValidateLandscapeRoot( const LandscapeRoot& root );

    /// Samples along one side of every tile of @p root: QuadsPerTile + 1, the last row shared with the
    /// neighbour (LandscapeData.hpp, SampleLandscapeHeight).
    inline uint32_t LandscapeTileSamples( const LandscapeRoot& root )
    {
        return root.QuadsPerTile + 1u;
    }

    /// Width of one tile in centimetres: QuadsPerTile · SpacingCm.
    inline float LandscapeTileExtentCm( const LandscapeRoot& root )
    {
        return static_cast<float>( root.QuadsPerTile ) * root.SpacingCm;
    }

    /// Where tile (@p tileX, @p tileZ)'s sample grid sits: the frame LandscapeData's sampling functions take.
    /// Negative coordinates are legal — a landscape extends either side of its root, as UE's section
    /// coordinates do.
    LandscapeFrame LandscapeTileFrame( const LandscapeRoot& root, int32_t tileX, int32_t tileZ );

    /// A rectangle on the ground, centimetres. Both edges inclusive: a tile's last row is its neighbour's first.
    struct LandscapeTileRect
    {
        float MinX = 0.0f;
        float MinZ = 0.0f;
        float MaxX = 0.0f;
        float MaxZ = 0.0f;
    };

    /// The ground rectangle tile (@p tileX, @p tileZ) covers. This is what WorldPartition places, and it is
    /// XZ only because the partition is: height does not partition (WorldPartitionRules.hpp, CellCoord).
    LandscapeTileRect LandscapeTileBounds( const LandscapeRoot& root, int32_t tileX, int32_t tileZ );

    /// Refuses a tile whose sample grid is not the size its root says every tile is, naming both. A tile of
    /// another size is not a smaller terrain, it is a seam — the shared edge row would be sampled from two
    /// different spacings.
    Common::BoolResultStr CheckTileMatchesRoot( const LandscapeTileData& tile, const LandscapeRoot& root );
} // namespace Desert::World::Landscape
