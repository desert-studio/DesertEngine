#pragma once

// WHICH CELLS OF A PARTITIONED WORLD SHOULD BE RESIDENT FOR A SET OF STREAMING SOURCES, AND IN WHAT ORDER.
//
// A pure function of the partition plan (WorldPartitionRules.hpp), the grid settings and the sources:
// no ECS, no GPU, no I/O. What it answers is the WISH — the set of cells that should be loaded now and
// which first. What it does not answer is how to get there: keeping a cell a little longer after it
// leaves range (hysteresis) and how many cells may activate in one frame (budget) are the residency
// layer's (WP5), which calls this every frame.
//
// ── THE RULE ──────────────────────────────────────────────────────────────────────────────────────
//
// A cell of level L is wanted when the distance from a source to the cell's SQUARE (CellSize·2^L wide)
// is at most its grid's LoadingRange times the source's RangeScale. The square and not its centre: a
// high-level cell is wide precisely because it holds something wide, and that something may reach right
// up to the source while the centre is kilometres away. Every level of a grid shares the grid's range,
// which is the pattern of UE's spatial hash (a grid has one loading range and its levels are the same
// grid at coarser cells — UE:Engine/Source/Runtime/Engine/Private/WorldPartition/RuntimeSpatialHash/
// RuntimeSpatialHashGridHelper.h, FSquare2DGridHelper::ForEachIntersectingCells).
//
// Distance is two-dimensional, X and Z, for the reason the grid is (WorldPartitionGridSerialized): the
// vertical axis is not partitioned, so a camera high above a cell is exactly as near to it as one on the
// ground below.
//
// ── THE ORDER ─────────────────────────────────────────────────────────────────────────────────────
//
// Always-loaded composites come first and always, sources or not: they are the part of the world that
// holds for every camera position. Cells follow nearest-first, by the distance to the NEAREST source;
// equal distances fall back to the plan's own cell order (level, X, Z), so the result never depends on
// the order sources were listed in or on a container's iteration order. A source standing inside a cell
// puts that cell, and every coarser cell above it, at distance zero — the finer one first.
//
// ── THE COST ──────────────────────────────────────────────────────────────────────────────────────
//
// The world's cells are not walked. For each source and each level, the cells whose squares can touch
// the source's circle are enumerated (the circle's bounding box in cell coordinates) and looked up in
// the plan by binary search — `WorldPartitionPlan::Cells` is sorted by (level, X, Z), which is the
// index. A level whose enumeration would be larger than the cells it actually holds (a huge RangeScale,
// or a level with a handful of cells) is scanned instead, so the cost is the smaller of the two, and
// `CellsExamined` states it.

#include <Common/Core/ResultStr.hpp>
#include <Engine/Core/Serialize/SceneFormat.hpp>
#include <Engine/Core/Serialize/WorldPartitionRules.hpp>

#include <glm/vec3.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <vector>

namespace Desert::Core::Rules
{
    // A POINT THE WORLD LOADS AROUND: a camera, a player, a cinematic's target. World units (cm); Y is
    // carried because that is what a caller has, and ignored.
    struct StreamingSource
    {
        glm::vec3 Position{ 0.0f };
        // Multiplies the grid's LoadingRange for this source only — UE's per-source shape scale. 1 is the
        // grid's own range; 0 loads only the cells the source stands in.
        float RangeScale = 1.0f;
    };

    struct WantedCell
    {
        std::size_t Cell     = kNoRecord; // index into WorldPartitionPlan::Cells
        double      Distance = 0.0;       // from the nearest source to the cell's square, cm; 0 inside it
        std::size_t Source   = kNoRecord; // that source; the lowest index when two are equally near
    };

    struct StreamingWish
    {
        // The plan's always-loaded composites, as indices into WorldPartitionPlan::Composites. Loaded
        // before any cell.
        std::vector<std::size_t> AlwaysLoaded;
        // Wanted cells, nearest first; see THE ORDER.
        std::vector<WantedCell> Cells;
        // How many cell squares the query measured, over all sources and levels: its cost, in the one
        // unit that does not depend on the machine.
        std::size_t CellsExamined = 0;
    };

    // Distance on the ground from (x, z) to the square of @p cell at a level whose cells are
    // @p levelCellSize wide. Zero inside the square and on its edge.
    [[nodiscard]] inline double DistanceToCellSquare( double x, double z, const CellCoord& cell,
                                                      double levelCellSize )
    {
        const double lowX  = static_cast<double>( cell.X ) * levelCellSize;
        const double lowZ  = static_cast<double>( cell.Z ) * levelCellSize;
        const double highX = lowX + levelCellSize;
        const double highZ = lowZ + levelCellSize;
        const double dx    = std::max( { lowX - x, 0.0, x - highX } );
        const double dz    = std::max( { lowZ - z, 0.0, z - highZ } );
        return std::sqrt( dx * dx + dz * dz );
    }

    namespace Detail
    {
        // Plan order: level, then X, then Z — the order PlanWorldPartition sorts `Cells` in.
        [[nodiscard]] inline bool CellBefore( const PlannedCell& cell, int level, const CellCoord& coord )
        {
            if ( cell.Level != level )
                return cell.Level < level;
            if ( cell.Cell.X != coord.X )
                return cell.Cell.X < coord.X;
            return cell.Cell.Z < coord.Z;
        }

        // The first and last cell index a source's circle can touch along one axis, clamped to what an
        // int32 cell coordinate can hold. A cell k spans [k·s, (k+1)·s]; it can touch the interval
        // [c − r, c + r] when (k+1)·s ≥ c − r and k·s ≤ c + r. Rounding only widens the range, and the
        // exact distance test decides.
        struct AxisSpan
        {
            double First = 0.0;
            double Last  = -1.0;
        };

        [[nodiscard]] inline AxisSpan CellsAlong( double centre, double radius, double levelCellSize )
        {
            constexpr double kLowest  = -2147483648.0;
            constexpr double kHighest = 2147483647.0;
            AxisSpan         span;
            span.First = std::clamp( std::ceil( ( centre - radius ) / levelCellSize ) - 1.0, kLowest, kHighest );
            span.Last  = std::clamp( std::floor( ( centre + radius ) / levelCellSize ), kLowest, kHighest );
            return span;
        }
    } // namespace Detail

    // THE QUERY. @p settings is the world's partition block; the plan places every composite on
    // `Grids[0]` until WP22 lets a record choose its grid (WorldPartitionPlan::UnusedGrids), so that is
    // the grid whose CellSize and LoadingRange are read here.
    //
    // Refused, by name and number, rather than answered with an empty set: a source whose position or
    // scale is not a finite number or whose scale is negative (a NaN camera would otherwise unload the
    // whole world without a word), and a plan with cells but a settings block with no grid.
    [[nodiscard]] inline Common::ResultStr<StreamingWish>
    QueryStreamingCells( const WorldPartitionPlan& plan, const WorldPartitionSerialized& settings,
                         std::span<const StreamingSource> sources )
    {
        for ( std::size_t index = 0; index < sources.size(); ++index )
        {
            const StreamingSource& source = sources[index];
            if ( !std::isfinite( source.Position.x ) || !std::isfinite( source.Position.z ) ||
                 !std::isfinite( source.RangeScale ) || source.RangeScale < 0.0f )
            {
                return Common::MakeError<StreamingWish>(
                     "QueryStreamingCells: streaming source " + std::to_string( index ) + " has position (" +
                     std::to_string( source.Position.x ) + ", " + std::to_string( source.Position.z ) +
                     ") and RangeScale " + std::to_string( source.RangeScale ) +
                     "; a position must be finite and a scale finite and non-negative" );
            }
        }

        StreamingWish wish;
        wish.AlwaysLoaded = plan.AlwaysLoaded;
        if ( plan.Cells.empty() || sources.empty() )
            return Common::MakeSuccess( std::move( wish ) );

        if ( settings.Grids.empty() )
        {
            return Common::MakeError<StreamingWish>( "QueryStreamingCells: the plan has " +
                                                     std::to_string( plan.Cells.size() ) +
                                                     " cell(s) but the partition settings state no grid" );
        }
        const WorldPartitionGridSerialized& grid = settings.Grids.front();
        if ( !( grid.CellSize > 0.0f ) || !std::isfinite( grid.CellSize ) || !( grid.LoadingRange >= 0.0f ) ||
             !std::isfinite( grid.LoadingRange ) )
        {
            return Common::MakeError<StreamingWish>( "QueryStreamingCells: grid 0 has CellSize " +
                                                     std::to_string( grid.CellSize ) + " and LoadingRange " +
                                                     std::to_string( grid.LoadingRange ) +
                                                     "; a cell must be wider than zero and a range non-negative" );
        }

        // Where each level's cells begin in the sorted plan; levelBegin[L + 1] is where they end. Found by
        // binary search too — a pass over the cells here would be the very walk this query avoids.
        const int                lastLevel = plan.Cells.back().Level;
        std::vector<std::size_t> levelBegin( static_cast<std::size_t>( lastLevel ) + 2, plan.Cells.size() );
        for ( int level = 0; level <= lastLevel; ++level )
        {
            const auto first =
                 std::lower_bound( plan.Cells.begin(), plan.Cells.end(), level,
                                   []( const PlannedCell& cell, int key ) { return cell.Level < key; } );
            levelBegin[static_cast<std::size_t>( level )] = static_cast<std::size_t>( first - plan.Cells.begin() );
        }

        std::vector<WantedCell> hits;
        for ( std::size_t sourceIndex = 0; sourceIndex < sources.size(); ++sourceIndex )
        {
            const double x = static_cast<double>( sources[sourceIndex].Position.x );
            const double z = static_cast<double>( sources[sourceIndex].Position.z );
            const double radius =
                 static_cast<double>( grid.LoadingRange ) * static_cast<double>( sources[sourceIndex].RangeScale );

            for ( int level = 0; level <= lastLevel; ++level )
            {
                const auto begin = plan.Cells.begin() +
                                   static_cast<std::ptrdiff_t>( levelBegin[static_cast<std::size_t>( level )] );
                const auto end = plan.Cells.begin() +
                                 static_cast<std::ptrdiff_t>( levelBegin[static_cast<std::size_t>( level ) + 1] );
                const double size = LevelCellSize( grid.CellSize, level );

                const auto consider = [&]( std::size_t cellIndex )
                {
                    ++wish.CellsExamined;
                    const double distance = DistanceToCellSquare( x, z, plan.Cells[cellIndex].Cell, size );
                    if ( distance <= radius )
                        hits.push_back( WantedCell{ cellIndex, distance, sourceIndex } );
                };

                const Detail::AxisSpan alongX = Detail::CellsAlong( x, radius, size );
                const Detail::AxisSpan alongZ = Detail::CellsAlong( z, radius, size );
                const double           enumerated =
                     ( alongX.Last - alongX.First + 1.0 ) * ( alongZ.Last - alongZ.First + 1.0 );
                if ( enumerated > static_cast<double>( end - begin ) )
                {
                    for ( auto cell = begin; cell != end; ++cell )
                        consider( static_cast<std::size_t>( cell - plan.Cells.begin() ) );
                    continue;
                }

                for ( double cellX = alongX.First; cellX <= alongX.Last; cellX += 1.0 )
                {
                    for ( double cellZ = alongZ.First; cellZ <= alongZ.Last; cellZ += 1.0 )
                    {
                        const CellCoord coord{ static_cast<std::int32_t>( cellX ),
                                               static_cast<std::int32_t>( cellZ ) };
                        const auto      found = std::lower_bound(
                             begin, end, coord, [level]( const PlannedCell& cell, const CellCoord& key )
                             { return Detail::CellBefore( cell, level, key ); } );
                        if ( found != end && found->Cell == coord )
                            consider( static_cast<std::size_t>( found - plan.Cells.begin() ) );
                    }
                }
            }
        }

        // Union over sources: each cell once, at its nearest source (the lowest index among equals).
        std::sort( hits.begin(), hits.end(),
                   []( const WantedCell& a, const WantedCell& b )
                   {
                       if ( a.Cell != b.Cell )
                           return a.Cell < b.Cell;
                       if ( a.Distance != b.Distance )
                           return a.Distance < b.Distance;
                       return a.Source < b.Source;
                   } );
        hits.erase( std::unique( hits.begin(), hits.end(),
                                 []( const WantedCell& a, const WantedCell& b ) { return a.Cell == b.Cell; } ),
                    hits.end() );
        std::sort( hits.begin(), hits.end(),
                   []( const WantedCell& a, const WantedCell& b )
                   {
                       if ( a.Distance != b.Distance )
                           return a.Distance < b.Distance;
                       return a.Cell < b.Cell;
                   } );
        wish.Cells = std::move( hits );
        return Common::MakeSuccess( std::move( wish ) );
    }
} // namespace Desert::Core::Rules
