#include <Engine/Graphic/API/Vulkan/VulkanQueue.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanUtils/VulkanHelper.hpp>
#include <Engine/Graphic/DeviceLost.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanContext.hpp>

#include <Engine/Graphic/API/Vulkan/VulkanSwapChain.hpp>
#include <Engine/Graphic/API/Vulkan/CommandBufferAllocator.hpp>
#include <Engine/Core/EngineContext.hpp>

#include <Common/Core/DestructorGuard.hpp>

namespace Desert::Graphic::API::Vulkan
{
    namespace
    {
        Common::ResultStr<VkSemaphore> CreateSemaphore( VkDevice device )
        {
            VkSemaphoreCreateInfo createInfo{
                 .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = VK_NULL_HANDLE, .flags = 0 };

            VkSemaphore semaphore;

            VK_RETURN_RESULT_IF_FALSE_TYPE( VkSemaphore,
                                            vkCreateSemaphore( device, &createInfo, VK_NULL_HANDLE, &semaphore ) );

            return Common::MakeSuccess( semaphore );
        }
    } // namespace

    VulkanQueue::VulkanQueue( VulkanSwapChain* swapChain ) : m_SwapChain( swapChain )
    {
    }

    void VulkanQueue::PrepareFrame()
    {
        if ( !Graphic::DeviceLost::AllowWork() )
            return;

        uint32_t currentIndex = EngineContext::GetInstance().GetCurrentFrameIndex();

        VkDevice device = SP_CAST( VulkanLogicalDevice, EngineContext::GetInstance().GetDevice() )
                               ->GetVulkanLogicalDevice();

        // THIS RESULT USED TO GO STRAIGHT ON THE FLOOR, and it is the first line of the crash that
        // motivated all of this: "vkResetFences(): pFences[0] is in use. (a VK_ERROR_DEVICE_LOST has
        // occurred, the fence must be destroyed)". The engine had already lost the device one submit
        // earlier, asked nothing, and carried on into an acquire and a swapchain rebuild. Reading the
        // result here is what turns three further illegal calls into one honest stop.
        const VkResult reset = vkResetFences( device, 1, &m_WaitFences[currentIndex] );
        if ( reset != VK_SUCCESS )
        {
            if ( !NoteIfDeviceLost( reset, "vkResetFences", __FILE__, __LINE__ ) )
                LOG_ERROR( "[PrepareFrame] vkResetFences failed: {}", VkResultToString( reset ) );
            return;
        }

        const auto acquire = m_SwapChain->AcquireNextImage( m_FrameSemaphores[currentIndex].PresentComplete, &m_ImageIndex );
        if ( !acquire )
        {
            // A LOST DEVICE IS NOT A RESIZE, and telling them apart is the whole fix. Recreating the
            // swapchain here is right for VK_ERROR_OUT_OF_DATE_KHR and catastrophic for device loss: the
            // rebuild is what reached vkCreateSwapchainKHR, got VK_ERROR_DEVICE_LOST from it, and aborted
            // inside VK_CHECK_RESULT with everything unsaved. AcquireNextImage latches on the way out, so
            // this question is already answered by the time it returns.
            if ( Graphic::DeviceLost::IsLost() )
                return;

            // Most commonly VK_ERROR_OUT_OF_DATE_KHR after a window resize — recreate the swapchain (it
            // re-queries the surface extent) and re-acquire from the fresh swapchain.
            m_SwapChain->OnResize( m_SwapChain->GetWidth(), m_SwapChain->GetHeight() );
            if ( Graphic::DeviceLost::IsLost() )
                return;
            const auto reacquire =
                 m_SwapChain->AcquireNextImage( m_FrameSemaphores[currentIndex].PresentComplete, &m_ImageIndex );
            if ( !reacquire )
                LOG_ERROR( "[AcquireNextImage] Error after swapchain recreate: {}", reacquire.GetError() );
        }
    }

    void VulkanQueue::Submit()
    {
        if ( !Graphic::DeviceLost::AllowWork() )
            return;

        uint32_t currentIndex = EngineContext::GetInstance().GetCurrentFrameIndex();

        const auto& queue =
             SP_CAST( VulkanLogicalDevice, EngineContext::GetInstance().GetDevice() )->GetGraphicsQueue();

        VkPipelineStageFlags waitStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo         submitInfo    = {};
        submitInfo.sType                   = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.pWaitDstStageMask       = &waitStageMask;
        submitInfo.pWaitSemaphores         = &m_FrameSemaphores[currentIndex].PresentComplete;
        submitInfo.waitSemaphoreCount      = 1;
        submitInfo.pSignalSemaphores       = &m_FrameSemaphores[currentIndex].RenderComplete;
        submitInfo.signalSemaphoreCount    = 1;
        submitInfo.pCommandBuffers         = &m_DrawCommandBuffers[currentIndex];
        submitInfo.commandBufferCount      = 1;

        VK_CHECK_RESULT( vkQueueSubmit( queue, 1, &submitInfo, m_WaitFences[currentIndex] ) );
    }

    void VulkanQueue::Present()
    {
        if ( !Graphic::DeviceLost::AllowWork() )
            return;

        uint32_t currentIndex = EngineContext::GetInstance().GetCurrentFrameIndex();
        VkDevice device = SP_CAST( VulkanLogicalDevice, EngineContext::GetInstance().GetDevice() )
                               ->GetVulkanLogicalDevice();
        const auto& queue =
             SP_CAST( VulkanLogicalDevice, EngineContext::GetInstance().GetDevice() )->GetGraphicsQueue();

        const auto& queuePresent = QueuePresent( queue, m_ImageIndex, m_FrameSemaphores[currentIndex].RenderComplete );
        if ( !queuePresent.IsSuccess() )
        {
            LOG_INFO( "[QueuePresent] Error: {}", queuePresent.GetError() );
        }

        // Present is the OTHER place the loss surfaces first (a submit executes asynchronously, so the
        // frame that killed the device is usually already gone by the time anyone is told). Nothing below
        // may run once it has: waiting on a fence of a dead device is the second illegal call in the
        // recorded sequence, and the deletion queue destroys objects the frame still nominally owns.
        if ( Graphic::DeviceLost::IsLost() )
            return;

        Engine::FrameManager::GetInstance().NextFrame();
        uint32_t       newCurrentFrame = Engine::FrameManager::GetInstance().GetCurrentFrameIndex();
        const VkResult waited = vkWaitForFences( device, 1, &m_WaitFences[newCurrentFrame], VK_TRUE, UINT64_MAX );
        if ( waited != VK_SUCCESS )
        {
            // Also previously unchecked. A dropped result here is worse than most: this is the ONE place
            // the CPU learns that a submitted frame finished, so a device that died mid-frame reports it
            // here first and nowhere else.
            if ( !NoteIfDeviceLost( waited, "vkWaitForFences", __FILE__, __LINE__ ) )
                LOG_ERROR( "[Present] vkWaitForFences failed: {}", VkResultToString( waited ) );
            return;
        }

        SP_CAST( VulkanContext, EngineContext::GetInstance().GetRendererContext() )
             ->GetVulkanAllocator()
             ->ProcessDeletionQueue();
    }

    VkCommandBuffer VulkanQueue::GetDrawCommandBuffer() const
    {
        return m_DrawCommandBuffers[EngineContext::GetInstance().GetCurrentFrameIndex()];
    }

    Common::ResultStr<VkResult> VulkanQueue::QueuePresent( VkQueue queue, uint32_t imageIndex,
                                                        VkSemaphore waitSemaphore )
    {
        VkPresentInfoKHR presentInfo = {};
        presentInfo.sType            = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        presentInfo.pNext            = NULL;
        presentInfo.swapchainCount   = 1;
        presentInfo.pSwapchains      = &m_SwapChain->m_SwapChain;
        presentInfo.pImageIndices    = &imageIndex;
        if ( waitSemaphore != VK_NULL_HANDLE )
        {
            presentInfo.pWaitSemaphores    = &waitSemaphore;
            presentInfo.waitSemaphoreCount = 1;
        }
        auto res = ( vkQueuePresentKHR( queue, &presentInfo ) );
        if ( res == VK_SUCCESS )
        {
            return Common::MakeSuccess( VK_SUCCESS );
        }

        // ASKED BEFORE THE RESIZE BRANCH, and the order is the fix. Device loss and "the window moved" are
        // both non-VK_SUCCESS results out of the same call, and answering the wrong one rebuilds a
        // swapchain on a dead device.
        if ( NoteIfDeviceLost( res, "vkQueuePresentKHR", __FILE__, __LINE__ ) )
            return Common::MakeFormattedError<VkResult>( "result: {}", VkResultToString( res ) );

        // Window was resized/minimized between acquire and present — recreate the swapchain (it re-queries
        // the current surface extent) and treat this frame as handled. Standard Vulkan resize handling.
        if ( res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR )
        {
            m_SwapChain->OnResize( m_SwapChain->GetWidth(), m_SwapChain->GetHeight() );
            return Common::MakeSuccess( VK_SUCCESS );
        }

        return Common::MakeFormattedError<VkResult>( "result: {}", VkResultToString( res ) );
    }

    Common::ResultStr<VkResult> VulkanQueue::Init()
    {
        VkDevice device = SP_CAST( VulkanLogicalDevice, EngineContext::GetInstance().GetDevice() )
                               ->GetVulkanLogicalDevice();

        uint32_t backBufferCount = m_SwapChain->GetBackBufferCount();

        m_FrameSemaphores.resize( backBufferCount );
        for ( uint32_t i = 0; i < backBufferCount; i++ )
        {
            // These two survived Г7's by-name pass over unchecked allocations for one reason: there
            // was no name to look at. A failed vkCreateSemaphore stored VK_NULL_HANDLE and every
            // frame afterwards submitted against a null semaphore — the failure was indistinguishable
            // from success at the assignment, and the validation layer complained somewhere else
            // entirely. Init returns a result, so it can refuse instead.
            auto present = CreateSemaphore( device );
            if ( !present )
            {
                return Common::MakeFormattedError<VkResult>( "frame {} present semaphore: {}", i,
                                                             present.GetError() );
            }
            auto render = CreateSemaphore( device );
            if ( !render )
            {
                return Common::MakeFormattedError<VkResult>( "frame {} render semaphore: {}", i,
                                                             render.GetError() );
            }
            m_FrameSemaphores[i].PresentComplete = present.GetValue();
            m_FrameSemaphores[i].RenderComplete  = render.GetValue();
        }

        m_DrawCommandBuffers.resize( backBufferCount );
        m_ComputeCommandBuffers.resize( backBufferCount );

        for ( uint32_t i = 0; i < backBufferCount; i++ )
        {
            // graphic
            {
                const auto&                 gPool = CommandBufferAllocator::GetInstance().GetCommandGraphicPool();
                VkCommandBufferAllocateInfo allocateInfo;
                allocateInfo.pNext              = VK_NULL_HANDLE;
                allocateInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
                allocateInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                allocateInfo.commandBufferCount = 1;
                allocateInfo.commandPool        = gPool[i];

                VK_RETURN_RESULT_IF_FALSE_TYPE(
                     VkResult, vkAllocateCommandBuffers( device, &allocateInfo, &m_DrawCommandBuffers[i] ) );
            }

            // compute
            {
                const auto& cPool = CommandBufferAllocator::GetInstance().GetCommandComputePool();

                VkCommandBufferAllocateInfo allocateInfo;
                allocateInfo.pNext              = VK_NULL_HANDLE;
                allocateInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
                allocateInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                allocateInfo.commandBufferCount = 1;
                allocateInfo.commandPool        = cPool[i];

                VK_RETURN_RESULT_IF_FALSE_TYPE(
                     VkResult, vkAllocateCommandBuffers( device, &allocateInfo, &m_ComputeCommandBuffers[i] ) );
            }
        }

        m_WaitFences.resize( backBufferCount );

        VkFenceCreateInfo fenceCreateInfo{};
        fenceCreateInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceCreateInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        for ( size_t i = 0; i < m_WaitFences.size(); ++i )
        {

            VK_RETURN_RESULT_IF_FALSE(
                 vkCreateFence( device, &fenceCreateInfo, nullptr, &m_WaitFences[i] ) );
        }

        return Common::MakeSuccess( VK_SUCCESS );
    }

    VulkanQueue::~VulkanQueue()
    try
    {
        Release();
    }
    DESERT_DESTRUCTOR_GUARD( "~VulkanQueue" )

    void VulkanQueue::Release()
    {
        // The engine's device is reached through EngineContext rather than held, and at teardown it can
        // already be gone: dropping the handles is then the correct answer, because vkDestroyDevice takes
        // its own children with it and a destroyed device cannot be passed to vkDestroySemaphore.
        const auto engineDevice = EngineContext::GetInstance().GetDevice();
        if ( !engineDevice )
        {
            m_FrameSemaphores.clear();
            m_WaitFences.clear();
            return;
        }

        VkDevice device = SP_CAST( VulkanLogicalDevice, engineDevice )->GetVulkanLogicalDevice();
        if ( device == VK_NULL_HANDLE )
        {
            m_FrameSemaphores.clear();
            m_WaitFences.clear();
            return;
        }

        for ( auto& sem : m_FrameSemaphores )
        {
            if ( sem.PresentComplete != VK_NULL_HANDLE ) vkDestroySemaphore( device, sem.PresentComplete, nullptr );
            if ( sem.RenderComplete != VK_NULL_HANDLE ) vkDestroySemaphore( device, sem.RenderComplete, nullptr );
        }
        m_FrameSemaphores.clear();

        for ( auto& fence : m_WaitFences )
        {
            if ( fence != VK_NULL_HANDLE )
            {
                vkDestroyFence( device, fence, nullptr );
                fence = VK_NULL_HANDLE;
            }
        }
        m_WaitFences.clear();
    }
} // namespace Desert::Graphic::API::Vulkan
