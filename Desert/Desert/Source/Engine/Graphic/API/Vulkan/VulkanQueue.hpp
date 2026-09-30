#pragma once

#include <Engine/Graphic/API/Vulkan/VulkanRdgQueues.hpp>

#include <vulkan/vulkan.h>
#include <cstdint>
#include <span>
#include <vector>

namespace Desert::Graphic::API::Vulkan
{
    // `class` and not `struct`: the definition in VulkanSwapChain.hpp uses class, and the Microsoft C++ ABI
    // encodes the class-key into the decorated name, so the mismatch is a Windows-only link
    // error waiting for the day this forward declaration is the one a caller sees first.
    class VulkanSwapChain;

    class VulkanQueue final
    {
    public:
        VulkanQueue( VulkanSwapChain* swapChain );

        /// Release() was PUBLIC AND CALLED FROM NOWHERE — six VkSemaphores and three VkFences of this
        /// engine's every session ended their life inside vkDestroyDevice's leak report. It is a
        /// destructor's job and it is one now: VulkanSwapChain owns this object and dies before the
        /// device, so the handles below are still valid here.
        ~VulkanQueue();

        void PrepareFrame();
        // Submits the frame IN ORDER (RDG-CONTRACTS B(3)): the frame command buffer split at every graph and
        // the graphs' segment submissions. The first Graphics entry also waits for the swapchain image; the
        // last entry (Graphics: the frame's tail) signals RenderComplete and the frame fence, which covers
        // every AsyncCompute entry too (each one is joined by a later Graphics entry).
        Common::BoolResultStr Submit( std::span<const VulkanRdgSubmission> frame );
        void Present();

        Common::ResultStr<VkResult> Init();

        const auto& GetDrawCommandBuffers() const
        {
            return m_DrawCommandBuffers;
        }

        const auto& GetComputeCommandBuffers() const
        {
            return m_ComputeCommandBuffers;
        }

        uint32_t GetImageIndex() const { return m_ImageIndex; }
        VkCommandBuffer GetDrawCommandBuffer() const;

    private:
        void Release();

        Common::ResultStr<VkResult> QueuePresent( VkQueue queue, uint32_t imageIndex, VkSemaphore waitSemaphore );

    private:
        uint32_t m_ImageIndex = ~0;

        VulkanSwapChain* m_SwapChain;

        struct Semaphores
        {
            VkSemaphore PresentComplete;
            VkSemaphore RenderComplete;
        };

        std::vector<Semaphores>      m_FrameSemaphores;
        std::vector<VkCommandBuffer> m_DrawCommandBuffers;
        std::vector<VkCommandBuffer> m_ComputeCommandBuffers;
        std::vector<VkFence>         m_WaitFences;
    };
} // namespace Desert::Graphic::API::Vulkan
