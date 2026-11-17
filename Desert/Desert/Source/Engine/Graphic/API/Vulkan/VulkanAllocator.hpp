#pragma once

#include <unordered_map>

#include <Common/Core/Singleton.hpp>

#include <Engine/Graphic/API/Vulkan/VulkanDevice.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanContext.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanUtils/VulkanHelper.hpp>
#include <Engine/Graphic/AllocationLedger.hpp>
#include <Engine/Graphic/MappedMemory.hpp>

#include <VulkanAllocator/vk_mem_alloc.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>

namespace Desert::Graphic::API::Vulkan
{
    struct AllocatedData
    {
        std::string Tag;
        uint32_t    Size;
    };

    struct BufferDeletionEntry
    {
        VkBuffer      Buffer;
        VmaAllocation Allocation;
        uint64_t      QueuedFrame;
    };

    struct ImageDeletionEntry
    {
        VkImage                  Image;
        VmaAllocation            Allocation;
        VkImageView              ImageView;
        std::vector<VkImageView> MipImageViews;
        uint64_t                 QueuedFrame;
    };

    struct FramebufferDeletionEntry
    {
        VkFramebuffer Framebuffer;
        uint64_t      QueuedFrame;
    };

    struct RenderPassDeletionEntry
    {
        VkRenderPass RenderPass;
        uint64_t     QueuedFrame;
    };

    // The AS object only: its storage buffer is queued separately (RT_DestroyBuffer) on the same frame, and the
    // drain destroys every AS before any buffer so the object never outlives the memory it was created in.
    struct AccelerationStructureDeletionEntry
    {
        VkAccelerationStructureKHR            AccelerationStructure;
        PFN_vkDestroyAccelerationStructureKHR Destroy; // the device's entry point (RayTracingDeviceFunctions)
        uint64_t                              QueuedFrame;
    };

    struct QueryPoolDeletionEntry
    {
        VkQueryPool QueryPool;
        uint64_t    QueuedFrame;
    };

    // A pipeline and the layout it was created with. A rebuilt pipeline (the ray tracing pipeline after its hit
    // group set changed) may still be bound in a command buffer of a frame in flight, so both wait for that frame.
    struct PipelineDeletionEntry
    {
        VkPipeline       Pipeline;
        VkPipelineLayout Layout; // VK_NULL_HANDLE when the layout outlives the pipeline
        uint64_t         QueuedFrame;
    };

    // An object some other runtime owns on this device (a vendor upscaler SDK's context and the images, views and
    // pipelines inside it), destroyed by its own call once the frame that last recorded with it has finished.
    struct HostObjectDeletionEntry
    {
        std::function<void()> Destroy;
        uint64_t              QueuedFrame;
    };

    struct DescriptorPoolDeletionEntry
    {
        VkDescriptorPool DescriptorPool;
        uint64_t         QueuedFrame;
        // Empty: destroy the pool. Non-empty: free these sets back into it and keep the pool. One queue
        // for both, because a pool destroyed first frees its sets implicitly and the drain has to know.
        std::vector<VkDescriptorSet> Sets;
    };

    // ONE DEFERRED-DELETION QUEUE, BUCKETED BY THE FRAME IT WAS QUEUED ON (FPS-CPU2-VKFRAME; UE: the deferred
    // deletion queue's per-fence batches). The drain used to move EVERY queued entry of every kind into a taken
    // or a fresh kept vector each frame - O(queued) under m_DeletionMutex and an allocation per non-empty queue
    // even when nothing retired. An entry goes into bucket QueuedFrame % kBuckets; a bucket is taken whole once
    // its NEWEST frame is retired, so the drain touches only retiring entries and every bucket keeps its
    // capacity. kBuckets exceeds every frames-in-flight count plus one, so with a drain per frame a bucket
    // holds one frame; a bucket that mixes two frames (no drain for kBuckets frames) is taken with its newer
    // one - later, never earlier. Every member is called under the owner's m_DeletionMutex.
    template <typename Entry>
    class DeletionRing
    {
    public:
        static constexpr std::size_t kBuckets = 8;

        void push_back( Entry&& entry )
        {
            Bucket& bucket = m_Buckets[entry.QueuedFrame % kBuckets];
            bucket.Newest  = std::max( bucket.Newest, entry.QueuedFrame );
            bucket.Entries.push_back( std::move( entry ) );
            ++m_Size;
        }
        [[nodiscard]] std::size_t size() const noexcept
        {
            return m_Size;
        }
        [[nodiscard]] bool empty() const noexcept
        {
            return m_Size == 0;
        }
        template <typename Visit>
        void ForEach( const Visit& visit ) const
        {
            for ( const Bucket& bucket : m_Buckets )
            {
                for ( const Entry& entry : bucket.Entries )
                    visit( entry );
            }
        }
        // Appends to @p out, oldest frame first, the entries of every bucket whose newest frame @p takeFrame
        // accepts; those buckets are emptied with their capacity kept.
        template <typename TakeFrame>
        void Take( const TakeFrame& takeFrame, std::vector<Entry>& out )
        {
            std::array<std::size_t, kBuckets> order{};
            std::size_t                       count = 0;
            for ( std::size_t index = 0; index < kBuckets; ++index )
            {
                if ( m_Buckets[index].Entries.empty() || !takeFrame( m_Buckets[index].Newest ) )
                    continue;
                // Insertion by frame: at most kBuckets buckets.
                std::size_t at = count++;
                for ( ; at > 0 && m_Buckets[order[at - 1]].Newest > m_Buckets[index].Newest; --at )
                    order[at] = order[at - 1];
                order[at] = index;
            }
            for ( std::size_t taken = 0; taken < count; ++taken )
            {
                Bucket& bucket = m_Buckets[order[taken]];
                for ( Entry& entry : bucket.Entries )
                    out.push_back( std::move( entry ) );
                m_Size -= bucket.Entries.size();
                bucket.Entries.clear();
                bucket.Newest = 0;
            }
        }
        template <typename Erase>
        void EraseIf( const Erase& erase )
        {
            for ( Bucket& bucket : m_Buckets )
            {
                m_Size -= static_cast<std::size_t>( std::erase_if( bucket.Entries, erase ) );
                if ( bucket.Entries.empty() )
                    bucket.Newest = 0;
            }
        }

    private:
        struct Bucket
        {
            std::vector<Entry> Entries;
            uint64_t           Newest = 0;
        };
        std::array<Bucket, kBuckets> m_Buckets;
        std::size_t                  m_Size = 0;
    };

    class VulkanAllocator
    {
    public:
        ~VulkanAllocator();

        Common::ResultStr<VmaAllocation> RT_AllocateImage( const std::string&       tag,
                                                        const VkImageCreateInfo& imageCreateInfo,
                                                        VmaMemoryUsage usage, VkImage& outImage );

        /// THE "NEW MEMORY IS GARBAGE" DEBUG MODE (DESERT_POISON_NEW_MEMORY=1, Debug builds only).
        /// MoltenVK hands out fresh memory zeroed, Windows drivers hand out whatever the last owner left,
        /// so a target read before its first write looks right on this machine and speckled on the
        /// owner's. With the variable set, every host-visible buffer is filled with 0xCD on allocation and
        /// every 2D image without initial data is cleared to NaN (float formats) / 0xCD (integer formats),
        /// which makes a read-before-write visible here. Announced once in the log when active; always
        /// false in Release, where getenv is never consulted.
        [[nodiscard]] static bool PoisonNewMemory();

        Common::ResultStr<VmaAllocation> RT_AllocateBuffer( const std::string&        tag,
                                                         const VkBufferCreateInfo& bufferCreateInfo,
                                                         VmaMemoryUsage usage, VkBuffer& outBuffer );

        /// WHAT THE ALLOCATOR ACTUALLY RESERVED, which is not what the caller asked for: VMA rounds a
        /// request up to the memory type's alignment and to its own block granularity, and a driver may
        /// pad an optimally-tiled image by a good deal more. The ledger wants the number that is charged
        /// against `VK_EXT_memory_budget`, not the one a width, a height and a bytes-per-pixel multiply
        /// out to several files away from the allocation — the same reason `MapAllocation` takes its size
        /// from here rather than from its caller. 0 for a null allocation or before the allocator exists,
        /// which the ledger already reads as "this row reported no size".
        [[nodiscard]] static std::size_t AllocationSize( VmaAllocation allocation );

        void RT_DestroyBuffer( VkBuffer buffer, VmaAllocation allocation );
        // An image's sampler is not its own (VulkanSamplerCache owns every VkSampler), so it is not queued here.
        /// THE STAGING COPY OF A BLOCKING ONE-OFF UPLOAD. @p flushed is what RT_Flush*CommandBuffer* answered for
        /// the command buffer that read it: a success is a waited fence, so the GPU is done with the buffer and it
        /// is destroyed NOW; a failure may have left it pending, so it is queued as RT_DestroyBuffer would.
        /// Queuing every staging copy tied its memory to the frame ring: on Bistro the splash ran ~400 uploads
        /// (~11 MB each) while frames were minutes apart, and the device footprint passed the 6 GB cap.
        void RT_ReleaseStaging( VkBuffer buffer, VmaAllocation allocation,
                                const Common::ResultStr<VkResult>& flushed );
        void RT_DestroyImage( VkImage image, VmaAllocation allocation, VkImageView imageView = VK_NULL_HANDLE,
                              const std::vector<VkImageView>& mipImageViews = {} );
        void RT_DestroyFramebuffer( VkFramebuffer framebuffer );
        void RT_DestroyRenderPass( VkRenderPass renderPass );
        // A timestamp pool the GPU profiler outgrew: the frame that last wrote into it may still be in
        // flight, so it waits for that frame like any other per-frame object.
        void RT_DestroyQueryPool( VkQueryPool queryPool );
        // A pipeline (and its layout, when it owns one) a frame in flight may still have bound.
        void RT_DestroyPipeline( VkPipeline pipeline, VkPipelineLayout layout );
        // A BLAS or TLAS a frame in flight may still trace: waits for that frame like a buffer does.
        void RT_DestroyAccelerationStructure( VkAccelerationStructureKHR            accelerationStructure,
                                              PFN_vkDestroyAccelerationStructureKHR destroy );
        // Takes every set allocated from the pool with it, so it waits for the frame that last bound one of
        // them exactly as a buffer does: a material or a view destroyed mid-session may still have its sets
        // in a command buffer the GPU has not finished.
        void RT_DestroyDescriptorPool( VkDescriptorPool descriptorPool );
        // Frees @p sets back into @p pool once the frame that may still bind them has finished. The pool
        // must have been created with VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT.
        void RT_FreeDescriptorSets( VkDescriptorPool pool, std::vector<VkDescriptorSet> sets );
        // Runs @p destroy once the frame current now has retired (IsRetired), like every queued destruction: for
        // GPU objects created and destroyed through a third-party runtime's own API (the FidelityFX context of a
        // view whose extent changed), which the typed queues above cannot name.
        void RT_DestroyLater( std::function<void()> destroy );
        /// A SERVICE'S HOST-SIDE RETIREMENT (RendererContext::RetireAfterFramesInFlight): drops an engine wrapper
        /// (an Image2D, a Mesh, a pipeline record) a render worker of an earlier frame may still read by raw
        /// pointer. Retired by the same frame rule as RT_DestroyLater, but in its own queue, because unlike a
        /// third-party context it holds no GPU object of its own: the wrapper's destructor queues its device
        /// handles through the typed queues above, by the frame current then. That is what lets a view close
        /// release these at once (ReleaseServiceRetirementsNow) without idling the device.
        void RT_RetireServiceObject( std::function<void()> retire );
        /// UE FlushPendingDeletes at a view's close: runs every queued service retirement now, whatever frame
        /// queued it, and answers how many. The caller has joined the render task (no worker reads a raw
        /// pointer a service answered) and is outside any frame's recording. The device handles the wrappers
        /// release still wait for their frames (above). Repeats until a pass finds nothing.
        std::size_t ReleaseServiceRetirementsNow();

        /// The per-frame drain, called once per frame at its start (VulkanRendererAPI::BeginFrame), after WaitSlot
        /// of the frame now current: destroys every entry whose queuing frame that fence covers (IsRetired).
        void ProcessDeletionQueue();
        /// THE ONE RETIRE RULE, for every queue and every enqueuing thread. An entry carries the absolute frame
        /// number (FrameManager::GetAbsoluteFrameCount) current when it was queued. ProcessDeletionQueue runs
        /// once frame @p current has begun and its slot's fence has been waited, which is the fence of frame
        /// current - framesInFlight: the last frame that could record with an object queued at frame q is q, so
        /// the entry is safe once q <= current - framesInFlight. The ring SLOT it used to be tagged with could not
        /// say this: an entry queued by another thread between NextFrame and the drain took the NEW slot's tag
        /// and was destroyed at once, under the frame just submitted (UE: deferred deletion by frame + fence).
        [[nodiscard]] static constexpr bool IsRetired( uint64_t queued, uint64_t current,
                                                       uint32_t framesInFlight ) noexcept
        {
            return queued + framesInFlight <= current;
        }
        /// At most once a second, from the memory sampling job (ScheduleMemorySampling), and only while the log
        /// level lets a debug line through: owed deletions, the ledger's live bytes and largest tags, and VMA's
        /// per-heap block/allocation bytes against the driver's usage, as one debug line. Growth inside the
        /// ledger, inside VMA's blocks, or only in the driver's number names three different causes. In dev
        /// builds also the subsystem census at every new footprint peak, whatever the level.
        void LogCensusIfDue();

        /// ONE MEASUREMENT WINDOW PER SCENE OPEN (FIELD-MEM p4). The peak census above writes only past the run's
        /// highest footprint + 128 MB, so a second open of the same world that stays under the first one's peak
        /// was never measured. Called at the start of every scene open: writes the closing line of the previous
        /// window, starts a new one at the current footprint and lets the census report again from it.
        /// "[Memory] scene open #N '<scene>' peak P MB, owed O MB" is written whenever the window's peak climbs
        /// (O = deferred destructions still queued at that peak); the last such line of a window is its peak.
        void BeginSceneOpen( std::string_view scene );
        /// Folds the process footprint now into the open window. By the memory sampling job (started by every
        /// ProcessDeletionQueue that finds none in flight) and by the opener at the end of the open, which runs
        /// without frames. Takes m_SamplingMutex.
        void SampleSceneOpen();

        /// Destroys EVERYTHING still queued, whatever frame it was queued on, and answers how many.
        /// For teardown only — see the argument at the definition. The caller must have idled the device.
        std::size_t DrainDeletionQueue();

        /// How many deferred destructions are still owed. Nonzero after the last frame means leaked.
        [[nodiscard]] std::size_t QueuedCount() const;

        /// Every live buffer and image this allocator made, by the creator's tag. A row leaves when the object
        /// is really destroyed (DestroyQueued), not when it is queued. See AllocationLedger for why.
        [[nodiscard]] const AllocationLedger& Ledger() const noexcept
        {
            return m_Ledger;
        }

        /// A mapping you cannot write through without having asked whether it exists — see
        /// Engine/Graphic/MappedMemory.hpp for the whole argument, and for the ten `memcpy`s into a
        /// possibly-null pointer that this return type makes uncompilable rather than merely loud.
        ///
        /// The mapping unmaps itself, so there is no `UnmapMemory` to pair with this call any more: the
        /// pairing was hand-written at thirteen sites and an early return past one of them leaked a
        /// mapping with nothing to say so.
        NO_DISCARD MappedMemory MapMemory( VmaAllocation allocation );

        void Init( const std::shared_ptr<VulkanLogicalDevice>& device, VkInstance instance );

        /// The capability rows of the device this allocator serves (copied at Init): what a buffer's usage
        /// may name (MeshBufferUsage) is decided by what that device enabled.
        [[nodiscard]] const DeviceCaps& GetDeviceCaps() const
        {
            return m_DeviceCaps;
        }

        void Shutdown();

        /// THE DEVICE'S ZERO BUFFER: 64 KB of zeros, host-visible, usable as a uniform or storage buffer, made on
        /// first use and destroyed by Shutdown. A binding nothing has filled yet points here (UE's global null
        /// resources), so a read before the real resource arrives is defined. One per device, never per user.
        [[nodiscard]] Common::ResultStr<VkBuffer> RT_ZeroBuffer();

        static VmaAllocator& GetVMAAllocator();

        /// The running renderer's allocator (EngineContext's renderer context, cast to Vulkan): the one accessor
        /// a Vulkan source reaches it through.
        static VulkanAllocator& Current();

        VulkanAllocator() = default;

    private:
        // The unmap MappedMemory holds as its release hook. Private and static: nothing outside this
        // class may unmap an allocation, because the only object that knows a mapping is still live is
        // the MappedMemory that owns it, and a second unmap of the same allocation is undefined
        // behaviour in VMA. It takes `void*` rather than `VmaAllocation` so that MappedMemory — which
        // lives in Engine/Graphic and must not know Vulkan — can hold its address.
        static void UnmapAllocation( void* allocation );

        // THE ONE "THE HOST WROTE THIS RANGE" AND "THE HOST WILL READ THIS RANGE": vmaFlushAllocation /
        // vmaInvalidateAllocation over [offset, offset + bytes) of a mapped allocation (VMA aligns the range to
        // nonCoherentAtomSize). MapMemory hands them to the MappedMemory only for memory WITHOUT
        // HOST_COHERENT, so every Write/Fill/ReadInto on a non-coherent heap goes through them and a coherent
        // heap never calls them. Private for the reason UnmapAllocation is: only a live mapping may flush.
        static Common::BoolResultStr FlushMappedRange( void* allocation, std::size_t offset, std::size_t bytes );
        static Common::BoolResultStr InvalidateMappedRange( void* allocation, std::size_t offset,
                                                            std::size_t bytes );

        // THE ONE "THE HOST WROTE THESE RANGES": vmaFlushAllocations over every range a MappedWriteBatch recorded
        // in one pass — one vkFlushMappedMemoryRanges for the pass instead of one per range. Handed out beside
        // FlushMappedRange, so only for memory without HOST_COHERENT.
        static Common::BoolResultStr FlushMappedRanges( void* const* allocations, const uint64_t* offsets,
                                                        const uint64_t* sizes, std::size_t count );

        /// The one body behind ProcessDeletionQueue and DrainDeletionQueue. They differ ONLY in which
        /// queued frames they take, and that is the whole reason they share this: two copies of four
        /// erase loops is where a fifth resource kind gets destroyed on one path and leaked on the other.
        /// @p takeFrame: bool( uint64_t queuedFrame ), asked once per bucket (DeletionRing::Take).
        template <typename TakeFrame>
        std::size_t DestroyQueued( const TakeFrame& takeFrame );

        /// THE MEMORY SAMPLING JOB (FPS-CPU2-VKFRAME; UE: stat memory sampled off the frame path). The scene-open
        /// footprint (SampleSceneOpen) and the once-a-second census (LogCensusIfDue) were run by the per-frame
        /// drain on the render thread: two GetProcessMemoryInfo calls every frame and, once a second, a census
        /// that sorted the ledger and formatted lines (p99 3.1 ms / max 3.7 ms in "Vulkan: slot retire"). They
        /// now run as one Background job the drain starts when none is in flight; Shutdown waits for it.
        void              ScheduleMemorySampling();
        void              RunMemorySampling();
        void              WaitMemorySampling() noexcept;
        std::atomic<bool> m_SamplingInFlight{ false };
        // The scene-open window and the census's state: written by the sampling job, BeginSceneOpen (the
        // opener's thread) and SampleSceneOpen's callers.
        mutable std::mutex m_SamplingMutex;

        friend class Common::Singleton<VulkanAllocator>;

        /// The device this allocator was created for. See Init() for why it is held rather than looked up.
        VkDevice m_Device = VK_NULL_HANDLE;
        DeviceCaps m_DeviceCaps;
        /// The queue-family sharing every buffer and image made here is created with (copied at Init from
        /// VulkanPhysicalDevice::GetResourceSharing; VulkanResourceSharing.hpp has the rule and why). Create infos
        /// point into this copy, which lives as long as the allocator.
        VulkanResourceSharing m_Sharing;

        // THE DELETION QUEUES ARE SHARED BY THREADS (UE's pending-delete list under its lock). RT_Destroy* is
        // reached from any thread that drops a GPU object or submits an upload:
        // CommandBufferAllocator::CollectFinished retires a staging copy on whichever thread calls
        // RT_SubmitGraphicThenRelease, and an object's last owner queues its destruction on the thread that drops
        // it. m_DeletionMutex guards all nine vectors: every push, every read (census, OwedBytes, QueuedCount) and
        // the take in DestroyQueued. DestroyQueued MOVES the retired entries out under the lock and destroys them
        // after it is released, so a host-object destroy that queues again cannot deadlock and no vkDestroy* or
        // vmaDestroy* runs under it.
        mutable std::mutex                               m_DeletionMutex;
        DeletionRing<BufferDeletionEntry>                m_BufferDeletionQueue;
        DeletionRing<ImageDeletionEntry>                 m_ImageDeletionQueue;
        DeletionRing<FramebufferDeletionEntry>           m_FramebufferDeletionQueue;
        DeletionRing<RenderPassDeletionEntry>            m_RenderPassDeletionQueue;
        DeletionRing<QueryPoolDeletionEntry>             m_QueryPoolDeletionQueue;
        DeletionRing<PipelineDeletionEntry>              m_PipelineDeletionQueue;
        DeletionRing<AccelerationStructureDeletionEntry> m_AccelerationStructureDeletionQueue;
        DeletionRing<DescriptorPoolDeletionEntry>        m_DescriptorPoolDeletionQueue;
        DeletionRing<HostObjectDeletionEntry>            m_HostObjectDeletionQueue;
        // RT_RetireServiceObject's: engine wrappers only, never a third-party GPU object (see there).
        DeletionRing<HostObjectDeletionEntry> m_ServiceRetireQueue;

        AllocationLedger m_Ledger;
        std::mutex                            m_ZeroBufferMutex;
        VkBuffer                              m_ZeroBuffer           = VK_NULL_HANDLE;
        VmaAllocation                         m_ZeroBufferAllocation = VK_NULL_HANDLE;
        std::chrono::steady_clock::time_point m_LastCensus{};
        // The previous census's bytes per tag: the census names the tags that GREW since it, because a
        // steady growth spread over tags below the largest six is otherwise invisible.
        std::unordered_map<std::string, std::size_t> m_LastCensusBytes;
        // The process resident peak (MemoryWatch) the last full census was written at: the next one is written
        // when the peak has climbed kCensusPeakStep past it, so a run killed for its memory ends on a census
        // taken at most one step below the kill.
        uint64_t m_CensusPeakReported = 0;

        /// Bytes of the buffers and images queued for deferred destruction (the census's "owed" row).
        /// Takes m_DeletionMutex.
        [[nodiscard]] std::size_t OwedBytes() const;
        /// QueuedCount's body. Caller holds m_DeletionMutex.
        [[nodiscard]] std::size_t QueuedCountLocked() const;
        void                      LogSceneOpenPeak( std::string_view when ) const;
        uint32_t                  m_SceneOpenIndex = 0; // 0 = no open yet
        std::string               m_SceneOpenName;
        uint64_t                  m_SceneOpenPeak       = 0;
        std::size_t               m_SceneOpenOwedAtPeak = 0;
        uint64_t                  m_SceneOpenPeakLogged = 0;
    };
} // namespace Desert::Graphic::API::Vulkan