#pragma once

// Ported from UE 5.8 Engine/Source/Runtime/Foliage/{Public,Private}/ProceduralFoliageSpawner.{h,cpp} and the tile
// stitching of ProceduralFoliageComponent.cpp (GetTileRegion, GetTileLayout, GenerateProceduralContent). Adapted:
// the types are FoliageTypeData values (their Procedural block, RandomYaw and RandomPitchAngle drive the
// simulation) rather than UFoliageType objects; tiles are simulated in order on the calling thread (no editor
// progress or cancel); RAND_MAX is UE's Windows value on every platform; a composite tile's neighbours are copied
// with per-axis reach (see ProceduralFoliageTile::CopyInstancesToTile) and the top neighbours over the same
// [0, Overlap) strip as the right one (UE uses [-Overlap, Overlap) for the top ones); the result is the placed
// instances on the ground plane, before any trace against the world.

#include <Engine/Assets/Serialization/FoliageType.hpp>
#include <Engine/World/Foliage/Procedural/ProceduralFoliageTile.hpp>

#include <Common/Core/ResultStr.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace Desert::World::Foliage::Procedural
{
    /// UE UProceduralFoliageSpawner's own numbers, with its defaults.
    struct ProceduralFoliageSpawnerSettings
    {
        /// The side of one simulated tile, cm (UE default 100 m).
        float TileSize = 10000.0f;
        /// The smallest quadtree cell, cm.
        float MinimumQuadTreeSize = 100.0f;
        /// How many different tiles are simulated; the world repeats them in a hashed pattern.
        int32_t NumUniqueTiles = 10;
        int32_t RandomSeed     = 42;
    };

    /**
     * @brief Simulates NumUniqueTiles tiles once, then hands one to every tile coordinate of the world (UE
     *        UProceduralFoliageSpawner).
     */
    class ProceduralFoliageSpawner
    {
    public:
        ProceduralFoliageSpawner( ProceduralFoliageSpawnerSettings                    settings,
                                  std::vector<Assets::Serialization::FoliageTypeData> types );

        /// Rejects settings the simulation cannot honour, naming the field.
        [[nodiscard]] Common::BoolResultStr Validate() const;

        /// Simulates the unique tiles (UE Simulate). @p numSteps < 0 runs every type's full NumSteps.
        void Simulate( int32_t numSteps = -1 );

        /// The simulated tile standing at tile coordinate (x, y); nullptr before Simulate (UE GetRandomTile).
        [[nodiscard]] const ProceduralFoliageTile* GetRandomTile( int32_t x, int32_t y ) const;

        [[nodiscard]] const ProceduralFoliageSpawnerSettings& Settings() const
        {
            return m_Settings;
        }
        [[nodiscard]] std::span<const Assets::Serialization::FoliageTypeData> Types() const
        {
            return m_Types;
        }

    private:
        ProceduralFoliageSpawnerSettings                    m_Settings;
        std::vector<Assets::Serialization::FoliageTypeData> m_Types;
        std::vector<std::unique_ptr<ProceduralFoliageTile>> m_Tiles;
    };

    /// Which tiles cover a region (UE FTileLayout): tile (BottomLeftX + i, BottomLeftY + j) for i < NumTilesX,
    /// j < NumTilesY.
    struct ProceduralFoliageTileLayout
    {
        int32_t BottomLeftX = 0;
        int32_t BottomLeftY = 0;
        int32_t NumTilesX   = 0;
        int32_t NumTilesY   = 0;
    };

    /// The tiles that cover [@p min, @p max] on the ground plane once @p tileOverlap is taken off each side (UE
    /// GetTileLayout).
    [[nodiscard]] ProceduralFoliageTileLayout TileLayoutFor( glm::vec2 min, glm::vec2 max, float tileSize,
                                                             float tileOverlap );

    /// One instance the procedural foliage places, on the world's ground plane.
    struct ProceduralFoliagePlacement
    {
        /// World (X, Z), cm.
        glm::vec2 Location{ 0.0f };
        float     YawDegrees   = 0.0f;
        float     PitchDegrees = 0.0f;
        float     Scale        = 1.0f;
        float     Age          = 0.0f;
        /// Index into the spawner's types.
        uint32_t TypeIndex = 0;

        [[nodiscard]] bool operator==( const ProceduralFoliagePlacement& ) const = default;
    };

    /**
     * @brief Every instance the simulated spawner places over @p layout, with tile (BottomLeftX, BottomLeftY)'s
     *        corner at @p origin (UE GenerateProceduralContent).
     *
     * Each tile is composed with the overlap strips of its right, top and top-right neighbours, so instances on a
     * seam compete across it and are placed once. Seamless when @p tileOverlap is at least twice the largest grown
     * radius of any type; 0 leaves the tiles independent (UE's default, with seams).
     */
    [[nodiscard]] std::vector<ProceduralFoliagePlacement>
    GenerateProceduralContent( const ProceduralFoliageSpawner& spawner, const ProceduralFoliageTileLayout& layout,
                               glm::vec2 origin, float tileOverlap );
} // namespace Desert::World::Foliage::Procedural
