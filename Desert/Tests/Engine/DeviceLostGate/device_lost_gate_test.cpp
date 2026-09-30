// AFTER THE FIRST VK_ERROR_DEVICE_LOST THE ENGINE ISSUES NO MORE DEVICE WORK.
//
// That sentence is a RELATION between two things — what the driver told us, and what we did next — and it
// is exactly the relation nobody was asserting. The incident it comes from: a submit failed
// asynchronously (`kIOGPUCommandBufferCallbackErrorInnocentVictim`, another process's GPU reset took this
// one with it), and the engine went on to reset a fence the dead device still held, acquire an image on a
// semaphore with pending operations, tear down the swapchain and build a new one — dying at last inside
// VK_CHECK_RESULT with every unsaved change gone.
//
// WHAT THIS SUITE PROVES AND WHAT IT DOES NOT. It drives `Graphic::DeviceLost` — the latch every discovery
// point reports into and every work-issuing entry point asks — through the exact sequence of that
// incident, and asserts the count of work admitted after the loss is ZERO. It cannot prove that the
// production call sites route through the latch: that is a statement about the source text, and its own
// suite (`DeviceLostCensus`) makes it one.
//
// Together the two are the property. Apart, each is a side of it — which is the defect shape this
// project's verification skill spends a whole section on.

#include <Engine/Graphic/DeviceLost.hpp>

#include <gtest/gtest.h>

// <atomic> is named explicitly rather than left to whatever <thread> happens to pull in. It compiles
// either way on this toolchain, and the MSVC build is the one that would find out otherwise -- the same
// class of Windows-only surprise this project has already paid for twice.
#include <atomic>
#include <string>
#include <thread>
#include <vector>

using Desert::Graphic::DeviceLost;

namespace
{
    // Restores the latch, because it is process-wide by design (a device that came back is not a thing
    // that happens, so nothing in the engine clears it) and gtest runs every case in one process.
    class LatchGuard
    {
    public:
        LatchGuard()
        {
            DeviceLost::ResetForTests();
        }
        ~LatchGuard()
        {
            DeviceLost::ResetForTests();
        }
        LatchGuard( const LatchGuard& )            = delete;
        LatchGuard& operator=( const LatchGuard& ) = delete;
    };

    // The frame loop, in the order the recorded crash walked it. Each entry is a point that would issue
    // device work; `AllowWork()` is what each of them asks before doing so.
    const char* const k_FrameSequence[] = {
         "VulkanQueue::PrepareFrame",              // vkResetFences  <- "pFences[0] is in use"
         "VulkanSwapChain::AcquireNextImage",      // vkAcquireNextImageKHR <- "Semaphore must not have..."
         "VulkanSwapChain::ApplyRequestedRebuild", // the rebuild
         "VulkanSwapChain::CreateSwapChain",       // vkCreateSwapchainKHR <- the abort, at line 165
         "VulkanRendererAPI::BeginFrame",          // vkBeginCommandBuffer
         "VulkanQueue::Submit",                    // vkQueueSubmit
         "VulkanQueue::Present",                   // vkQueuePresentKHR + vkWaitForFences
         "VulkanRendererAPI::WaitDeviceIdle",      // vkDeviceWaitIdle
    };

    // Runs the sequence and answers how many of its steps were allowed to issue work.
    std::size_t WorkIssuedOverOneFrame()
    {
        std::size_t issued = 0;
        for ( const char* step : k_FrameSequence )
        {
            (void)step;
            if ( DeviceLost::AllowWork() )
                ++issued;
        }
        return issued;
    }
} // namespace

TEST( DeviceLostGate, AHealthyDeviceAdmitsEveryStepOfTheFrame )
{
    LatchGuard guard;

    // The control. Without it, "zero work after the loss" is satisfied by a gate that never lets anything
    // through at all, and the suite would pass over an engine that cannot draw.
    EXPECT_FALSE( DeviceLost::IsLost() );
    EXPECT_EQ( WorkIssuedOverOneFrame(), std::size( k_FrameSequence ) );
    EXPECT_EQ( DeviceLost::RefusedCalls(), 0u );
    EXPECT_EQ( DeviceLost::ReportCount(), 0u );
}

TEST( DeviceLostGate, AfterTheFirstLossNotOneFurtherCallIsIssued )
{
    LatchGuard guard;

    // One frame's worth of work goes out normally...
    ASSERT_EQ( WorkIssuedOverOneFrame(), std::size( k_FrameSequence ) );

    // ...then the submit of that frame fails asynchronously. This is the whole event.
    EXPECT_TRUE( DeviceLost::Report( "VulkanQueue::Submit / vkQueueSubmit", "VK_ERROR_DEVICE_LOST" ) );
    EXPECT_TRUE( DeviceLost::IsLost() );

    // THE ASSERTION THE TASK IS ABOUT. Ten frames' worth of the same sequence, and not one step of it is
    // allowed to reach the device. Before this change the count over the first of those frames was at
    // least four — reset, acquire, teardown, create — and the fourth aborted the process.
    std::size_t issuedAfterLoss = 0;
    for ( int frame = 0; frame < 10; ++frame )
        issuedAfterLoss += WorkIssuedOverOneFrame();

    EXPECT_EQ( issuedAfterLoss, 0u ) << "the engine issued " << issuedAfterLoss
                                     << " Vulkan calls at a device it had already been told was gone";
    EXPECT_EQ( DeviceLost::RefusedCalls(), 10u * std::size( k_FrameSequence ) );
}

TEST( DeviceLostGate, TheHumanIsToldOnceNoMatterHowManyPlacesNoticed )
{
    LatchGuard guard;

    // Every route into the loss reports, because whichever one gets there first is not knowable in
    // advance: the submit, the fence reset, the acquire, the present, the driver's own callback.
    EXPECT_TRUE( DeviceLost::Report( "the Vulkan driver's own debug callback", "Lost VkDevice after ..." ) );
    EXPECT_FALSE( DeviceLost::Report( "VulkanQueue::PrepareFrame / vkResetFences", "VK_ERROR_DEVICE_LOST" ) );
    EXPECT_FALSE(
         DeviceLost::Report( "VulkanSwapChain::CreateSwapChain / vkCreateSwapchainKHR", "VK_ERROR_DEVICE_LOST" ) );
    EXPECT_FALSE( DeviceLost::Report( "VulkanQueue::Present / vkQueuePresentKHR", "VK_ERROR_DEVICE_LOST" ) );

    // ONE explanation, from the FIRST reporter. The twenty consequence lines are exactly what sent a
    // reader off to fix a synchronisation defect that did not exist.
    EXPECT_EQ( DeviceLost::ReportCount(), 1u );
    EXPECT_EQ( DeviceLost::FirstSite(), "the Vulkan driver's own debug callback" );
}

TEST( DeviceLostGate, TheExplanationNamesTheCauseTheRemedyAndTheTrap )
{
    LatchGuard guard;

    (void)DeviceLost::Report( "VulkanQueue::Submit / vkQueueSubmit", "VK_ERROR_DEVICE_LOST" );
    const std::string text = DeviceLost::Explanation();

    // Not a spelling check on prose — these four are the four things the message exists to say, and a
    // rewrite that drops any of them has removed the point of it rather than reworded it.
    EXPECT_NE( text.find( "VK_ERROR_DEVICE_LOST" ), std::string::npos ) << "the driver's own word for it";
    EXPECT_NE( text.find( "innocent victim" ), std::string::npos )
         << "that this is almost certainly someone else's GPU reset and not the user's doing";
    EXPECT_NE( text.find( "autosave" ), std::string::npos ) << "what happens to unsaved work";
    EXPECT_NE( text.find( "consequence" ), std::string::npos )
         << "that every Vulkan error after this line follows from it -- the trap that cost an "
            "afternoon of looking for a synchronisation defect that was not there";
}

TEST( DeviceLostGate, TwoThreadsDiscoveringItAtOnceStillProduceOneMessage )
{
    LatchGuard guard;

    // Not theoretical: the driver's debug callback lands on whatever thread the Metal command buffer
    // completed on, while the render thread is inside its own next call. Both meet the loss.
    std::vector<std::thread> racers;
    std::atomic<int>         firsts{ 0 };
    for ( int i = 0; i < 8; ++i )
        racers.emplace_back(
             [&firsts]
             {
                 if ( DeviceLost::Report( "a racing reporter", "VK_ERROR_DEVICE_LOST" ) )
                     firsts.fetch_add( 1 );
             } );
    for ( auto& t : racers )
        t.join();

    EXPECT_EQ( firsts.load(), 1 ) << "exactly one reporter must own the event";
    EXPECT_EQ( DeviceLost::ReportCount(), 1u );
    EXPECT_TRUE( DeviceLost::IsLost() );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
