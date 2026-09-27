// WP12 — THE OVERLOAD POLICY (owner decision O2: "fly on HLOD, block only when there is no cell under the
// player's feet"), over Rules::AssessStreaming (Engine/Core/Serialize/WorldPartitionStreamingPerformance.hpp).
//
// The plan is written by hand — two 1000 cm cells side by side, (0,0) and (1,0) — because the question is only
// about residency states and squares: what the cells hold is irrelevant beyond their record counts, which
// weigh the queue against the frame's activation budget.
#include <Engine/Core/Serialize/WorldPartitionStreamingPerformance.hpp>

#include <gtest/gtest.h>

#include <array>

namespace Rules = Desert::Core::Rules;

namespace
{
    constexpr std::size_t kHere  = 0; // unit of cell (0,0): the source stands in it
    constexpr std::size_t kThere = 1; // unit of cell (1,0): beyond it

    Rules::WorldPartitionPlan TwoCells( std::size_t recordsHere, std::size_t recordsThere )
    {
        Rules::WorldPartitionPlan plan;
        const auto                composite = [&plan]( std::size_t records )
        {
            Rules::PlannedComposite made;
            for ( std::size_t r = 0; r < records; ++r )
                made.Members.push_back( plan.Composites.size() * 1000 + r );
            plan.Composites.push_back( made );
            return plan.Composites.size() - 1;
        };
        Rules::PlannedCell here;
        here.Square     = { 0.0f, 0.0f, 1000.0f, 1000.0f };
        here.Composites = { composite( recordsHere ) };
        Rules::PlannedCell there;
        there.Cell       = { 1, 0 };
        there.Square     = { 1000.0f, 0.0f, 2000.0f, 1000.0f };
        there.Composites = { composite( recordsThere ) };
        plan.Cells       = { here, there };
        return plan;
    }

    Rules::ResidencyState States( Rules::Residency here, Rules::Residency there )
    {
        Rules::ResidencyState state;
        state.Units.resize( 2 );
        state.Units[kHere].State  = here;
        state.Units[kThere].State = there;
        return state;
    }

    const std::array<Rules::StreamingSource, 1> kStandingHere{
         Rules::StreamingSource{ { 500.0f, 200.0f, 500.0f } } };

    Rules::ResidencySettings Budget()
    {
        Rules::ResidencySettings settings;
        settings.ActivationBudgetMs = 2.0;
        settings.MsPerRecord        = 0.02; // 100 records fill one frame
        return settings;
    }
} // namespace

// KEEPING UP: the queue fits in one frame's budget, the cell underfoot is resident — nothing to report.
TEST( WorldPartitionStreamingPerformance, AQueueThatFitsTheFrameNeitherSlowsNorBlocks )
{
    const auto plan = TwoCells( 10, 50 );
    const auto seen = Rules::AssessStreaming(
         plan, Budget(), States( Rules::Residency::Activated, Rules::Residency::Loaded ), kStandingHere );
    EXPECT_EQ( seen.Performance, Rules::StreamingPerformance::Good );
    EXPECT_FALSE( seen.Blocks() );
    EXPECT_EQ( seen.QueuedUnits, 1u );
    EXPECT_EQ( seen.QueuedRecords, 50u );
    EXPECT_DOUBLE_EQ( seen.QueuedMs, 1.0 );
}

// NOT KEEPING UP, BUT THE GROUND IS THERE: slow, and play goes on — the far cell is on its HLOD (WP11).
TEST( WorldPartitionStreamingPerformance, FallingBehindWithACellUnderfootFliesOnTheHLOD )
{
    const auto plan = TwoCells( 10, 400 );
    for ( const Rules::Residency farCell : { Rules::Residency::Loading, Rules::Residency::Loaded } )
    {
        const auto seen = Rules::AssessStreaming( plan, Budget(), States( Rules::Residency::Activated, farCell ),
                                                  kStandingHere );
        EXPECT_EQ( seen.Performance, Rules::StreamingPerformance::Slow );
        EXPECT_FALSE( seen.Blocks() );
        EXPECT_GT( seen.QueuedMs, Budget().ActivationBudgetMs );
    }
}

// NO RESIDENT CELL UNDERFOOT: the game waits — and the moment the cell activates, the wait is over, whatever
// the rest of the queue still holds.
TEST( WorldPartitionStreamingPerformance, NoResidentCellUnderfootBlocksUntilItActivates )
{
    const auto plan = TwoCells( 10, 400 );
    for ( const Rules::Residency here :
          { Rules::Residency::Unloaded, Rules::Residency::Loading, Rules::Residency::Loaded } )
    {
        const auto seen =
             Rules::AssessStreaming( plan, Budget(), States( here, Rules::Residency::Loading ), kStandingHere );
        EXPECT_EQ( seen.Performance, Rules::StreamingPerformance::Critical );
        EXPECT_TRUE( seen.Blocks() );
        EXPECT_EQ( seen.UnderSource, kHere );
    }
    const auto after = Rules::AssessStreaming(
         plan, Budget(), States( Rules::Residency::Activated, Rules::Residency::Loading ), kStandingHere );
    EXPECT_FALSE( after.Blocks() );
    EXPECT_EQ( after.Performance, Rules::StreamingPerformance::Slow );

    // Before the first step the state is unsized: nothing is resident, so a source inside a cell waits.
    EXPECT_TRUE( Rules::AssessStreaming( plan, Budget(), Rules::ResidencyState{}, kStandingHere ).Blocks() );
}

// The two stated exceptions: an empty stretch of the world has nothing to wait for, and a cell that FAILED is
// logged and retried rather than waited on for ever.
TEST( WorldPartitionStreamingPerformance, NoCellAtAllOrAFailedCellDoesNotBlock )
{
    const auto                                  plan = TwoCells( 10, 10 );
    const std::array<Rules::StreamingSource, 1> away{ Rules::StreamingSource{ { 5000.0f, 0.0f, 5000.0f } } };
    EXPECT_FALSE( Rules::AssessStreaming( plan, Budget(),
                                          States( Rules::Residency::Unloaded, Rules::Residency::Unloaded ), away )
                       .Blocks() );

    const auto failed = Rules::AssessStreaming(
         plan, Budget(), States( Rules::Residency::Failed, Rules::Residency::Activated ), kStandingHere );
    EXPECT_FALSE( failed.Blocks() );
    EXPECT_EQ( failed.FailedUnderSrc, 1u );
}

// The square is half-open, as a grid cell is: a source on the shared edge stands in the cell to its +X.
TEST( WorldPartitionStreamingPerformance, ASourceOnAnEdgeStandsInTheCellToItsPositiveSide )
{
    const auto                                  plan = TwoCells( 10, 10 );
    const std::array<Rules::StreamingSource, 1> edge{ Rules::StreamingSource{ { 1000.0f, 0.0f, 10.0f } } };
    const auto                                  seen = Rules::AssessStreaming(
         plan, Budget(), States( Rules::Residency::Activated, Rules::Residency::Loading ), edge );
    EXPECT_TRUE( seen.Blocks() );
    EXPECT_EQ( seen.UnderSource, kThere );
}
