#pragma once

#include <Engine/Graphic/API/Vulkan/VulkanDevice.hpp>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
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
        /// and the upload timeline value its completion signals (NewestUploadSync's semaphore; one source of
        /// completion for every upload, no fence per submission). The buffer is freed only by
        /// RT_ReleaseSubmitted.
        struct Submitted
        {
            VkCommandBuffer Buffer = VK_NULL_HANDLE;
            OneShotPool*    Pool   = nullptr;
            uint64_t        Value  = 0;
        };

        /// Ends and submits @p commandBuffer on the graphics queue WITHOUT waiting for it — the caller polls
        /// IsComplete() on later frames. The flush above blocks the calling thread until the GPU is done,
        /// which is what made a thumbnail readback a main-thread stall (TH3).
        /// QUEUED, NOT WAITED, IN QUEUE ORDER WITH THE FRAMES (VK-SUBMIT-THREAD step 3): while a frame loop
        /// runs, the submission is an entry of its submit thread (VulkanFrameLoop::EnqueueUnwaited), after the
        /// frames handed to it so far, and the call returns at once with the value reserved at that place; it
        /// waits neither the submit thread nor the GPU. A queued submission the driver refuses still signals its
        /// value (from the host) and ends the run (the frame loop's failure), so no holder waits forever on it.
        /// Inline (submitted before the return) when no loop runs or its thread has exited.
        Common::ResultStr<Submitted> RT_SubmitCommandBufferGraphic( VkCommandBuffer commandBuffer );

        /// True once the submission's value is reached on the upload timeline, false while it is still queued
        /// or executing. Never blocks.
        [[nodiscard]] bool IsComplete( const Submitted& submitted ) const;

        /// Waits for the value (normally already reached; a host wait before its submission is legal for a
        /// timeline), then frees the buffer. Never on the submit thread for a submission still queued behind
        /// the entry it runs: that wait would be on itself.
        void RT_ReleaseSubmitted( const Submitted& submitted );

        /// AN UPLOAD NOBODY WAITS FOR (UE's deferred texture upload: the game thread never sleeps on the GPU).
        /// Ends and submits @p commandBuffer on the graphics queue WITHOUT waiting, and runs @p afterGpu (the
        /// release of what the buffer reads, i.e. its staging copy) once its fence has signalled - polled by
        /// CollectFinished on every frame and on every later submission here, or waited for at Destroy.
        /// Readers on the graphics queue need nothing more: submission order is the dependency, the recorded
        /// barriers make the copy visible. Readers on another queue wait the upload timeline it signals
        /// (NewestUploadSync). On a refused submit nothing reads the staging copy, so @p afterGpu runs at once
        /// and the refusal is answered; a queued submission refused later runs it once its value is signalled.
        /// WHY: every texture the TextureService finished paid a blocking one-off flush behind the whole frame's
        /// GPU work. On warm Bistro (1296 distance fields) that was 2-8 s per texture and ~45% of a 6.4 s
        /// main-thread frame (sample of the p8 boot), and the slow frames then piled up 1 GB of owed deletions.
        Common::BoolResultStr RT_SubmitGraphicThenRelease( VkCommandBuffer       commandBuffer,
                                                           std::function<void()> afterGpu );

        /// Releases every unwaited submission whose fence has signalled; answers how many. Never blocks.
        std::size_t CollectFinished();

        /// THE UPLOAD SYNC POINT (UE: the async compute pipe waits on the graphics fence of the work submitted
        /// before it). Every RT_SubmitCommandBufferGraphic signals @p Timeline with the next value, in queue
        /// submission order; @p Value is the newest one already submitted (or host-signalled after a refusal),
        /// never one only reserved for a queued submission (0: none yet, nothing to wait). A reader on ANOTHER
        /// queue (the frame's first async-compute submission, a one-off on the compute or transfer queue) reads
        /// it in queue order, on the submit thread, so it waits Timeline >= Value on the GPU and is ordered
        /// after every upload handed over before it. Graphics-queue readers need nothing: submission order
        /// already covers them.
        struct UploadSyncPoint
        {
            VkSemaphore Timeline = VK_NULL_HANDLE;
            uint64_t    Value    = 0;
        };
        [[nodiscard]] UploadSyncPoint NewestUploadSync() const;

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

        /// Submissions made by RT_SubmitGraphicThenRelease, oldest first, until their value is seen reached.
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

        /// The timeline semaphore of UploadSyncPoint, created with the allocator (VK_NULL_HANDLE when
        /// vkCreateSemaphore refused: every graphics one-off submit is then refused by name).
        VkSemaphore m_UploadTimeline = VK_NULL_HANDLE;

        /// THE UPLOAD VALUES (VK-SUBMIT-THREAD step 3). A timeline must be signalled with increasing values in
        /// submission order. A queued upload's value is RESERVED at its place in the submit thread's queue
        /// (ReserveUploadValue, called by VulkanFrameLoop::EnqueueUnwaited under its hand-off lock) and
        /// signalled when the thread reaches it (SubmitQueuedUpload); an inline upload (no loop runs) reserves
        /// and submits in one step (SubmitUploadInline). Values never skip: a queued refusal host-signals its
        /// value, an inline refusal gives its value back. Lock order: the frame loop's hand-off lock, then
        /// m_UploadSubmitMutex, then the device's queue mutex (inside SubmitToQueue); never the other way, and
        /// m_UploadSubmitMutex is never held while waiting for the submit thread (the inline wait releases it).
        uint64_t ReserveUploadValue() noexcept;
        /// vkQueueSubmit of one unwaited graphics one-off that signals @p value; publishes nothing.
        VkResult SubmitUpload( VkCommandBuffer commandBuffer, uint64_t value ) const noexcept;
        /// The submit thread's half of a queued upload: submits it; on a refusal signals @p value from the host
        /// (after the value before it, VUID-VkSemaphoreSignalInfo-value-03259); publishes @p value either way.
        /// Empty on success, else the failure the frame loop records.
        std::string SubmitQueuedUpload( VkCommandBuffer commandBuffer, uint64_t value ) noexcept;
        /// An upload with no loop to queue on: waits until no reserved value is still unsubmitted (only at the
        /// loop's registration, when another uploader already queued on it), then reserves the next value into
        /// @p value, submits and publishes it. A refusal gives the value back.
        VkResult SubmitUploadInline( VkCommandBuffer commandBuffer, uint64_t& value ) noexcept;
        /// Publishes @p value as submitted and wakes an inline uploader waiting for it.
        void PublishUploadValue( uint64_t value ) noexcept;
        /// The upload timeline's current value (0 when it cannot be read, e.g. on a lost device). Never blocks.
        [[nodiscard]] uint64_t UploadCompletedValue() const noexcept;
        /// Waits on the host until the upload timeline reaches @p value; the VkResult of the wait.
        VkResult WaitUploadValue( uint64_t value ) const noexcept;

        /// Guards m_UploadReservedValue and every write of m_UploadSubmittedValue (see THE UPLOAD VALUES).
        std::mutex              m_UploadSubmitMutex;
        std::condition_variable m_UploadPublished; // a value was published (SubmitUploadInline waits on it)
        /// The newest value handed out; equal to m_UploadSubmittedValue when no queued upload is pending.
        uint64_t m_UploadReservedValue = 0;
        /// The newest value whose signal is already submitted (or host-signalled); published only after that,
        /// so a reader never waits on a value nothing will signal. The ONE source of UploadSyncPoint::Value.
        std::atomic<uint64_t> m_UploadSubmittedValue{ 0 };

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