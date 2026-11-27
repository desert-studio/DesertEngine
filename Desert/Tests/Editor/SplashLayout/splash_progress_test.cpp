// WHAT THE SPLASH SAYS, from one sample of the start's progress (STARTUP-PROGRESS): the phase with its place
// and its count, the running items with their ages, the time spent and left, the detail log newest at the
// bottom, and the capture's CSV row. The progress itself (weights, history, the monotonic fraction) is pinned
// by the engine's own suite; here it is driven only as the splash's thread drives it, through `Sample`.

#include <Editor/Splash/SplashLayout.hpp>

#include <Engine/Core/StartupProgress.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <map>
#include <string>
#include <vector>

namespace Splash = Desert::Editor::Splash;
namespace Core   = Desert::Core;

namespace
{
    Core::StartupView PhaseView()
    {
        Core::StartupView view;
        view.Phase            = "Building shader maps";
        view.PhaseNumber      = 3;
        view.PhaseCount       = 14;
        view.Fraction         = 0.627;
        view.ElapsedSeconds   = 192.4;
        view.EtaSeconds       = 65.2;
        view.Count            = Core::PhaseCountKind::Planned;
        return view;
    }

    Core::StartupProgress TwoPhaseProgress( const Core::StartupClock::time_point start )
    {
        return Core::StartupProgress( { { "first", "First", 1.0 }, { "second", "Second", 1.0 } }, {}, start );
    }
} // namespace

TEST( SplashText, BeforeTheFirstPhaseEveryLiveLineIsEmpty )
{
    // No phase means no plan running yet: a "0 %" or a "0:00" there would be numbers that say nothing.
    const Core::StartupView view;
    EXPECT_EQ( Splash::StageText( view ), "" );
    EXPECT_EQ( Splash::PercentText( view ), "" );
    EXPECT_EQ( Splash::ItemText( view ), "" );
    EXPECT_EQ( Splash::TimeText( view ), "" );
    for ( std::size_t row = 0; row < Splash::kLogLines; ++row )
        EXPECT_EQ( Splash::LogRowText( view, row ), "" );
}

TEST( SplashText, TheStageNamesThePhaseItsPlaceAndItsCount )
{
    Core::StartupView view = PhaseView();
    EXPECT_EQ( Splash::StageText( view ), "Building shader maps  \xC2\xB7  phase 3 / 14" );
    view.Done  = 1240;
    view.Total = 1910;
    EXPECT_EQ( Splash::StageText( view ), "Building shader maps  \xC2\xB7  phase 3 / 14  \xC2\xB7  1240 / 1910" );
}

TEST( SplashText, ThePercentageIsRoundedDownSoOneHundredMeansDone )
{
    Core::StartupView view = PhaseView();
    EXPECT_EQ( Splash::PercentText( view ), "62 %" );
    view.Fraction = 0.9999;
    EXPECT_EQ( Splash::PercentText( view ), "99 %" );
    view.Fraction = 1.0;
    EXPECT_EQ( Splash::PercentText( view ), "100 %" );
}

TEST( SplashText, ARunningItemAlwaysShowsItsSeconds )
{
    Core::StartupView view = PhaseView();
    view.Active            = { { "DeferredLighting Direct", {}, 12.43 }, { "Sky", {}, 0.2 } };
    EXPECT_EQ( Splash::ItemText( view ), "DeferredLighting Direct  12.4 s,  Sky  0.2 s" );
}

TEST( SplashText, TheTimeLineSaysWhatWasSpentAndWhatIsLeft )
{
    Core::StartupView view = PhaseView();
    EXPECT_EQ( Splash::TimeText( view ),
               Core::FormatDuration( 192.4 ) + " \xC2\xB7 about " + Core::FormatDuration( 66.0 ) + " left" );
    view.Finished = true;
    EXPECT_EQ( Splash::TimeText( view ), Core::FormatDuration( 192.4 ) );
}

TEST( SplashText, ClosingReplacesTheStageAndClearsTheItems )
{
    Core::StartupView view = PhaseView();
    view.Active            = { { "Sky", {}, 3.0 } };
    view.StopRequested     = true;
    EXPECT_EQ( Splash::StageText( view ), "Closing..." );
    EXPECT_EQ( Splash::ItemText( view ), "" );
}

TEST( SplashText, TheNewestLogLineIsTheBottomRow )
{
    Core::StartupView view = PhaseView();
    view.Log               = { { 1.0, "oldest" }, { 2.0, "middle" }, { 31.0, "newest" } };
    EXPECT_EQ( Splash::LogRowText( view, 0 ), Core::FormatDuration( 31.0 ) + "  newest" );
    EXPECT_EQ( Splash::LogRowText( view, 1 ), Core::FormatDuration( 2.0 ) + "  middle" );
    EXPECT_EQ( Splash::LogRowText( view, 2 ), Core::FormatDuration( 1.0 ) + "  oldest" );
    EXPECT_EQ( Splash::LogRowText( view, 3 ), "" );
}

TEST( SplashText, TheCaptureRowQuotesWhatMayHoldACommaAndKeepsItsColumns )
{
    Core::StartupView view = PhaseView();
    view.Phase             = "Loading \"the\" scene, part 1";
    view.Done              = 7;
    view.Total             = 9;
    view.Active            = { { "a,b", {}, 2.0 } };
    EXPECT_EQ( Splash::CaptureCsvRow( view ),
               "192400,0.627000,\"Loading \"\"the\"\" scene, part 1\",planned,7,9,66,1,\"a,b\",2.000" );
    view.Active.clear();
    EXPECT_EQ( Splash::CaptureCsvRow( view ).substr( Splash::CaptureCsvRow( view ).size() - 9 ), ",\"\",0.000" );
}

TEST( SplashText, SampledAsTheSplashSamplesItTheLinesFollowThePhasesAndTheBarNeverGoesBack )
{
    const Core::StartupClock::time_point start    = Core::StartupClock::now();
    Core::StartupProgress                progress = TwoPhaseProgress( start );
    Core::StartupClock::time_point       now      = start;
    double                               shown    = 0.0;
    const auto                           sample   = [&]( const double seconds )
    {
        now +=
             std::chrono::duration_cast<Core::StartupClock::duration>( std::chrono::duration<double>( seconds ) );
        const Core::StartupView view = progress.Sample( now, 0.001 );
        EXPECT_GE( view.Fraction, shown );
        shown = view.Fraction;
        return view;
    };

    EXPECT_EQ( Splash::StageText( sample( 0.0 ) ), "First  \xC2\xB7  phase 1 / 2" );

    ASSERT_TRUE( progress.BeginPhase( "first" ).IsSuccess() );
    progress.Expect( 2 );
    const uint64_t    token = progress.BeginItem( "Sky" );
    Core::StartupView view  = sample( 0.5 );
    EXPECT_EQ( Splash::StageText( view ), "First  \xC2\xB7  phase 1 / 2  \xC2\xB7  0 / 2" );
    EXPECT_EQ( Splash::ItemText( view ).rfind( "Sky", 0 ), 0u );
    progress.FinishItem( token );
    progress.FinishItem( progress.BeginItem( "Moon" ) );
    view = sample( 0.5 );
    EXPECT_EQ( Splash::StageText( view ), "First  \xC2\xB7  phase 1 / 2  \xC2\xB7  2 / 2" );
    EXPECT_LT( view.Fraction, 1.0 );

    ASSERT_TRUE( progress.BeginPhase( "second" ).IsSuccess() );
    view = sample( 0.5 );
    EXPECT_EQ( Splash::StageText( view ), "Second  \xC2\xB7  phase 2 / 2" );
    EXPECT_LT( view.Fraction, 1.0 );
    // The longest wait without a single number moving is what the splash exists to avoid.
    EXPECT_NE( Splash::TimeText( view ), "" );

    progress.Finish();
    view = sample( 0.5 );
    EXPECT_DOUBLE_EQ( view.Fraction, 1.0 );
    EXPECT_EQ( Splash::PercentText( view ), "100 %" );
}

TEST( SplashText, AKeyOutsideThePlanIsRefusedAndTheSplashKeepsTheLastPhase )
{
    const Core::StartupClock::time_point start    = Core::StartupClock::now();
    Core::StartupProgress                progress = TwoPhaseProgress( start );
    ASSERT_TRUE( progress.BeginPhase( "first" ).IsSuccess() );
    EXPECT_FALSE( progress.BeginPhase( "not.in.the.plan" ).IsSuccess() );
    EXPECT_EQ( Splash::StageText( progress.Sample( start, 0.001 ) ), "First  \xC2\xB7  phase 1 / 2" );
}

TEST( SplashText, TheCloseButtonsStopIsWhatTheNextSampleSays )
{
    const Core::StartupClock::time_point start    = Core::StartupClock::now();
    Core::StartupProgress                progress = TwoPhaseProgress( start );
    ASSERT_TRUE( progress.BeginPhase( "first" ).IsSuccess() );
    progress.RequestStop();
    EXPECT_TRUE( progress.StopRequested() );
    EXPECT_EQ( Splash::StageText( progress.Sample( start, 0.001 ) ), "Closing..." );
}
