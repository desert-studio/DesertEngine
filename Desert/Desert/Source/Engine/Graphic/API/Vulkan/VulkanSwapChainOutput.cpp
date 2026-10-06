#include <Engine/Graphic/API/Vulkan/VulkanSwapChainOutput.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanRenderGraph.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanUtils/VulkanHelper.hpp>
#include <Engine/Graphic/DeviceLost.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanContext.hpp>

#include <Engine/Graphic/API/Vulkan/VulkanSwapChain.hpp>
#include <Engine/Core/EngineContext.hpp>

#include <Common/Core/DestructorGuard.hpp>

namespace Desert::Graphic::API::Vulkan
{
    namespace
    {
        Common::ResultStr<VkSemaphore> MakeSemaphore( VkDevice device )
        {
            VkSemaphoreCreateInfo createInfo{
                 .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = VK_NULL_HANDLE, .flags = 0 };

            VkSemaphore semaphore;

            VK_RETURN_RESULT_IF_FALSE_TYPE( VkSemaphore,
                                            vkCreateSemaphore( device, &createInfo, VK_NULL_HANDLE, &semaphore ) );

            return Common::MakeSuccess( semaphore );
        }
    } // namespace

    VulkanSwapChainOutput::VulkanSwapChainOutput( VulkanSwapChain* swapChain ) : m_SwapChain( swapChain )
    {
    }

    void VulkanSwapChainOutput::AcquireImage()
    {
        if ( !Graphic::DeviceLost::AllowWork() )
            return;

        uint32_t currentIndex = EngineContext::GetInstance().GetCurrentFrameIndex();

        // SUBOPTIMAL IS AN IMAGE; ONLY OUT_OF_DATE IS A REBUILD (Engine/Graphic/SwapchainAcquire.hpp). The
        // rebuild keeps `currentIndex` current (FrameManager::AdoptSwapchainImageCount), so the frame loop's
        // fence of this slot, the semaphore acquired on here and the submit's wait all name the same frame slot.
        auto* const presentComplete = m_FrameSemaphores[currentIndex].PresentComplete;
        const auto  acquired        = Graphic::AcquireForFrame(
             [&] { return m_SwapChain->AcquireNextImage( presentComplete, &m_ImageIndex ); },
             [&] { return m_SwapChain->Rebuild( m_SwapChain->GetWidth(), m_SwapChain->GetHeight() ); } );
        // A LOST DEVICE IS NOT A RESIZE: AcquireNextImage and Rebuild both refuse on a lost device and the
        // latch already carries the explanation, so only a failure of another kind is worth a line here.
        if ( !acquired.IsSuccess() && !Graphic::DeviceLost::IsLost() )
            LOG_ERROR( "[AcquireNextImage] {}", acquired.GetError() );
    }

    VulkanFrameOutput VulkanSwapChainOutput::GetFrameOutput() const
    {
        const Semaphores& slot = m_FrameSemaphores[EngineContext::GetInstance().GetCurrentFrameIndex()];
        return VulkanFrameOutput{ .ImageAcquired  = slot.PresentComplete,
                                  .ImageFirstUse  = RdgVulkanStages( RDG::kPresentAcquiredState.Stages ),
                                  .RenderComplete = slot.RenderComplete };
    }

    void VulkanSwapChainOutput::Present()
    {
        if ( !Graphic::DeviceLost::AllowWork() )
            return;

        uint32_t    currentIndex = EngineContext::GetInstance().GetCurrentFrameIndex();
        const auto& queue =
             SP_CAST( VulkanLogicalDevice, EngineContext::GetInstance().GetDevice() )->GetGraphicsQueue();

        const auto& queuePresent =
             QueuePresent( queue, m_ImageIndex, m_FrameSemaphores[currentIndex].RenderComplete );
        if ( !queuePresent.IsSuccess() )
        {
            LOG_INFO( "[QueuePresent] Error: {}", queuePresent.GetError() );
        }

        // Present is the OTHER place the loss surfaces first (a submit executes asynchronously, so the
        // frame that killed the device is usually already gone by the time anyone is told). The frame loop
        // asks DeviceLost::IsLost() after this returns, before it waits any fence of the dead device.
    }

    Common::ResultStr<VkResult> VulkanSwapChainOutput::QueuePresent( VkQueue queue, uint32_t imageIndex,
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

        // Window was resized/minimized between acquire and present — recreate the swapchain (it re-queries
        // the current surface extent) and treat this frame as handled. Standard Vulkan resize handling.
        if ( res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR )
        {
            m_SwapChain->OnResize( m_SwapChain->GetWidth(), m_SwapChain->GetHeight() );
            return Common::MakeSuccess( VK_SUCCESS );
        }

        return Common::MakeFormattedError<VkResult>( "result: {}", VkResultToString( res ) );
    }

    Common::ResultStr<VkResult> VulkanSwapChainOutput::Init()
    {
        VkDevice device =
             SP_CAST( VulkanLogicalDevice, EngineContext::GetInstance().GetDevice() )->GetVulkanLogicalDevice();

        uint32_t backBufferCount = m_SwapChain->GetBackBufferCount();

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
    try
    {
        Release();
    }
    DESERT_DESTRUCTOR_GUARD( "~VulkanSwapChainOutput" )

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
