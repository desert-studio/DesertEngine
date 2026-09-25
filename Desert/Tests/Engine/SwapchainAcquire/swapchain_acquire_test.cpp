// A SUBOPTIMAL ACQUIRE HAS ACQUIRED, AND A SWAPCHAIN REBUILD DOES NOT MOVE THE FRAME SLOT.
//
// The incident (SC1, every editor launch): showing the window after the splash made the acquire answer
// VK_SUBOPTIMAL_KHR. The frame loop treated it as "no image", rebuilt the swapchain -- which reset the
// frame-in-flight slot to 0 mid-frame -- and acquired again on the semaphore the first acquire had already
// signalled. The submit then waited on slot 0's semaphore, which nothing would ever signal, and the GPU
// timed out into VK_ERROR_DEVICE_LOST.
//
// The fake swapchain below keeps the one piece of state that matters, the pending signal on each slot's
// acquire semaphore, and fails the way the validation layer did. The loop driving it is the production
// rule (Graphic::AcquireForFrame) and the production slot bookkeeping (Engine::FrameManager).

#include <Engine/Core/FrameManager.hpp>
#include <Engine/Graphic/SwapchainAcquire.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <deque>
#include <string>

namespace
{
    using Desert::Engine::FrameManager;
    using Desert::Graphic::AcquireStatus;
    using Desert::Graphic::AcquireForFrame;

    constexpr uint32_t kImages = 3;

    struct FakeSwapchain
    {
        std::array<bool, kImages> SemaphoreSignalled{}; // per frame slot
        std::deque<AcquireStatus> Answers;              // what the driver says, in order
        int                       Rebuilds = 0;
        std::string               Violation;

        Common::ResultStr<AcquireStatus> Acquire( const uint32_t slot )
        {
            if ( SemaphoreSignalled[slot] )
                Violation = "vkAcquireNextImageKHR(): Semaphore must not be currently signaled";
            const AcquireStatus answer = Answers.empty() ? AcquireStatus::Acquired : Answers.front();
            if ( !Answers.empty() )
                Answers.pop_front();
            if ( Desert::Graphic::SignalsSemaphore( answer ) )
                SemaphoreSignalled[slot] = true;
            return Common::MakeSuccess( answer );
        }

        Common::ResultStr<bool> Rebuild()
        {
            ++Rebuilds;
            if ( !FrameManager::GetInstance().AdoptSwapchainImageCount( kImages ) )
                return Common::MakeError<bool>( "image count changed" );
            return Common::MakeSuccess( true );
        }

        void Submit( const uint32_t slot )
        {
            if ( !SemaphoreSignalled[slot] )
                Violation = "vkQueueSubmit(): waiting on semaphore that has no way to be signaled";
            SemaphoreSignalled[slot] = false;
        }
    };

    // One frame exactly as VulkanQueue::PrepareFrame / Submit / Present run it: the slot is read once before
    // the acquire, and the submit reads it again.
    void RunFrame( FakeSwapchain& swapchain )
    {
        auto&          frames = FrameManager::GetInstance();
        const uint32_t slot   = frames.GetCurrentFrameIndex();
        const auto     result = AcquireForFrame( [&] { return swapchain.Acquire( slot ); },
                                                 [&] { return swapchain.Rebuild(); } );
        ASSERT_TRUE( result.IsSuccess() ) << result.GetError();
        swapchain.Submit( frames.GetCurrentFrameIndex() );
        frames.NextFrame();
    }

    class SwapchainAcquire : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            // The first swapchain fixes the count (and resets the slot, once, for the whole process).
            ASSERT_TRUE( FrameManager::GetInstance().AdoptSwapchainImageCount( kImages ) );
            while ( FrameManager::GetInstance().GetCurrentFrameIndex() != 0 )
                FrameManager::GetInstance().NextFrame();
        }
    };
} // namespace

TEST_F( SwapchainAcquire, SuboptimalIsAnImageAndDoesNotRebuildBeforeRecording )
{
    FakeSwapchain swapchain;
    RunFrame( swapchain ); // slot 0
    swapchain.Answers = { AcquireStatus::AcquiredSuboptimal };
    RunFrame( swapchain ); // slot 1: the reveal frame
    RunFrame( swapchain );
    EXPECT_EQ( swapchain.Rebuilds, 0 );
    EXPECT_EQ( swapchain.Violation, "" );
}

TEST_F( SwapchainAcquire, OutOfDateRebuildsAndKeepsTheFrameSlot )
{
    FakeSwapchain swapchain;
    RunFrame( swapchain ); // slot 0
    swapchain.Answers = { AcquireStatus::OutOfDate, AcquireStatus::Acquired };
    RunFrame( swapchain ); // slot 1, rebuilt mid-PrepareFrame
    RunFrame( swapchain );
    RunFrame( swapchain );
    EXPECT_EQ( swapchain.Rebuilds, 1 );
    EXPECT_EQ( swapchain.Violation, "" );
}

TEST_F( SwapchainAcquire, RebuildKeepsTheAbsoluteFrameCount )
{
    auto&          frames = FrameManager::GetInstance();
    const uint64_t before = frames.GetAbsoluteFrameCount();
    frames.NextFrame();
    const uint32_t slot = frames.GetCurrentFrameIndex();
    ASSERT_TRUE( frames.AdoptSwapchainImageCount( kImages ) );
    EXPECT_EQ( frames.GetCurrentFrameIndex(), slot );
    EXPECT_EQ( frames.GetAbsoluteFrameCount(), before + 1 );
}

TEST_F( SwapchainAcquire, RebuildWithADifferentImageCountIsRefused )
{
    EXPECT_FALSE( FrameManager::GetInstance().AdoptSwapchainImageCount( kImages + 1 ) );
    EXPECT_EQ( FrameManager::GetInstance().GetMaxFramesInFlight(), kImages );
}

TEST_F( SwapchainAcquire, OutOfDateTwiceInARowIsANamedFailure )
{
    FakeSwapchain swapchain;
    swapchain.Answers = { AcquireStatus::OutOfDate, AcquireStatus::OutOfDate };
    const auto result =
         AcquireForFrame( [&] { return swapchain.Acquire( 0 ); }, [&] { return swapchain.Rebuild(); } );
    ASSERT_FALSE( result.IsSuccess() );
    EXPECT_NE( result.GetError().find( "out of date again" ), std::string::npos );
}

int main( int argc, char** argv )
{
    // The engine creates this singleton at startup; GetInstance on an uncreated one dereferences null.
    Desert::Engine::FrameManager::CreateInstance();
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
