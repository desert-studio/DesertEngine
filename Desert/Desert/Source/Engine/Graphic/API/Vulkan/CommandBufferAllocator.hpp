#pragma once

#include <Engine/Graphic/API/Vulkan/VulkanDevice.hpp>

#include <cstddef>
#include <unordered_map>

namespace Desert::Graphic::API::Vulkan
{
    // Must be >= the swapchain image count (set later by VulkanSwapChain).
    // CommandBufferAllocator is created before the swapchain exists, so we can't
    // read FrameManager here — allocate for the maximum supported in-flight count.
    static constexpr uint32_t k_MaxCommandPoolFrames = 3U;

    class CommandBufferAllocator : public Common::Singleton<CommandBufferAllocator>
    {
    public:
        CommandBufferAllocator( const std::shared_ptr<VulkanLogicalDevice>& device );

        Common::ResultStr<VkCommandBuffer> RT_GetCommandBufferCompute( bool begin = false );
        Common::ResultStr<VkCommandBuffer> RT_AllocateCommandBufferGraphic( bool begin = false );
        Common::ResultStr<VkCommandBuffer> RT_AllocateCommandBufferTransferOps( bool begin = false );

        Common::ResultStr<VkResult> RT_FlushCommandBufferCompute( VkCommandBuffer commandBuffer );
        Common::ResultStr<VkResult> RT_FlushCommandBufferGraphic( VkCommandBuffer commandBuffer );
        Common::ResultStr<VkResult> RT_FlushCommandBufferTransferOps( VkCommandBuffer commandBuffer );

        /// A one-off graphics submission that nobody waited for: the buffer, the pool that allocated it and
        /// the fence its completion signals. The buffer is freed only by RT_ReleaseSubmitted.
        struct Submitted
        {
            VkCommandBuffer Buffer = VK_NULL_HANDLE;
            VkCommandPool   Pool   = VK_NULL_HANDLE;
            VkFence         Fence  = VK_NULL_HANDLE;
        };

        /// Ends and submits @p commandBuffer on the graphics queue WITHOUT waiting for it — the caller polls
        /// IsComplete() on later frames. The flush above blocks the calling thread until the GPU is done,
        /// which is what made a thumbnail readback a main-thread stall (TH3).
        Common::ResultStr<Submitted> RT_SubmitCommandBufferGraphic( VkCommandBuffer commandBuffer );

        /// True once the submission's fence has signalled. Never blocks.
        [[nodiscard]] bool IsComplete( const Submitted& submitted ) const;

        /// Waits for the fence (normally already signalled), then frees the buffer and destroys the fence.
        void RT_ReleaseSubmitted( const Submitted& submitted );

        /// Destroys the nine command pools, and with them every command buffer ever allocated from one.
        /// EXPLICIT, because this object is a Common::Singleton: its unique_ptr is a namespace-scope
        /// static, so a destructor would run at __cxa_finalize, long after vkDestroyDevice. Called from
        /// VulkanContext::Shutdown, which the device's own teardown reaches while the device is alive.
        void Destroy();

        /// How many one-off command buffers are out — handed to a caller and not yet flushed back.
        /// Nonzero at teardown means a caller took one and never returned it.
        [[nodiscard]] std::size_t OutstandingOneShotCount() const;

        const auto& GetCommandGraphicPool() const
        {
            return m_CommandGraphicPool;
        }

        const auto& GetCommandComputePool() const
        {
            return m_ComputeCommandPool;
        }

    private:
        /// Frees @p commandBuffer from the pool it was actually allocated from, after its fence.
        Common::ResultStr<VkResult> FlushOneShot( VkCommandBuffer commandBuffer, VkQueue queue );

        /// WHICH POOL EACH ONE-OFF BUFFER CAME FROM. Not recomputed at flush time from the current frame
        /// index, which is what the three Flush functions used to do: the index can advance between the
        /// allocation and the flush, and vkFreeCommandBuffers against a pool that did not allocate the
        /// buffer is undefined behaviour, not a diagnosable mistake. Recording it is also what lets a
        /// flush of a buffer this allocator never handed out be REFUSED by name instead of guessed at.
        std::unordered_map<VkCommandBuffer, VkCommandPool> m_OneShotPools;

        std::vector<VkCommandPool> m_CommandGraphicPool;
        std::vector<VkCommandPool> m_ComputeCommandPool;
        std::vector<VkCommandPool> m_TransferOpsCommandPool;

        VkQueue m_GraphicsQueue;
        VkQueue m_ComputeQueue;
        VkQueue m_TransferOpsQueue;

        VkDevice m_LogicalDevice;
    };
} // namespace Desert::Graphic::API::Vulkan