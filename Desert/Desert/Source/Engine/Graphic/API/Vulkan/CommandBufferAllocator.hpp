#pragma once

#include <Engine/Graphic/API/Vulkan/VulkanDevice.hpp>

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <source_location>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Desert::Graphic::API::Vulkan
{
    /// ONE-OFF COMMAND BUFFERS (staging copies of meshes, textures, mip chains, readbacks) come from a command
    /// pool OWNED BY THE RECORDING THREAD, one per thread and queue family, created on that thread's first
    /// request. A VkCommandPool is externally synchronised for as long as ANY of its buffers is being
    /// recorded (vkBeginCommandBuffer .. vkEndCommandBuffer, every vkCmd* between), so a pool shared by the
    /// upload threads would need a lock held across the whole recording; a pool per thread needs none
    /// (the same split as UE's FVulkanCommandBufferPool per context). Only the owning thread ever calls into
    /// its pool: a buffer released from another thread is queued on the pool and freed by its owner.
    class CommandBufferAllocator : public Common::Singleton<CommandBufferAllocator>
    {
        struct OneShotPool;

    public:
        CommandBufferAllocator( const std::shared_ptr<VulkanLogicalDevice>& device );

        Common::ResultStr<VkCommandBuffer> RT_GetCommandBufferCompute( bool begin = false );
        Common::ResultStr<VkCommandBuffer> RT_AllocateCommandBufferGraphic( bool begin = false );
        Common::ResultStr<VkCommandBuffer> RT_AllocateCommandBufferTransferOps( bool begin = false );

        /// Submits and BLOCKS until the GPU is done. @p where names the caller in the profiler: every wait is a
        /// row "FlushOneShot wait: <file>:<line>", so a per-frame CPU stall names the call site that pays it.
        Common::ResultStr<VkResult>
        RT_FlushCommandBufferCompute( VkCommandBuffer      commandBuffer,
                                      std::source_location where = std::source_location::current() );
        Common::ResultStr<VkResult>
        RT_FlushCommandBufferGraphic( VkCommandBuffer      commandBuffer,
                                      std::source_location where = std::source_location::current() );
        Common::ResultStr<VkResult>
        RT_FlushCommandBufferTransferOps( VkCommandBuffer      commandBuffer,
                                          std::source_location where = std::source_location::current() );

        /// A one-off graphics submission that nobody waited for: the buffer, the thread pool that allocated it
        /// and the fence its completion signals. The buffer is freed only by RT_ReleaseSubmitted.
        struct Submitted
        {
            VkCommandBuffer Buffer = VK_NULL_HANDLE;
            OneShotPool*    Pool   = nullptr;
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

        /// AN UPLOAD NOBODY WAITS FOR (UE's deferred texture upload: the game thread never sleeps on the GPU).
        /// Ends and submits @p commandBuffer on the graphics queue WITHOUT waiting, and runs @p afterGpu (the
        /// release of what the buffer reads, i.e. its staging copy) once its fence has signalled - polled by
        /// CollectFinished on every frame and on every later submission here, or waited for at Destroy.
        /// Readers on the graphics queue need nothing more: submission order is the dependency, the recorded
        /// barriers make the copy visible. On a refused submit nothing reads the staging copy, so @p afterGpu
        /// runs at once and the refusal is answered.
        /// WHY: every texture the TextureService finished paid a blocking one-off flush behind the whole frame's
        /// GPU work. On warm Bistro (1296 distance fields) that was 2-8 s per texture and ~45% of a 6.4 s
        /// main-thread frame (sample of the p8 boot), and the slow frames then piled up 1 GB of owed deletions.
        Common::BoolResultStr RT_SubmitGraphicThenRelease( VkCommandBuffer       commandBuffer,
                                                           std::function<void()> afterGpu );

        /// Releases every unwaited submission whose fence has signalled; answers how many. Never blocks.
        std::size_t CollectFinished();

        /// Destroys every thread's command pools, and with them every command buffer ever allocated from one.
        /// No thread may be recording a one-off buffer by then.
        /// EXPLICIT, because this object is a Common::Singleton: its unique_ptr is a namespace-scope
        /// static, so a destructor would run at __cxa_finalize, long after vkDestroyDevice. Called from
        /// VulkanContext::Shutdown, which the device's own teardown reaches while the device is alive.
        void Destroy();

        /// How many one-off command buffers are out — handed to a caller and not yet flushed back.
        /// Nonzero at teardown means a caller took one and never returned it.
        [[nodiscard]] std::size_t OutstandingOneShotCount() const;

    private:
        /// One thread's pool for one queue family. Handle and Owner never change after creation;
        /// PendingFree is written by other threads and is guarded by m_PoolMutex.
        struct OneShotPool
        {
            VkCommandPool                Handle = VK_NULL_HANDLE;
            std::thread::id              Owner;
            std::vector<VkCommandBuffer> PendingFree;
        };

        /// Allocates (and optionally begins) a primary buffer from the calling thread's pool for @p family.
        Common::ResultStr<VkCommandBuffer> AllocateOneShot( uint32_t family, bool begin );
        /// Submits @p commandBuffer, waits for it, then releases it to the pool it actually came from.
        Common::ResultStr<VkResult> FlushOneShot( VkCommandBuffer commandBuffer, VkQueue queue,
                                                  const std::source_location& where );
        /// The profiler row of one wait site; the string lives in m_WaitSites, so the name outlives the scope.
        const char* WaitSiteName( const std::source_location& where );
        /// Frees @p commandBuffer now if the calling thread owns @p pool, else queues it for the owner.
        /// The buffer must not be pending execution. Called without m_PoolMutex held.
        void ReleaseOneShot( OneShotPool& pool, VkCommandBuffer commandBuffer );

        /// GUARDS m_ThreadPools, m_OneShotPools and every pool's PendingFree. Never held across a vkCmd*:
        /// the Vulkan calls on a pool are made by its owning thread only, which is what synchronises them.
        mutable std::mutex m_PoolMutex;

        /// Submissions made by RT_SubmitGraphicThenRelease, oldest first, until their fence is seen signalled.
        /// Guarded by m_InFlightMutex, which is never held while m_PoolMutex is taken by the same thread first.
        struct InFlight
        {
            Submitted             Upload;
            std::function<void()> AfterGpu;
        };
        std::mutex            m_InFlightMutex;
        std::vector<InFlight> m_InFlight;
        /// Waits for every in-flight submission and releases it (teardown only).
        void WaitAndReleaseInFlight();
        /// Host-waits every in-flight graphics upload: a one-off on another queue is not ordered behind them.
        void WaitInFlightUploads();

        /// The pools, keyed by (thread, queue family). Pools are never destroyed before Destroy(), so a
        /// OneShotPool* in m_OneShotPools or in a Submitted stays valid for the allocator's whole life.
        std::map<std::pair<std::thread::id, uint32_t>, std::unique_ptr<OneShotPool>> m_ThreadPools;

        /// WHICH POOL EACH ONE-OFF BUFFER CAME FROM — recorded, not recomputed at flush time: the flush may
        /// run on another thread, and vkFreeCommandBuffers against a pool that did not allocate the buffer
        /// is undefined behaviour. Recording it is also what lets a flush of a buffer this allocator never
        /// handed out be REFUSED by name instead of guessed at.
        std::unordered_map<VkCommandBuffer, OneShotPool*> m_OneShotPools;

        /// The profiler row names of the blocking flushes' call sites (WaitSiteName). A set's nodes never move,
        /// so a c_str() handed to the profiler stays valid. Guarded by m_PoolMutex.
        std::set<std::string> m_WaitSites;

        uint32_t m_GraphicsFamily    = 0;
        uint32_t m_ComputeFamily     = 0;
        uint32_t m_TransferOpsFamily = 0;

        VkQueue m_GraphicsQueue;
        VkQueue m_ComputeQueue;
        VkQueue m_TransferOpsQueue;

        VkDevice m_LogicalDevice;
        /// The owner of the queues, for SubmitToQueue. Outlives this allocator: VulkanContext::Shutdown
        /// destroys the allocator while the device is still alive.
        const VulkanLogicalDevice* m_Device = nullptr;
    };
} // namespace Desert::Graphic::API::Vulkan