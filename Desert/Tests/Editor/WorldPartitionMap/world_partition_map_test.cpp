// THE WORLD PARTITION PANEL'S VIEW (Editor/Panels/WorldPartition/WorldPartitionMap.hpp).
//
// WHAT IS ASSERTED:
//   1. The transform is UE's: origin at the centre, +X right, +Z down, and ScreenToWorld inverts it.
//   2. A wheel notch keeps the world point under the cursor fixed; a drag moves the world with the cursor.
//   3. Focus and Follow put the box / the source at the centre at UE's fill.
//   4. Visible cells: off-screen cells are culled, coarse levels paint first, the level filter holds.
//   5. THE RELATION: the cell the streaming source stands in, after the REAL executor began around it, is
//      Resident and paints UE's "visible" green; a far cell is Unloaded red; Edit has no state.

#include <Editor/Panels/WorldPartition/WorldPartitionMap.hpp>
#include <Engine/Core/Serialize/WorldPartitionResidencyExecutor.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <set>
#include <vector>

namespace Map = Desert::Editor::WorldPartitionMap;
using Desert::Assets::EntityData;
using Desert::Core::WorldPartitionGridSerialized;
using Desert::Core::WorldPartitionSerialized;
using Desert::Core::Rules::CellBounds;
using Desert::Core::Rules::PlanWorldPartition;
using Desert::Core::Rules::Residency;
using Desert::Core::Rules::ResidencyExecutor;
using Desert::Core::Rules::ResidencySettings;
using Desert::Core::Rules::ResidencyState;
using Desert::Core::Rules::ResidencyWorld;
using Desert::Core::Rules::StreamingSource;
using Desert::Core::Rules::WorldPartitionPlan;

namespace
{
    constexpr float  kCell  = 1000.0f;
    constexpr float  kRange = 1500.0f;
    const glm::dvec2 kScreenSize{ 800.0, 600.0 };
    constexpr double kEps = 1e-6;

    WorldPartitionSerialized Grid()
    {
        WorldPartitionSerialized partition;
        partition.Grids.push_back( WorldPartitionGridSerialized{ kCell, kRange } );
        return partition;
    }

    EntityData Record( std::uint64_t id, glm::vec3 at )
    {
        EntityData data;
        data.id          = Common::UUID( id );
        data.Tag         = "Block";
        data.Translation = at;
        return data;
    }

    // One record in the middle of each cell of a 12 x 3 strip along +X.
    std::vector<EntityData> Strip()
    {
        std::vector<EntityData> records;
        for ( int column = 0; column < 12; ++column )
            for ( int row = 0; row < 3; ++row )
                records.push_back( Record( 1000 + static_cast<std::uint64_t>( column * 10 + row ),
                                           { ( static_cast<float>( column ) + 0.5f ) * kCell, 0.0f,
                                             ( static_cast<float>( row ) + 0.5f ) * kCell } ) );
        return records;
    }

    std::size_t CellIndex( const WorldPartitionPlan& plan, int level, int x, int z )
    {
        for ( std::size_t cell = 0; cell < plan.Cells.size(); ++cell )
            if ( plan.Cells[cell].Level == level && plan.Cells[cell].Cell.X == x && plan.Cells[cell].Cell.Z == z )
                return cell;
        ADD_FAILURE() << "no cell L" << level << " (" << x << ", " << z << ")";
        return 0;
    }

    struct SetWorld final : ResidencyWorld
    {
        std::set<std::size_t> Live;
        Common::BoolResultStr Activate( std::size_t, std::span<const std::size_t> records ) override
        {
            Live.insert( records.begin(), records.end() );
            return Common::MakeSuccess( true );
        }
        void Destroy( std::span<const std::size_t> records ) override
        {
            for ( const std::size_t record : records )
                Live.erase( record );
        }
    };

    void ExpectNear( glm::dvec2 a, glm::dvec2 b, double eps = kEps )
    {
        EXPECT_NEAR( a.x, b.x, eps );
        EXPECT_NEAR( a.y, b.y, eps );
    }
} // namespace

TEST( WorldPartitionMap, TheOriginIsTheCentreXIsRightZIsDown )
{
    const Map::View view{ glm::dvec2( 0.0 ), 0.1 };
    ExpectNear( Map::WorldToScreen( view, kScreenSize, { 0.0, 0.0 } ), { 400.0, 300.0 } );
    ExpectNear( Map::WorldToScreen( view, kScreenSize, { 1000.0, 0.0 } ), { 500.0, 300.0 } );
    ExpectNear( Map::WorldToScreen( view, kScreenSize, { 0.0, 1000.0 } ), { 400.0, 400.0 } );

    const Map::View  moved{ glm::dvec2( -250.0, 730.0 ), 0.037 };
    const glm::dvec2 world{ 12345.0, -678.0 };
    ExpectNear( Map::ScreenToWorld( moved, kScreenSize, Map::WorldToScreen( moved, kScreenSize, world ) ), world,
                1e-6 );
}

TEST( WorldPartitionMap, AWheelNotchKeepsTheWorldUnderTheCursor )
{
    Map::View        view{ glm::dvec2( 300.0, -200.0 ), 0.05 };
    const glm::dvec2 mouse{ 610.0, 140.0 };
    const glm::dvec2 before = Map::ScreenToWorld( view, kScreenSize, mouse );
    Map::Zoom( view, kScreenSize, mouse, 1.0f );
    EXPECT_GT( view.Scale, 0.05 );
    ExpectNear( Map::ScreenToWorld( view, kScreenSize, mouse ), before, 1e-6 );
    Map::Zoom( view, kScreenSize, mouse, -3.0f );
    EXPECT_LT( view.Scale, 0.05 );
    ExpectNear( Map::ScreenToWorld( view, kScreenSize, mouse ), before, 1e-6 );

    for ( int notch = 0; notch < 400; ++notch )
        Map::Zoom( view, kScreenSize, mouse, 8.0f );
    EXPECT_DOUBLE_EQ( view.Scale, Map::kMaxScale );
}

TEST( WorldPartitionMap, ADragMovesTheWorldWithTheCursor )
{
    Map::View        view{ glm::dvec2( 0.0 ), 0.2 };
    const glm::dvec2 grabbed{ 100.0, 100.0 };
    const glm::dvec2 world = Map::ScreenToWorld( view, kScreenSize, grabbed );
    Map::Pan( view, { 35.0, -12.0 } );
    ExpectNear( Map::WorldToScreen( view, kScreenSize, world ), grabbed + glm::dvec2( 35.0, -12.0 ) );
}

TEST( WorldPartitionMap, FocusAndFollowCentreWhatTheyShow )
{
    Map::View        view;
    const CellBounds box{ 1000.0f, -2000.0f, 5000.0f, 0.0f }; // 4000 x 2000 cm around (3000, -1000)
    Map::Focus( view, kScreenSize, box );
    ExpectNear( Map::WorldToScreen( view, kScreenSize, { 3000.0, -1000.0 } ), kScreenSize * 0.5 );
    // Width is the tighter side: 800 px over 4000 cm at 85 %.
    EXPECT_NEAR( view.Scale, 800.0 / 4000.0 * Map::kFocusFill, kEps );

    const Map::View follow = Map::Follow( kScreenSize, { -4200.0, 900.0 } );
    ExpectNear( Map::WorldToScreen( follow, kScreenSize, { -4200.0, 900.0 } ), kScreenSize * 0.5 );
    EXPECT_NEAR( follow.Scale, 300.0 / Map::kFollowExtentCm, kEps );
}

TEST( WorldPartitionMap, OnlyCellsOnScreenArePaintedCoarseFirst )
{
    const WorldPartitionPlan plan = PlanWorldPartition( Strip(), Grid() );
    ASSERT_EQ( plan.Cells.size(), 36u );

    // 1 px per 10 cm around (1500, 1500): 8000 x 6000 cm, so columns 0..5 and all rows.
    const Map::View view{ glm::dvec2( -1500.0, -1500.0 ), 0.1 };
    const auto      visible = Map::VisibleCells( plan, view, kScreenSize, -1 );
    for ( const std::size_t cell : visible )
        EXPECT_LE( plan.Cells[cell].Cell.X, 5 ) << "an off-screen cell was kept";
    EXPECT_EQ( visible.size(), 6u * 3u );

    for ( std::size_t i = 1; i < visible.size(); ++i )
        EXPECT_GE( plan.Cells[visible[i - 1]].Level, plan.Cells[visible[i]].Level );
    EXPECT_TRUE( Map::VisibleCells( plan, view, kScreenSize, 3 ).empty() );

    const auto under = Map::CellAt( plan, { 2500.0, 1500.0 }, -1 );
    if ( !under.has_value() )
    {
        ADD_FAILURE() << "no cell under (2500, 1500)";
        return;
    }
    EXPECT_EQ( *under, CellIndex( plan, 0, 2, 1 ) );
    EXPECT_FALSE( Map::CellAt( plan, { -5000.0, 1500.0 }, -1 ).has_value() );
}

TEST( WorldPartitionMap, GridLevelsAndLabels )
{
    const Map::View view{ glm::dvec2( 0.0 ), 0.01 }; // a 1000 cm cell is 10 px
    EXPECT_EQ( Map::GridLineLevel( view, kCell, 10.0 ), 0 );
    EXPECT_EQ( Map::GridLineLevel( view, kCell, 11.0 ), 1 );
    EXPECT_EQ( Map::GridLineLevel( view, kCell, 40.0 ), 2 );
    EXPECT_EQ( Map::LevelLabel( 12800.0f, 0 ), "L0 · 128 m" );
    EXPECT_EQ( Map::LevelLabel( 12800.0f, 3 ), "L3 · 1024 m" );
    EXPECT_EQ( Map::LevelLabel( 50.0f, 0 ), "L0 · 0.5 m" );
}

TEST( WorldPartitionMap, TheCellUnderTheStreamingSourceIsResidentAndPaintsResident )
{
    const std::vector<EntityData> records = Strip();
    const WorldPartitionPlan      plan    = PlanWorldPartition( records, Grid() );
    SetWorld                      world;
    for ( std::size_t record = 0; record < records.size(); ++record )
        world.Live.insert( record );
    const StreamingSource source{ { 2.5f * kCell, 0.0f, 1.5f * kCell } };
    auto                  begun =
         ResidencyExecutor::Begin( plan, Grid(), records, ResidencySettings{}, std::span( &source, 1 ), world );
    ASSERT_TRUE( begun.IsSuccess() ) << begun.GetError();
    const ResidencyExecutor executor = begun.ExtractValue();

    const std::size_t under = CellIndex( executor.Plan(), 0, 2, 1 );
    EXPECT_EQ( Map::StateOf( executor.Plan(), &executor.State(), under ), Map::CellState::Resident );
    EXPECT_EQ( Map::ColorOf( Map::StateOf( executor.Plan(), &executor.State(), under ) ),
               glm::vec4( 0.0f, 1.0f, 0.0f, 0.25f ) );

    const std::size_t distant = CellIndex( executor.Plan(), 0, 11, 1 );
    EXPECT_EQ( Map::StateOf( executor.Plan(), &executor.State(), distant ), Map::CellState::Unloaded );
    EXPECT_EQ( Map::ColorOf( Map::CellState::Unloaded ), glm::vec4( 1.0f, 0.0f, 0.0f, 0.25f ) );

    EXPECT_EQ( Map::StateOf( plan, nullptr, under ), Map::CellState::Unstreamed );
}

TEST( WorldPartitionMap, EveryResidencyHasItsOwnColour )
{
    WorldPartitionPlan plan;
    plan.AlwaysLoaded = { 0 }; // unit 0 is the composite; cell c is unit c + 1
    plan.Cells.resize( 5 );
    ResidencyState state;
    state.Units.resize( 6 );
    const Residency order[] = { Residency::Unloaded, Residency::Loading, Residency::Loaded, Residency::Activated,
                                Residency::Failed };
    const Map::CellState  expected[] = { Map::CellState::Unloaded, Map::CellState::Loading, Map::CellState::Loaded,
                                         Map::CellState::Resident, Map::CellState::Failed };
    std::set<std::string> names;
    for ( std::size_t cell = 0; cell < 5; ++cell )
    {
        state.Units[cell + 1].State = order[cell];
        EXPECT_EQ( Map::StateOf( plan, &state, cell ), expected[cell] ) << cell;
        names.insert( Map::NameOf( expected[cell] ) );
    }
    EXPECT_EQ( names.size(), 5u );
    EXPECT_EQ( Map::ColorOf( Map::CellState::Loading ), glm::vec4( 1.0f, 1.0f, 0.0f, 0.25f ) );
    EXPECT_EQ( Map::ColorOf( Map::CellState::Loaded ), glm::vec4( 0.0f, 1.0f, 1.0f, 0.25f ) );
    EXPECT_NEAR( Map::RadiusPixels( Map::View{ glm::dvec2( 0.0 ), 0.02 }, 25600.0 ), 512.0, kEps );
}

// THE LEGEND ADDS UP. A Play frame showed 23 Resident + 33 Unloaded for a 64-cell partition: the legend counted
// the cells on screen, and Follow keeps 100 m on screen. Whatever residency every cell is in, at every level
// filter, the rows sum to the number of cells at that level, and each residency has a row of its own.
TEST( WorldPartitionMap, TheLegendNamesEveryStateAndSumsToTheCellCount )
{
    const std::vector<EntityData> records = Strip();
    const WorldPartitionPlan      plan    = PlanWorldPartition( records, Grid() );
    ASSERT_FALSE( plan.Cells.empty() );

    const auto sum = []( const std::vector<Map::LegendRow>& rows )
    {
        std::size_t total = 0;
        for ( const Map::LegendRow& row : rows )
            total += row.Count;
        return total;
    };
    const auto cellsAt = [&plan]( int level )
    {
        std::size_t count = 0;
        for ( const auto& cell : plan.Cells )
            count += ( level < 0 || cell.Level == level ) ? 1u : 0u;
        return count;
    };

    for ( const int level : { -1, 0, 1 } )
    {
        EXPECT_EQ( sum( Map::Legend( plan, nullptr, level ) ), cellsAt( level ) ) << "Edit, level " << level;

        ResidencyState before; // before the first step: no units yet, every cell Unloaded
        EXPECT_EQ( sum( Map::Legend( plan, &before, level ) ), cellsAt( level ) )
             << "empty state, level " << level;

        for ( const Residency residency : { Residency::Unloaded, Residency::Loading, Residency::Loaded,
                                            Residency::Activated, Residency::Failed } )
        {
            ResidencyState state;
            state.Units.resize( plan.AlwaysLoaded.size() + plan.Cells.size() );
            for ( auto& unit : state.Units )
                unit.State = residency;
            const std::vector<Map::LegendRow> rows = Map::Legend( plan, &state, level );
            EXPECT_EQ( sum( rows ), cellsAt( level ) )
                 << "residency " << static_cast<int>( residency ) << ", level " << level;
            if ( cellsAt( level ) == 0 )
                continue;
            const Map::CellState expected = Map::StateOf( plan, &state, 0 );
            const auto           row      = std::ranges::find( rows, expected, &Map::LegendRow::State );
            if ( row == rows.end() )
            {
                ADD_FAILURE() << "no legend row for " << Map::NameOf( expected );
                continue;
            }
            EXPECT_EQ( row->Count, cellsAt( level ) ) << Map::NameOf( expected );
        }
    }

    // Every state is named in Play even at zero, and Edit names its one state.
    EXPECT_EQ( Map::LegendStates( true ).size(), 5u );
    EXPECT_EQ( Map::LegendStates( false ).size(), 1u );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
