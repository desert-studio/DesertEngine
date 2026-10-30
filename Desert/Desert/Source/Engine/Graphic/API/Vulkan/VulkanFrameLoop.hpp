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
    // What a window adds to a frame (VulkanSwapChainOutput::GetFrameOutput): the first Graphics submission at or
    // after FirstEntryAfterAcquire waits ImageAcquired at ImageFirstUse — the stage the back buffer is first
    // written — and the frame's last submission signals RenderComplete, which the output's present waits. The
    // image is acquired late (at the back-buffer import, PRESENT-COST), so the entries before
    // FirstEntryAfterAcquire were recorded before its index existed and cannot touch it: they run without waiting
    // on the display. A frame with no window, or one that holds no image, has none.
    struct VulkanFrameOutput
    {
        VkSemaphore          ImageAcquired  = VK_NULL_HANDLE;
        VkPipelineStageFlags ImageFirstUse  = 0; // RDG::kPresentAcquiredState's stages, set by the output
        VkSemaphore          RenderComplete = VK_NULL_HANDLE;
        // Index into the frame's submissions at the acquire (VulkanRendererAPI::ImportBackBuffer); 0 = the first.
        size_t FirstEntryAfterAcquire = 0;
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
        // first Graphics entry from output->FirstEntryAfterAcquire on also waits for the acquired image and the
        // last one signals render-complete.
        Common::BoolResultStr Submit( uint32_t slot, std::span<const VulkanRdgSubmission> frame,
                                      const VulkanFrameOutput* output );
        // Waits for the slot's last submitted frame (a slot never submitted is signalled).
        Common::BoolResultStr WaitSlot( uint32_t slot );

        VulkanRdgQueueObjects& GetQueueObjects()
        {
            return m_QueueObjects;
        }

    private:
        // One frame entry as Submit hands it to vkQueueSubmit: the entry's waits and signals plus the window's
        // and the uploads' (Submit), with the timeline info that points into them.
        struct PreparedSubmission
        {
            VkQueue                           Queue = VK_NULL_HANDLE;
            std::vector<VkSemaphore>          Waits;
            std::vector<VkPipelineStageFlags> Stages;
            std::vector<VkSemaphore>          Signals;
            std::vector<uint64_t>             WaitValues;
            std::vector<uint64_t>             SignalValues;
            VkTimelineSemaphoreSubmitInfo Timeline{ .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
            bool                          HasTimeline = false;
        };

        VkDevice              m_Device = VK_NULL_HANDLE;
        VulkanRdgQueueObjects m_QueueObjects;
        std::vector<VkFence>  m_Fences; // per slot; VK_NULL_HANDLE when its creation failed (refused by name)
        // Submit's per-frame storage, kept across frames so a steady frame allocates nothing to submit.
        std::vector<PreparedSubmission> m_Prepared;
        std::vector<VkSubmitInfo>       m_SubmitInfos;
    };
} // namespace Desert::Graphic::API::Vulkan
