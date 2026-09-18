#include <Engine/Graphic/API/Vulkan/CommandBufferAllocator.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanUtils/VulkanHelper.hpp>
#include <Engine/Graphic/DeviceLost.hpp>

#include <Engine/Core/EngineContext.hpp>

#include <functional>

namespace Desert::Graphic::API::Vulkan
{
    namespace
    {
        Common::ResultStr<VkResult> FlushCommandBuffer( VkDevice device, VkCommandPool commandPool,
                                                        VkCommandBuffer commandBuffer, VkQueue queue )
        {
            if ( commandBuffer == VK_NULL_HANDLE )
            {
                return Common::MakeError<VkResult>( "Command buffer is VK_NULL_HANDLE" );
            }

            // This is a SUBMIT AND A BLOCKING WAIT — the one-off upload path every texture, mesh and
            // mipmap chain goes through. On a lost device the submit cannot execute and the wait cannot
            // ever be signalled, so issuing them is at best pointless and at worst a hang.
            if ( !Graphic::DeviceLost::AllowWork() )
            {
                return Common::MakeError<VkResult>(
                     "the device is lost; the one-off command buffer is dropped rather than submitted." );
            }

            VK_RETURN_RESULT_IF_FALSE_TYPE( VkResult, vkEndCommandBuffer( commandBuffer ) )

            VkSubmitInfo submitInfo       = {};
            submitInfo.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submitInfo.commandBufferCount = 1;
            submitInfo.pCommandBuffers    = &commandBuffer;

            VkFence fence;
            // Create fence to ensure that the command buffer has finished executing
            VkFenceCreateInfo fenceCreateInfo = {};
            fenceCreateInfo.sType             = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            fenceCreateInfo.flags             = 0;
            VK_RETURN_RESULT_IF_FALSE( vkCreateFence( device, &fenceCreateInfo, nullptr, &fence ) );
            VK_RETURN_RESULT_IF_FALSE_TYPE( VkResult, vkQueueSubmit( queue, 1, &submitInfo, fence ) );

            // Wait for the fence to signal that command buffer has finished executing
            VK_CHECK_RESULT( vkWaitForFences( device, 1, &fence, VK_TRUE, UINT64_MAX ) );
            vkDestroyFence( device, fence, nullptr );

            // THE POOL PARAMETER WAS COMMENTED OUT AND THE BUFFER WAS NEVER FREED. Every one-off upload in
            // the engine — each texture's staging copy, each mipmap chain, each vertex/index upload, each
            // swapchain resize barrier — took a fresh VkCommandBuffer out of a pool that is never reset,
            // so the count only ever went up: 211 of them were still alive at vkDestroyDevice on a session
            // that merely opened the default scene and quit. The fence above already proves the GPU is
            // done with it, which is exactly the condition vkFreeCommandBuffers requires.
            vkFreeCommandBuffers( device, commandPool, 1, &commandBuffer );

            return Common::MakeSuccess( VK_SUCCESS );
        }
    } // namespace

    CommandBufferAllocator::CommandBufferAllocator( const std::shared_ptr<VulkanLogicalDevice>& device )
    {
        const uint32_t frames = k_MaxCommandPoolFrames;

        // Graphic
        {
            m_CommandGraphicPool.resize( frames );

            VkCommandPoolCreateInfo cmdPoolInfo = {};
            cmdPoolInfo.sType                   = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            cmdPoolInfo.queueFamilyIndex        = device->GetPhysicalDevice()->GetGraphicsFamily();
            cmdPoolInfo.flags                   = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            for ( auto& cmdPool : m_CommandGraphicPool )
            {
                VK_CHECK_RESULT(
                     vkCreateCommandPool( device->GetVulkanLogicalDevice(), &cmdPoolInfo, nullptr, &cmdPool ) );
            }
        }

        // Compute
        {
            m_ComputeCommandPool.resize( frames );

            VkCommandPoolCreateInfo cmdPoolInfo = {};
            cmdPoolInfo.sType                   = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            cmdPoolInfo.queueFamilyIndex        = device->GetPhysicalDevice()->GetComputeFamily();
            cmdPoolInfo.flags                   = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            for ( auto& cmdPool : m_ComputeCommandPool )
            {
                VK_CHECK_RESULT(
                     vkCreateCommandPool( device->GetVulkanLogicalDevice(), &cmdPoolInfo, nullptr, &cmdPool ) );
            }
        }

        // Transfer Ops
        {
            m_TransferOpsCommandPool.resize( frames );

            VkCommandPoolCreateInfo cmdPoolInfo = {};
            cmdPoolInfo.sType                   = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            cmdPoolInfo.queueFamilyIndex        = device->GetPhysicalDevice()->GetTransferFamily();
            cmdPoolInfo.flags                   = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            for ( auto& cmdPool : m_TransferOpsCommandPool )
            {
                VK_CHECK_RESULT(
                     vkCreateCommandPool( device->GetVulkanLogicalDevice(), &cmdPoolInfo, nullptr, &cmdPool ) );
            }
        }

        m_GraphicsQueue    = device->m_GraphicsQueue;
        m_ComputeQueue     = device->m_ComputeQueue;
        m_TransferOpsQueue = device->m_TransferQueue;

        m_LogicalDevice = device->m_LogicalDevice;
    }

    Common::ResultStr<VkCommandBuffer> CommandBufferAllocator::RT_GetCommandBufferCompute( bool begin /*= false */ )
    {
        if ( !Graphic::DeviceLost::AllowWork() )
            return Common::MakeError<VkCommandBuffer>( "the device is lost; no command buffer is allocated." );

        const auto                  frame = EngineContext::GetInstance().GetCurrentFrameIndex();
        VkCommandBufferAllocateInfo allocateInfo;
        allocateInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocateInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocateInfo.commandBufferCount = 1;
        allocateInfo.commandPool        = m_ComputeCommandPool[frame];
        allocateInfo.pNext              = VK_NULL_HANDLE;

        VkCommandBuffer cmdBuffer;
        VK_RETURN_RESULT_IF_FALSE_TYPE( VkCommandBuffer,
                                        vkAllocateCommandBuffers( m_LogicalDevice, &allocateInfo, &cmdBuffer ) );
        m_OneShotPools[cmdBuffer] = allocateInfo.commandPool;

        if ( begin )
        {
            VkCommandBufferBeginInfo cmdBufferBeginInfo{};
            cmdBufferBeginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            VK_RETURN_RESULT_IF_FALSE_TYPE( VkCommandBuffer,
                                            vkBeginCommandBuffer( cmdBuffer, &cmdBufferBeginInfo ) );
        }

        return Common::MakeSuccess( cmdBuffer );
    }

    Common::ResultStr<VkCommandBuffer>
    CommandBufferAllocator::RT_AllocateCommandBufferGraphic( bool begin /*= false */ )
    {
        // A REFUSAL HERE IS SAFE ONLY BECAUSE EVERY CALLER NOW READS IT. `Common::ResultStr::GetValue()`
        // hands back a default-constructed T on a failed result, which for a VkCommandBuffer is
        // VK_NULL_HANDLE — and recording into VK_NULL_HANDLE is undefined behaviour, not a no-op. Six call
        // sites took that value without asking; they check now.
        if ( !Graphic::DeviceLost::AllowWork() )
            return Common::MakeError<VkCommandBuffer>( "the device is lost; no command buffer is allocated." );

        const auto frame = EngineContext::GetInstance().GetCurrentFrameIndex();

        VkCommandBufferAllocateInfo allocateInfo;
        allocateInfo.pNext              = VK_NULL_HANDLE;
        allocateInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocateInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocateInfo.commandBufferCount = 1;
        allocateInfo.commandPool        = m_CommandGraphicPool[frame];

        VkCommandBuffer cmdBuffer;
        VK_RETURN_RESULT_IF_FALSE_TYPE( VkCommandBuffer,
                                        vkAllocateCommandBuffers( m_LogicalDevice, &allocateInfo, &cmdBuffer ) );
        m_OneShotPools[cmdBuffer] = allocateInfo.commandPool;

        if ( begin )
        {
            VkCommandBufferBeginInfo cmdBufferBeginInfo{};
            cmdBufferBeginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            VK_RETURN_RESULT_IF_FALSE_TYPE( VkCommandBuffer,
                                            vkBeginCommandBuffer( cmdBuffer, &cmdBufferBeginInfo ) );
        }

        return Common::MakeSuccess( cmdBuffer );
    }

    Common::ResultStr<VkResult> CommandBufferAllocator::FlushOneShot( VkCommandBuffer commandBuffer,
                                                                      VkQueue         queue )
    {
        const auto entry = m_OneShotPools.find( commandBuffer );
        if ( entry == m_OneShotPools.end() )
        {
            // A buffer this allocator did not hand out, or one already flushed. Freeing it against a
            // guessed pool is undefined; saying so is the only honest answer, and it names the handle.
            //
            // LOGGED AS WELL AS RETURNED, and that is not belt-and-braces: seventeen of the nineteen call
            // sites throw this result away, so a refusal that only travelled in the return value would be
            // a dropped upload with nothing anywhere to say it happened.
            LOG_ERROR( "[CommandBuffers] flush of command buffer {} refused: it was not allocated here, or "
                       "was already flushed. Nothing was submitted.",
                       static_cast<const void*>( commandBuffer ) );
            return Common::MakeFormattedError<VkResult>(
                 "command buffer {} was not allocated by CommandBufferAllocator (or was already flushed); "
                 "it cannot be submitted and freed here.",
                 static_cast<const void*>( commandBuffer ) );
        }

        VkCommandPool pool = entry->second;
        // Erased BEFORE the flush, not after: FlushCommandBuffer has four early returns and the buffer is
        // freed on the success path only, so leaving the row in place on a refusal would make the next
        // flush of a recycled handle free a buffer that is still being recorded into.
        m_OneShotPools.erase( entry );

        return FlushCommandBuffer( m_LogicalDevice, pool, commandBuffer, queue );
    }

    Common::ResultStr<VkResult>
    CommandBufferAllocator::RT_FlushCommandBufferCompute( VkCommandBuffer commandBuffer )
    {
        return FlushOneShot( commandBuffer, m_ComputeQueue );
    }

    Common::ResultStr<VkResult>
    CommandBufferAllocator::RT_FlushCommandBufferGraphic( VkCommandBuffer commandBuffer )
    {
        return FlushOneShot( commandBuffer, m_GraphicsQueue );
    }

    Common::ResultStr<VkCommandBuffer>
    CommandBufferAllocator::RT_AllocateCommandBufferTransferOps( bool begin /*= false */ )
    {
        const auto frame = EngineContext::GetInstance().GetCurrentFrameIndex();

        VkCommandBufferAllocateInfo allocateInfo;
        allocateInfo.pNext              = VK_NULL_HANDLE;
        allocateInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocateInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocateInfo.commandBufferCount = 1;
        allocateInfo.commandPool        = m_TransferOpsCommandPool[frame];

        VkCommandBuffer cmdBuffer;
        VK_RETURN_RESULT_IF_FALSE_TYPE( VkCommandBuffer,
                                        vkAllocateCommandBuffers( m_LogicalDevice, &allocateInfo, &cmdBuffer ) );
        m_OneShotPools[cmdBuffer] = allocateInfo.commandPool;

        if ( begin )
        {
            VkCommandBufferBeginInfo cmdBufferBeginInfo{};
            cmdBufferBeginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            VK_RETURN_RESULT_IF_FALSE_TYPE( VkCommandBuffer,
                                            vkBeginCommandBuffer( cmdBuffer, &cmdBufferBeginInfo ) );
        }

        return Common::MakeSuccess( cmdBuffer );
    }

    Common::ResultStr<VkResult>
    CommandBufferAllocator::RT_FlushCommandBufferTransferOps( VkCommandBuffer commandBuffer )
    {
        return FlushOneShot( commandBuffer, m_TransferOpsQueue );
    }

    std::size_t CommandBufferAllocator::OutstandingOneShotCount() const
    {
        return m_OneShotPools.size();
    }

    void CommandBufferAllocator::Destroy()
    {
        if ( m_LogicalDevice == VK_NULL_HANDLE )
            return;

        // A buffer still out means a caller took one and never flushed it back. Said out loud rather than
        // swallowed: the pools below take it with them either way, so silence here would hide a recording
        // path that never submits — which is a dropped upload, not only a leak.
        if ( !m_OneShotPools.empty() )
        {
            LOG_ERROR( "[CommandBuffers] {} one-off command buffer(s) were allocated and never flushed; "
                       "whatever they recorded was never submitted.",
                       m_OneShotPools.size() );
            m_OneShotPools.clear();
        }

        // Nine pools — three queue families times k_MaxCommandPoolFrames — and not one of them was ever
        // destroyed: this class had no destructor at all, and a Common::Singleton could not have used one
        // anyway (see the header). Destroying a pool frees every command buffer allocated from it, which
        // is how the per-frame draw and compute buffers VulkanQueue::Init takes out of these same pools
        // are released too.
        for ( const auto& pools : { std::ref( m_CommandGraphicPool ), std::ref( m_ComputeCommandPool ),
                                    std::ref( m_TransferOpsCommandPool ) } )
        {
            for ( VkCommandPool& pool : pools.get() )
            {
                if ( pool != VK_NULL_HANDLE )
                {
                    vkDestroyCommandPool( m_LogicalDevice, pool, nullptr );
                    pool = VK_NULL_HANDLE;
                }
            }
            pools.get().clear();
        }

        m_LogicalDevice = VK_NULL_HANDLE;
    }

} // namespace Desert::Graphic::API::Vulkan