// A CELL OR ITS HLOD, FRAME BY FRAME — NEVER NEITHER, NEVER BOTH (WP11).
//
// The residency executor is driven here as in world_partition_streamer_test.cpp, against a world that is a set of
// live record indices AND a visibility bit per HLOD, through the one frame function the engine's streamer calls
// (Rules::TickWithHLODs). The HLODs are built by the real builder (WorldPartitionHLODRules.hpp) from records that
// draw a cube each, one HLOD per cell.
//
// WHAT IS ASSERTED — the relation, every frame, for every cell that has an HLOD:
//     (the cell's records are entities)  XOR  (its HLOD is visible)
//
//   1. THE START. After Begin and the first switch, the neighbourhood draws itself and every other cell its HLOD.
//   2. THE FLY-THROUGH, out and back. The relation holds in every frame, including THE frame a cell becomes
//      resident and THE frame it is destroyed — both of which the flight is required to contain.
//   3. READ BUT NOT MADE. A cell that is Loading or Loaded is not drawn, so its HLOD is; the flight is required to
//      contain such a frame.
//   4. A FAILED CELL keeps its HLOD visible.
//   5. ONLY CHANGES ARE TOLD: a second switch on the same state tells the world nothing.
//   6. REFUSALS: an HLOD for an always-loaded unit, two HLODs for one cell, a unit the plan does not have.
//   7. THE HOLES are named by reason.

#include <Common/Json/Document.hpp>
#include <Engine/Core/Serialize/WorldPartitionHLODSwitch.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using Desert::Assets::EntityData;
using Desert::Core::WorldPartitionGridSerialized;
using Desert::Core::WorldPartitionSerialized;
using Desert::Core::Rules::HLODExclusion;
using Desert::Core::Rules::HLODRuntime;
using Desert::Core::Rules::HLODVisibilityWorld;
using Desert::Core::Rules::LoadOutcome;
using Desert::Core::Rules::PlanWorldPartition;
using Desert::Core::Rules::Residency;
using Desert::Core::Rules::ResidencyExecutor;
using Desert::Core::Rules::ResidencySettings;
using Desert::Core::Rules::ResidencyUnitMembers;
using Desert::Core::Rules::ResidencyWorld;
using Desert::Core::Rules::StreamingSource;
using Desert::Core::Rules::WorldPartitionPlan;

namespace
{
    constexpr float  kCell    = 1000.0f;
    constexpr float  kRange   = 1500.0f;
    constexpr int    kColumns = 16;
    constexpr int    kRows    = 3;
    constexpr double kFrame   = 1.0 / 64.0;

    WorldPartitionSerialized Grid()
    {
        WorldPartitionSerialized partition;
        partition.Grids.push_back( WorldPartitionGridSerialized{ kCell, kRange } );
        return partition;
    }

    glm::vec3 CellCentre( int column, int row )
    {
        return { ( static_cast<float>( column ) + 0.5f ) * kCell, 0.0f,
                 ( static_cast<float>( row ) + 0.5f ) * kCell };
    }

    EntityData Cube( std::uint64_t id, glm::vec3 translation )
    {
        EntityData data;
        data.id          = Common::UUID( id );
        data.Tag         = "Cube";
        data.Translation = translation;
        Desert::Assets::StaticMeshComponentSer mesh;
        mesh.Primitive                = Desert::Geometry::PrimitiveType::Cube;
        data.Components["StaticMesh"] = Common::Json::FromStruct( mesh );
        return data;
    }

    // A camera (always-loaded) and a strip of cells along +X, two cubes per cell.
    std::vector<EntityData> World()
    {
        std::vector<EntityData> records;
        EntityData              camera;
        camera.id          = Common::UUID( 1 );
        camera.Tag         = "Camera";
        camera.Translation = { 50.0f, 0.0f, 50.0f };
        auto block         = Common::Json::Parse( R"({"IsMainCamera": true})" );
        EXPECT_TRUE( block.IsSuccess() );
        camera.Components["Camera"] = block.ExtractValue();
        records.push_back( camera );
        for ( int column = 0; column < kColumns; ++column )
            for ( int row = 0; row < kRows; ++row )
            {
                const auto id = 1000 + static_cast<std::uint64_t>( column * 10 + row ) * 2;
                records.push_back( Cube( id, CellCentre( column, row ) ) );
                records.push_back( Cube( id + 1, CellCentre( column, row ) + glm::vec3( 100.0f, 0.0f, 0.0f ) ) );
            }
        return records;
    }

    // The engine's world is a Scene; this one is the records that are entities and each HLOD's Visible bit.
    struct HLODWorld final : ResidencyWorld, HLODVisibilityWorld
    {
        std::set<std::size_t> Live;
        std::vector<int>      Visible; // per HLOD: -1 never told, 0 hidden, 1 shown
        std::size_t           Switches  = 0;
        bool                  FailLoads = false;

        explicit HLODWorld( std::size_t records )
        {
            for ( std::size_t record = 0; record < records; ++record )
                Live.insert( record );
        }

        Common::BoolResultStr Activate( std::size_t /*unit*/, std::span<const std::size_t> records ) override
        {
            Live.insert( records.begin(), records.end() );
            return Common::MakeSuccess( true );
        }
        void Destroy( std::span<const std::size_t> records ) override
        {
            for ( const std::size_t record : records )
                Live.erase( record );
        }
        void StartLoad( std::size_t unit, std::uint64_t ticket ) override
        {
            m_Loads.push_back( { unit, ticket, !FailLoads, FailLoads ? "the disk is gone" : "" } );
        }
        std::vector<LoadOutcome> TakeLoadOutcomes() override
        {
            return std::exchange( m_Loads, {} );
        }
        void SetHLODVisible( std::size_t hlod, bool visible ) override
        {
            Visible.at( hlod ) = visible ? 1 : 0;
            ++Switches;
        }

    private:
        std::vector<LoadOutcome> m_Loads;
    };

    struct Streaming
    {
        std::vector<EntityData>          Records;
        HLODWorld                        World;
        std::optional<ResidencyExecutor> Executor;
        std::optional<HLODRuntime>       HLODs;
        std::vector<std::size_t>         Units; // per HLOD: its cell unit
    };

    ResidencySettings Settings()
    {
        ResidencySettings settings;
        settings.ActivationBudgetMs = 1.0;
        settings.MsPerRecord        = 0.4; // two records a frame: a cell a frame, so cells wait Loaded
        return settings;
    }

    // Begin as the streamer does: the executor, then every cell's HLOD from the builder, then the first switch.
    void Begin( Streaming& s, StreamingSource source )
    {
        const WorldPartitionPlan plan = PlanWorldPartition( s.Records, Grid() );
        auto                     begun =
             ResidencyExecutor::Begin( plan, Grid(), s.Records, Settings(), std::span( &source, 1 ), s.World );
        ASSERT_TRUE( begun.IsSuccess() ) << begun.GetError();
        s.Executor = begun.ExtractValue();

        std::unordered_map<Common::UUID, std::size_t> byId;
        for ( std::size_t record = 0; record < s.Records.size(); ++record )
            byId.emplace( *s.Records[record].id, record ); // NOLINT(bugprone-unchecked-optional-access)
        const auto           world = Desert::Core::Rules::Detail::ComposeWorld( s.Records, byId );
        Common::Json::Issues issues;
        for ( std::size_t unit = plan.AlwaysLoaded.size(); unit < Desert::Core::Rules::ResidencyUnitCount( plan );
              ++unit )
        {
            const auto built = Desert::Core::Rules::BuildInstancingHLOD(
                 s.Records, world, ResidencyUnitMembers( plan, unit ), issues );
            if ( !built.Batches.empty() )
                s.Units.push_back( unit );
        }
        ASSERT_TRUE( issues.empty() );
        ASSERT_EQ( s.Units.size(), static_cast<std::size_t>( kColumns * kRows ) ) << "one HLOD per cell";
        auto hlods = HLODRuntime::Make( s.Executor->Plan(), s.Units );
        ASSERT_TRUE( hlods.IsSuccess() ) << hlods.GetError();
        s.HLODs = hlods.ExtractValue();
        s.World.Visible.assign( s.Units.size(), -1 );
        auto synced = s.HLODs->Sync( s.Executor->State(), s.World );
        ASSERT_TRUE( synced.IsSuccess() ) << synced.GetError();
    }

    // Per HLOD: is its cell drawn by its own records right now? A cell is whole or absent — never partly.
    std::vector<bool> CellsDrawn( const Streaming& s )
    {
        std::vector<bool> drawn;
        for ( const std::size_t unit : s.Units )
        {
            std::size_t live    = 0;
            const auto  members = ResidencyUnitMembers( s.Executor->Plan(), unit );
            for ( const std::size_t record : members )
                live += s.World.Live.count( record );
            EXPECT_TRUE( live == 0 || live == members.size() ) << "cell unit " << unit << " is partly resident";
            drawn.push_back( live == members.size() );
        }
        return drawn;
    }

    // THE RELATION: exactly one of the cell and its HLOD is drawn.
    void ExpectOneOfEach( const Streaming& s, const std::string& when )
    {
        const std::vector<bool> drawn = CellsDrawn( s );
        for ( std::size_t hlod = 0; hlod < s.Units.size(); ++hlod )
        {
            ASSERT_NE( s.World.Visible[hlod], -1 ) << when << ": HLOD " << hlod << " was never told";
            EXPECT_NE( drawn[hlod], s.World.Visible[hlod] == 1 )
                 << when << ": cell unit " << s.Units[hlod]
                 << ( drawn[hlod] ? " is drawn AND its HLOD is visible"
                                  : " is not drawn and neither is its HLOD" );
        }
    }

    void Tick( Streaming& s, StreamingSource source, double now )
    {
        auto tick = Desert::Core::Rules::TickWithHLODs( *s.Executor, *s.HLODs, std::span( &source, 1 ), now,
                                                        s.World, s.World );
        ASSERT_TRUE( tick.IsSuccess() ) << tick.GetError();
    }
} // namespace

TEST( WorldPartitionHLODSwitch, AtTheStartEveryCellIsDrawnByItselfOrByItsHLOD )
{
    Streaming s{ World(), HLODWorld( 0 ), {}, {}, {} };
    s.World = HLODWorld( s.Records.size() );
    Begin( s, StreamingSource{ CellCentre( 2, 1 ) } );
    ExpectOneOfEach( s, "after Begin" );
    const std::vector<bool> drawn = CellsDrawn( s );
    const auto              shown = std::count( s.World.Visible.begin(), s.World.Visible.end(), 1 );
    EXPECT_GT( shown, 0 ) << "no cell is away at the start, the strip is too short to show anything";
    EXPECT_LT( static_cast<std::size_t>( shown ), drawn.size() ) << "no cell is resident at the start";
}

TEST( WorldPartitionHLODSwitch, AFlightOutAndBackNeverHasAHoleOrADouble )
{
    Streaming s{ World(), HLODWorld( 0 ), {}, {}, {} };
    s.World = HLODWorld( s.Records.size() );
    Begin( s, StreamingSource{ CellCentre( 0, 1 ) } );

    std::size_t       becameResident = 0;
    std::size_t       wentAway       = 0;
    std::size_t       readNotMade    = 0;
    std::vector<bool> before         = CellsDrawn( s );
    double            now            = 0.0;
    int               frame          = 0;
    const auto        fly            = [&]( float x )
    {
        Tick( s, StreamingSource{ { x, 0.0f, CellCentre( 0, 1 ).z } }, now );
        now += kFrame;
        ++frame;
        ExpectOneOfEach( s, "frame " + std::to_string( frame ) + " at x " + std::to_string( x ) );
        const std::vector<bool> after = CellsDrawn( s );
        for ( std::size_t hlod = 0; hlod < after.size(); ++hlod )
        {
            becameResident += ( !before[hlod] && after[hlod] ) ? 1 : 0;
            wentAway += ( before[hlod] && !after[hlod] ) ? 1 : 0;
            const Residency state = s.Executor->State().Units[s.Units[hlod]].State;
            readNotMade += ( state == Residency::Loading || state == Residency::Loaded ) ? 1 : 0;
        }
        before = after;
    };
    for ( float x = CellCentre( 0, 1 ).x; x <= CellCentre( kColumns - 1, 1 ).x; x += 50.0f )
        fly( x );
    for ( float x = CellCentre( kColumns - 1, 1 ).x; x >= CellCentre( 0, 1 ).x; x -= 50.0f )
        fly( x );
    for ( int rest = 0; rest < 30; ++rest )
        fly( CellCentre( 0, 1 ).x );

    // The flight must actually contain the frames the relation is about.
    EXPECT_GT( becameResident, static_cast<std::size_t>( kColumns ) )
         << "no frame in which a cell became resident";
    EXPECT_GT( wentAway, static_cast<std::size_t>( kColumns ) ) << "no frame in which a cell was destroyed";
    EXPECT_GT( readNotMade, 0u ) << "no frame with a cell read but not yet made";
}

TEST( WorldPartitionHLODSwitch, AFailedCellKeepsItsHLOD )
{
    Streaming s{ World(), HLODWorld( 0 ), {}, {}, {} };
    s.World = HLODWorld( s.Records.size() );
    Begin( s, StreamingSource{ CellCentre( 0, 1 ) } );
    s.World.FailLoads = true;
    double now        = 0.0;
    for ( int frame = 0; frame < 20; ++frame, now += kFrame )
        Tick( s, StreamingSource{ CellCentre( 8, 1 ) }, now );
    ExpectOneOfEach( s, "after failed loads" );
    std::size_t failed = 0;
    for ( std::size_t hlod = 0; hlod < s.Units.size(); ++hlod )
        if ( s.Executor->State().Units[s.Units[hlod]].State == Residency::Failed )
        {
            ++failed;
            EXPECT_EQ( s.World.Visible[hlod], 1 ) << "failed cell unit " << s.Units[hlod] << " draws nothing";
        }
    EXPECT_GT( failed, 0u ) << "no load failed; the test proves nothing";
}

TEST( WorldPartitionHLODSwitch, OnlyChangesAreTold )
{
    Streaming s{ World(), HLODWorld( 0 ), {}, {}, {} };
    s.World = HLODWorld( s.Records.size() );
    Begin( s, StreamingSource{ CellCentre( 2, 1 ) } );
    EXPECT_EQ( s.World.Switches, s.Units.size() ) << "the first switch tells every HLOD";
    auto again = s.HLODs->Sync( s.Executor->State(), s.World );
    ASSERT_TRUE( again.IsSuccess() );
    EXPECT_EQ( again.GetValue(), 0u );
    EXPECT_EQ( s.World.Switches, s.Units.size() );
}

TEST( WorldPartitionHLODSwitch, AnHLODThatCannotStandInIsRefusedByName )
{
    const std::vector<EntityData> records = World();
    const WorldPartitionPlan      plan    = PlanWorldPartition( records, Grid() );
    ASSERT_FALSE( plan.AlwaysLoaded.empty() );
    const std::size_t firstCell = plan.AlwaysLoaded.size();

    const std::vector<std::size_t> always{ 0 };
    auto                           refused = HLODRuntime::Make( plan, always );
    ASSERT_FALSE( refused.IsSuccess() );
    EXPECT_NE( refused.GetError().find( "always-loaded" ), std::string::npos ) << refused.GetError();

    const std::vector<std::size_t> twice{ firstCell, firstCell };
    refused = HLODRuntime::Make( plan, twice );
    ASSERT_FALSE( refused.IsSuccess() );
    EXPECT_NE( refused.GetError().find( "two HLODs" ), std::string::npos ) << refused.GetError();

    const std::vector<std::size_t> beyond{ Desert::Core::Rules::ResidencyUnitCount( plan ) };
    refused = HLODRuntime::Make( plan, beyond );
    ASSERT_FALSE( refused.IsSuccess() );
}

TEST( WorldPartitionHLODSwitch, TheHolesAreNamedByReason )
{
    const std::vector<HLODExclusion> holes{ HLODExclusion::SkinnedMesh, HLODExclusion::PrefabInstance,
                                            HLODExclusion::SkinnedMesh };
    EXPECT_EQ( Desert::Core::Rules::DescribeHLODHoles( holes ), "1 PrefabInstance, 2 SkinnedMesh" );
    EXPECT_EQ( Desert::Core::Rules::DescribeHLODHoles( {} ), "" );
}
