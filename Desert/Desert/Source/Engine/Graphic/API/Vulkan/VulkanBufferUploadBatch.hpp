#pragma once

#include <Engine/Graphic/API/Vulkan/VulkanAllocator.hpp>

#include <Common/Core/ResultStr.hpp>

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace Desert::Graphic::API::Vulkan
{
    /**
     * THE FRAME'S DEVICE-BUFFER UPLOAD BATCH (WP-OPT-UPLOAD-BATCH; UE 5.5 FRHICommandList::UpdateBuffer /
     * FRHIResourceUpdateBatcher: a buffer write is a copy recorded into the frame's upload work, from a staging
     * ring, and the game thread never builds a submission of its own).
     *
     * Every write of a device-local buffer (a GeometryPool arena range, a static vertex or index buffer) used to
     * allocate a VMA staging buffer, map it, allocate and record a one-off command buffer and queue one
     * submission: 3-4 per static mesh, ~25 ms of game thread per HLOD on the flight (four HLODs arriving on one
     * frame = 100 ms). Now Write copies the bytes into a slice of the stream-in ring (VulkanUploadRing; a
     * dedicated staging buffer only when the ring is full) and appends the copy; Flush records every copy appended
     * so far into ONE command buffer and queues ONE unwaited submission
     * (CommandBufferAllocator::RT_SubmitGraphicThenRelease), which drops the ring leases and releases the
     * dedicated copies once the upload timeline reaches it.
     *
     * READY = QUEUE ORDER, NOT A CPU FENCE POLL. The batch is flushed at the head of every hand-off to the queue
     * that could read what it writes (VulkanFrameLoop::Submit, SubmitOrdered, EnqueueUnwaited), on the calling
     * thread, before that hand-off: the frame that first draws the mesh is submitted after its copies on the same
     * graphics queue, the recorded barriers make the bytes visible, and a reader on another queue waits the upload
     * timeline value the batch signalled (CommandBufferAllocator::NewestUploadSync). So a mesh committed during a
     * frame is drawn by that frame with its bytes in place - no hole, and no previous representation to keep.
     * Flushes are serialised (a concurrent hand-off waits for the flush in progress to be queued, so nothing that
     * reads the copies can overtake them); a flush on the submit thread itself is skipped (its entries already run
     * in queue order, and the next frame's Submit takes the batch before any frame reads it).
     */
    class VulkanBufferUploadBatch
    {
    public:
        static VulkanBufferUploadBatch& Get() noexcept;

        /// Copies @p size bytes of @p data now (the caller may drop them on return) and queues their copy to
        /// @p dst at @p offset. Any thread; never waits for the GPU or the submit thread.
        Common::BoolResultStr Write( VulkanAllocator& allocator, VkBuffer dst, uint64_t offset, const void* data,
                                     uint64_t size );

        /// Records every queued copy into one command buffer and queues it unwaited. Nothing queued, or a flush
        /// already running on this thread (the batch's own submission reaching the hooks), or the submit thread:
        /// nothing is done.
        Common::BoolResultStr Flush();

        struct Stats
        {
            uint64_t Writes          = 0;
            uint64_t Flushes         = 0;
            uint64_t Bytes           = 0;
            uint64_t DedicatedCopies = 0; // the ring was full: a staging buffer of its own
        };
        [[nodiscard]] Stats GetStats() const;

    private:
        struct Copy
        {
            VkBuffer Src       = VK_NULL_HANDLE;
            uint64_t SrcOffset = 0;
            VkBuffer Dst       = VK_NULL_HANDLE;
            uint64_t DstOffset = 0;
            uint64_t Size      = 0;
        };
        struct Pending
        {
            VulkanAllocator*                                Allocator = nullptr;
            std::vector<Copy>                               Copies;
            std::vector<std::shared_ptr<const void>>        Leases;    // ring slices read by the copies
            std::vector<std::pair<VkBuffer, VmaAllocation>> Dedicated; // staging buffers of their own
            [[nodiscard]] bool                              Empty() const
            {
                return Copies.empty();
            }
        };

        mutable std::mutex m_Mutex;      // m_Pending, m_Stats
        std::mutex         m_FlushMutex; // one flush from swap to queued hand-off
        Pending            m_Pending;
        Stats              m_Stats;
    };
} // namespace Desert::Graphic::API::Vulkan
