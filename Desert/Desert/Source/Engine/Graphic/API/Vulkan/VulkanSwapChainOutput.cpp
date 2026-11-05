#include <Engine/Graphic/API/Vulkan/VulkanSwapChainOutput.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanRenderGraph.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanUtils/VulkanHelper.hpp>
#include <Engine/Graphic/DeviceLost.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanContext.hpp>

#include <Engine/Graphic/API/Vulkan/VulkanSwapChain.hpp>
#include <Engine/Core/EngineContext.hpp>


namespace Desert::Graphic::API::Vulkan
{
    namespace
    {
        Common::ResultStr<VkSemaphore> MakeSemaphore( VkDevice device )
        {
            const VkSemaphoreCreateInfo createInfo{
                 .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = VK_NULL_HANDLE, .flags = 0 };

            VkSemaphore semaphore = VK_NULL_HANDLE;

            VK_RETURN_RESULT_IF_FALSE_TYPE( VkSemaphore,
                                            vkCreateSemaphore( device, &createInfo, VK_NULL_HANDLE, &semaphore ) );

            return Common::MakeSuccess( semaphore );
        }
    } // namespace

    VulkanSwapChainOutput::VulkanSwapChainOutput( VulkanSwapChain* swapChain ) : m_SwapChain( swapChain )
    {
    }

    void VulkanSwapChainOutput::ApplyFrameBoundaryRebuild()
    {
        if ( !Graphic::DeviceLost::AllowWork() )
            return;

        // EVT-2c: a resize only REQUESTS a rebuild; it is applied here, at the frame boundary before anything of
        // the frame is recorded, so no frame ever records against a swapchain that changed under it. The acquire
        // is no longer here (AcquireImage, late), and that is why the two are separate: a rebuild late in the
        // frame would release the images and the composite framebuffer the frame already recorded against.
        // The rebuild destroys the images and the swapchain the submit thread's queued presents name: they go out
        // first (Application::Run joined the render task when a rebuild is pending: every frame is handed over).
        if ( m_SwapChain->RebuildPending() )
            VulkanFrameLoop::DrainSubmitThread();
        if ( const auto requested = m_SwapChain->ApplyRequestedRebuild();
             !requested.IsSuccess() && !Graphic::DeviceLost::IsLost() )
        {
            LOG_ERROR( "[SwapChain] rebuild at the frame boundary failed: {}", requested.GetError() );
        }
    }

    bool VulkanSwapChainOutput::AcquireImage()
    {
        if ( !Graphic::DeviceLost::AllowWork() )
            return false;
        if ( m_Acquired )
            return true;

        // THE SWAPCHAIN IS EXTERNALLY SYNCHRONISED (vkAcquireNextImageKHR and vkQueuePresentKHR both name it):
        // the present runs on the submit thread (VK-SUBMIT-THREAD), so the acquire holds the same mutex and first
        // waits until every image handed over was presented. Without that wait this acquire could be the second
        // image the application holds unpresented, which a swapchain of minImageCount + 1 images does not allow
        // with an infinite timeout. Normally nothing to wait: the previous frame's present went out while this
        // frame was recorded.
        std::unique_lock<std::mutex> swapchainLock( m_PresentMutex );
        if ( m_PresentsIssued != m_PresentsTaken )
        {
            DESERT_PROFILE_WAIT( "Vulkan: wait for previous present" );
            m_PresentIssued.wait( swapchainLock, [this]() { return m_PresentsIssued == m_PresentsTaken; } );
        }

        const uint32_t currentIndex = EngineContext::GetInstance().GetCurrentFrameIndex();

        // SUBOPTIMAL IS AN IMAGE; OUT_OF_DATE IS NONE, and the rule for both is Graphic::AcquireLate
        // (Engine/Graphic/SwapchainAcquire.hpp): an OUT_OF_DATE requests the rebuild for the next frame boundary
        // (ApplyFrameBoundaryRebuild), this frame is submitted without the output and not presented, and the
        // next frame acquires from the rebuilt swapchain. The window's resize callback requests the rebuild
        // first in the common case, so an OUT_OF_DATE here is the rare case of a surface that changed between
        // the boundary and the last pass.
        auto* const presentComplete = m_FrameSemaphores[currentIndex].PresentComplete;
        const auto  held =
             Graphic::AcquireLate( [&] { return m_SwapChain->AcquireNextImage( presentComplete, &m_ImageIndex ); },
                                   [&] { m_SwapChain->RequestRebuildAtCurrentSize(); } );
        if ( !held.IsSuccess() )
        {
            // A LOST DEVICE IS NOT A RESIZE: AcquireNextImage refuses on a lost device and the latch already
            // carries the explanation, so only a failure of another kind is worth a line here.
            if ( !Graphic::DeviceLost::IsLost() )
                LOG_ERROR( "[AcquireNextImage] {}", held.GetError() );
            return false;
        }
        if ( !held.GetValue() )
            return false;
        m_Acquired = true;
        return true;
    }

    VulkanFrameOutput VulkanSwapChainOutput::GetFrameOutput() const
    {
        const Semaphores& slot = m_FrameSemaphores[EngineContext::GetInstance().GetCurrentFrameIndex()];
        return VulkanFrameOutput{ .ImageAcquired  = slot.PresentComplete,
                                  .ImageFirstUse  = RdgVulkanStages( RDG::kPresentAcquiredState.Stages ),
                                  .RenderComplete = slot.RenderComplete };
    }

    std::optional<VulkanPresentRequest> VulkanSwapChainOutput::TakePresent()
    {
        // A frame that held no image (it never imported the back buffer, or its late acquire was OUT_OF_DATE)
        // was submitted without the output: nothing signals RenderComplete and there is no image to hand back.
        if ( !m_Acquired )
            return std::nullopt;
        const uint32_t currentIndex = EngineContext::GetInstance().GetCurrentFrameIndex();
        // Given up by the hand-off: the submit thread presents it, whatever the present answers (an OUT_OF_DATE
        // or SUBOPTIMAL present still releases the image).
        m_Acquired = false;
        {
            const std::scoped_lock swapchainLock( m_PresentMutex );
            ++m_PresentsTaken;
        }
        return VulkanPresentRequest{ .Output         = this,
                                     .ImageIndex     = m_ImageIndex,
                                     .RenderComplete = m_FrameSemaphores[currentIndex].RenderComplete };
    }

    void VulkanSwapChainOutput::Present()
    {
        if ( !Graphic::DeviceLost::AllowWork() )
            return;
        if ( const std::optional<VulkanPresentRequest> taken = TakePresent() )
        {
            VulkanFrameLoop::DrainSubmitThread();
            Present( *taken );
        }
    }

    void VulkanSwapChainOutput::AbandonPresent( const VulkanPresentRequest& /*request*/ )
    {
        {
            const std::scoped_lock swapchainLock( m_PresentMutex );
            ++m_PresentsIssued;
        }
        m_PresentIssued.notify_all();
    }

    void VulkanSwapChainOutput::Present( const VulkanPresentRequest& request )
    {
        // A WAIT row (FPS-CPU-2, PRESENT-COST): vkQueuePresentKHR blocks while the swapchain's queue of
        // presented images is full (vsync, or the display behind the GPU); the driver's own present work is in
        // the same row and is small next to that. Enclosing rows count it in their wait column.
        DESERT_PROFILE_WAIT( "Vulkan: Present" );
        // The swapchain is externally synchronised: this present and the frame thread's acquire hold one mutex,
        // and the present counts as issued however it ends, so the next acquire never waits for it forever.
        std::unique_lock<std::mutex> swapchainLock( m_PresentMutex );
        if ( Graphic::DeviceLost::AllowWork() )
        {
            const auto& queue =
                 SP_CAST( VulkanLogicalDevice, EngineContext::GetInstance().GetDevice() )->GetGraphicsQueue();
            // An OUT_OF_DATE or SUBOPTIMAL answer requests the rebuild (QueuePresent). The request is
            // mutex-guarded; Application::Run reads it at the next frame boundary, joins the render task, and
            // ApplyFrameBoundaryRebuild drains this thread before it rebuilds.
            const auto& queuePresent = QueuePresent( queue, request.ImageIndex, request.RenderComplete );
            if ( !queuePresent.IsSuccess() )
            {
                LOG_INFO( "[QueuePresent] Error: {}", queuePresent.GetError() );
            }
        }
        ++m_PresentsIssued;
        swapchainLock.unlock();
        m_PresentIssued.notify_all();

        // Present is the OTHER place the loss surfaces first (a submit executes asynchronously, so the
        // frame that killed the device is usually already gone by the time anyone is told). The frame loop
        // asks DeviceLost::IsLost() after this returns, before it waits any fence of the dead device.
    }

    Common::ResultStr<VkResult> VulkanSwapChainOutput::QueuePresent( VkQueue queue, uint32_t imageIndex,
                                                                     VkSemaphore waitSemaphore )
    {
        VkPresentInfoKHR presentInfo = {};
        presentInfo.sType            = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        presentInfo.pNext            = nullptr;
        presentInfo.swapchainCount   = 1;
        presentInfo.pSwapchains      = &m_SwapChain->m_SwapChain;
        presentInfo.pImageIndices    = &imageIndex;
        if ( waitSemaphore != VK_NULL_HANDLE )
        {
            presentInfo.pWaitSemaphores    = &waitSemaphore;
            presentInfo.waitSemaphoreCount = 1;
        }
        auto res = SP_CAST( VulkanLogicalDevice, EngineContext::GetInstance().GetDevice() )
                        ->PresentToQueue( queue, presentInfo );
        if ( res == VK_SUCCESS )
        {
            return Common::MakeSuccess( VK_SUCCESS );
        }

        // ASKED BEFORE THE RESIZE BRANCH, and the order is the fix. Device loss and "the window moved" are
        // both non-VK_SUCCESS results out of the same call, and answering the wrong one rebuilds a
        // swapchain on a dead device.
        if ( NoteIfDeviceLost( res, "vkQueuePresentKHR", __FILE__, __LINE__ ) )
            return Common::MakeFormattedError<VkResult>( "result: {}", VkResultToString( res ) );

        // Window was resized/minimized between acquire and present: the rebuild is requested and applied at the
        // next frame boundary (AcquireImage), the same path as an OUT_OF_DATE acquire. This frame is handled.
        if ( res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR )
        {
            m_SwapChain->RequestRebuildAtCurrentSize();
            return Common::MakeSuccess( VK_SUCCESS );
        }

        return Common::MakeFormattedError<VkResult>( "result: {}", VkResultToString( res ) );
    }

    Common::ResultStr<VkResult> VulkanSwapChainOutput::Init()
    {
        VkDevice device =
             SP_CAST( VulkanLogicalDevice, EngineContext::GetInstance().GetDevice() )->GetVulkanLogicalDevice();

        const uint32_t backBufferCount = m_SwapChain->GetBackBufferCount();

        m_FrameSemaphores.resize( backBufferCount );
        for ( uint32_t i = 0; i < backBufferCount; i++ )
        {
            // These two survived Г7's by-name pass over unchecked allocations for one reason: there
            // was no name to look at. A failed vkCreateSemaphore stored VK_NULL_HANDLE and every
            // frame afterwards submitted against a null semaphore — the failure was indistinguishable
            // from success at the assignment, and the validation layer complained somewhere else
            // entirely. Init returns a result, so it can refuse instead.
            auto present = MakeSemaphore( device );
            if ( !present )
            {
                return Common::MakeFormattedError<VkResult>( "frame {} present semaphore: {}", i,
                                                             present.GetError() );
            }
            auto render = MakeSemaphore( device );
            if ( !render )
            {
                return Common::MakeFormattedError<VkResult>( "frame {} render semaphore: {}", i,
                                                             render.GetError() );
            }
            m_FrameSemaphores[i].PresentComplete = present.GetValue();
            m_FrameSemaphores[i].RenderComplete  = render.GetValue();
        }

        return Common::MakeSuccess( VK_SUCCESS );
    }

    VulkanSwapChainOutput::~VulkanSwapChainOutput()
    {
        Release();
    }

    void VulkanSwapChainOutput::Release()
    {
        // The engine's device is reached through EngineContext rather than held, and at teardown it can
        // already be gone: dropping the handles is then the correct answer, because vkDestroyDevice takes
        // its own children with it and a destroyed device cannot be passed to vkDestroySemaphore.
        const auto engineDevice = EngineContext::GetInstance().GetDevice();
        if ( !engineDevice )
        {
            m_FrameSemaphores.clear();
            return;
        }

        VkDevice device = SP_CAST( VulkanLogicalDevice, engineDevice )->GetVulkanLogicalDevice();
        if ( device == VK_NULL_HANDLE )
        {
            m_FrameSemaphores.clear();
            return;
        }

        for ( auto& sem : m_FrameSemaphores )
        {
            if ( sem.PresentComplete != VK_NULL_HANDLE )
                vkDestroySemaphore( device, sem.PresentComplete, nullptr );
            if ( sem.RenderComplete != VK_NULL_HANDLE )
                vkDestroySemaphore( device, sem.RenderComplete, nullptr );
        }
        m_FrameSemaphores.clear();
    }
} // namespace Desert::Graphic::API::Vulkan
