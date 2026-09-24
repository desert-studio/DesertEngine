// THE SPLASH'S PROGRESS MODEL: the bar is weighted by the work each stage is, it never moves back, it
// reaches 100 % exactly when the start is over, and each stage reports how long it took.

#include <Editor/Splash/SplashLayout.hpp>
#include <Editor/Splash/SplashProgress.hpp>

#include <gtest/gtest.h>

namespace Splash = Desert::Editor::Splash;

TEST( SplashProgress, AStageWeighsItsWorkNotItsPlaceInTheList )
{
    // Two stages, the second ten times the work. Finishing the FIRST must show 1/11 of the bar, not
    // one half — a step counter would say 50 % here and then crawl through the long stage.
    Splash::ProgressModel model;
    const std::size_t     small = model.AddStage( "Small", 1.0, 10 );
    const std::size_t     large = model.AddStage( "Large", 1.0, 100 );
    model.BeginStage( small, 0.0 );
    model.BeginStage( large, 1.0 );
    EXPECT_NEAR( model.Fraction(), 10.0 / 110.0, 1e-9 );

    // Swap the order: the same work gives the same share, whatever the stage's index.
    Splash::ProgressModel swapped;
    const std::size_t     first  = swapped.AddStage( "Large", 1.0, 100 );
    const std::size_t     second = swapped.AddStage( "Small", 1.0, 10 );
    swapped.BeginStage( first, 0.0 );
    swapped.BeginStage( second, 1.0 );
    EXPECT_NEAR( swapped.Fraction(), 100.0 / 110.0, 1e-9 );
}

TEST( SplashProgress, TheUnitCostScalesAStagesWeight )
{
    // 5 shader programs at cost 4 weigh as much as 20 textures at cost 1.
    Splash::ProgressModel model;
    const std::size_t     shaders  = model.AddStage( "Shaders", 4.0, 5 );
    const std::size_t     textures = model.AddStage( "Textures", 1.0, 20 );
    model.BeginStage( shaders, 0.0 );
    model.BeginStage( textures, 1.0 );
    EXPECT_NEAR( model.Fraction(), 0.5, 1e-9 );
}

TEST( SplashProgress, ItemsInsideTheRunningStageMoveTheBar )
{
    Splash::ProgressModel model;
    const std::size_t     stage = model.AddStage( "Cooking textures", 1.0, 200 );
    model.BeginStage( stage, 0.0 );
    model.Step( "T_Rock_Albedo", 50 );
    EXPECT_NEAR( model.Fraction(), 0.25, 1e-9 );
    const Splash::ProgressSnapshot snapshot = model.Snapshot();
    EXPECT_EQ( snapshot.Stage, "Cooking textures" );
    EXPECT_EQ( snapshot.Item, "T_Rock_Albedo (51 / 200)" );
}

TEST( SplashProgress, TheBarNeverMovesBackWhenARealCountExceedsTheEstimate )
{
    // The textures were estimated at 10; the registry says 1000 when the stage begins. The honest
    // share drops (the total grew); the bar holds what it showed and the work catches up with it.
    Splash::ProgressModel model;
    const std::size_t     shaders  = model.AddStage( "Shaders", 1.0, 90 );
    const std::size_t     textures = model.AddStage( "Textures", 1.0, 10 );
    model.BeginStage( shaders, 0.0 );
    model.Step( "Lit", 90 );
    const double before = model.Fraction();
    EXPECT_NEAR( before, 0.9, 1e-9 );

    model.BeginStage( textures, 1.0, 1000 );
    EXPECT_EQ( model.Fraction(), before );
    double last = before;
    for ( std::size_t i = 0; i < 1000; i += 37 )
    {
        model.Step( "T", i );
        const double now = model.Fraction();
        EXPECT_GE( now, last ) << "at item " << i;
        last = now;
    }
}

TEST( SplashProgress, AStepBackwardsDoesNotMoveTheBarBack )
{
    Splash::ProgressModel model;
    const std::size_t     stage = model.AddStage( "Scene assets", 1.0, 10 );
    model.BeginStage( stage, 0.0 );
    model.Step( "A", 6 );
    const double shown = model.Fraction();
    model.Step( "B", 2 );
    EXPECT_EQ( model.Fraction(), shown );
}

TEST( SplashProgress, TheEndIsExactlyOneHundredPercentEvenWithSkippedStages )
{
    Splash::ProgressModel model;
    const std::size_t     ran = model.AddStage( "Ran", 1.0, 3 );
    model.AddStage( "Skipped", 1.0, 7 ); // nothing to do this start, never begun
    model.BeginStage( ran, 0.0 );
    EXPECT_LT( model.Fraction(), 1.0 );
    model.Finish( 2.0 );
    EXPECT_EQ( model.Fraction(), 1.0 );
    EXPECT_EQ( Splash::FormatPercent( model.Fraction() ), "100%" );
}

TEST( SplashProgress, OneHundredPercentOnlyWhenEverythingIsDone )
{
    Splash::ProgressModel model;
    const std::size_t     stage = model.AddStage( "Last", 1.0, 1000 );
    model.BeginStage( stage, 0.0 );
    model.Step( "Almost", 999 );
    EXPECT_EQ( Splash::FormatPercent( model.Fraction() ), "99%" );
}

TEST( SplashProgress, AFinishedStageReportsItsDurationAndCount )
{
    Splash::ProgressModel model;
    const std::size_t     a = model.AddStage( "Shaders", 1.0, 4 );
    const std::size_t     b = model.AddStage( "Textures", 1.0, 0 );
    EXPECT_FALSE( model.BeginStage( a, 1.5 ).has_value() );
    const std::optional<Splash::StageTiming> timing = model.BeginStage( b, 4.0, 12 );
    ASSERT_TRUE( timing.has_value() );
    EXPECT_EQ( timing->Name, "Shaders" );
    EXPECT_EQ( timing->Units, 4u );
    EXPECT_DOUBLE_EQ( timing->Seconds, 2.5 );
    const std::optional<Splash::StageTiming> last = model.Finish( 5.0 );
    ASSERT_TRUE( last.has_value() );
    EXPECT_EQ( last->Units, 12u );
    EXPECT_DOUBLE_EQ( last->Seconds, 1.0 );
}

TEST( SplashProgress, AnItemOfAOneItemStageHasNoCounter )
{
    EXPECT_EQ( Splash::ProgressModel::FormatItem( "Desert.deproj", 0, 1 ), "Desert.deproj" );
    EXPECT_EQ( Splash::ProgressModel::FormatItem( "Lit", 211, 212 ), "Lit (212 / 212)" );
}

TEST( SplashProgress, NoPlanIsAnEmptyBar )
{
    Splash::ProgressModel model;
    EXPECT_EQ( model.Fraction(), 0.0 );
    EXPECT_EQ( model.Snapshot().Stage, "" );
}

TEST( SplashProgress, ACountThatGrowsDuringTheStageHoldsTheBar )
{
    // The scene's reads keep starting while the settle waits for them: the stage's count grows.
    Splash::ProgressModel model;
    const std::size_t     stage = model.AddStage( "Scene", 1.0, 4 );
    model.BeginStage( stage, 0.0 );
    model.Step( "Scene assets", 3, 4 );
    const double shown = model.Fraction();
    model.Step( "Scene assets", 3, 40 );
    EXPECT_EQ( model.Fraction(), shown );
    EXPECT_EQ( model.Snapshot().Item, "Scene assets (4 / 40)" );
}

TEST( SplashProgress, ATextureThatIsCookedWeighsItsCookNotOneCheck )
{
    // Ten textures, one of them with no cooked artifact: its cook (4.26 s for 3.18 MB of BC7 source) is
    // the stage, and the bar must spend most of the stage's share on that one item, not a tenth.
    Splash::ProgressModel model;
    std::vector<double>   costs( 10, 0.0036 );
    costs[3]                   = 0.0036 + 4.26;
    const std::size_t textures = model.AddStage( "Cooking textures", 0.0036, costs );
    const std::size_t rest     = model.AddStage( "Rest", 1.0, 1 );
    model.BeginStage( textures, 0.0 );
    model.Step( "a.png", 3 ); // three checks done, the cook next
    EXPECT_NEAR( model.Fraction(), 3 * 0.0036 / ( 4.26 + 10 * 0.0036 + 1.0 ), 1e-9 );
    model.Step( "b.png", 4 ); // the cook done
    EXPECT_NEAR( model.Fraction(), ( 4.26 + 4 * 0.0036 ) / ( 4.26 + 10 * 0.0036 + 1.0 ), 1e-9 );
    EXPECT_EQ( model.Snapshot().Item, "b.png (5 / 10)" );
    // An item the plan did not count costs the stage's unit cost.
    model.Step( "c.png", 11, 12 );
    EXPECT_NEAR( model.Fraction(), ( 4.26 + 11 * 0.0036 ) / ( 4.26 + 12 * 0.0036 + 1.0 ), 1e-9 );
    model.BeginStage( rest, 1.0 );
    EXPECT_NEAR( model.Fraction(), ( 4.26 + 12 * 0.0036 ) / ( 4.26 + 12 * 0.0036 + 1.0 ), 1e-9 );
}

TEST( SplashProgress, AFixedCostWeighsEvenWithNoItemsAndCountsOnlyWhenTheStageEnds )
{
    // The settle: 0.75 s of first frames with no read outstanding. With its fixed cost it holds its share
    // of the bar until it ends; without it, it weighed one read and the bar sat at 99 % through it.
    Splash::ProgressModel model;
    const std::size_t     shaders = model.AddStage( "Shaders", 0.048, 78 );
    const std::size_t     settle  = model.AddStage( "Scene", 0.048, 1, 0.75 );
    model.BeginStage( shaders, 0.0 );
    model.BeginStage( settle, 3.73, std::size_t{ 0 } );
    const double total = 0.048 * 78 + 0.75 + 0.048;
    EXPECT_NEAR( model.Fraction(), 0.048 * 78 / total, 1e-9 );
    EXPECT_LT( model.Fraction(), 0.84 );
    model.Finish( 4.48 );
    EXPECT_EQ( model.Fraction(), 1.0 );
}
