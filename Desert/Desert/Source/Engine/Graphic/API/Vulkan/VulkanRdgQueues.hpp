#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/RDG/RDGCompileResult.hpp>

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace Desert::Graphic::API::Vulkan
{
    // RDG-CONTRACTS B(3). The per-frame-slot objects the graph's pipe segments record and sync with: a command
    // pool per (slot, pipe) whose command buffers are reused after the pool reset, and a pool of binary
    // semaphores per slot (grown on demand). Owned by the frame loop next to the transient allocator.
    // Never: hand out an object of a slot before BeginFrameSlot(slot) followed that slot's fence wait;
    // allocate an AsyncCompute command buffer when it was built without a compute family.
    class VulkanRdgQueueObjects
    {
    public:
        VulkanRdgQueueObjects( VkDevice device, uint32_t graphicsFamily, std::optional<uint32_t> computeFamily,
                               uint32_t frameSlots );
        ~VulkanRdgQueueObjects();
        VulkanRdgQueueObjects( const VulkanRdgQueueObjects& )            = delete;
        VulkanRdgQueueObjects& operator=( const VulkanRdgQueueObjects& ) = delete;

        // The frame loop, after the slot's fence: resets the slot's command pools; its command buffers and
        // semaphores become reusable.
        Common::BoolResultStr BeginFrameSlot( uint32_t slot );
        // A primary command buffer of the current slot on @p pipe, begun (one-time submit).
        Common::ResultStr<VkCommandBuffer> BeginCommandBuffer( RDG::Pipe pipe );
        // An unused binary semaphore of the current slot (unsignalled: every semaphore handed out in a slot's
        // previous frame was signalled and waited by that frame's submissions, which the fence covered).
        Common::ResultStr<VkSemaphore> AcquireSemaphore();

    private:
        struct PipePool
        {
            VkCommandPool                Pool = VK_NULL_HANDLE;
            std::vector<VkCommandBuffer> Buffers;
            uint32_t                     Used = 0;
        };
        struct Slot
        {
            std::array<PipePool, 2>  Pipes; // indexed by RDG::Pipe
            std::vector<VkSemaphore> Semaphores;
            uint32_t                 SemaphoresUsed = 0;
        };

        VkDevice          m_Device = VK_NULL_HANDLE;
        std::vector<Slot> m_Slots;
        uint32_t          m_Slot = 0;
    };

    // RDG-CONTRACTS B(3). The queues the graph records onto and the per-frame-slot objects that sync them.
    // Built by the frame loop from VulkanDevice (graphics queue + family, compute queue + family when the device
    // has a distinct compute family) and by the device suite on its own headless device.
    // Never: report SeparateComputeFamily when the families are equal or the compute queue is null.
    struct VulkanRdgQueueSet
    {
        VkQueue  GraphicsQueue         = VK_NULL_HANDLE;
        uint32_t GraphicsFamily        = 0;
        VkQueue  ComputeQueue          = VK_NULL_HANDLE; // null when the device has no separate compute family
        uint32_t ComputeFamily         = 0;
        VulkanRdgQueueObjects* Objects = nullptr; // per-slot command pools and semaphores

        RDG::PipeCapabilities GetCapabilities() const;
        VkQueue               QueueOf( RDG::Pipe pipe ) const;
        uint32_t              FamilyOf( RDG::Pipe pipe ) const;
    };

    // RDG-CONTRACTS B(3). One queue submission the backend assembled from a PipeSegment. The caller submits the
    // list IN ORDER (vkQueueSubmit per entry, the frame fence on the last Graphics entry); the backend never
    // submits, exactly as it never submits today.
    struct VulkanRdgSubmission
    {
        RDG::Pipe                         OnPipe        = RDG::Pipe::Graphics;
        VkQueue                           Queue         = VK_NULL_HANDLE;
        VkCommandBuffer                   CommandBuffer = VK_NULL_HANDLE;
        std::vector<VkSemaphore>          WaitSemaphores;
        std::vector<VkPipelineStageFlags> WaitStages;
        std::vector<VkSemaphore>          SignalSemaphores;
    };

    // The backend's segment state for the graphs executed since the last TakeSubmissions: one command buffer per
    // PipeSegment, one semaphore per CrossPipeSync, and the graphics tail that records FinalBarriers.
    //   * the last segment of a graph stays open when it is on Graphics and no join waits at the graph end: the
    //     final barriers record into it;
    //   * otherwise a tail graphics command buffer opens after the last segment, waiting on every
    //     kJoinAtGraphEnd join, and the final barriers (with their ownership acquires) record into it.
    class VulkanRdgSegmentRecorder
    {
    public:
        // BeginGraph: returns the command buffer the graph records into before its first segment (a graph with
        // no segment records only its final barriers, into an opened tail), else VK_NULL_HANDLE.
        Common::ResultStr<VkCommandBuffer> BeginGraph( const RDG::CompileResult& result,
                                                       const VulkanRdgQueueSet&  queues );
        Common::ResultStr<VkCommandBuffer> BeginSegment( const RDG::PipeSegment&  segment,
                                                         const VulkanRdgQueueSet& queues );
        // Returns the command buffer recording continues in after this segment (the open tail), or VK_NULL_HANDLE.
        Common::ResultStr<VkCommandBuffer> EndSegment( const RDG::PipeSegment&  segment,
                                                       const VulkanRdgQueueSet& queues );
        // The graph ended (its final barriers are recorded): its compile result is let go, and its submissions
        // and open tail stay for Take.
        void EndGraph();
        // Drops what the abandoned graph queued; earlier graphs' submissions stay.
        void                             AbandonGraph();
        std::vector<VulkanRdgSubmission> Take();

    private:
        Common::ResultStr<VkSemaphore> SemaphoreOf( uint32_t sync, const VulkanRdgQueueSet& queues );
        Common::BoolResultStr          Open( RDG::Pipe pipe, const VulkanRdgQueueSet& queues );
        Common::BoolResultStr          Close();

        // The graph's compile result (Builder::Execute's local) between BeginGraph and EndGraph / AbandonGraph /
        // Take; null outside it, so no segment call can read the result of a graph whose Execute has returned.
        const RDG::CompileResult*          m_Result = nullptr;
        std::vector<VkSemaphore>           m_SyncSemaphores; // by CompileResult::Syncs index, this graph only
        uint32_t                           m_NextSegment      = 0;
        size_t                             m_GraphSubmissions = 0; // first submission of the current graph
        std::optional<VulkanRdgSubmission> m_Open;
        std::vector<VulkanRdgSubmission>   m_Submissions;
    };

    // Vulkan 1.0 stage mask of graph stages (VulkanRenderGraph.cpp).
    VkPipelineStageFlags RdgVulkanStages( RDG::PipelineStageFlags stages );
} // namespace Desert::Graphic::API::Vulkan
