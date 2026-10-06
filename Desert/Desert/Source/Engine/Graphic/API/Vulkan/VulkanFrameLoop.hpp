#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanRdgQueues.hpp>

#include <vulkan/vulkan.h>

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace Desert::Graphic::API::Vulkan
{
    // What a window adds to a frame (VulkanSwapChainOutput::GetFrameOutput): the frame's first Graphics
    // submission waits ImageAcquired at ImageFirstUse — the stage the back buffer is first written — and its last
    // submission signals RenderComplete, which the output's present waits. A frame with no window has none.
    struct VulkanFrameOutput
    {
        VkSemaphore          ImageAcquired  = VK_NULL_HANDLE;
        VkPipelineStageFlags ImageFirstUse  = 0; // RDG::kPresentAcquiredState's stages, set by the output
        VkSemaphore          RenderComplete = VK_NULL_HANDLE;
    };

    // THE FRAME'S ONE SUBMISSION PATH, owned by the frame loop and not by a window. Per frame slot: the fence the
    // slot's last submission signals, and the slot's graph objects (VulkanRdgQueueObjects: command pools and
    // semaphores). Every frame is begun here (BeginSlot), submitted here (Submit) and waited here (WaitSlot); a
    // window is only an optional VulkanFrameOutput of Submit, so a frame with no window (EngineHost) is the same
    // frame. CPU/GPU sync is the slot fence, never a device-wide wait.
    // Never: begin a slot's objects before its fence was waited; submit a frame whose last entry is not its
    // graphics tail; reset a slot fence anywhere but immediately before the submission that signals it.
    class VulkanFrameLoop
    {
    public:
        VulkanFrameLoop( VkDevice device, uint32_t graphicsFamily, std::optional<uint32_t> computeFamily,
                         uint32_t frameSlots );
        ~VulkanFrameLoop();
        VulkanFrameLoop( const VulkanFrameLoop& )            = delete;
        VulkanFrameLoop& operator=( const VulkanFrameLoop& ) = delete;

        // Waits for the slot's previous frame, then begins the slot's graph objects (their command buffers and
        // semaphores become reusable).
        Common::BoolResultStr BeginSlot( uint32_t slot );
        // Submits the frame IN ORDER (RDG-CONTRACTS B(3)): the frame command buffer split at every graph and the
        // graphs' segment submissions. The last entry (the frame's graphics tail) signals the slot fence, which
        // covers every AsyncCompute entry too (each one is joined by a later Graphics entry). With @p output, the
        // first Graphics entry also waits for the acquired image and the last one signals render-complete.
        Common::BoolResultStr Submit( uint32_t slot, std::span<const VulkanRdgSubmission> frame,
                                      const VulkanFrameOutput* output );
        // Waits for the slot's last submitted frame (a slot never submitted is signalled).
        Common::BoolResultStr WaitSlot( uint32_t slot );

        VulkanRdgQueueObjects& GetQueueObjects()
        {
            return m_QueueObjects;
        }

    private:
        VkDevice              m_Device = VK_NULL_HANDLE;
        VulkanRdgQueueObjects m_QueueObjects;
        std::vector<VkFence>  m_Fences; // per slot; VK_NULL_HANDLE when its creation failed (refused by name)
    };
} // namespace Desert::Graphic::API::Vulkan
