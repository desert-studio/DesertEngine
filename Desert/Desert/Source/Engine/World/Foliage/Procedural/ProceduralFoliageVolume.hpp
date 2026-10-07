#pragma once

// Ported from UE 5.8 Engine/Source/Runtime/Foliage/Private/ProceduralFoliageComponent.cpp (GetBounds,
// GenerateProceduralContent, ExtractDesiredInstances via ProceduralFoliageTile.cpp, RemoveProceduralContent) and
// the procedural branch of FoliageEdMode's AddInstances. Adapted: the volume is an axis-aligned box (UE: the
// brush's bounds); the world is two callbacks (a segment trace and the type's placement rules), so this file is a
// pure function of its inputs and the Scene half lives with the editor's foliage tool; an instance is filed into
// the FO-6 cell field of its type (FoliageCells.hpp) instead of an IFA; ownership is per FIELD (a field holds the
// instances of one volume, one type, one cell — UE tags each instance with the component's ProceduralGuid).

#include <Engine/World/Foliage/FoliageCells.hpp>
#include <Engine/World/Foliage/Procedural/ProceduralFoliageSpawner.hpp>

#include <Common/Core/ResultStr.hpp>
#include <Common/Core/UUID.hpp>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace Desert::World::Foliage::Procedural
{
    /// The volume procedural foliage fills: world, cm, Y up.
    struct ProceduralFoliageBox
    {
        glm::vec3 Min{ 0.0f };
        glm::vec3 Max{ 0.0f };
    };

    /// UE FDesiredFoliageInstance: a simulated placement and the vertical segment through the volume that finds
    /// its ground (top to bottom).
    struct ProceduralFoliageDesired
    {
        ProceduralFoliagePlacement Placement;
        glm::vec3                  TraceStart{ 0.0f };
        glm::vec3                  TraceEnd{ 0.0f };
    };

    /**
     * @brief Every placement of the simulated @p spawner whose location lies in the volume's ground rectangle,
     *        with its trace (UE GenerateProceduralContent + ExtractDesiredInstances).
     *
     * The tiles are anchored to the world (tile (i, j) covers [i, i+1) x [j, j+1) TileSizes), so two volumes with
     * the same spawner grow the same forest where they meet.
     */
    [[nodiscard]] std::vector<ProceduralFoliageDesired>
    DesiredInstancesInVolume( const ProceduralFoliageSpawner& spawner, const ProceduralFoliageBox& volume,
                              float tileOverlap );

    struct ProceduralFoliageGround
    {
        glm::vec3 Point{ 0.0f };
        glm::vec3 Normal{ 0.0f, 1.0f, 0.0f };
    };

    /// The nearest allowed surface on [start, end], or nullopt (the editor: Scene::Raycast through the volume's
    /// surface filter).
    using ProceduralFoliageTrace =
         std::function<std::optional<ProceduralFoliageGround>( const glm::vec3& start, const glm::vec3& end )>;
    /// The instance a placement becomes on @p ground, or nullopt when the type's rules refuse the spot (the
    /// editor: the type's height, slope, layer, Z offset and normal alignment).
    using ProceduralFoliagePlace = std::function<std::optional<glm::mat4>( const ProceduralFoliagePlacement&,
                                                                           const ProceduralFoliageGround& )>;

    /// One field the volume writes: the instances of one type in one cell.
    struct ProceduralFoliageTypeField
    {
        uint32_t               TypeIndex = 0;
        CellCoord              Cell;
        std::vector<glm::mat4> Instances;
    };

    /**
     * @brief Traces and places every desired instance, filed by type then cell (cell {0, 0} for every instance
     *        when @p cellSize is nullopt — a world that is not partitioned keeps one field per type).
     *
     * Fields come ordered by type index, then cell X, then Z; instances keep the order of @p desired. An error
     * when a placement names a type index at or past @p typeCount or the cell size is not positive.
     */
    [[nodiscard]] Common::ResultStr<std::vector<ProceduralFoliageTypeField>>
    PlaceProceduralFoliage( std::span<const ProceduralFoliageDesired> desired, uint32_t typeCount,
                            const ProceduralFoliageTrace& trace, const ProceduralFoliagePlace& place,
                            std::optional<double> cellSize );

    /// A field already in the world, as resimulation sees it: who owns it (Null = painted by hand), its type (an
    /// index into the volume's types; UINT32_MAX = a type the volume no longer lists) and its cell.
    struct ProceduralFoliageExistingField
    {
        Common::UUID Owner     = Common::UUID::Null();
        uint32_t     TypeIndex = UINT32_MAX;
        CellCoord    Cell;
    };

    /// What resimulation does to the world's fields (UE RemoveProceduralContent + AddInstances, per field).
    struct ProceduralFoliageFieldPlan
    {
        /// Indices into the existing fields to delete: the owner's fields nothing fresh lands in.
        std::vector<size_t> Remove;
        /// (existing index, fresh index): an owned field of the same type and cell takes the fresh instances.
        std::vector<std::pair<size_t, size_t>> Rewrite;
        /// Fresh indices that need a new field.
        std::vector<size_t> Create;
    };

    /// Replaces @p owner's fields with @p fresh and leaves every other field (painted, or another volume's)
    /// untouched. A fresh field reuses the owner's field of its type and cell, so a resimulation that changes
    /// nothing keeps every field's identity.
    [[nodiscard]] ProceduralFoliageFieldPlan
    PlanProceduralFields( std::span<const ProceduralFoliageExistingField> existing, const Common::UUID& owner,
                          std::span<const ProceduralFoliageTypeField> fresh );
} // namespace Desert::World::Foliage::Procedural
