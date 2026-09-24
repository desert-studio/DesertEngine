// CELL RESIDENCY: HOW A PARTITIONED WORLD FOLLOWS ITS STREAMING SOURCES, ONE FRAME AT A TIME.
//
// Every case drives StepResidency through a simulated loader that completes (or fails) each StartLoad a fixed
// number of frames later — and keeps completing loads that were cancelled, so stale reports are exercised.
//
// WHAT IS ASSERTED:
//
//   1. THE FLY-BY. A source crossing a row of cells: each frame's activations stay within the budget (or are a
//      single unit), come in the query's priority order, and when the source stops everything it wants is
//      activated and nothing else is resident. From rest, activations and load starts both follow the priority
//      order exactly, and no more loads are in flight than MaxConcurrentLoads.
//   2. THE REVERSAL. A source pacing back and forth across a load boundary unloads and reloads nothing; the same
//      pacing with no margin does — which is what proves the margin is what holds it.
//   3. ALWAYS-LOADED. First to load and activate, with no source at all, and never unloaded by a teleport.
//   4. FAILURE. A failed load is Failed with the loader's reason, retried after the delay and not before, with
//      the delay growing; a failure without a reason and an outcome for no unit are refused. A report for a
//      cancelled load is stale and does not complete the load that replaced it.
//   5. THE TELEPORT. Everything outside the new band leaves in one frame; the new neighbourhood activates
//      under the budget.
//   6. DETERMINISM. The same run twice gives the same actions; the loader's reports in another order give the
//      same result.

#include <Engine/Core/Serialize/WorldPartitionResidencyRules.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using Desert::Core::WorldPartitionGridSerialized;
using Desert::Core::WorldPartitionSerialized;
using Desert::Core::Rules::CellCoord;
using Desert::Core::Rules::LoadOutcome;
using Desert::Core::Rules::PlannedCell;
using Desert::Core::Rules::PlannedComposite;
using Desert::Core::Rules::QueryStreamingCells;
using Desert::Core::Rules::Residency;
using Desert::Core::Rules::ResidencyAction;
using Desert::Core::Rules::ResidencyActionKind;
using Desert::Core::Rules::ResidencySettings;
using Desert::Core::Rules::ResidencyState;
using Desert::Core::Rules::ResidencyStep;
using Desert::Core::Rules::ResidencyUnitCount;
using Desert::Core::Rules::StepResidency;
using Desert::Core::Rules::StreamingSource;
using Desert::Core::Rules::WorldPartitionPlan;

namespace
{
    constexpr float  kCell  = 1000.0f;    // 10 m cells
    constexpr float  kRange = 1500.0f;    // a source wants cells up to 15 m from it
    constexpr double kFrame = 1.0 / 64.0; // exact in binary, so retry times land on a frame

    WorldPartitionSerialized Grid()
    {
        WorldPartitionGridSerialized grid;
        grid.CellSize     = kCell;
        grid.LoadingRange = kRange;
        return WorldPartitionSerialized{ { grid } };
    }

    // One always-loaded composite (unit 0) and a row of @p count level-0 cells along +X at Z = 0, cell i
    // (unit i + 1) holding one composite of @p records members.
    WorldPartitionPlan Row( int count, std::size_t records = 10 )
    {
        WorldPartitionPlan plan;
        PlannedComposite   always;
        always.Members = std::vector<std::size_t>( 4, 0 );
        plan.Composites.push_back( always );
        plan.AlwaysLoaded.push_back( 0 );
        for ( int i = 0; i < count; ++i )
        {
            PlannedComposite composite;
            composite.Members = std::vector<std::size_t>( records, 0 );
            plan.Composites.push_back( composite );
            PlannedCell cell;
            cell.Cell = CellCoord{ i, 0 };
            cell.Composites.push_back( plan.Composites.size() - 1 );
            plan.Cells.push_back( cell );
        }
        plan.LevelCount = 1;
        return plan;
    }

    StreamingSource At( float x )
    {
        return StreamingSource{ glm::vec3( x, 0.0f, 0.5f * kCell ), 1.0f };
    }

    // The query's priority order as units: always-loaded first, then the wish's cells.
    std::vector<std::size_t> WantedUnits( const WorldPartitionPlan&           plan,
                                          const std::vector<StreamingSource>& sources )
    {
        const auto wish = QueryStreamingCells( plan, Grid(), sources );
        EXPECT_TRUE( wish.IsSuccess() );
        std::vector<std::size_t> units;
        for ( std::size_t unit = 0; unit < plan.AlwaysLoaded.size(); ++unit )
            units.push_back( unit );
        if ( wish.IsSuccess() )
            for ( const auto& cell : wish.GetValue().Cells )
                units.push_back( plan.AlwaysLoaded.size() + cell.Cell );
        return units;
    }

    // A world and a loader. Each StartLoad reports back `Latency` frames later — a success, or a failure while
    // the unit has failures left to spend. Cancelled loads still report: the loader never hears of a cancel in
    // time, which is the case the ticket exists for.
    struct Sim
    {
        WorldPartitionPlan                        Plan;
        ResidencySettings                         Settings;
        ResidencyState                            State;
        int                                       Latency = 1;
        std::map<std::size_t, int>                FailuresLeft;
        std::multimap<int, LoadOutcome>           Pending; // by the frame it reports in
        bool                                      ReverseOutcomes = false;
        int                                       Frame           = 0;
        std::vector<std::vector<ResidencyAction>> Log;
        std::size_t                               Stale = 0;

        ResidencyStep Step( const std::vector<StreamingSource>& sources )
        {
            std::vector<LoadOutcome> due;
            for ( auto it = Pending.begin(); it != Pending.end() && it->first <= Frame; it = Pending.erase( it ) )
                due.push_back( it->second );
            if ( ReverseOutcomes )
                std::reverse( due.begin(), due.end() );
            auto result = StepResidency( Plan, Grid(), sources, Settings, State, due, Frame * kFrame );
            EXPECT_TRUE( result.IsSuccess() ) << ( result.IsSuccess() ? "" : result.GetError() );
            if ( !result.IsSuccess() )
                return {};
            ResidencyStep step = result.GetValue();
            for ( const ResidencyAction& action : step.Actions )
            {
                if ( action.Kind != ResidencyActionKind::StartLoad )
                    continue;
                LoadOutcome outcome{ action.Unit, action.Ticket, true, {} };
                if ( auto left = FailuresLeft.find( action.Unit ); left != FailuresLeft.end() && left->second > 0 )
                {
                    --left->second;
                    outcome.Ok     = false;
                    outcome.Reason = "cell " + std::to_string( action.Unit ) + ": file not found";
                }
                Pending.emplace( Frame + Latency, outcome );
            }
            State = step.State;
            Stale += step.StaleOutcomes;
            Log.push_back( step.Actions );
            ++Frame;
            return step;
        }

        [[nodiscard]] std::set<std::size_t> In( Residency residency ) const
        {
            std::set<std::size_t> units;
            for ( std::size_t unit = 0; unit < State.Units.size(); ++unit )
                if ( State.Units[unit].State == residency )
                    units.insert( unit );
            return units;
        }
    };

    std::vector<std::size_t> Of( const std::vector<ResidencyAction>& actions, ResidencyActionKind kind )
    {
        std::vector<std::size_t> units;
        for ( const ResidencyAction& action : actions )
            if ( action.Kind == kind )
                units.push_back( action.Unit );
        return units;
    }

    // Every frame: the estimate stays in the budget unless one unit alone exceeds it, and the frame's
    // activations are a subsequence of the query's priority order.
    void ExpectBudgetAndOrder( const Sim& sim, const ResidencyStep& step,
                               const std::vector<StreamingSource>& sources )
    {
        const auto activated = Of( step.Actions, ResidencyActionKind::Activate );
        if ( activated.size() > 1 )
        {
            EXPECT_LE( step.ActivationMs, sim.Settings.ActivationBudgetMs + 1e-9 ) << "frame " << sim.Frame - 1;
        }
        const auto order = WantedUnits( sim.Plan, sources );
        auto       from  = order.begin();
        for ( const std::size_t unit : activated )
        {
            from = std::find( from, order.end(), unit );
            ASSERT_NE( from, order.end() ) << "frame " << sim.Frame - 1 << ": unit " << unit
                                           << " activated out of priority order or unwanted";
        }
    }

    std::set<std::size_t> AsSet( const std::vector<std::size_t>& units )
    {
        return { units.begin(), units.end() };
    }
} // namespace

TEST( WorldPartitionResidency, AFlyByActivatesInPriorityOrderWithinTheBudget )
{
    Sim sim;
    sim.Plan                        = Row( 40 );
    sim.Settings.ActivationBudgetMs = 1.0; // 10 records · 0.05 ms = 0.5 ms a cell: two cells a frame
    sim.Settings.MsPerRecord        = 0.05;
    sim.Settings.MaxConcurrentLoads = 8;
    sim.Latency                     = 3;

    std::size_t mostInOneFrame = 0;
    std::size_t waited         = 0;
    for ( float x = 500.0f; x <= 35000.0f; x += 400.0f ) // 4 m a frame
    {
        const std::vector<StreamingSource> sources{ At( x ) };
        const ResidencyStep                step = sim.Step( sources );
        ExpectBudgetAndOrder( sim, step, sources );
        mostInOneFrame = std::max( mostInOneFrame, Of( step.Actions, ResidencyActionKind::Activate ).size() );
        waited += step.WaitingForBudget;
        EXPECT_LE( sim.In( Residency::Loading ).size(), sim.Settings.MaxConcurrentLoads );
    }
    EXPECT_EQ( mostInOneFrame, 2u ) << "the budget fits exactly two cells";
    EXPECT_GT( waited, 0u ) << "the start of the fly-by must queue behind the budget, or the test proves nothing";

    // The source stops: once the loader drains, exactly what it wants is activated and nothing else is held.
    const std::vector<StreamingSource> stopped{ At( 35000.0f ) };
    for ( int frame = 0; frame < 20; ++frame )
        ExpectBudgetAndOrder( sim, sim.Step( stopped ), stopped );
    EXPECT_EQ( sim.In( Residency::Activated ), AsSet( WantedUnits( sim.Plan, stopped ) ) );
    EXPECT_TRUE( sim.In( Residency::Loaded ).empty() );
    EXPECT_TRUE( sim.In( Residency::Loading ).empty() );
}

TEST( WorldPartitionResidency, FromRestTheWorldFillsInExactlyThePriorityOrder )
{
    Sim sim;
    sim.Plan                        = Row( 12 );
    sim.Settings.ActivationBudgetMs = 0.5; // one cell a frame
    sim.Settings.MaxConcurrentLoads = 16;
    const std::vector<StreamingSource> sources{ At( 5200.0f ) };
    std::vector<std::size_t>           activated;
    for ( int frame = 0; frame < 12; ++frame )
    {
        const auto step = sim.Step( sources );
        for ( const std::size_t unit : Of( step.Actions, ResidencyActionKind::Activate ) )
            activated.push_back( unit );
    }
    EXPECT_EQ( activated, WantedUnits( sim.Plan, sources ) );
}

TEST( WorldPartitionResidency, AReportForACancelledLoadIsStaleAndDoesNotCompleteItsSuccessor )
{
    Sim sim;
    sim.Plan    = Row( 8 );
    sim.Latency = 5;
    const std::vector<StreamingSource> home{ At( 500.0f ) };
    const auto                         started = Of( sim.Step( home ).Actions, ResidencyActionKind::StartLoad );
    ASSERT_EQ( started.size(), 4u ); // always-loaded + cells 0..2
    const auto away = sim.Step( { At( 1.0e7f ) } );
    EXPECT_EQ( AsSet( Of( away.Actions, ResidencyActionKind::CancelLoad ) ), AsSet( { 1, 2, 3 } ) );
    const auto again = sim.Step( home );
    EXPECT_EQ( AsSet( Of( again.Actions, ResidencyActionKind::StartLoad ) ), AsSet( { 1, 2, 3 } ) );

    // Frame 5: the three cancelled loads report success. Their successors (due at frame 7) are still loading.
    sim.Step( home );
    sim.Step( home );
    const auto stale = sim.Step( home );
    EXPECT_EQ( stale.StaleOutcomes, 3u );
    EXPECT_EQ( sim.In( Residency::Loading ), AsSet( { 1, 2, 3 } ) );
    EXPECT_EQ( sim.In( Residency::Activated ), AsSet( { 0 } ) );
    sim.Step( home );
    sim.Step( home );
    EXPECT_EQ( sim.In( Residency::Activated ), AsSet( { 0, 1, 2, 3 } ) );
}

TEST( WorldPartitionResidency, LoadsInFlightAreBoundedAndStartInPriorityOrder )
{
    Sim sim;
    sim.Plan                        = Row( 12 );
    sim.Settings.MaxConcurrentLoads = 2;
    sim.Latency                     = 3;
    const std::vector<StreamingSource> sources{ At( 5200.0f ) };
    std::vector<std::size_t>           started;
    for ( int frame = 0; frame < 20; ++frame )
    {
        for ( const std::size_t unit : Of( sim.Step( sources ).Actions, ResidencyActionKind::StartLoad ) )
            started.push_back( unit );
        EXPECT_LE( sim.In( Residency::Loading ).size(), 2u ) << "frame " << frame;
    }
    EXPECT_EQ( started, WantedUnits( sim.Plan, sources ) );
}

TEST( WorldPartitionResidency, PacingAcrossALoadBoundaryDoesNotFlicker )
{
    // Cell 5 spans [5000, 6000]: 14 m from x = 3600 (wanted), 16 m from x = 3400 (inside the 18.75 m band).
    // Cell 1 spans [1000, 2000]: the mirror. Each pace moves one of them across the loading range.
    const auto pace = []( float margin )
    {
        Sim sim;
        sim.Plan                  = Row( 12 );
        sim.Settings.UnloadMargin = margin;
        for ( int frame = 0; frame < 10; ++frame )
            sim.Step( { At( 3600.0f ) } );
        std::size_t churn = 0;
        for ( int frame = 0; frame < 60; ++frame )
        {
            const auto step = sim.Step( { At( frame % 2 == 0 ? 3400.0f : 3600.0f ) } );
            churn += Of( step.Actions, ResidencyActionKind::Unload ).size() +
                     Of( step.Actions, ResidencyActionKind::CancelLoad ).size();
        }
        return std::make_pair( churn, sim.In( Residency::Activated ) );
    };

    const auto [churn, activated] = pace( 0.25f );
    EXPECT_EQ( churn, 0u );
    EXPECT_TRUE( activated.contains( 1 + 1 ) && activated.contains( 1 + 5 ) ) << "both edge cells stay activated";

    const std::size_t churnWithoutMargin = pace( 0.0f ).first;
    EXPECT_GT( churnWithoutMargin, 20u )
         << "with no band the same pacing must churn, or the margin is not what holds";
}

TEST( WorldPartitionResidency, TheBandIsLeftByDistanceAndReturningCancelsNothing )
{
    Sim sim;
    sim.Plan = Row( 12 );
    for ( int frame = 0; frame < 10; ++frame )
        sim.Step( { At( 3600.0f ) } );
    ASSERT_EQ( sim.State.Units[1 + 5].State, Residency::Activated );

    // 18.7 m from cell 5: inside the band — kept however long the source stays there.
    for ( int frame = 0; frame < 600; ++frame )
        EXPECT_TRUE( Of( sim.Step( { At( 3130.0f ) } ).Actions, ResidencyActionKind::Unload ).empty() );
    EXPECT_EQ( sim.State.Units[1 + 5].State, Residency::Activated );

    // 18.8 m: past it — deactivated and unloaded in that frame.
    const auto step = sim.Step( { At( 3120.0f ) } );
    EXPECT_EQ( Of( step.Actions, ResidencyActionKind::Deactivate ), std::vector<std::size_t>{ 1 + 5 } );
    EXPECT_EQ( Of( step.Actions, ResidencyActionKind::Unload ), std::vector<std::size_t>{ 1 + 5 } );
    EXPECT_EQ( sim.State.Units[1 + 5].State, Residency::Unloaded );
}

TEST( WorldPartitionResidency, AlwaysLoadedComesFirstNeedsNoSourceAndSurvivesATeleport )
{
    Sim sim;
    sim.Plan   = Row( 8 );
    auto first = sim.Step( {} );
    EXPECT_EQ( Of( first.Actions, ResidencyActionKind::StartLoad ), std::vector<std::size_t>{ 0 } );
    sim.Step( {} );
    EXPECT_EQ( sim.In( Residency::Activated ), std::set<std::size_t>{ 0 } );

    const auto withSource = sim.Step( { At( 2500.0f ) } );
    ASSERT_FALSE( Of( withSource.Actions, ResidencyActionKind::StartLoad ).empty() );
    for ( int frame = 0; frame < 10; ++frame )
        sim.Step( { At( 2500.0f ) } );
    for ( int frame = 0; frame < 10; ++frame )
    {
        const auto step     = sim.Step( { At( 1.0e7f ) } );
        const auto unloaded = Of( step.Actions, ResidencyActionKind::Unload );
        EXPECT_EQ( std::count( unloaded.begin(), unloaded.end(), 0u ), 0 );
    }
    EXPECT_EQ( sim.In( Residency::Activated ), std::set<std::size_t>{ 0 } );
}

TEST( WorldPartitionResidency, AFailedLoadStatesItsReasonAndIsRetriedOnScheduleNotEveryFrame )
{
    Sim sim;
    sim.Plan                       = Row( 4 );
    sim.Settings.RetryDelaySeconds = 1.0;
    sim.Settings.RetryBackoff      = 2.0;
    const std::size_t failing      = 1 + 1;
    sim.FailuresLeft[failing]      = 2;

    std::vector<int> startedAt;
    for ( int frame = 0; frame < 300; ++frame )
    {
        const auto step = sim.Step( { At( 1500.0f ) } );
        for ( const std::size_t unit : Of( step.Actions, ResidencyActionKind::StartLoad ) )
            if ( unit == failing )
                startedAt.push_back( frame );
        if ( frame == 2 )
        {
            EXPECT_EQ( sim.State.Units[failing].State, Residency::Failed );
            EXPECT_EQ( sim.State.Units[failing].FailureReason, "cell 2: file not found" );
            EXPECT_EQ( sim.State.Units[failing].Failures, 1u );
        }
    }
    // Started at 0, failure reported at 1; retried 1 s = 64 frames later (65), failure reported at 66; retried
    // 2 s = 128 frames later (194).
    EXPECT_EQ( startedAt, ( std::vector<int>{ 0, 65, 194 } ) );
    EXPECT_EQ( sim.State.Units[failing].State, Residency::Activated );
    EXPECT_EQ( sim.State.Units[failing].Failures, 0u );
    EXPECT_TRUE( sim.State.Units[failing].FailureReason.empty() );
}

TEST( WorldPartitionResidency, AReportWithoutAReasonOrForNoUnitOrAForeignStateIsRefused )
{
    const WorldPartitionPlan           plan = Row( 3 );
    const ResidencySettings            settings;
    const std::vector<StreamingSource> sources{ At( 0.0f ) };

    const std::vector<LoadOutcome> noReason{ LoadOutcome{ 1, 1, false, {} } };
    const auto                     silent = StepResidency( plan, Grid(), sources, settings, {}, noReason, 0.0 );
    ASSERT_FALSE( silent.IsSuccess() );
    EXPECT_NE( silent.GetError().find( "gave no reason" ), std::string::npos ) << silent.GetError();

    const std::vector<LoadOutcome> noUnit{ LoadOutcome{ 99, 1, true, {} } };
    const auto                     nowhere = StepResidency( plan, Grid(), sources, settings, {}, noUnit, 0.0 );
    ASSERT_FALSE( nowhere.IsSuccess() );
    EXPECT_NE( nowhere.GetError().find( "unit 99 of 4" ), std::string::npos ) << nowhere.GetError();

    ResidencyState foreign;
    foreign.Units.resize( 7 );
    const auto other = StepResidency( plan, Grid(), sources, settings, foreign, {}, 0.0 );
    ASSERT_FALSE( other.IsSuccess() );
    EXPECT_NE( other.GetError().find( "7 unit(s) and the plan 4" ), std::string::npos ) << other.GetError();

    ResidencySettings noLoads;
    noLoads.MaxConcurrentLoads = 0;
    EXPECT_FALSE( StepResidency( plan, Grid(), sources, noLoads, {}, {}, 0.0 ).IsSuccess() );
    EXPECT_EQ( ResidencyUnitCount( plan ), 4u );
}

TEST( WorldPartitionResidency, ATeleportDropsTheOldWorldInOneFrameAndFillsTheNewUnderBudget )
{
    Sim sim;
    sim.Plan                        = Row( 60 );
    sim.Settings.ActivationBudgetMs = 1.0;
    sim.Settings.MaxConcurrentLoads = 8;
    const std::vector<StreamingSource> home{ At( 3500.0f ) };
    for ( int frame = 0; frame < 20; ++frame )
        sim.Step( home );
    const auto before = sim.In( Residency::Activated );
    ASSERT_GT( before.size(), 3u );

    const std::vector<StreamingSource> away{ At( 50500.0f ) };
    const auto                         jump = sim.Step( away );
    auto                               old  = before;
    old.erase( 0 );
    EXPECT_EQ( AsSet( Of( jump.Actions, ResidencyActionKind::Deactivate ) ), old );
    EXPECT_EQ( AsSet( Of( jump.Actions, ResidencyActionKind::Unload ) ), old );
    ExpectBudgetAndOrder( sim, jump, away );

    for ( int frame = 0; frame < 20; ++frame )
        ExpectBudgetAndOrder( sim, sim.Step( away ), away );
    EXPECT_EQ( sim.In( Residency::Activated ), AsSet( WantedUnits( sim.Plan, away ) ) );
}

TEST( WorldPartitionResidency, TheSameRunTwiceAndReportsInAnotherOrderGiveTheSameActions )
{
    const auto run = []( bool reverse )
    {
        Sim sim;
        sim.Plan                        = Row( 30 );
        sim.Settings.ActivationBudgetMs = 1.0;
        sim.Latency                     = 2;
        sim.ReverseOutcomes             = reverse;
        sim.FailuresLeft[1 + 7]         = 1;
        for ( float x = 0.0f; x < 25000.0f; x += 300.0f )
            sim.Step( { At( x ), At( 25000.0f - x ) } );
        sim.Step( { At( 12000.0f ) } ); // a teleport of both into one
        for ( int frame = 0; frame < 20; ++frame )
            sim.Step( { At( 12000.0f ) } );
        return sim.Log;
    };
    const auto once = run( false );
    const auto flat = []( const std::vector<std::vector<ResidencyAction>>& log )
    {
        std::vector<std::tuple<int, std::size_t, std::uint64_t>> out;
        for ( const auto& frame : log )
        {
            out.emplace_back( -1, 0, 0 ); // the frame boundary
            for ( const ResidencyAction& action : frame )
                out.emplace_back( static_cast<int>( action.Kind ), action.Unit, action.Ticket );
        }
        return out;
    };
    EXPECT_EQ( flat( once ), flat( run( false ) ) );
    EXPECT_EQ( flat( once ), flat( run( true ) ) );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
