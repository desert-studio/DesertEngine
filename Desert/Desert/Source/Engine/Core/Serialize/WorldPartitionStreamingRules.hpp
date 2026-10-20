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
// is at most the source's radius: its own LoadingRange when it states one, else the grid's, times its
// RangeScale (SourceRadius). The square and not its centre: a
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
// ── THE SHAPES ────────────────────────────────────────────────────────────────────────────────────
//
// A source loads around its SHAPES (UE's FStreamingSourceShape, WorldPartitionStreamingSource.h): with none,
// one circle of SourceRadius at its position — the rule above. Each shape is a circle or a SECTOR of one
// (a pie slice SectorAngle wide, centred on the shape's axis), placed by an offset and a yaw in the
// source's own frame, so a sector turns with the entity that carries it. Its radius is the grid's range
// times its LoadingRangeScale, or its own Radius; the source's RangeScale multiplies either, and its
// AngleScale widens a sector (the residency layer's unload band widens both, as UE's ExtraRadius and
// ExtraAngle). A cell is wanted when ANY shape of ANY source reaches its square: for a sector, when the
// part of the square inside the slice is within the radius of the slice's apex — exact, the square is
// clipped by the slice's half-planes (UE's FSphericalSector::IntersectsBox is a conservative reject).
//
// ── THE ORDER ─────────────────────────────────────────────────────────────────────────────────────
//
// Always-loaded composites come first and always, sources or not: they are the part of the world that
// holds for every camera position. Cells follow by PRIORITY first — a cell carries the highest Priority
// of the sources that want it, and higher is loaded sooner (UE's EStreamingSourcePriority: the player
// before a cinematic's look-ahead) — then by the SORT KEY, the distance weighted by where the cell lies
// against the source's heading (UE's UWorldPartitionRuntimeCellData::AppendStreamingSourceInfo: its spatial
// sorting priority is the distance ratio times (cos·0.5 + 0.5) of the angle between the heading and the
// cell, as seen from the cell). Here the weight multiplies the distance in cm, so a source with no heading
// keeps the plain nearest-first order and sources of different ranges stay comparable: a cell straight
// ahead weighs 0, one abeam half, one straight behind its whole distance. The heading is the shape's axis
// (the source's Facing turned by the shape's yaw), and with UseVelocityForSorting the source's Velocity is
// added to it (UE's bUseVelocityContributionToCellsSorting) — so a fast car loads the road ahead before
// the field it just left. Equal keys fall back to the distance, then to the plan's own cell order (level,
// X, Z), so the result never depends on the order sources were listed in or on a container's iteration
// order. A source standing inside a cell puts that cell, and every coarser cell above it, at distance
// zero — the finer one first.
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

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace Desert::Core::Rules
{
    // ONE SHAPE A SOURCE LOADS WITHIN (UE's FStreamingSourceShape); see THE SHAPES. Placed in the source's
    // frame: its forward is the source's Facing, its right is that turned a quarter clockwise seen from above.
    struct StreamingSourceShape
    {
        // On: the radius is the grid's LoadingRange times LoadingRangeScale. Off: Radius, cm.
        bool  UseGridLoadingRange = true;
        float LoadingRangeScale   = 1.0f;
        float Radius              = 0.0f;
        // On: only a slice SectorAngle degrees wide (0..360) around the shape's axis; 360 is the circle.
        bool  IsSector    = false;
        float SectorAngle = 360.0f;
        // Where the shape's centre sits from the source, cm, in the source's frame: X right, Z backward
        // (an entity's local axes, whose forward is −Z). Y is carried and ignored, as the position's is.
        glm::vec3 Location{ 0.0f };
        // Degrees the shape's axis is turned from the source's forward, counter-clockwise seen from above
        // (a turn about +Y, the entity's own yaw).
        float Yaw = 0.0f;
    };

    // A POINT THE WORLD LOADS AROUND: a player's pawn, a cinematic's target, an instrument's camera — in
    // Play never the view by itself (ECS::StreamingSourceComponent is what makes an entity one). World
    // units (cm); Y is carried because that is what a caller has, and ignored.
    struct StreamingSource
    {
        glm::vec3 Position{ 0.0f };
        // Multiplies the loading range for this source only — UE's per-source shape scale. 1 is the range
        // itself; 0 loads only the cells the source stands in. The residency layer widens it for its
        // unload band, which is why an override below is scaled too.
        float RangeScale = 1.0f;
        // Multiplies every sector's angle (capped at the full circle): the residency layer widens it with the
        // range for its unload band, so a turning sector does not drop a cell on its edge every frame.
        float AngleScale = 1.0f;
        // This source's own loading range in cm instead of the grid's (UE's FStreamingSourceShape with
        // bUseGridLoadingRange off), for the circle it loads in when it states no Shapes.
        std::optional<float> LoadingRange;
        // Higher loads first; see THE ORDER. It never changes WHICH cells are wanted.
        int Priority = 0;
        // Where the source faces on the ground (X, Z of a direction; Y ignored): its shapes' frame and the
        // heading THE ORDER weighs cells by. nullopt — an instrument with no direction — is the world's −Z for
        // the shapes and no heading for the order.
        std::optional<glm::vec3> Facing;
        // cm/s; added to the heading of the order when UseVelocityForSorting (UE's Velocity and
        // bUseVelocityContributionToCellsSorting). It never changes WHICH cells are wanted.
        glm::vec3 Velocity{ 0.0f };
        bool      UseVelocityForSorting = false;
        // Empty = one circle of SourceRadius at Position (UE: no shapes, a sphere of the grid's range).
        std::vector<StreamingSourceShape> Shapes;
        // The grids this source streams, by WorldPartitionGridSerialized::Name (UE's
        // FWorldPartitionStreamingSource::TargetGrids); empty = every grid. A grid not named here is not
        // queried for this source at all - see TargetsGrid. A name no grid of the world carries targets nothing;
        // the streamer names it (WorldStreamer::Tick), it is never matched by a guess.
        std::vector<std::string> TargetGrids;
    };

    // Whether @p source streams @p grid: it names no grid (all of them), or names this one.
    [[nodiscard]] inline bool TargetsGrid( const StreamingSource&              source,
                                           const WorldPartitionGridSerialized& grid )
    {
        return source.TargetGrids.empty() || std::find( source.TargetGrids.begin(), source.TargetGrids.end(),
                                                        grid.Name ) != source.TargetGrids.end();
    }

    // The radius a source with no Shapes loads within on @p grid, cm.
    [[nodiscard]] inline double SourceRadius( const StreamingSource&              source,
                                              const WorldPartitionGridSerialized& grid )
    {
        const float range = source.LoadingRange.value_or( grid.LoadingRange );
        return static_cast<double>( range ) * static_cast<double>( source.RangeScale );
    }

    // ONE SHAPE ON THE GROUND, in world cm: what the query tests cells against and the panel paints.
    struct SourceSector
    {
        glm::dvec2 Centre{ 0.0 };     // (X, Z)
        double     Radius = 0.0;      // cm
        glm::dvec2 Axis{ 0.0, -1.0 }; // unit, (X, Z)
        double     Angle = 360.0;     // degrees, (0, 360]; 360 is the circle
        // The heading THE ORDER weighs cells by: Axis turned toward the velocity when the source asks, or
        // zero when the source has neither a facing nor a velocity that counts.
        glm::dvec2 Heading{ 0.0 };

        [[nodiscard]] bool IsCircle() const
        {
            return Angle >= 360.0;
        }
    };

    namespace Detail
    {
        [[nodiscard]] inline glm::dvec2 Ground( const glm::vec3& v )
        {
            return { static_cast<double>( v.x ), static_cast<double>( v.z ) };
        }
        // @p v turned @p degrees counter-clockwise seen from above (about +Y): in (X, Z), +X goes to −Z.
        [[nodiscard]] inline glm::dvec2 TurnAboutUp( const glm::dvec2& v, double degrees )
        {
            const double r = degrees * 3.14159265358979323846 / 180.0;
            const double c = std::cos( r );
            const double s = std::sin( r );
            return { v.x * c + v.y * s, -v.x * s + v.y * c };
        }
        [[nodiscard]] inline glm::dvec2 UnitOrZero( const glm::dvec2& v )
        {
            const double length = std::sqrt( v.x * v.x + v.y * v.y );
            return length > 1e-9 ? v / length : glm::dvec2( 0.0 );
        }
    } // namespace Detail

    // Every shape of @p source on @p grid as the query tests it (UE's FStreamingSourceShapeHelper::
    // ForEachShape, projected to 2D): one circle when it states none. A shape with no radius or no angle
    // reaches nothing and is left out, as UE leaves out an invalid FSphericalSector. A grid the source does not
    // target (TargetsGrid) has no sector at all, so every reader of the shapes - the query, the assessment, the
    // map - honours the targeting the same way (UE FWorldPartitionStreamingSource::TargetGrids in
    // UWorldPartitionRuntimeHash::ForEachStreamingCellsSources).
    [[nodiscard]] inline std::vector<SourceSector> SourceSectors( const StreamingSource&              source,
                                                                  const WorldPartitionGridSerialized& grid )
    {
        if ( !TargetsGrid( source, grid ) )
            return {};
        const glm::dvec2 position = Detail::Ground( source.Position );
        glm::dvec2       forward =
             source.Facing ? Detail::UnitOrZero( Detail::Ground( *source.Facing ) ) : glm::dvec2( 0.0 );
        const bool faces = forward != glm::dvec2( 0.0 );
        if ( !faces )
            forward = { 0.0, -1.0 };
        const glm::dvec2 right( -forward.y, forward.x );
        const glm::dvec2 velocity =
             source.UseVelocityForSorting ? Detail::Ground( source.Velocity ) : glm::dvec2( 0.0 );
        const auto heading = [&]( const glm::dvec2& axis )
        { return Detail::UnitOrZero( ( faces ? axis : glm::dvec2( 0.0 ) ) + velocity ); };

        std::vector<SourceSector> sectors;
        if ( source.Shapes.empty() )
        {
            SourceSector circle;
            circle.Centre  = position;
            circle.Radius  = SourceRadius( source, grid );
            circle.Axis    = forward;
            circle.Heading = heading( forward );
            sectors.push_back( circle );
            return sectors;
        }
        for ( const StreamingSourceShape& shape : source.Shapes )
        {
            SourceSector sector;
            const double range = shape.UseGridLoadingRange
                                      ? static_cast<double>( grid.LoadingRange ) * shape.LoadingRangeScale
                                      : static_cast<double>( shape.Radius );
            sector.Radius      = range * static_cast<double>( source.RangeScale );
            sector.Angle       = shape.IsSector ? std::min( 360.0, static_cast<double>( shape.SectorAngle ) *
                                                                        static_cast<double>( source.AngleScale ) )
                                                : 360.0;
            if ( !( sector.Angle > 0.0 ) )
                continue;
            // Local X is right, local Z is backward (the forward is −Z).
            sector.Centre = position + right * static_cast<double>( shape.Location.x ) -
                            forward * static_cast<double>( shape.Location.z );
            sector.Axis    = Detail::TurnAboutUp( forward, static_cast<double>( shape.Yaw ) );
            sector.Heading = heading( sector.Axis );
            sectors.push_back( sector );
        }
        return sectors;
    }

    struct WantedCell
    {
        std::size_t Cell     = kNoRecord; // index into WorldPartitionPlan::Cells
        double      Distance = 0.0;       // from the nearest wanting shape of Priority to the square, cm; 0 inside
        std::size_t Source   = kNoRecord; // that source; the lowest index when two are equally near
        int         Priority = 0;         // the highest Priority among the sources that want the cell
        double      SortKey  = 0.0;       // Distance weighted by that shape's heading (THE ORDER); the lowest wins
        int         GridPriority = 0;     // the cell's grid's Priority (UE FSpatialHashRuntimeGrid::Priority)
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

    namespace Detail
    {
        // The part of a convex polygon where n·(p − apex) ≥ 0 (one Sutherland–Hodgman pass).
        [[nodiscard]] inline std::vector<glm::dvec2> ClipToHalfPlane( const std::vector<glm::dvec2>& polygon,
                                                                      const glm::dvec2& apex, const glm::dvec2& n )
        {
            std::vector<glm::dvec2> out;
            const std::size_t       count = polygon.size();
            for ( std::size_t i = 0; i < count; ++i )
            {
                const glm::dvec2& a  = polygon[i];
                const glm::dvec2& b  = polygon[( i + 1 ) % count];
                const double      da = n.x * ( a.x - apex.x ) + n.y * ( a.y - apex.y );
                const double      db = n.x * ( b.x - apex.x ) + n.y * ( b.y - apex.y );
                if ( da >= 0.0 )
                    out.push_back( a );
                if ( ( da >= 0.0 ) != ( db >= 0.0 ) )
                    out.push_back( a + ( b - a ) * ( da / ( da - db ) ) );
            }
            return out;
        }

        // Distance from @p p to a convex polygon (counter-clockwise or clockwise); 0 inside or on it, +∞ for
        // an empty one.
        [[nodiscard]] inline double DistanceToConvexPolygon( const glm::dvec2&              p,
                                                             const std::vector<glm::dvec2>& polygon )
        {
            if ( polygon.empty() )
                return std::numeric_limits<double>::infinity();
            bool   anyLeft = false, anyRight = false;
            double nearest = std::numeric_limits<double>::infinity();
            for ( std::size_t i = 0; i < polygon.size(); ++i )
            {
                const glm::dvec2& a     = polygon[i];
                const glm::dvec2& b     = polygon[( i + 1 ) % polygon.size()];
                const glm::dvec2  edge  = b - a;
                const double      cross = edge.x * ( p.y - a.y ) - edge.y * ( p.x - a.x );
                anyLeft                 = anyLeft || cross > 0.0;
                anyRight                = anyRight || cross < 0.0;
                const double lengthSq   = edge.x * edge.x + edge.y * edge.y;
                const double t =
                     lengthSq > 0.0
                          ? std::clamp( ( ( p.x - a.x ) * edge.x + ( p.y - a.y ) * edge.y ) / lengthSq, 0.0, 1.0 )
                          : 0.0;
                const glm::dvec2 closest = a + edge * t;
                nearest                  = std::min( nearest, std::hypot( p.x - closest.x, p.y - closest.y ) );
            }
            return ( anyLeft && anyRight ) ? nearest : 0.0;
        }

        // The two inward normals of a convex wedge (half-angle ≤ 90°) at an apex, around @p axis: a point is
        // in the wedge when it is on the positive side of both.
        [[nodiscard]] inline std::pair<glm::dvec2, glm::dvec2> WedgeNormals( const glm::dvec2& axis,
                                                                             double            halfDegrees )
        {
            const glm::dvec2 left  = TurnAboutUp( axis, halfDegrees );  // the boundary ray on one side
            const glm::dvec2 right = TurnAboutUp( axis, -halfDegrees ); // and on the other
            // Each normal is its ray turned a quarter toward the axis.
            glm::dvec2 nLeft( -left.y, left.x );
            if ( nLeft.x * axis.x + nLeft.y * axis.y < 0.0 )
                nLeft = -nLeft;
            glm::dvec2 nRight( -right.y, right.x );
            if ( nRight.x * axis.x + nRight.y * axis.y < 0.0 )
                nRight = -nRight;
            return { nLeft, nRight };
        }
    } // namespace Detail

    // Distance on the ground from @p sector's centre to the part of @p cell's square (a level whose cells are
    // @p levelCellSize wide) that lies inside the sector's slice: zero when the centre is in that part, +∞
    // when the slice misses the square. For a circle, DistanceToCellSquare. A cell is in the sector when this
    // is at most its radius.
    [[nodiscard]] inline double DistanceToCellInSector( const SourceSector& sector, const CellCoord& cell,
                                                        double levelCellSize )
    {
        if ( sector.IsCircle() )
            return DistanceToCellSquare( sector.Centre.x, sector.Centre.y, cell, levelCellSize );
        const double                  lowX = static_cast<double>( cell.X ) * levelCellSize;
        const double                  lowZ = static_cast<double>( cell.Z ) * levelCellSize;
        const std::vector<glm::dvec2> square{ { lowX, lowZ },
                                              { lowX + levelCellSize, lowZ },
                                              { lowX + levelCellSize, lowZ + levelCellSize },
                                              { lowX, lowZ + levelCellSize } };
        const double                  half = sector.Angle * 0.5;
        if ( half <= 90.0 )
        {
            // A convex slice: the square clipped by both of its sides.
            const auto [a, b] = Detail::WedgeNormals( sector.Axis, half );
            return Detail::DistanceToConvexPolygon(
                 sector.Centre, Detail::ClipToHalfPlane( Detail::ClipToHalfPlane( square, sector.Centre, a ),
                                                         sector.Centre, b ) );
        }
        // A reflex slice is everything but the convex wedge behind it, i.e. the union of the two half-planes
        // outside that wedge's sides: the nearer of the square's two parts.
        const auto [a, b] = Detail::WedgeNormals( -sector.Axis, 180.0 - half );
        return std::min( Detail::DistanceToConvexPolygon( sector.Centre,
                                                          Detail::ClipToHalfPlane( square, sector.Centre, -a ) ),
                         Detail::DistanceToConvexPolygon( sector.Centre,
                                                          Detail::ClipToHalfPlane( square, sector.Centre, -b ) ) );
    }

    // THE ORDER's weight of a cell for one shape: (cos·0.5 + 0.5) of the angle between the shape's heading
    // and the direction from the cell's nearest point to the shape's centre — 0 straight ahead, 1 straight
    // behind (UE's SourceCosAngleRatio); 1 with no heading, so the key is the plain distance; 0 inside.
    [[nodiscard]] inline double HeadingWeight( const SourceSector& sector, const CellCoord& cell,
                                               double levelCellSize )
    {
        if ( sector.Heading == glm::dvec2( 0.0 ) )
            return 1.0;
        const double     lowX = static_cast<double>( cell.X ) * levelCellSize;
        const double     lowZ = static_cast<double>( cell.Z ) * levelCellSize;
        const glm::dvec2 closest( std::clamp( sector.Centre.x, lowX, lowX + levelCellSize ),
                                  std::clamp( sector.Centre.y, lowZ, lowZ + levelCellSize ) );
        const glm::dvec2 toSource = Detail::UnitOrZero( sector.Centre - closest );
        if ( toSource == glm::dvec2( 0.0 ) )
            return 0.0;
        const double cosine = sector.Heading.x * toSource.x + sector.Heading.y * toSource.y;
        return cosine * 0.5 + 0.5;
    }

    // THE QUERY. @p settings is the world's partition block. Every grid is queried with ITS OWN cell size and
    // loading range (UE ForEachStreamingCellsSources walks each runtime grid with the grid's range), so a cell of
    // a small-range grid is wanted only within that range while a main-grid cell beside it is wanted further out.
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
            if ( source.LoadingRange.has_value() &&
                 ( !std::isfinite( *source.LoadingRange ) || *source.LoadingRange < 0.0f ) )
            {
                return Common::MakeError<StreamingWish>(
                     "QueryStreamingCells: streaming source " + std::to_string( index ) +
                     " overrides LoadingRange with " + std::to_string( *source.LoadingRange ) +
                     " cm; a range must be finite and non-negative" );
            }
            if ( !std::isfinite( source.AngleScale ) || source.AngleScale < 0.0f ||
                 ( source.Facing.has_value() &&
                   ( !std::isfinite( source.Facing->x ) || !std::isfinite( source.Facing->z ) ) ) ||
                 !std::isfinite( source.Velocity.x ) || !std::isfinite( source.Velocity.z ) )
            {
                return Common::MakeError<StreamingWish>( std::format(
                     "QueryStreamingCells: streaming source {} has AngleScale {}, a facing that is {} and "
                     "velocity "
                     "({}, {}); a scale must be finite and non-negative, a facing and a velocity finite",
                     index, source.AngleScale, source.Facing ? "stated" : "absent", source.Velocity.x,
                     source.Velocity.z ) );
            }
            for ( std::size_t shapeIndex = 0; shapeIndex < source.Shapes.size(); ++shapeIndex )
            {
                const StreamingSourceShape& shape = source.Shapes[shapeIndex];
                const bool valid = std::isfinite( shape.LoadingRangeScale ) && shape.LoadingRangeScale >= 0.0f &&
                                   std::isfinite( shape.Radius ) && shape.Radius >= 0.0f &&
                                   std::isfinite( shape.SectorAngle ) && shape.SectorAngle >= 0.0f &&
                                   shape.SectorAngle <= 360.0f && std::isfinite( shape.Location.x ) &&
                                   std::isfinite( shape.Location.z ) && std::isfinite( shape.Yaw );
                if ( !valid )
                {
                    return Common::MakeError<StreamingWish>( std::format(
                         "QueryStreamingCells: streaming source {} shape {} has LoadingRangeScale {}, Radius {} "
                         "cm, "
                         "SectorAngle {}, Location ({}, {}) and Yaw {}; scales and radii must be finite and "
                         "non-negative, an angle within 0..360 and a placement finite",
                         index, shapeIndex, shape.LoadingRangeScale, shape.Radius, shape.SectorAngle,
                         shape.Location.x, shape.Location.z, shape.Yaw ) );
                }
            }
        }

        StreamingWish wish;
        wish.AlwaysLoaded = plan.AlwaysLoaded;
        if ( plan.Cells.empty() || sources.empty() )
            return Common::MakeSuccess( std::move( wish ) );

        if ( settings.Grids.empty() )
        {
            return Common::MakeError<StreamingWish>( std::format(
                 "QueryStreamingCells: the plan has {} cell(s) but the partition settings state no grid",
                 plan.Cells.size() ) );
        }
        for ( std::size_t gridIndex = 0; gridIndex < settings.Grids.size(); ++gridIndex )
        {
            const WorldPartitionGridSerialized& grid = settings.Grids[gridIndex];
            if ( !( grid.CellSize > 0.0f ) || !std::isfinite( grid.CellSize ) || !( grid.LoadingRange >= 0.0f ) ||
                 !std::isfinite( grid.LoadingRange ) )
            {
                return Common::MakeError<StreamingWish>( std::format(
                     "QueryStreamingCells: grid {} '{}' has CellSize {} and LoadingRange {}; a cell must be wider "
                     "than zero and a range non-negative",
                     gridIndex, grid.Name, grid.CellSize, grid.LoadingRange ) );
            }
        }
        if ( plan.Cells.back().Grid >= settings.Grids.size() )
        {
            return Common::MakeError<StreamingWish>(
                 std::format( "QueryStreamingCells: the plan has a cell of grid {} but the settings state {} grid(s)",
                              plan.Cells.back().Grid, settings.Grids.size() ) );
        }

        std::vector<WantedCell> hits;
        for ( std::size_t gridIndex = 0; gridIndex < settings.Grids.size(); ++gridIndex )
        {
            const WorldPartitionGridSerialized& grid = settings.Grids[gridIndex];
            // This grid's cells: the plan is ordered by grid first, so they are one run, found by binary search.
            const auto gridFirst = std::lower_bound( plan.Cells.begin(), plan.Cells.end(), gridIndex,
                                                     []( const PlannedCell& cell, std::size_t key )
                                                     { return cell.Grid < key; } );
            const auto gridLast  = std::upper_bound( gridFirst, plan.Cells.end(), gridIndex,
                                                     []( std::size_t key, const PlannedCell& cell )
                                                     { return key < cell.Grid; } );
            if ( gridFirst == gridLast )
                continue;

            // Where each level's cells begin in this grid's run; levelBegin[L + 1] is where they end.
            const int                lastLevel = ( gridLast - 1 )->Level;
            std::vector<std::size_t> levelBegin( static_cast<std::size_t>( lastLevel ) + 2,
                                                 static_cast<std::size_t>( gridLast - plan.Cells.begin() ) );
            for ( int level = 0; level <= lastLevel; ++level )
            {
                const auto first =
                     std::lower_bound( gridFirst, gridLast, level,
                                       []( const PlannedCell& cell, int key ) { return cell.Level < key; } );
                levelBegin[static_cast<std::size_t>( level )] = static_cast<std::size_t>( first - plan.Cells.begin() );
            }

            for ( std::size_t sourceIndex = 0; sourceIndex < sources.size(); ++sourceIndex )
            {
                const int priority = sources[sourceIndex].Priority;
                for ( const SourceSector& sector : SourceSectors( sources[sourceIndex], grid ) )
                {
                    const double x      = sector.Centre.x;
                    const double z      = sector.Centre.y;
                    const double radius = sector.Radius;

                    for ( int level = 0; level <= lastLevel; ++level )
                    {
                        const auto begin = plan.Cells.begin() + static_cast<std::ptrdiff_t>(
                                                                     levelBegin[static_cast<std::size_t>( level )] );
                        const auto end = plan.Cells.begin() + static_cast<std::ptrdiff_t>(
                                                                   levelBegin[static_cast<std::size_t>( level ) + 1] );
                        const double size = LevelCellSize( grid.CellSize, level );

                        const auto consider = [&]( std::size_t cellIndex )
                        {
                            ++wish.CellsExamined;
                            const CellCoord& coord    = plan.Cells[cellIndex].Cell;
                            const double     distance = DistanceToCellInSector( sector, coord, size );
                            if ( distance <= radius )
                            {
                                hits.push_back( WantedCell{ cellIndex, distance, sourceIndex, priority,
                                                            distance * HeadingWeight( sector, coord, size ),
                                                        grid.Priority } );
                            }
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
            }
        }

        // Union over sources and shapes: each cell once, at its highest-priority source and, among those, the
        // lowest sort key, then the nearest (the lowest index among equals).
        std::sort( hits.begin(), hits.end(),
                   []( const WantedCell& a, const WantedCell& b )
                   {
                       if ( a.Cell != b.Cell )
                           return a.Cell < b.Cell;
                       if ( a.Priority != b.Priority )
                           return a.Priority > b.Priority;
                       if ( a.GridPriority != b.GridPriority )
                           return a.GridPriority > b.GridPriority;
                       if ( a.SortKey != b.SortKey )
                           return a.SortKey < b.SortKey;
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
                       if ( a.Priority != b.Priority )
                           return a.Priority > b.Priority;
                       if ( a.GridPriority != b.GridPriority )
                           return a.GridPriority > b.GridPriority;
                       if ( a.SortKey != b.SortKey )
                           return a.SortKey < b.SortKey;
                       if ( a.Distance != b.Distance )
                           return a.Distance < b.Distance;
                       return a.Cell < b.Cell;
                   } );
        wish.Cells = std::move( hits );
        return Common::MakeSuccess( std::move( wish ) );
    }
} // namespace Desert::Core::Rules
