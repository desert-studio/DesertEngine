#pragma once

// FOLIAGE FILED BY WORLD PARTITION CELL (FO-6).
//
// UE's pattern: a partitioned world keeps its foliage not in one AInstancedFoliageActor per level but in one
// per grid cell (APartitionActor, UActorPartitionSubsystem), each an external actor streamed with its cell;
// the brush writes an instance into the actor of the cell it lands in, and moving an instance over a cell
// edge moves it into the neighbour's actor (AInstancedFoliageActor::MoveInstancesToNewIFA). Ours: a foliage
// field (FoliageComponent + InstancedStaticMeshComponent) per TYPE per CELL. Without it one field holds the
// type's instances over the whole map, its footprint is the whole map, and the partitioner has to promote
// the field to a cell big enough to hold it — the grown cell of WP1 is then every field of grass.
//
// This file is the pure part: which cell an instance belongs to, and how a set of fields is merged into one
// buffer (so the brush sees every instance under it whatever field holds it) and written back cell by cell.
// Which ENTITIES those fields are is the editor's (FoliagePaintTool); nothing here knows the Scene.
//
// THE INVARIANT: every instance of a field lies in the field's cell, and so does the field entity's own
// position (FoliageCellAnchor) — the partitioner's footprint of a record is its position plus its instance
// points (WorldPartitionRules.hpp, AppendFootprint), so both must be in the cell for the record to be.

#include <Engine/Core/Serialize/WorldPartitionRules.hpp>

#include <Common/Core/ResultStr.hpp>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace Desert::World::Foliage
{
    using CellCoord = Core::Rules::CellCoord;

    // One field as the partition sees it: the cell it is filed under, its instances (world matrices, as
    // InstancedStaticMeshComponent::InstanceTransforms) and which of them are selected (ascending indices,
    // Tools::FoliageSelection).
    struct FoliageCellField
    {
        CellCoord                  Cell;
        std::vector<glm::mat4>     Instances;
        std::vector<std::uint32_t> Selected;
    };

    // Several fields of one type as one buffer, in the order they were gathered.
    struct FoliageGathered
    {
        std::vector<glm::mat4>     Instances;
        std::vector<std::uint32_t> Selected;
    };

    // The level-0 cell an instance is filed under: the cell of its ground position (the translation's X and
    // Z), the same Rules::CellOf the partitioner files the instance point by.
    [[nodiscard]] CellCoord FoliageCellOf( const glm::mat4& instance, double cellSize );

    // Where a cell's field entity stands: the cell's centre at height 0. Inside the cell, so the field's own
    // position never widens its footprint past the cell.
    [[nodiscard]] glm::vec3 FoliageCellAnchor( const CellCoord& cell, double cellSize );

    // Whether a brush disc (centre x/z, radius, cm) reaches into the cell's square — the fields the brush has
    // to see are exactly those.
    [[nodiscard]] bool FoliageCellTouchesDisc( const CellCoord& cell, double cellSize, float x, float z,
                                               float radius );

    // The fields' instances end to end, selections renumbered. An error names the field and index when a
    // selection points past its field's instances.
    [[nodiscard]] Common::ResultStr<FoliageGathered> GatherFoliage( std::span<const FoliageCellField> fields );

    // Every instance of @p gathered filed under its cell. The result holds one field per entry of @p cells, in
    // that order (a cell whose instances all left comes back empty — the entity stays and undo can refill it),
    // then one per cell not in @p cells that received instances, ordered by X then Z. Instances keep their
    // relative order, so a field nothing moved in or out of comes back identical; the selection travels with
    // its instance. An error when the cell size is not a positive number, a cell is listed twice, or a
    // selected index is past the instances.
    [[nodiscard]] Common::ResultStr<std::vector<FoliageCellField>>
    ScatterFoliage( const FoliageGathered& gathered, std::span<const CellCoord> cells, double cellSize );
} // namespace Desert::World::Foliage
