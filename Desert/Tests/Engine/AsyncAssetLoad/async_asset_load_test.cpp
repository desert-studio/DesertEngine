// WHAT MAKES LAZINESS SAFE, ASSERTED ONE RULE AT A TIME.
//
// `Docs/World/GAP_ANALYSIS.md` §3.1 states the trap this suite guards, and it is written in our own
// tree at `AssetPreloader.cpp`: the cloud volumes load eagerly because "deferring would buy a stall
// exactly where the sky first appears". That sentence is true, and it is the reason T2.2 and T2.3 are
// one unit -- laziness without asynchrony and without an expressible "not here yet" does not remove a
// cost, it moves it into a frame. So the properties below are not conveniences of an API. Each one is
// a defect class that a lazy loader without it reintroduces:
//
//   * completion deferred ALWAYS -> no delegate runs before the caller has stored its own handle;
//   * the request handle IS the keep-alive -> "who wants this" and "what keeps it" cannot disagree;
//   * `Cancel()` != `Release()`, cancel delegate MANDATORY -> a cancelled caller is never left waiting
//     for a call that is not coming (`05` §2.3, and T2.3's one bolded word);
//   * one read per asset however many callers ask -> `LoadFromFile` is not reentrant anywhere in this
//     engine, and two jobs on one asset is a data race in every implementation of it;
//   * a worker read is counted ASYNC and never IN-FRAME -> otherwise the instrument that measures this
//     whole tier reports the fix as the disease.

#include <Engine/Assets/AsyncAssetLoader.hpp>
#include <Engine/Assets/SyncLoadLedger.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

using Desert::Assets::AssetBase;
using Desert::Assets::AssetPriority;
using Desert::Assets::AssetTypeID;
using Desert::Assets::AsyncAssetLoader;
using Desert::Assets::LoadOutcome;
using Desert::Assets::LoadRequest;
using Desert::Assets::SyncLoadLedger;

namespace
{
    /// An asset whose "file" is a member: the read is a sleep and a flag, so a test can hold a worker
    /// inside `LoadFromFile` for as long as it needs without touching a disk. Reading a real file
    /// would make every timing here a property of this machine's page cache.
    class ProbeAsset final : public AssetBase
    {
    public:
        explicit ProbeAsset( const std::string& name, const bool succeeds = true )
             : AssetBase( AssetPriority::Medium, Common::Filepath( name ), AssetTypeID::Unknown ),
               m_Succeeds( succeeds )
        {
        }

        Common::BoolResultStr Unload() override
        {
            m_Ready = false;
            return BOOLSUCCESS;
        }

        bool IsReadyForUse() const override
        {
            return m_Ready;
        }

        static AssetTypeID GetTypeID()
        {
            return AssetTypeID::Unknown;
        }

        /// How many times a worker actually ran the read. The number two callers must never produce.
        std::atomic<int> Reads{ 0 };
        /// Held high to keep a worker inside the read, so a test can cancel or release mid-flight.
        std::atomic<bool> HoldInsideRead{ false };
        /// Raised by the worker the moment it is inside the read; lets a test wait for that instead of
        /// sleeping a guessed interval.
        std::atomic<bool> InsideRead{ false };

    protected:
        Common::BoolResultStr LoadFromFile() override
        {
            Reads.fetch_add( 1, std::memory_order_relaxed );
            InsideRead.store( true, std::memory_order_release );
            while ( HoldInsideRead.load( std::memory_order_acquire ) )
                std::this_thread::yield();
            InsideRead.store( false, std::memory_order_release );

            if ( !m_Succeeds )
                return Common::MakeFormattedError<bool>( "the probe was told to fail" );

            m_Ready = true;
            return BOOLSUCCESS;
        }

    private:
        bool m_Ready    = false;
        bool m_Succeeds = true;
    };

    class AsyncAssetLoad : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            AsyncAssetLoader::Get().ResetForTest();
            SyncLoadLedger::ResetForTest();
        }

        void TearDown() override
        {
            AsyncAssetLoader::Get().ShutdownAndDrain();
        }

        /// Pump until the loader is quiet or the deadline passes. THE DEADLINE IS THE POINT: a test
        /// that spun forever on a wedged request would report as a hang rather than as a failure, and
        /// a hang in CI is indistinguishable from a machine that went away.
        static bool PumpUntilQuiet( const int milliseconds = 5000 )
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds( milliseconds );
            while ( std::chrono::steady_clock::now() < deadline )
            {
                AsyncAssetLoader::Get().Pump();
                if ( AsyncAssetLoader::Get().Outstanding() == 0 )
                    return true;
                std::this_thread::yield();
            }
            return false;
        }

        static void WaitUntilInsideRead( const ProbeAsset& asset )
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds( 5 );
            while ( !asset.InsideRead.load( std::memory_order_acquire ) &&
                    std::chrono::steady_clock::now() < deadline )
                std::this_thread::yield();
        }
    };
} // namespace

// ── ALWAYS DEFERRED ──────────────────────────────────────────────────────────────────────────────

TEST_F( AsyncAssetLoad, CompletionNeverRunsInsideRequestEvenWhenTheAssetIsAlreadyResident )
{
    auto asset = std::make_shared<ProbeAsset>( "resident.probe" );
    ASSERT_TRUE( asset->Load().IsSuccess() );
    ASSERT_TRUE( asset->IsReadyForUse() );

    bool ready   = false;
    auto request = AsyncAssetLoader::Get().Request(
         asset, [&ready]( const auto&, LoadOutcome, const std::string& ) { ready = true; }, [] {} );

    // THE ONE CASE THAT COULD HAVE BEEN ANSWERED IMMEDIATELY, and it is the case that would let a
    // delegate run before `request` above exists. Every caller that stores its handle in the same
    // statement -- which is every caller -- would then be re-entered mid-construction.
    EXPECT_FALSE( ready ) << "a resident asset completed inside Request(). The caller's handle did not "
                             "exist yet when its own delegate ran.";
    EXPECT_TRUE( request.IsValid() );
    EXPECT_EQ( AsyncAssetLoader::Get().Outstanding(), 1u );

    AsyncAssetLoader::Get().Pump();
    EXPECT_TRUE( ready );
    EXPECT_EQ( AsyncAssetLoader::Get().Outstanding(), 0u );

    // AND NO READ HAPPENED. A resident asset is not re-read to satisfy the deferral rule.
    EXPECT_EQ( asset->Reads.load(), 1 ) << "the resident short-circuit re-read the file.";
    EXPECT_EQ( AsyncAssetLoader::Get().StartedCount(), 0u );
}

TEST_F( AsyncAssetLoad, CompletionNeverRunsInsideRequestWhenAReadHasToHappen )
{
    auto asset = std::make_shared<ProbeAsset>( "cold.probe" );

    std::atomic<bool> ready{ false };
    auto              request = AsyncAssetLoader::Get().Request(
         asset, [&ready]( const auto&, LoadOutcome, const std::string& ) { ready.store( true ); }, [] {} );

    EXPECT_FALSE( ready.load() );
    ASSERT_TRUE( PumpUntilQuiet() );
    EXPECT_TRUE( ready.load() );
    EXPECT_TRUE( asset->IsReadyForUse() );
    EXPECT_EQ( asset->Reads.load(), 1 );
}

// ── THE HANDLE IS THE KEEP-ALIVE ─────────────────────────────────────────────────────────────────

TEST_F( AsyncAssetLoad, TheLiveRequestIsWhatKeepsTheAssetInMemory )
{
    std::weak_ptr<ProbeAsset> observer;
    {
        auto asset = std::make_shared<ProbeAsset>( "keepalive.probe" );
        observer   = asset;

        auto request = AsyncAssetLoader::Get().Request(
             asset, []( const auto&, LoadOutcome, const std::string& ) {}, [] {} );
        ASSERT_TRUE( request.IsValid() );

        // The caller drops its own reference. The request is the only thing left holding the asset,
        // and that is the design: "who wants this loaded" and "what keeps it resident" are one fact.
        asset.reset();
        EXPECT_FALSE( observer.expired() ) << "the asset died while a request for it was still live.";

        ASSERT_TRUE( PumpUntilQuiet() );
    }
    EXPECT_TRUE( observer.expired() );
}

// ── CANCEL IS NOT RELEASE ────────────────────────────────────────────────────────────────────────

TEST_F( AsyncAssetLoad, CancelFiresTheCancelDelegateAndNeverTheCompletion )
{
    auto asset = std::make_shared<ProbeAsset>( "cancelled.probe" );
    asset->HoldInsideRead.store( true );

    int  completions = 0;
    int  cancels     = 0;
    auto request     = AsyncAssetLoader::Get().Request(
         asset, [&completions]( const auto&, LoadOutcome, const std::string& ) { ++completions; },
         [&cancels] { ++cancels; } );

    WaitUntilInsideRead( *asset );
    request.Cancel();

    // DEFERRED LIKE EVERYTHING ELSE. A cancel that ran inside `Cancel()` would re-enter the caller
    // from the middle of its own teardown, which is the reentrancy the deferral rule removes.
    EXPECT_EQ( cancels, 0 );

    asset->HoldInsideRead.store( false );
    ASSERT_TRUE( PumpUntilQuiet() );

    EXPECT_EQ( cancels, 1 );
    EXPECT_EQ( completions, 0 ) << "a cancelled request also completed. One request owes exactly one "
                                   "delegate call, and which one is the caller's decision.";
    EXPECT_FALSE( request.IsValid() );
    EXPECT_EQ( AsyncAssetLoader::Get().CancelledCount(), 1u );
}

TEST_F( AsyncAssetLoad, CancelAfterTheReadLandedStillWinsOverTheQueuedCompletion )
{
    auto asset = std::make_shared<ProbeAsset>( "late-cancel.probe" );

    int  completions = 0;
    int  cancels     = 0;
    auto request     = AsyncAssetLoader::Get().Request(
         asset, [&completions]( const auto&, LoadOutcome, const std::string& ) { ++completions; },
         [&cancels] { ++cancels; } );

    // Let the worker finish, but do not pump: the completion is now queued and owed.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds( 5 );
    while ( !asset->IsReadyForUse() && std::chrono::steady_clock::now() < deadline )
        std::this_thread::yield();
    ASSERT_TRUE( asset->IsReadyForUse() );

    request.Cancel();
    ASSERT_TRUE( PumpUntilQuiet() );

    EXPECT_EQ( cancels, 1 );
    EXPECT_EQ( completions, 0 ) << "the queued completion beat a cancel that had already been asked "
                                   "for. Which delegate fires must be decided by the caller, not by "
                                   "whether a tick happened to have run.";
}

TEST_F( AsyncAssetLoad, ReleaseFiresNothingAtAll )
{
    auto asset = std::make_shared<ProbeAsset>( "released.probe" );

    int completions = 0;
    int cancels     = 0;
    {
        auto request = AsyncAssetLoader::Get().Request(
             asset, [&completions]( const auto&, LoadOutcome, const std::string& ) { ++completions; },
             [&cancels] { ++cancels; } );
        // The destructor releases. The caller stopped caring; it does not need to be told what it did.
    }

    ASSERT_TRUE( PumpUntilQuiet() );
    EXPECT_EQ( completions, 0 );
    EXPECT_EQ( cancels, 0 ) << "a released request fired the cancel delegate. `Release` and `Cancel` "
                               "are different answers to different questions; a caller that destroyed "
                               "its handle mid-teardown would be re-entered by its own cancel path.";
    EXPECT_EQ( AsyncAssetLoader::Get().CancelledCount(), 0u );
}

TEST_F( AsyncAssetLoad, CancelBeforeAWorkerReachesItSkipsTheReadEntirely )
{
    // THE HALF OF CANCEL THAT SAVES WORK. A scene closed while its content is queued must not sit
    // through the reads it no longer needs -- that is T2.3's "a scene that was closed should not wait
    // for its loads", and it is only true if the job checks before reading rather than after.
    auto blocker = std::make_shared<ProbeAsset>( "blocker.probe" );
    blocker->HoldInsideRead.store( true );

    std::vector<LoadRequest> blockers;
    const size_t             workers = 64; // more than any worker count, so the queue is certainly full
    for ( size_t i = 0; i < workers; ++i )
    {
        blockers.push_back( AsyncAssetLoader::Get().Request(
             std::make_shared<ProbeAsset>( "blocker" + std::to_string( i ) + ".probe" ),
             []( const auto&, LoadOutcome, const std::string& ) {}, [] {} ) );
        blockers.back();
    }

    auto victim = std::make_shared<ProbeAsset>( "victim.probe" );
    auto request =
         AsyncAssetLoader::Get().Request( victim, []( const auto&, LoadOutcome, const std::string& ) {}, [] {} );
    request.Cancel();

    blocker->HoldInsideRead.store( false );
    for ( auto& held : blockers )
        held.Release();
    blockers.clear();

    ASSERT_TRUE( PumpUntilQuiet() );
    EXPECT_EQ( victim->Reads.load(), 0 ) << "a request cancelled before a worker reached it was read "
                                            "anyway.";
}

// ── THE MANDATORY CANCEL DELEGATE ────────────────────────────────────────────────────────────────

TEST_F( AsyncAssetLoad, ARequestWithNoCancelDelegateIsRefusedAndQueuesNothing )
{
    auto asset = std::make_shared<ProbeAsset>( "no-cancel.probe" );

    bool ready   = false;
    auto request = AsyncAssetLoader::Get().Request(
         asset, [&ready]( const auto&, LoadOutcome, const std::string& ) { ready = true; },
         AsyncAssetLoader::OnCancel{} );

    EXPECT_FALSE( request.IsValid() );
    EXPECT_EQ( AsyncAssetLoader::Get().Outstanding(), 0u );

    ASSERT_TRUE( PumpUntilQuiet() );
    EXPECT_FALSE( ready );
    EXPECT_EQ( asset->Reads.load(), 0 ) << "a refused request read the file anyway, so the refusal was "
                                           "an empty successful answer wearing a log line.";
}

TEST_F( AsyncAssetLoad, ARequestWithNoCompletionDelegateIsRefusedAndQueuesNothing )
{
    auto asset   = std::make_shared<ProbeAsset>( "no-ready.probe" );
    auto request = AsyncAssetLoader::Get().Request( asset, AsyncAssetLoader::OnReady{}, [] {} );

    EXPECT_FALSE( request.IsValid() );
    EXPECT_EQ( AsyncAssetLoader::Get().Outstanding(), 0u );
    ASSERT_TRUE( PumpUntilQuiet() );
    EXPECT_EQ( asset->Reads.load(), 0 );
}

TEST_F( AsyncAssetLoad, ARequestWithNoAssetIsRefused )
{
    auto request =
         AsyncAssetLoader::Get().Request( nullptr, []( const auto&, LoadOutcome, const std::string& ) {}, [] {} );
    EXPECT_FALSE( request.IsValid() );
    EXPECT_EQ( AsyncAssetLoader::Get().Outstanding(), 0u );
}

// ── ONE READ PER ASSET, HOWEVER MANY CALLERS ─────────────────────────────────────────────────────

TEST_F( AsyncAssetLoad, TwoRequestsForOneAssetProduceOneReadAndTwoCompletions )
{
    auto asset = std::make_shared<ProbeAsset>( "shared.probe" );
    asset->HoldInsideRead.store( true );

    int  first  = 0;
    int  second = 0;
    auto a      = AsyncAssetLoader::Get().Request(
         asset, [&first]( const auto&, LoadOutcome, const std::string& ) { ++first; }, [] {} );
    WaitUntilInsideRead( *asset );
    auto b = AsyncAssetLoader::Get().Request(
         asset, [&second]( const auto&, LoadOutcome, const std::string& ) { ++second; }, [] {} );

    asset->HoldInsideRead.store( false );
    ASSERT_TRUE( PumpUntilQuiet() );

    EXPECT_EQ( asset->Reads.load(), 1 ) << "two requests for one asset ran two reads. `LoadFromFile` is "
                                           "not reentrant in any implementation in this engine, so that "
                                           "is a data race and not merely wasted work.";
    EXPECT_EQ( first, 1 );
    EXPECT_EQ( second, 1 ) << "the second caller for an asset already being read was never told it "
                              "arrived.";
    EXPECT_EQ( AsyncAssetLoader::Get().StartedCount(), 1u );
}

// ── FAILURE REACHES THE CALLER AS A FAILURE ──────────────────────────────────────────────────────

TEST_F( AsyncAssetLoad, AFailedReadCompletesWithFailedAndCarriesTheReason )
{
    auto asset = std::make_shared<ProbeAsset>( "broken.probe", /*succeeds=*/false );

    LoadOutcome outcome = LoadOutcome::Loaded;
    std::string reason;
    auto        request = AsyncAssetLoader::Get().Request(
         asset,
         [&outcome, &reason]( const auto&, const LoadOutcome result, const std::string& error )
         {
             outcome = result;
             reason  = error;
         },
         [] {} );

    ASSERT_TRUE( PumpUntilQuiet() );
    EXPECT_EQ( outcome, LoadOutcome::Failed );
    EXPECT_FALSE( reason.empty() ) << "a failed read reported failure with no reason, which is the "
                                      "empty answer the caller cannot log.";
}

// ── THE LEDGER TELLS THE FIX APART FROM THE DISEASE ──────────────────────────────────────────────

TEST_F( AsyncAssetLoad, AWorkerReadIsCountedAsyncAndNeverAsAnInFrameHitch )
{
    // The boot is over: every synchronous load from here on is a hitch by the ledger's definition.
    SyncLoadLedger::NoteBootFinished();
    ASSERT_EQ( SyncLoadLedger::InFrameLoads(), 0u );

    auto asset = std::make_shared<ProbeAsset>( "worker.probe" );
    auto request =
         AsyncAssetLoader::Get().Request( asset, []( const auto&, LoadOutcome, const std::string& ) {}, [] {} );
    ASSERT_TRUE( PumpUntilQuiet() );

    EXPECT_EQ( SyncLoadLedger::AsyncLoads(), 1u );
    EXPECT_EQ( SyncLoadLedger::InFrameLoads(), 0u )
         << "a read on a JobSystem worker was counted as a frame that stopped to read a file. That is "
            "the one number this whole tier is judged by, and counting async work into it would make "
            "moving fifteen cloud files off the boot look exactly like introducing fifteen hitches.";
    EXPECT_EQ( SyncLoadLedger::Loads(), 1u ) << "an async read left the total. `Loads()` is how many "
                                                "files were read; the thread does not change that.";

    // NEGATIVE CONTROL, and without it the assertion above proves only that the counter exists: the
    // SAME read on the main thread must still be counted as the hitch it is.
    auto onMainThread = std::make_shared<ProbeAsset>( "main-thread.probe" );
    ASSERT_TRUE( onMainThread->Load().IsSuccess() );
    EXPECT_EQ( SyncLoadLedger::InFrameLoads(), 1u )
         << "a blocking load on the main thread after boot was NOT counted in-frame, so the async "
            "exemption above is exempting everything.";
    EXPECT_EQ( SyncLoadLedger::AsyncLoads(), 1u );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
