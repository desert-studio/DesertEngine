#include <Editor/Import/BackgroundCook.hpp>

#include <gtest/gtest.h>

#include <functional>
#include <vector>

// AL1-11 / owner decision V2: the startup cook never blocks the reveal. Every row of the two tables that decide
// it is pinned here, together with the queue that carries a source from the startup to its completion.
namespace
{
    using namespace Desert::Editor;

    TEST( BackgroundStartupCook, AFreshOrStaleCookOnTheDiskIsUsedNowAndVerifiedInTheBackground )
    {
        // Fresh and stale look alike at startup on purpose: telling them apart costs the read being moved.
        EXPECT_EQ( DecideStartupCook( true ), StartupCookAction::UseCookedVerifyInBackground );
    }

    TEST( BackgroundStartupCook, AMissingCookIsPendingUntilTheBackgroundCookLands )
    {
        EXPECT_EQ( DecideStartupCook( false ), StartupCookAction::PendingCookInBackground );
    }

    TEST( BackgroundStartupCook, AFreshCookNeedsNothingWhenItsVerdictArrives )
    {
        EXPECT_EQ( DecideCookCompletion( StartupCookAction::UseCookedVerifyInBackground, CookVerdict::UpToDate ),
                   CookCompletionAction::Nothing );
    }

    TEST( BackgroundStartupCook, AStaleCookThatWasRecookedIsReloaded )
    {
        EXPECT_EQ( DecideCookCompletion( StartupCookAction::UseCookedVerifyInBackground, CookVerdict::Cooked ),
                   CookCompletionAction::Reload );
    }

    TEST( BackgroundStartupCook, APendingAssetIsRegisteredWhenItsCookLands )
    {
        EXPECT_EQ( DecideCookCompletion( StartupCookAction::PendingCookInBackground, CookVerdict::Cooked ),
                   CookCompletionAction::Register );
    }

    TEST( BackgroundStartupCook, AFailedCookIsReportedAndNeverSubstitutedSilently )
    {
        EXPECT_EQ( DecideCookCompletion( StartupCookAction::UseCookedVerifyInBackground, CookVerdict::Failed ),
                   CookCompletionAction::ReportFailure );
        EXPECT_EQ( DecideCookCompletion( StartupCookAction::PendingCookInBackground, CookVerdict::Failed ),
                   CookCompletionAction::ReportFailure );
    }

    TEST( BackgroundStartupCook, TheQueueRunsNothingOnTheCallerAndCountsWhatIsOutstanding )
    {
        std::vector<std::function<void()>> held; // a worker that has not run yet
        int                                cooks = 0;
        BackgroundCookQueue                queue(
             [&cooks]( const std::filesystem::path& source )
             {
                 ++cooks;
                 return source.filename() == "stale.fbx" ? CookVerdict::Cooked : CookVerdict::UpToDate;
             },
             [&held]( std::function<void()> job ) { held.push_back( std::move( job ) ); } );

        queue.Enqueue( "Meshes/stale.fbx", true );
        queue.Enqueue( "Meshes/missing.fbx", false );
        EXPECT_EQ( cooks, 0 ) << "Enqueue must hand the cook to the worker, not run it on the main thread";
        EXPECT_EQ( queue.Outstanding(), 2u );
        EXPECT_TRUE( queue.Drain().empty() );

        for ( auto& job : held )
            job();
        EXPECT_EQ( queue.Outstanding(), 0u );
        EXPECT_EQ( queue.Total(), 2u );

        const auto done = queue.Drain();
        ASSERT_EQ( done.size(), 2u );
        EXPECT_EQ( DecideCookCompletion( done[0].Started, done[0].Verdict ), CookCompletionAction::Reload );
        EXPECT_EQ( DecideCookCompletion( done[1].Started, done[1].Verdict ), CookCompletionAction::Nothing );
        EXPECT_TRUE( queue.Drain().empty() ) << "a completion is handed out once";
    }
} // namespace

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
