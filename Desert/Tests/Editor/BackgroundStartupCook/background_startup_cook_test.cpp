#include <Editor/Import/BackgroundCook.hpp>

#include <Common/Core/AssetHandle.hpp>

#include <gtest/gtest.h>

#include <functional>
#include <vector>

// AL1-11 / owner decision V2: the startup cook never blocks the reveal. Since AF4h a static mesh's cook is a DDC
// entry keyed by the source's bytes, so fresh loads now and stale == missing == Pending until the worker's
// cook lands. Every row of the completion table is pinned here, with the queue that carries a source there.
namespace
{
    using namespace Desert::Editor;

    TEST( BackgroundStartupCook, AFreshCacheEntryNeedsNothingWhenTheCheckReturns )
    {
        EXPECT_EQ( DecideCookCompletion( CookVerdict::UpToDate ), CookCompletionAction::Nothing );
        EXPECT_EQ( DecideCookCompletion( CookVerdict::NotCookable ), CookCompletionAction::Nothing );
    }

    TEST( BackgroundStartupCook, AStaleOrMissingEntryThatWasCookedReloadsThePendingAsset )
    {
        EXPECT_EQ( DecideCookCompletion( CookVerdict::Cooked ), CookCompletionAction::Reload );
    }

    TEST( BackgroundStartupCook, AFailedCookIsReportedAndNeverSubstitutedSilently )
    {
        EXPECT_EQ( DecideCookCompletion( CookVerdict::Failed ), CookCompletionAction::ReportFailure );
    }

    TEST( BackgroundStartupCook, APendingMeshKeepsItsHandleAcrossTheCook )
    {
        // The scene names a static mesh by its PATH's handle, not by the envelope the cook mints, so the asset
        // that was Pending before the cook and the one that resolves after it are the same handle.
        const std::filesystem::path cooked = "Resources/Assets/Meshes/base.stmesh";
        const auto                  before = Common::AssetHandle::FromCookedPath( cooked );
        const auto                  after  = Common::AssetHandle::FromCookedPath( cooked );
        EXPECT_EQ( static_cast<uint64_t>( before ), static_cast<uint64_t>( after ) );
        EXPECT_NE( static_cast<uint64_t>( before ), 0u );
        EXPECT_NE( static_cast<uint64_t>( before ), static_cast<uint64_t>( Common::AssetHandle::FromCookedPath(
                                                         "Resources/Assets/Meshes/base_basic_pbr.stmesh" ) ) );
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

        queue.Enqueue( "Meshes/stale.fbx" );
        queue.Enqueue( "Meshes/fresh.fbx" );
        EXPECT_EQ( cooks, 0 ) << "Enqueue must hand the cook to the worker, not run it on the main thread";
        EXPECT_EQ( queue.Outstanding(), 2u );
        EXPECT_TRUE( queue.Drain().empty() );

        for ( auto& job : held )
            job();
        EXPECT_EQ( queue.Outstanding(), 0u );
        EXPECT_EQ( queue.Total(), 2u );

        const auto done = queue.Drain();
        ASSERT_EQ( done.size(), 2u );
        EXPECT_EQ( DecideCookCompletion( done[0].Verdict ), CookCompletionAction::Reload );
        EXPECT_EQ( DecideCookCompletion( done[1].Verdict ), CookCompletionAction::Nothing );
        EXPECT_TRUE( queue.Drain().empty() ) << "a completion is handed out once";
    }
} // namespace

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
