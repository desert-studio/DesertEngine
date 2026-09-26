// THE STREAMING QUERY: WHICH CELLS A SET OF SOURCES WANTS, AND IN WHAT ORDER.
//
// WHAT IS ASSERTED:
//
//   1. THE RULE. A cell is wanted when a source is at most LoadingRange from its SQUARE — exactly at the
//      range included, a centimetre beyond excluded, on both sides of the origin — and a level-L square
//      is CellSize·2^L wide, so a coarse cell is wanted from where its centre is far out of range.
//   2. THE SET. Always-loaded composites are in it with no source at all; two sources are a union, each
//      cell at its nearest source; an empty plan is an empty wish; a bad source or grid is refused.
//   3. THE ORDER. Nearest first, plan order among equals, and independent of the order sources are given.
//   4. THE REFERENCE. On random plans — negative coordinates, several levels, scales from zero to one
//      that covers the world — the answer equals a walk over every cell, distance computed separately.
//   5. THE COST. The query measures a few dozen squares on a 50 000-cell world, not 50 000, and one
//      query's time is printed (Release is the number that counts).

#include <Common/Json/Json.hpp>
#include <Engine/Core/Serialize/WorldPartitionStreamingRules.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <random>
#include <string>
#include <tuple>
#include <vector>

using Desert::Assets::EntityData;
using Desert::Core::WorldPartitionGridSerialized;
using Desert::Core::WorldPartitionSerialized;
using Desert::Core::Rules::CellCoord;
using Desert::Core::Rules::kNoRecord;
using Desert::Core::Rules::PlannedCell;
using Desert::Core::Rules::PlanWorldPartition;
using Desert::Core::Rules::QueryStreamingCells;
using Desert::Core::Rules::StreamingSource;
using Desert::Core::Rules::StreamingWish;
using Desert::Core::Rules::WantedCell;
using Desert::Core::Rules::WorldPartitionPlan;

namespace
{
    WorldPartitionSerialized Grid( float cellSize, float loadingRange )
    {
        WorldPartitionGridSerialized grid;
        grid.CellSize     = cellSize;
        grid.LoadingRange = loadingRange;
        return WorldPartitionSerialized{ { grid } };
    }

    // A plan holding exactly these cells, sorted the way PlanWorldPartition sorts them. The cells hold no
    // composites: the query reads coordinates and levels only.
    WorldPartitionPlan PlanOf( std::vector<std::tuple<int, int, int>> cells )
    {
        std::sort( cells.begin(), cells.end() );
        cells.erase( std::unique( cells.begin(), cells.end() ), cells.end() );
        WorldPartitionPlan plan;
        for ( const auto& [level, x, z] : cells )
        {
            PlannedCell cell;
            cell.Level = level;
            cell.Cell  = CellCoord{ x, z };
            plan.Cells.push_back( cell );
            plan.LevelCount = std::max( plan.LevelCount, level + 1 );
        }
        return plan;
    }

    StreamingSource At( float x, float z, float scale = 1.0f )
    {
        return StreamingSource{ glm::vec3( x, 0.0f, z ), scale };
    }

    StreamingWish Query( const WorldPartitionPlan& plan, const WorldPartitionSerialized& settings,
                         const std::vector<StreamingSource>& sources )
    {
        const auto wish = QueryStreamingCells( plan, settings, sources );
        EXPECT_TRUE( wish.IsSuccess() ) << ( wish.IsSuccess() ? "" : wish.GetError() );
        return wish.IsSuccess() ? wish.GetValue() : StreamingWish{};
    }

    // Wanted cells as (level, x, z), in the query's order.
    std::vector<std::tuple<int, int, int>> Coordinates( const WorldPartitionPlan& plan, const StreamingWish& wish )
    {
        std::vector<std::tuple<int, int, int>> out;
        for ( const WantedCell& wanted : wish.Cells )
        {
            const PlannedCell& cell = plan.Cells.at( wanted.Cell );
            out.emplace_back( cell.Level, cell.Cell.X, cell.Cell.Z );
        }
        return out;
    }

    // THE REFERENCE: every cell against every source, with its own distance arithmetic — the clamp of the
    // source onto the square, written differently from the query's on purpose.
    std::vector<WantedCell> EveryCell( const WorldPartitionPlan& plan, const WorldPartitionGridSerialized& grid,
                                       const std::vector<StreamingSource>& sources )
    {
        std::vector<WantedCell> out;
        for ( std::size_t index = 0; index < plan.Cells.size(); ++index )
        {
            const PlannedCell& cell = plan.Cells[index];
            const double       size = static_cast<double>( grid.CellSize ) * std::pow( 2.0, cell.Level );
            WantedCell         best{ index, 0.0, kNoRecord };
            for ( std::size_t source = 0; source < sources.size(); ++source )
            {
                const double x        = sources[source].Position.x;
                const double z        = sources[source].Position.z;
                const double closestX = std::clamp( x, cell.Cell.X * size, ( cell.Cell.X + 1 ) * size );
                const double closestZ = std::clamp( z, cell.Cell.Z * size, ( cell.Cell.Z + 1 ) * size );
                const double reach    = std::hypot( x - closestX, z - closestZ );
                const double allowed  = static_cast<double>( grid.LoadingRange ) * sources[source].RangeScale;
                if ( reach <= allowed && ( best.Source == kNoRecord || reach < best.Distance ) )
                    best = WantedCell{ index, reach, source };
            }
            if ( best.Source != kNoRecord )
                out.push_back( best );
        }
        std::stable_sort( out.begin(), out.end(),
                          []( const WantedCell& a, const WantedCell& b ) { return a.Distance < b.Distance; } );
        return out;
    }

    void ExpectSameAsReference( const WorldPartitionPlan& plan, const WorldPartitionSerialized& settings,
                                const std::vector<StreamingSource>& sources, const char* what )
    {
        const StreamingWish           wish      = Query( plan, settings, sources );
        const std::vector<WantedCell> reference = EveryCell( plan, settings.Grids.front(), sources );
        ASSERT_EQ( wish.Cells.size(), reference.size() ) << what;
        for ( std::size_t index = 0; index < reference.size(); ++index )
        {
            EXPECT_EQ( wish.Cells[index].Cell, reference[index].Cell ) << what << " rank " << index;
            EXPECT_EQ( wish.Cells[index].Source, reference[index].Source ) << what << " rank " << index;
            EXPECT_NEAR( wish.Cells[index].Distance, reference[index].Distance, 1e-6 )
                 << what << " rank " << index;
        }
    }
} // namespace

// ── 1. THE RULE ─────────────────────────────────────────────────────────────────────────────────────

TEST( WorldPartitionStreaming, ACellExactlyAtLoadingRangeIsWantedAndOneCentimetreFurtherIsNot )
{
    // Level-0 cells 128 m wide, range 256 m. Cell (2,0) spans X [25600, 38400]; cell (-3,0) spans
    // [-38400, -25600]. Both are exactly 25600 from a source at X = 0, Z inside the row.
    const WorldPartitionPlan       plan     = PlanOf( { { 0, 2, 0 }, { 0, -3, 0 }, { 0, 2, 2 } } );
    const WorldPartitionSerialized settings = Grid( 12800.0f, 25600.0f );

    const StreamingWish atRange = Query( plan, settings, { At( 0.0f, 5000.0f ) } );
    EXPECT_EQ( Coordinates( plan, atRange ),
               ( std::vector<std::tuple<int, int, int>>{ { 0, -3, 0 }, { 0, 2, 0 } } ) );
    ASSERT_EQ( atRange.Cells.size(), 2u );
    EXPECT_DOUBLE_EQ( atRange.Cells[0].Distance, 25600.0 );

    // A centimetre towards the negative side: (-3,0) is now 25599 away, (2,0) is 25601 and drops out.
    const StreamingWish shifted = Query( plan, settings, { At( -1.0f, 5000.0f ) } );
    EXPECT_EQ( Coordinates( plan, shifted ), ( std::vector<std::tuple<int, int, int>>{ { 0, -3, 0 } } ) );

    // (2,2) is 25600 along each axis — 36204 on the diagonal — so the corner, not the axis, decides.
    const StreamingWish corner = Query( plan, settings, { At( 0.0f, 0.0f ) } );
    EXPECT_EQ( Coordinates( plan, corner ),
               ( std::vector<std::tuple<int, int, int>>{ { 0, -3, 0 }, { 0, 2, 0 } } ) );
}

TEST( WorldPartitionStreaming, AHighLevelCellIsWantedFromWhereItsCentreIsOutOfRange )
{
    // Level 3 of a 128 m grid is 1024 m. Its cell (0,0) spans X [0, 102400]; a source at X = 120000 is
    // 17600 from that square and 68800 from its centre. The level-0 cell (0,0) — [0, 12800] — is 107200
    // away and is not wanted: the level, not only the coordinate, decides the square.
    const WorldPartitionPlan       plan     = PlanOf( { { 3, 0, 0 }, { 0, 0, 0 } } );
    const WorldPartitionSerialized settings = Grid( 12800.0f, 25600.0f );

    const StreamingWish wish = Query( plan, settings, { At( 120000.0f, 51200.0f ) } );
    EXPECT_EQ( Coordinates( plan, wish ), ( std::vector<std::tuple<int, int, int>>{ { 3, 0, 0 } } ) );
    ASSERT_EQ( wish.Cells.size(), 1u );
    EXPECT_DOUBLE_EQ( wish.Cells[0].Distance, 17600.0 );
}

TEST( WorldPartitionStreaming, HeightDoesNotMoveASourceAwayFromACell )
{
    const WorldPartitionPlan       plan     = PlanOf( { { 0, 0, 0 } } );
    const WorldPartitionSerialized settings = Grid( 12800.0f, 25600.0f );
    const StreamingWish            wish =
         Query( plan, settings, { StreamingSource{ glm::vec3( 6400.0f, 1.0e6f, 6400.0f ), 1.0f } } );
    ASSERT_EQ( wish.Cells.size(), 1u );
    EXPECT_DOUBLE_EQ( wish.Cells[0].Distance, 0.0 );
}

// ── 2. THE SET ──────────────────────────────────────────────────────────────────────────────────────

TEST( WorldPartitionStreaming, AlwaysLoadedCompositesAreWantedWithNoSourceAtAll )
{
    // Through the planner: a sun (GLOBAL), a marked rock, and a crate in a cell.
    std::vector<EntityData> records( 3 );
    records[0].id                           = Common::UUID( 1 );
    records[0].Translation                  = glm::vec3( 0.0f );
    records[0].Components["DirectionLight"] = Common::Json::Value( Common::Json::Object{} );
    records[1].id                           = Common::UUID( 2 );
    records[1].Translation                  = glm::vec3( 90000.0f, 0.0f, 90000.0f );
    records[1].Components["AlwaysLoaded"]   = Common::Json::Value( Common::Json::Object{} );
    records[2].id                           = Common::UUID( 3 );
    records[2].Translation                  = glm::vec3( 500.0f, 0.0f, 500.0f );

    const WorldPartitionSerialized settings = Grid( 12800.0f, 25600.0f );
    const WorldPartitionPlan       plan     = PlanWorldPartition( records, settings );
    ASSERT_EQ( plan.AlwaysLoaded.size(), 2u );
    ASSERT_EQ( plan.Cells.size(), 1u );

    const StreamingWish none = Query( plan, settings, {} );
    EXPECT_EQ( none.AlwaysLoaded, plan.AlwaysLoaded );
    EXPECT_TRUE( none.Cells.empty() );

    // With a source far from everything, the always-loaded set is still all there, and no cell is.
    const StreamingWish distant = Query( plan, settings, { At( -1.0e6f, -1.0e6f ) } );
    EXPECT_EQ( distant.AlwaysLoaded, plan.AlwaysLoaded );
    EXPECT_TRUE( distant.Cells.empty() );
}

TEST( WorldPartitionStreaming, TwoSourcesAreAUnionAndEachCellGoesToItsNearestSource )
{
    // Two sources 1 km apart on a row of cells: each wants its own neighbourhood, and cell (4,0) — equally
    // wanted by neither at first — is wanted once, at the nearer source.
    std::vector<std::tuple<int, int, int>> row;
    for ( int x = -2; x <= 10; ++x )
        row.emplace_back( 0, x, 0 );
    const WorldPartitionPlan       plan     = PlanOf( row );
    const WorldPartitionSerialized settings = Grid( 12800.0f, 25600.0f );

    const StreamingWish left  = Query( plan, settings, { At( 6400.0f, 6400.0f ) } );
    const StreamingWish right = Query( plan, settings, { At( 108800.0f, 6400.0f ) } );
    const StreamingWish both  = Query( plan, settings, { At( 6400.0f, 6400.0f ), At( 108800.0f, 6400.0f ) } );

    EXPECT_EQ( both.Cells.size(), left.Cells.size() + right.Cells.size() )
         << "the two neighbourhoods do not overlap";
    for ( const WantedCell& wanted : both.Cells )
    {
        const int x = plan.Cells[wanted.Cell].Cell.X;
        EXPECT_EQ( wanted.Source, x <= 2 ? 0u : 1u ) << "cell " << x;
    }

    // Overlapping: sources 3 cells apart share cells; each shared one is listed once, at its nearer source.
    const StreamingWish overlap = Query( plan, settings, { At( 6400.0f, 6400.0f ), At( 44800.0f, 6400.0f ) } );
    std::vector<std::size_t> seen;
    seen.reserve( overlap.Cells.size() );
    for ( const WantedCell& wanted : overlap.Cells )
        seen.push_back( wanted.Cell );
    std::sort( seen.begin(), seen.end() );
    EXPECT_EQ( std::adjacent_find( seen.begin(), seen.end() ), seen.end() ) << "a cell was listed twice";
    ExpectSameAsReference( plan, settings, { At( 6400.0f, 6400.0f ), At( 44800.0f, 6400.0f ) }, "overlap" );
}

TEST( WorldPartitionStreaming, AnEmptyPlanWantsNothingAndIsNotAnError )
{
    const WorldPartitionPlan empty;
    const auto               wish = QueryStreamingCells( empty, WorldPartitionSerialized{},
                                                         std::vector<StreamingSource>{ At( 0.0f, 0.0f ) } );
    ASSERT_TRUE( wish.IsSuccess() ) << wish.GetError();
    EXPECT_TRUE( wish.GetValue().Cells.empty() );
    EXPECT_TRUE( wish.GetValue().AlwaysLoaded.empty() );
}

TEST( WorldPartitionStreaming, ABadSourceOrGridIsRefusedByNameNotAnsweredEmpty )
{
    const WorldPartitionPlan       plan     = PlanOf( { { 0, 0, 0 } } );
    const WorldPartitionSerialized settings = Grid( 12800.0f, 25600.0f );

    const auto nan = QueryStreamingCells(
         plan, settings, std::vector<StreamingSource>{ At( 0.0f, 0.0f ), At( std::nanf( "" ), 0.0f ) } );
    ASSERT_FALSE( nan.IsSuccess() );
    EXPECT_NE( nan.GetError().find( "source 1" ), std::string::npos ) << nan.GetError();

    const auto negative =
         QueryStreamingCells( plan, settings, std::vector<StreamingSource>{ At( 0.0f, 0.0f, -1.0f ) } );
    ASSERT_FALSE( negative.IsSuccess() );
    EXPECT_NE( negative.GetError().find( "RangeScale" ), std::string::npos ) << negative.GetError();

    const auto noGrid =
         QueryStreamingCells( plan, WorldPartitionSerialized{}, std::vector<StreamingSource>{ At( 0.0f, 0.0f ) } );
    ASSERT_FALSE( noGrid.IsSuccess() );
    EXPECT_NE( noGrid.GetError().find( "no grid" ), std::string::npos ) << noGrid.GetError();

    const auto zeroCell =
         QueryStreamingCells( plan, Grid( 0.0f, 25600.0f ), std::vector<StreamingSource>{ At( 0.0f, 0.0f ) } );
    ASSERT_FALSE( zeroCell.IsSuccess() );
    EXPECT_NE( zeroCell.GetError().find( "CellSize" ), std::string::npos ) << zeroCell.GetError();
}

// ── 3. THE ORDER ────────────────────────────────────────────────────────────────────────────────────

TEST( WorldPartitionStreaming, NearestFirstPlanOrderAmongEqualsAndSourceOrderDoesNotMatter )
{
    // A source inside level-0 cell (0,0) is also inside level-1 (0,0) and level-2 (0,0): three cells at
    // distance zero, finer first. Then the ring at 12800 cm and so on, in (level, X, Z) among equals.
    std::vector<std::tuple<int, int, int>> cells;
    for ( int x = -3; x <= 3; ++x )
    {
        for ( int z = -3; z <= 3; ++z )
            cells.emplace_back( 0, x, z );
    }
    cells.emplace_back( 1, 0, 0 );
    cells.emplace_back( 2, 0, 0 );
    const WorldPartitionPlan       plan     = PlanOf( cells );
    const WorldPartitionSerialized settings = Grid( 12800.0f, 25600.0f );

    const StreamingWish one = Query( plan, settings, { At( 6400.0f, 6400.0f ) } );
    ASSERT_GE( one.Cells.size(), 3u );
    EXPECT_EQ( Coordinates( plan, one )[0], std::make_tuple( 0, 0, 0 ) );
    EXPECT_EQ( Coordinates( plan, one )[1], std::make_tuple( 1, 0, 0 ) );
    EXPECT_EQ( Coordinates( plan, one )[2], std::make_tuple( 2, 0, 0 ) );
    for ( std::size_t rank = 1; rank < one.Cells.size(); ++rank )
    {
        const WantedCell& before = one.Cells[rank - 1];
        const WantedCell& after  = one.Cells[rank];
        EXPECT_TRUE( before.Distance < after.Distance ||
                     ( before.Distance == after.Distance && before.Cell < after.Cell ) )
             << "rank " << rank;
    }

    // Three sources, every permutation: the same cells, in the same order, at the same distances.
    std::vector<StreamingSource> sources = { At( 6400.0f, 6400.0f ), At( -30000.0f, 12000.0f ),
                                             At( 25600.0f, -25600.0f ) };
    const StreamingWish          first   = Query( plan, settings, sources );
    std::sort( sources.begin(), sources.end(),
               []( const StreamingSource& a, const StreamingSource& b ) { return a.Position.x < b.Position.x; } );
    do
    {
        const StreamingWish again = Query( plan, settings, sources );
        ASSERT_EQ( again.Cells.size(), first.Cells.size() );
        for ( std::size_t rank = 0; rank < first.Cells.size(); ++rank )
        {
            EXPECT_EQ( again.Cells[rank].Cell, first.Cells[rank].Cell ) << "rank " << rank;
            EXPECT_EQ( again.Cells[rank].Distance, first.Cells[rank].Distance ) << "rank " << rank;
        }
    } while ( std::next_permutation( sources.begin(), sources.end(),
                                     []( const StreamingSource& a, const StreamingSource& b )
                                     { return a.Position.x < b.Position.x; } ) );
}

// ── 4. THE REFERENCE ────────────────────────────────────────────────────────────────────────────────

TEST( WorldPartitionStreaming, RandomPlansAgreeWithAWalkOverEveryCell )
{
    std::mt19937 random( 20260924u );
    for ( int round = 0; round < 200; ++round )
    {
        // Up to four levels; coordinates either side of the origin; a sparse world, so the enumeration
        // mostly misses and the scan path is taken where a level holds few cells.
        std::uniform_int_distribution<int>     levels( 1, 4 );
        std::uniform_int_distribution<int>     count( 0, 300 );
        const int                              levelCount = levels( random );
        std::vector<std::tuple<int, int, int>> cells;
        const int                              total = count( random );
        for ( int index = 0; index < total; ++index )
        {
            const int level = std::uniform_int_distribution<int>( 0, levelCount - 1 )( random );
            const int reach = 24 >> level;
            std::uniform_int_distribution<int> coordinate( -reach, reach );
            cells.emplace_back( level, coordinate( random ), coordinate( random ) );
        }
        const WorldPartitionPlan       plan = PlanOf( cells );
        const float                    size = std::uniform_real_distribution<float>( 1000.0f, 20000.0f )( random );
        const float                    range = std::uniform_real_distribution<float>( 0.0f, 60000.0f )( random );
        const WorldPartitionSerialized settings = Grid( size, range );

        std::vector<StreamingSource>          sources;
        std::uniform_real_distribution<float> where( -30.0f * size, 30.0f * size );
        const std::array<float, 5>            scales      = { 0.0f, 0.5f, 1.0f, 2.0f, 40.0f };
        const int                             sourceCount = std::uniform_int_distribution<int>( 1, 3 )( random );
        sources.reserve( sourceCount );
        for ( int index = 0; index < sourceCount; ++index )
            sources.push_back( At( where( random ), where( random ),
                                   scales[std::uniform_int_distribution<std::size_t>( 0, 4 )( random )] ) );

        ExpectSameAsReference( plan, settings, sources, ( "round " + std::to_string( round ) ).c_str() );
        if ( HasFailure() )
            return;
    }
}

TEST( WorldPartitionStreaming, APlannedWorldAgreesWithAWalkOverEveryCell )
{
    // The query's index is the plan's own sort; this holds it to the planner's output, not a hand-built
    // plan. Boxes of every width either side of the origin, so several levels are populated.
    std::mt19937                          random( 7u );
    std::uniform_real_distribution<float> where( -400000.0f, 400000.0f );
    std::vector<EntityData>               records;
    for ( std::uint64_t id = 1; id <= 2000; ++id )
    {
        EntityData data;
        data.id          = Common::UUID( id );
        data.Translation = glm::vec3( where( random ), 0.0f, where( random ) );
        records.push_back( data );
        if ( id % 7 == 0 )
        {
            // A child 30 m away: the pair straddles a boundary often enough to promote.
            EntityData child;
            child.id          = Common::UUID( 100000 + id );
            child.parent      = Common::UUID( id );
            child.Translation = glm::vec3( 3000.0f * static_cast<float>( id % 5 ), 0.0f, -8000.0f );
            records.push_back( child );
        }
    }
    const WorldPartitionSerialized settings = Grid( 12800.0f, 25600.0f );
    const WorldPartitionPlan       plan     = PlanWorldPartition( records, settings );
    ASSERT_GT( plan.LevelCount, 1 );
    ASSERT_TRUE( std::any_of( plan.Cells.begin(), plan.Cells.end(),
                              []( const PlannedCell& cell ) { return cell.Level > 0; } ) );

    for ( int probe = 0; probe < 50; ++probe )
        ExpectSameAsReference(
             plan, settings,
             { At( where( random ), where( random ) ), At( where( random ), where( random ), 3.0f ) }, "planned" );
}

// ── 5. THE COST ─────────────────────────────────────────────────────────────────────────────────────

TEST( WorldPartitionStreaming, OneQueryOnAFiftyThousandCellWorldMeasuresAFewDozenSquares )
{
    // 224 × 224 level-0 cells (28.7 km square at 128 m) plus the levels above them: ~66 900 cells.
    std::vector<std::tuple<int, int, int>> cells;
    for ( int level = 0; level < 5; ++level )
    {
        const int side = 224 >> level;
        for ( int x = -side / 2; x < side / 2; ++x )
        {
            for ( int z = -side / 2; z < side / 2; ++z )
                cells.emplace_back( level, x, z );
        }
    }
    const WorldPartitionPlan       plan     = PlanOf( cells );
    const WorldPartitionSerialized settings = Grid( 12800.0f, 25600.0f );
    ASSERT_GE( plan.Cells.size(), 50000u );

    std::mt19937                              random( 11u );
    std::uniform_real_distribution<float>     where( -1.4e6f, 1.4e6f );
    std::vector<std::vector<StreamingSource>> probes;
    probes.reserve( 1000 );
    for ( int index = 0; index < 1000; ++index )
        probes.push_back( { At( where( random ), where( random ) ) } );

    std::size_t examined = 0;
    std::size_t wanted   = 0;
    const auto  start    = std::chrono::steady_clock::now();
    for ( const auto& probe : probes )
    {
        const StreamingWish wish = Query( plan, settings, probe );
        examined                 = std::max( examined, wish.CellsExamined );
        wanted += wish.Cells.size();
    }
    const double perQuery =
         std::chrono::duration<double, std::micro>( std::chrono::steady_clock::now() - start ).count() /
         static_cast<double>( probes.size() );

    const auto  referenceStart  = std::chrono::steady_clock::now();
    std::size_t referenceWanted = 0;
    for ( std::size_t index = 0; index < 100; ++index )
        referenceWanted += EveryCell( plan, settings.Grids.front(), probes[index] ).size();
    const double perReference =
         std::chrono::duration<double, std::micro>( std::chrono::steady_clock::now() - referenceStart ).count() /
         100.0;

    std::cout << "[ streaming ] " << plan.Cells.size() << " cells, 1 source: " << perQuery << " us/query, at most "
              << examined << " squares measured, "
              << static_cast<double>( wanted ) / static_cast<double>( probes.size() )
              << " cells wanted on average; walk over every cell " << perReference << " us/query ("
              << referenceWanted << " wanted over 100)\n";

    // Five levels, each a circle of 256 m over its own cells: at most 6 × 6 squares per level.
    EXPECT_LE( examined, 5u * 36u );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
