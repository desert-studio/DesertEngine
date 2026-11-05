#pragma once

#include <Common/Core/Profiler.hpp>
#include <Common/Core/ResultStr.hpp>

#include <Engine/Graphic/API/Vulkan/VulkanRdgQueues.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanRecordContext.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanResourceSharing.hpp>
#include <Engine/Graphic/AllocationLedger.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanSamplerCache.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>

#include <VulkanAllocator/vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <array>
#include <atomic>
#include <unordered_map>
#include <cstdint>
#include <compare>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <memory>
#include <shared_mutex>
#include <string>
#include <span>
#include <string_view>
#include <tuple>
#include <vector>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <unordered_map>

// The Vulkan side of the render graph (RDG2): physical resources for the graph's descriptions, a pool
// that hands the same images back while the graph does not change, the device's memory requirements for
// Compile, and the IBackend that records one vkCmdPipelineBarrier per pass and, for a raster pass, a
// VkRenderPass + VkFramebuffer it builds from the pass's declared targets and caches (the UE Vulkan RHI
// shape: FVulkanRenderPass / FVulkanFramebuffer). Vulkan 1.0 barriers and render passes only - owner
// decision 2026-09-27: no synchronization2, no dynamic rendering. The debug label and GPU timestamp of
// every pass are opened and closed in one place.
//
// Only raw handles come in (VulkanRdgDevice), no engine singleton, so the device suite drives exactly this
// code on a headless device it creates itself.
namespace Desert::Graphic::API::Vulkan
{
    class VulkanRdgTransientAllocator; // VulkanRdgTransient.hpp
    class VulkanGpuProfiler;           // VulkanGpuProfiler.hpp

    struct VulkanRdgDevice
    {
        VkDevice     Device    = VK_NULL_HANDLE;
        VmaAllocator Allocator = nullptr;
        // The packed depth+stencil format this device picked (ImageFormat::DEPTH24STENCIL8).
        VkFormat DepthStencilFormat = VK_FORMAT_UNDEFINED;
        // The physical device's limits: a RenderArea pass is refused by name past its framebuffer / viewport size.
        VkPhysicalDeviceLimits Limits{};
        // The queue-family sharing every transient and extracted resource is created with: the device's rule
        // (VulkanPhysicalDevice::GetResourceSharing, VulkanResourceSharing.hpp). A graph with an AsyncCompute
        // segment is refused unless it is CONCURRENT across both pipes' families, because the backend realises the
        // graph's fork/join release / acquire pairs without family indices (RecordBarriers).
        VulkanResourceSharing Sharing;

        // Debug labels exist only on an instance with VK_EXT_debug_utils; null means no labels.
        PFN_vkCmdBeginDebugUtilsLabelEXT CmdBeginLabel = nullptr;
        PFN_vkCmdEndDebugUtilsLabelEXT   CmdEndLabel   = nullptr;
        // GPU timestamps per pass; null when the build has no GPU profiler (Shipping).
        Common::Profiling::IGpuProfilerSink* Profiler = nullptr;
        // RDG-PARALLEL-3: the engine's Vulkan GPU profiler, which re-stamps the scope order of a chunk recorded on a
        // worker as the chunk is replayed (StampChunkOrder); null without one (a device suite).
        VulkanGpuProfiler* ChunkOrder = nullptr;
    };

    // Usage flags a transient is created with, from the graph's derived usage (bit i = 1 << Access i).
    VkImageUsageFlags  RdgImageUsage( uint32_t accessMask );
    VkBufferUsageFlags RdgBufferUsage( uint32_t accessMask );

    // The Vulkan translation of the graph's own state enums; one table each, no second copy.
    // RdgVulkanStages is declared in VulkanRdgQueues.hpp (included above).
    VkAccessFlags RdgVulkanAccess( RDG::MemoryAccessFlags access );
    VkImageLayout RdgVulkanLayout( RDG::ImageLayout layout );
    // The inverse of RdgVulkanLayout, for an image imported from its own layout record. A layout the graph
    // has no name for is refused rather than mapped to a near neighbour.
    std::optional<RDG::ImageLayout> RdgLayoutFromVulkan( VkImageLayout layout );
    // The one translation of a shared sampler state into Vulkan: a pass's Sampler entry and a compute bake's
    // SetSampler both acquire their VkSampler from the engine sampler cache through it.
    VkSamplerCreateInfo RdgSamplerCreateInfo( const RDG::SamplerDesc& desc );

    // One attachment of a render pass as the graph declared it. Format UNDEFINED marks an unused colour slot.
    struct RdgAttachmentKey
    {
        VkFormat            Format = VK_FORMAT_UNDEFINED;
        VkAttachmentLoadOp  Load   = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        VkAttachmentStoreOp Store  = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        VkImageLayout       Layout = VK_IMAGE_LAYOUT_UNDEFINED; // initial = final = subpass layout

        auto operator<=>( const RdgAttachmentKey& ) const = default;
    };

    struct RdgRenderPassKey
    {
        std::vector<RdgAttachmentKey>   Colours;  // by slot
        std::vector<RdgAttachmentKey>   Resolves; // by colour slot (Format UNDEFINED: not resolved); empty if none
        std::optional<RdgAttachmentKey> Depth;
        bool                            HasStencil = false;
        uint32_t                        Samples    = 1;

        auto operator<=>( const RdgRenderPassKey& ) const = default;
    };

    // The canonical render pass a pipeline is built against: formats and samples of the target layout,
    // load/store DONT_CARE. Render-pass compatibility ignores load/store and layouts, so a pipeline built
    // against it draws inside every render pass the backend builds for the same formats.
    RdgRenderPassKey RdgCompatibleRenderPassKey( const std::vector<VkFormat>& colourFormats, VkFormat depthFormat,
                                                 bool depthHasStencil, uint32_t samples );
    // The render-pass-compatibility class of a pass (Vulkan: formats and sample counts; load/store, layouts and,
    // for a single subpass, resolve attachments do not count). A graphics pipeline's variant is cached under it.
    RdgRenderPassKey RdgCompatibilityKey( const RdgRenderPassKey& key );
    // THE render pass builder: the backend's cached passes and the pipelines' canonical passes come from it.
    Common::ResultStr<VkRenderPass> CreateRdgRenderPass( VkDevice device, const RdgRenderPassKey& key );

    // RDG-PARALLEL-7 (UE FVulkanView::ViewId / FVulkanBuffer id): a physical graph resource's identity, unique for
    // the process and never reused. A cache keyed by what a descriptor names (VulkanRdgPassDescriptors::
    // AcquireCached) keys by it, not by the Vulkan handle: a destroyed image's or buffer's handle value can come
    // back for a new object, an identity cannot, so a stale entry never matches and simply ages out.
    [[nodiscard]] uint64_t NextRdgPhysicalIdentity() noexcept;

    class VulkanRdgTexture final : public RDG::IPhysicalTexture
    {
    public:
        // Creates an image owned by this object (freed on destruction).
        static Common::ResultStr<std::shared_ptr<VulkanRdgTexture>> Create( const VulkanRdgDevice&  device,
                                                                            const RDG::TextureDesc& desc,
                                                                            uint32_t                accessMask,
                                                                            std::string_view        name );
        // RDG-CONTRACTS A(1). Creates an image bound to @p memory at @p offset (a transient heap). The image is
        // destroyed with this object; the memory is not (the allocator owns it). Called only by
        // VulkanRdgTransientAllocator::PlaceTexture.
        static Common::ResultStr<std::shared_ptr<VulkanRdgTexture>>
        CreatePlaced( const VulkanRdgDevice& device, const RDG::TextureDesc& desc, uint32_t accessMask,
                      VmaAllocation memory, uint64_t offset, std::string_view name );
        // Wraps an image someone else owns (a swapchain image, an engine image); never destroyed here.
        static std::shared_ptr<VulkanRdgTexture> Wrap( VkDevice device, VkImage image, VkFormat format,
                                                       const RDG::TextureDesc& desc );
        ~VulkanRdgTexture() override;

        [[nodiscard]] RDG::BackendKind GetBackendKind() const override
        {
            return RDG::BackendKind::Vulkan;
        }

        // A view of @p range (kAllRemaining resolved against the description), created once and cached.
        // A range of one layer of a 2D texture is a 2D view; more layers make an array view; the whole of a
        // cube or a volume is a cube / 3D view. An attachment view is always 2D / 2D array.
        Common::ResultStr<VkImageView> GetView( RDG::SubresourceRange range         = RDG::SubresourceRange::All(),
                                                bool                  forAttachment = false );

        [[nodiscard]] VkImage GetImage() const
        {
            return m_Image;
        }
        [[nodiscard]] VkFormat GetFormat() const
        {
            return m_Format;
        }
        [[nodiscard]] VkImageAspectFlags GetAspect() const
        {
            return m_Aspect;
        }
        [[nodiscard]] const RDG::TextureDesc& GetDesc() const
        {
            return m_Desc;
        }
        // The same description and usage: an image created for one can stand in for the other.
        [[nodiscard]] bool     Matches( const RDG::TextureDesc& desc, uint32_t accessMask ) const;

        // Hands the image's ledger row to @p holder (VulkanRdgPool: the extractor while leased, the device while
        // idle). A placed or wrapped image has no row of its own — the heap's row / its owner's covers it.
        void ClaimLedger( const ResourceAttribution& holder )
        {
            m_Ledger.Claim( holder.Owner, Common::AssetHandle{}, holder.Instance );
        }
        [[nodiscard]] uint32_t GetAccessMask() const
        {
            return m_AccessMask;
        }
        // NextRdgPhysicalIdentity: this object's image and every view GetView made of it.
        [[nodiscard]] uint64_t GetIdentity() const
        {
            return m_Identity;
        }

    private:
        VulkanRdgTexture() = default;

        uint64_t           m_Identity   = NextRdgPhysicalIdentity();

        VkDevice           m_Device     = VK_NULL_HANDLE;
        VmaAllocator       m_Allocator  = nullptr; // null for a wrapped image
        VmaAllocation      m_Allocation = nullptr;
        VkImage            m_Image      = VK_NULL_HANDLE;
        VkFormat           m_Format     = VK_FORMAT_UNDEFINED;
        VkImageAspectFlags m_Aspect     = 0;
        RDG::TextureDesc   m_Desc;
        uint32_t           m_AccessMask = 0;
        // The dedicated allocation's ledger row (Create only): the image's kind and the allocation's bytes.
        ResourceOwnership                                                         m_Ledger;
        // Created lazily by GetView, which the recording threads call from exec lambdas: lookups share the lock,
        // a new view takes it alone.
        std::map<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t>, VkImageView> m_Views;
        std::shared_mutex                                                         m_ViewsLock;
    };

    // The view a sampled input bound inside a graph node names, for exactly the subresource the node declared:
    // All -> @p whole (the image's own view); one mip over every layer -> @p mipView of it (the image's own mip
    // view: a cube stays a cube, a volume a volume); any narrower layer range -> @p graphTexture's view of it
    // (the imported image's graph handle, which keeps its views alive for the frames in flight). A layer range
    // of an image the graph has no handle on, of a packed depth-stencil image (no single sampleable aspect), or
    // a missing view is an error naming the range.
    Common::ResultStr<VkImageView> SampledSubresourceView( const RDG::SubresourceRange& range, VkImageView whole,
                                                           const std::function<VkImageView( uint32_t )>& mipView,
                                                           VulkanRdgTexture* graphTexture );

    class VulkanRdgBuffer final : public RDG::IPhysicalBuffer
    {
    public:
        // Host-visible (and persistently mapped) when the usage includes HostRead: a readback target.
        static Common::ResultStr<std::shared_ptr<VulkanRdgBuffer>> Create( const VulkanRdgDevice& device,
                                                                           const RDG::BufferDesc& desc,
                                                                           uint32_t               accessMask,
                                                                           std::string_view       name );
        // RDG-CONTRACTS A(1). A buffer bound to @p memory at @p offset; see VulkanRdgTexture::CreatePlaced.
        static Common::ResultStr<std::shared_ptr<VulkanRdgBuffer>>
        CreatePlaced( const VulkanRdgDevice& device, const RDG::BufferDesc& desc, uint32_t accessMask,
                      VmaAllocation memory, uint64_t offset, std::string_view name );
        // The graph's handle on an engine buffer it does not own (Renderer::ImportBuffer): it records the
        // barriers and binds @p buffer, and never destroys it; the engine buffer outlives every graph that
        // imports it because its own frames-in-flight release waits for them. @p offset: the wrapped range
        // begins @p offset bytes into @p buffer (a mesh's span of a GeometryPool buffer,
        // VulkanRendererAPI::ImportVertexBufferRange); every descriptor, barrier, copy and device address the
        // graph derives from the handle is of [offset, offset + size).
        static std::shared_ptr<VulkanRdgBuffer> Wrap( VkBuffer buffer, uint64_t size, uint64_t offset = 0 );
        // Wrap of the buffer backing @p accelerationStructure (a view's TLAS): a block entry of kind
        // AccelerationStructure on it binds @p accelerationStructure.
        static std::shared_ptr<VulkanRdgBuffer>
        WrapAccelerationStructure( VkBuffer buffer, uint64_t size,
                                   VkAccelerationStructureKHR accelerationStructure );
        ~VulkanRdgBuffer() override;

        [[nodiscard]] RDG::BackendKind GetBackendKind() const override
        {
            return RDG::BackendKind::Vulkan;
        }

        [[nodiscard]] VkBuffer GetBuffer() const
        {
            return m_Buffer;
        }
        [[nodiscard]] uint64_t GetSize() const
        {
            return m_Size;
        }
        // Where this resource begins in GetBuffer() (0 except for a wrapped sub-range): every address or
        // descriptor of the resource adds it.
        [[nodiscard]] uint64_t GetOffset() const
        {
            return m_Offset;
        }
        [[nodiscard]] uint32_t GetAccessMask() const
        {
            return m_AccessMask;
        }
        // NextRdgPhysicalIdentity: this object's buffer range (and acceleration structure), fixed for its life.
        [[nodiscard]] uint64_t GetIdentity() const
        {
            return m_Identity;
        }
        // VK_NULL_HANDLE unless made by WrapAccelerationStructure.
        [[nodiscard]] VkAccelerationStructureKHR GetAccelerationStructure() const
        {
            return m_AccelerationStructure;
        }
        // Null unless host-visible. Reading it is only meaningful after the submission's fence.
        [[nodiscard]] const void* GetMapped() const
        {
            return m_Mapped;
        }
        // Makes device writes visible to the host on non-coherent memory; no-op on coherent memory.
        void InvalidateForHost() const;
        [[nodiscard]] std::span<const std::byte> ReadForHost() const override;

        // See VulkanRdgTexture::ClaimLedger.
        void ClaimLedger( const ResourceAttribution& holder )
        {
            m_Ledger.Claim( holder.Owner, Common::AssetHandle{}, holder.Instance );
        }

    private:
        VulkanRdgBuffer() = default;

        uint64_t      m_Identity   = NextRdgPhysicalIdentity();

        VmaAllocator  m_Allocator  = nullptr;
        VmaAllocation m_Allocation = nullptr;
        VkBuffer      m_Buffer     = VK_NULL_HANDLE;
        uint64_t      m_Size       = 0;
        uint64_t      m_Offset     = 0;
        uint32_t      m_AccessMask = 0;
        // The dedicated allocation's ledger row (Create only), as VulkanRdgTexture's.
        ResourceOwnership m_Ledger;
        void*         m_Mapped     = nullptr;

        VkAccelerationStructureKHR m_AccelerationStructure = VK_NULL_HANDLE;
    };

    // Physical resources for transients, WITHOUT aliasing: every transient gets its own image or buffer.
    // An entry is handed out again for the same description and usage, so an unchanged graph gets the same
    // images every frame and whatever descriptors point at them stay valid.
    //
    // Entries are shared by every frame slot (UE FRenderTargetPool): an entry is handed out again only when
    // nothing holds it and its last hand-out was in a frame the caller has waited for (the renderer's
    // frames-in-flight fence), so the first barrier of a reused image - from UNDEFINED, waiting on nothing -
    // never races the GPU still reading it. Per-slot lists (p2) kept a history target's copies once per slot
    // (4 where 3 suffice). An entry not handed out since the last waited frame is destroyed at BeginFrame; an
    // entry still held by an extraction target is never handed out twice.
    class VulkanRdgPool
    {
    public:
        VulkanRdgPool( const VulkanRdgDevice& device, uint32_t frameSlots );

        // Begins the next frame. The caller has waited for the fence of the frame slot it is about to record
        // into, i.e. for the frame frameSlots before this one.
        void BeginFrame();

        // Keeps @p texture alive until the BeginFrame frameSlots frames after the frame current now: by then the
        // caller has waited for this frame's fence, and fences are waited in submission order, so every frame
        // that could have recorded the texture's views has completed. The engine image's graph handle goes here
        // when the image is released or resized (IVulkanImage::DropGraphTexture), instead of destroying views a
        // frame in flight may still reference. ANY THREAD: the image's last owner may drop it on a loader
        // thread, so the entry is tagged with the frame NUMBER (atomic) under its own mutex - a ring-slot tag read
        // racing BeginFrame could name the slot just begun and be released under the frame still in flight.
        void Retire( std::shared_ptr<RDG::IPhysicalTexture> texture );

        // An extracted transient's dedicated resource, LEASED to @p holder (RDG::ResourceView::ExtractOwner): its
        // ledger row is the holder's while any copy of the returned pointer lives, and the device's (the pool's)
        // from the moment the last one is dropped until it is handed out again or freed.
        Common::ResultStr<std::shared_ptr<VulkanRdgTexture>> AcquireTexture( const RDG::TextureDesc&    desc,
                                                                             uint32_t                   accessMask,
                                                                             std::string_view           name,
                                                                             const ResourceAttribution& holder );
        Common::ResultStr<std::shared_ptr<VulkanRdgBuffer>>  AcquireBuffer( const RDG::BufferDesc&     desc,
                                                                            uint32_t                   accessMask,
                                                                            std::string_view           name,
                                                                            const ResourceAttribution& holder );

        [[nodiscard]] size_t GetTextureCount() const;
        [[nodiscard]] size_t GetBufferCount() const;

    private:
        template <class T>
        struct Entry
        {
            std::shared_ptr<T> Resource;
            uint64_t           LastFrame = 0; // frame counter of the last hand-out
        };
        struct RetiredTexture
        {
            std::shared_ptr<RDG::IPhysicalTexture> Texture;
            uint64_t                               Frame = 0; // m_Frame when it was retired
        };
        // Frame @p frame has completed: the caller waited for it at a BeginFrame since.
        [[nodiscard]] bool Fenced( uint64_t frame ) const noexcept
        {
            return frame + m_FrameSlots <= m_Frame.load();
        }

        const VulkanRdgDevice&               m_Device;
        uint32_t                             m_FrameSlots = 1;
        std::vector<Entry<VulkanRdgTexture>> m_Textures;
        std::vector<Entry<VulkanRdgBuffer>>  m_Buffers;
        // Written by BeginFrame (render thread), read by Retire on any thread.
        std::atomic<uint64_t>       m_Frame{ 1 };
        std::mutex                  m_RetiredMutex; // guards m_Retired only
        std::vector<RetiredTexture> m_Retired;
    };

    // The dedicated (extracted) render graph resources' own ledger, by resource name: VMA allocations outside
    // VulkanAllocator's tag ledger, rolled up by the memory census (VulkanAllocator::LogCensusIfDue).
    [[nodiscard]] const AllocationLedger& RdgDedicatedLedger();

    // RDG-CONTRACTS A(3). Descriptor sets for graph resources, allocated from a pool per frame slot that is reset
    // when the slot is re-begun. The one way a pass binds a transient to a pipeline: allocate a set for the
    // pipeline's layout inside exec, write the pass's bindings into it, bind it, forget it.
    // RDG-PARALLEL-7 (RDG-REC-3b): a set no other route writes into comes from the content cache instead
    // (AcquireCached), which keeps it across frames for every exec that would write the same descriptors.
    // Never: return a set to a caller outside the exec lambda that allocated it; write a set with a view of a
    // resource the pass did not declare (the binding comes from PassContext::GetTexture, which refuses those);
    // write into a cached set after it was published.
    class VulkanRdgPassDescriptors
    {
    public:
        // @p accelerationStructures: the device has VK_KHR_acceleration_structure enabled, so the pools also hold
        // ACCELERATION_STRUCTURE_KHR descriptors (a pool size of that type is invalid without it).
        VulkanRdgPassDescriptors( VkDevice device, VulkanSamplerCache& samplers, uint32_t frameSlots,
                                  bool accelerationStructures );
        ~VulkanRdgPassDescriptors();
        VulkanRdgPassDescriptors( const VulkanRdgPassDescriptors& )            = delete;
        VulkanRdgPassDescriptors& operator=( const VulkanRdgPassDescriptors& ) = delete;

        Common::ResultStr<VkDescriptorSet> Allocate( VkDescriptorSetLayout layout );
        // @p type is the layout binding's type: SAMPLED_IMAGE or STORAGE_IMAGE - a layout alone does not say
        // whether an image in GENERAL is sampled or stored. A pass never writes a COMBINED_IMAGE_SAMPLER: its
        // sampler state is a SAMPLER slot of its own (WriteSampler). @p element is the descriptor of an arrayed
        // binding (a separate texture2D u_X[N]).
        Common::BoolResultStr WriteTexture( VkDescriptorSet set, uint32_t binding, VkDescriptorType type,
                                            const RDG::TextureBinding& texture, RDG::SubresourceRange range,
                                            VkImageLayout layout, uint32_t element = 0 );
        // A separate sampler slot (VK_DESCRIPTOR_TYPE_SAMPLER).
        void WriteSampler( VkDescriptorSet set, uint32_t binding, VkSampler sampler );
        // @p type: UNIFORM_BUFFER or STORAGE_BUFFER; the whole buffer is bound.
        Common::BoolResultStr WriteBuffer( VkDescriptorSet set, uint32_t binding, VkDescriptorType type,
                                           const RDG::BufferBinding& buffer );
        // An ACCELERATION_STRUCTURE_KHR slot: the acceleration structure @p tlas stores
        // (VulkanRdgBuffer::WrapAccelerationStructure), written through
        // VkWriteDescriptorSetAccelerationStructureKHR.
        Common::BoolResultStr WriteAccelerationStructure( VkDescriptorSet set, uint32_t binding,
                                                          VkDescriptorType type, const RDG::BufferBinding& tlas );

        // RDG-PARALLEL-6 (UE FVulkanDescriptorSetWriter): the Write* calls above QUEUE their descriptor on the
        // calling thread; one vkUpdateDescriptorSets per WriteBatch submits them all. A batch is opened around
        // the writes of one exec's sets and flushed when it closes, before the sets are bound. Per thread, so
        // two chunk workers writing the sets of their own arenas never share the queue. Nestable: the outermost
        // flushes.
        class WriteBatch
        {
        public:
            explicit WriteBatch( VulkanRdgPassDescriptors& descriptors );
            ~WriteBatch();
            WriteBatch( const WriteBatch& )            = delete;
            WriteBatch& operator=( const WriteBatch& ) = delete;

        private:
            VulkanRdgPassDescriptors& m_Descriptors;
        };

    private:
        // Queues @p write (its info pointers are patched when the batch flushes); with no batch open on this
        // thread it is written at once, so a write is never left unsubmitted.
        void Queue( const VkWriteDescriptorSet& write, const VkDescriptorImageInfo* image,
                    const VkDescriptorBufferInfo* buffer, VkAccelerationStructureKHR structure );
        void Flush();

    public:
        // RDG-PARALLEL-7 (RDG-REC-3b; UE FVulkanDescriptorSetCache, keyed like FVulkanHashableDescriptorInfo): a
        // set written with exactly the descriptors @p key names lives across frames, and every exec whose key is
        // equal binds it again with no allocation and no vkUpdateDescriptorSets. @p key: the layout handle first,
        // then per descriptor its slot, type, the identity of what it names (VulkanRdgTexture/VulkanRdgBuffer::
        // GetIdentity, never a view or buffer handle: those are reused after a destroy) and its range/layout. So
        // a change of resource or view is a new key, never a stale hit; an entry no exec found for longer than
        // the frames in flight is freed (BeginFrameSlot). Safe for chunks: the cache is sharded under locks, and
        // a fresh set is seen only by the thread that allocated it until that thread's batch wrote it and it
        // calls PublishCached.
        struct CachedSet
        {
            VkDescriptorSet  Set  = VK_NULL_HANDLE;
            VkDescriptorPool Pool = VK_NULL_HANDLE;
            bool             Hit  = false; // written before: bind it as it is and write nothing into it
        };
        Common::ResultStr<CachedSet> AcquireCached( VkDescriptorSetLayout layout, std::span<const uint64_t> key );
        // After the WriteBatch that wrote @p fresh (Hit false) has flushed: makes it findable under @p key. When
        // another thread published an equal key first, that entry stays and @p fresh is freed once no frame in
        // flight can bind it.
        void PublishCached( std::span<const uint64_t> key, const CachedSet& fresh );
        // A fresh set that is not published (an error return before its writes ended): freed once no frame in
        // flight can bind it.
        void DiscardCached( const CachedSet& fresh );

        // The frame loop, after the slot's fence: resets that slot's pools (every arena) and frees the cached
        // sets no frame in flight can bind any more.
        void BeginFrameSlot( uint32_t slot );
        // RDG-PARALLEL-3 (UE: a descriptor pool per parallel command list context): arena 0 is the calling
        // thread's, arena 1 + i graph chunk i's (VulkanRdgBackend::BeginChunk binds it to its worker), so no two
        // threads allocate from one VkDescriptorPool (externally synchronized). Calling thread, before any worker
        // starts: the current slot has arenas 0 .. @p arenas - 1.
        void ReserveArenas( uint32_t arenas );
        // Binds arena @p arena to the calling thread (Allocate draws from it); returns the one bound before.
        static uint32_t BindArena( uint32_t arena ) noexcept
        {
            const uint32_t previous = t_Arena;
            t_Arena                 = arena;
            return previous;
        }
        // The sampler @p desc describes, from the device's VulkanSamplerCache: made on its first request, shared
        // by every entry (and every texture) with an equal state, owned by the cache. Nothing is created per
        // frame.
        Common::ResultStr<VkSampler> GetSampler( const RDG::SamplerDesc& desc );

    private:
        struct SlotPools
        {
            std::vector<VkDescriptorPool> Pools; // Pools[Current] allocates; a full one is followed by a new one
            size_t                        Current = 0;
        };

        Common::ResultStr<VkDescriptorPool> AddPool( SlotPools& slot );
        // One pool shape for every pool of this object; @p flags: FREE_DESCRIPTOR_SET for the cache's pools.
        Common::ResultStr<VkDescriptorPool> CreatePool( VkDescriptorPoolCreateFlags flags );
        // BeginFrameSlot: frees the cached and retired sets last bound before the oldest frame in flight.
        void EvictCached();

        struct CachedSetEntry
        {
            std::vector<uint64_t> Key;
            VkDescriptorSet       Set      = VK_NULL_HANDLE;
            VkDescriptorPool      Pool     = VK_NULL_HANDLE;
            uint64_t              LastUsed = 0; // m_CacheFrame of the last exec that acquired it
        };
        struct CachedSetShard
        {
            std::mutex                                                Lock;
            std::unordered_map<uint64_t, std::vector<CachedSetEntry>> Buckets; // by the key's hash
        };
        struct RetiredCachedSet
        {
            VkDescriptorSet  Set   = VK_NULL_HANDLE;
            VkDescriptorPool Pool  = VK_NULL_HANDLE;
            uint64_t         Frame = 0; // m_CacheFrame it was last bindable in
        };
        static constexpr size_t kCachedSetShards = 16;

        VkDevice                            m_Device = VK_NULL_HANDLE;
        std::vector<std::vector<SlotPools>> m_Slots; // per frame slot, its arenas (0: the calling thread's)
        uint32_t                            m_Slot = 0;
        static inline thread_local uint32_t t_Arena = 0;
        VulkanSamplerCache&    m_SamplerCache;
        bool                   m_AccelerationStructures = false;
        // The content cache (AcquireCached). Its pools allow vkFreeDescriptorSets and are never reset.
        std::array<CachedSetShard, kCachedSetShards> m_CachedShards;
        std::mutex                    m_CachedPoolLock; // m_CachedPools, m_CachedPoolCurrent, m_RetiredCached
        std::vector<VkDescriptorPool> m_CachedPools;
        size_t                        m_CachedPoolCurrent = 0;
        std::vector<RetiredCachedSet> m_RetiredCached;
        std::atomic<uint64_t>         m_CacheFrame{ 1 }; // advanced by BeginFrameSlot
    };

    // THE PASS'S BOUND SETS (GPU-COST-36; UE FMeshDrawShaderBindings: a mesh draw command's descriptor bindings
    // are built once with the command, and every draw of it only binds them). Before, every draw of a pass
    // resolved its block against the shader, allocated a fresh set per descriptor set of the pipeline layout,
    // wrote every entry and copied the material's slots in (VulkanRendererAPI::BindGraphicsPassState) - the same
    // descriptors for every draw of one (block, shader, layout, material): hundreds of draws of a few blocks.
    // The memo keeps, for the ONE pass exec between VulkanRdgBackend::BeginPass and EndPass, the sets one such
    // combination was written into; a later draw of the same combination binds them. A set is written before its
    // first bind and never after it, so every draw that binds it reads the same descriptors it would have had in a
    // set of its own. Never kept across passes: a pass's blocks name only its own resources, and the pool the sets
    // came from is reset with the frame slot.
    class VulkanRdgPassSetMemo
    {
    public:
        // What the sets were written from. Block: the pass's declared block (RDG::PassBindings::GetBlock); Shader and
        // Layout: the recording pipeline's (the reflection the block was resolved against, the set layouts the sets
        // were allocated for); Material and MaterialSet / MaterialBindings: the material whose written slots were
        // copied into set 0 (0 / empty for none); MaterialPush and BlockPushBytes: the push-constant routes the
        // resolution checked. Shader and Material are addresses compared as identities, never dereferenced.
        struct Key
        {
            RDG::BindingBlockRef      Block;
            std::uintptr_t            Shader         = 0;
            VkPipelineLayout          Layout         = VK_NULL_HANDLE;
            std::uintptr_t            Material       = 0;
            VkDescriptorSet           MaterialSet    = VK_NULL_HANDLE;
            std::span<const uint32_t> MaterialBindings;
            bool                      MaterialPush   = false;
            size_t                    BlockPushBytes = 0;
        };

        // The sets written for @p key in this pass exec, or null.
        [[nodiscard]] const std::vector<VkDescriptorSet>* Find( const Key& key ) const;
        // The sets just written for @p key (its MaterialBindings are copied).
        void Keep( const Key& key, std::vector<VkDescriptorSet> sets );
        // A pass exec begins or ends: nothing written for another pass is bound by this one.
        void Clear();

    private:
        struct Entry
        {
            RDG::BindingBlockRef         Block;
            std::uintptr_t               Shader         = 0;
            VkPipelineLayout             Layout         = VK_NULL_HANDLE;
            std::uintptr_t               Material       = 0;
            VkDescriptorSet              MaterialSet    = VK_NULL_HANDLE;
            std::vector<uint32_t>        MaterialBindings;
            bool                         MaterialPush   = false;
            size_t                       BlockPushBytes = 0;
            std::vector<VkDescriptorSet> Sets;
        };
        [[nodiscard]] static bool Matches( const Entry& entry, const Key& key );

        std::vector<Entry> m_Entries;
        mutable size_t     m_LastHit = 0; // consecutive draws of one block hit the same entry
    };

    // Memory requirements from the device, for Compile's aliasing plan: the same create info the pool uses,
    // asked of a temporary image / buffer that never gets memory, cached per description and usage.
    class VulkanRdgMemoryRequirements final : public RDG::IMemoryRequirementsProvider
    {
    public:
        explicit VulkanRdgMemoryRequirements( const VulkanRdgDevice& device ) : m_Device( device )
        {
        }

        Common::ResultStr<RDG::MemoryRequirements> GetTextureRequirements( const RDG::TextureDesc& desc,
                                                                           uint32_t accessMask ) const override;
        Common::ResultStr<RDG::MemoryRequirements> GetBufferRequirements( const RDG::BufferDesc& desc,
                                                                          uint32_t accessMask ) const override;

    private:
        const VulkanRdgDevice&                                 m_Device;
        // CPU-FRAME-5: keyed by the create info's inputs as values - no formatted string and no string compare
        // per lookup. Samples is part of the image's create info (RdgImageInfo), so it is part of the key.
        struct TextureKey
        {
            uint32_t Width      = 0;
            uint32_t Height     = 0;
            uint32_t Depth      = 0;
            uint32_t Format     = 0;
            uint32_t Mips       = 0;
            uint32_t Layers     = 0;
            uint32_t Dim        = 0;
            uint32_t Samples    = 0;
            uint32_t AccessMask = 0;

            bool operator==( const TextureKey& ) const = default;
        };
        struct BufferKey
        {
            uint64_t Bytes      = 0;
            uint32_t AccessMask = 0;

            bool operator==( const BufferKey& ) const = default;
        };
        struct KeyHash
        {
            size_t operator()( const TextureKey& key ) const noexcept;
            size_t operator()( const BufferKey& key ) const noexcept;
        };
        mutable std::unordered_map<TextureKey, RDG::MemoryRequirements, KeyHash> m_TextureCache;
        mutable std::unordered_map<BufferKey, RDG::MemoryRequirements, KeyHash>  m_BufferCache;
    };

    class VulkanRdgBackend final : public RDG::IBackend
    {
    public:
        VulkanRdgBackend( const VulkanRdgDevice& device, VulkanRdgPool& pool );
        ~VulkanRdgBackend() override;
        VulkanRdgBackend( const VulkanRdgBackend& )            = delete;
        VulkanRdgBackend& operator=( const VulkanRdgBackend& ) = delete;

        // What an exec lambda records with, and what its bindings are. Refused for a context or binding
        // that does not come from this backend.
        static Common::ResultStr<VkCommandBuffer>   CommandBufferOf( const RDG::PassContext& context );
        static Common::ResultStr<VulkanRdgTexture*> TextureOf( const RDG::TextureBinding& binding );
        static Common::ResultStr<VulkanRdgBuffer*>  BufferOf( const RDG::BufferBinding& binding );
        // The command buffer the open segment (or the graph tail) records into; VK_NULL_HANDLE outside a
        // graph. For a backend that forwards to this one (its contexts are not this backend's).
        // On a worker recording a graph chunk: that chunk's primary.
        VkCommandBuffer GetRecordingCommandBuffer() const
        {
            return RecordingBuffer();
        }
        // The engine's command recorder (VulkanRendererAPI, the RHI command list every renderer's Record()
        // goes through) records a pass into the command buffer of the segment the pass runs on, as a pass
        // context would: it is told the backend's recording command buffer each time that changes (a
        // segment begins or ends, the graph begins, is abandoned or hands its submissions over).
        // CPU-FRAME-6: told at every pass's begin and end, before anything the backend records there: the bound
        // state of the recording command buffer is not known across a pass exec (it may record through anything,
        // ImGui or NGX), so the engine recorder forgets its bind cache (VulkanRendererAPI::m_BoundGraphics).
        using PassBoundaryListener = std::function<void()>;
        void SetPassBoundaryListener( PassBoundaryListener listener )
        {
            m_PassBoundaryListener = std::move( listener );
        }
        using RecordingListener = std::function<void( VkCommandBuffer )>;
        void SetRecordingListener( RecordingListener listener )
        {
            m_RecordingListener = std::move( listener );
        }

        RDG::BackendKind GetKind() const override
        {
            return RDG::BackendKind::Vulkan;
        }
        const RDG::IMemoryRequirementsProvider& GetMemoryRequirements() const override
        {
            return m_Memory;
        }
        Common::BoolResultStr BeginGraph( const RDG::GraphView& graph ) override;
        // Resolves the render pass, framebuffer and attachment views of every pass that opens a render pass,
        // by CompiledPass::Pass; BeginRenderPass of that planned pass then only records.
        void                  PrepareRecording( std::span<const RDG::CompiledPass> passes ) override;
        void                  BeginPass( const RDG::CompiledPass& pass ) override;
        void                  RecordBarriers( std::span<const RDG::Barrier> barriers ) override;
        Common::BoolResultStr BeginRenderPass( const RDG::CompiledPass& pass ) override;
        void                  EndRenderPass() override;
        void                  EndPass( const RDG::CompiledPass& pass ) override;
        Common::BoolResultStr EndGraph( std::span<const RDG::Barrier> finalBarriers ) override;
        void                  AbandonGraph() override;
        // vkCmdUpdateBuffer in chunks of 64 KiB (its per-call limit), outside any render pass.
        Common::BoolResultStr UploadBuffer( uint32_t resource, std::span<const std::byte> bytes ) override;

        // The compatibility key of the render pass this backend has open on its command buffer, empty between
        // passes. Pipelines bound by an exec lambda are resolved against it
        // (VulkanRendererAPI::BindGraphicsPipeline).
        // On a worker recording a graph chunk: the chunk's.
        const std::optional<RdgRenderPassKey>& GetOpenRenderPass() const
        {
            const Chunk* const chunk = BoundChunk();
            return chunk != nullptr ? chunk->OpenRenderPass : m_OpenRenderPass;
        }

        std::shared_ptr<RDG::IPhysicalTexture> GetPhysicalTexture( uint32_t resource ) const override;
        std::shared_ptr<RDG::IPhysicalBuffer>  GetPhysicalBuffer( uint32_t resource ) const override;

        // ── RDG-CONTRACTS additions ──────────────────────────────────────────────────────────────────────
        // A(1) / B(3): what the backend records into for the frame in @p slot, set by the frame loop after the
        // slot's fence wait and before Execute. Replaces SetCommandBuffer: the backend allocates one command
        // buffer per PipeSegment from @p queues' per-slot pools and begins / ends it itself.
        Common::BoolResultStr BeginFrame( uint32_t slot, const VulkanRdgQueueSet& queues,
                                          VulkanRdgTransientAllocator& transients,
                                          VulkanRdgPassDescriptors&    descriptors );
        // B(3): the submissions of the last Execute, in submission order; the caller submits and clears them.
        std::vector<VulkanRdgSubmission> TakeSubmissions();
        // A(3): the per-frame descriptor sets an exec lambda binds transients with.
        static Common::ResultStr<VulkanRdgPassDescriptors*> DescriptorsOf( const RDG::PassContext& context );
        // The memo of the sets the running pass exec wrote (VulkanRdgPassSetMemo); cleared by BeginPass / EndPass.
        static Common::ResultStr<VulkanRdgPassSetMemo*> SetMemoOf( const RDG::PassContext& context );

        // The allocator BeginFrame bound: every Execute follows a BeginFrame, so a call before the first one
        // is a programming error and aborts.
        RDG::ITransientAllocator&     GetTransientAllocator() override;
        RDG::PipeCapabilities         GetPipeCapabilities() const override;
        RDG::AsyncComputeFallbackLog& GetAsyncComputeFallbackLog() override;
        RDG::PassFaultReporter&       GetPassFaultReporter() override;
        // RDG-RECORD-COMPILE-CACHE: nullptr while RenderConfig::RdgCompileCache is off.
        RDG::CompileCache*            GetCompileCache() override;
        Common::BoolResultStr         BeginPipeSegment( const RDG::PipeSegment& segment ) override;
        Common::BoolResultStr         EndPipeSegment( const RDG::PipeSegment& segment ) override;
        // Vulkan 1.0 queue family ownership transfer: a VkImage/BufferMemoryBarrier with srcQueueFamilyIndex =
        // the family of SrcPipe and dstQueueFamilyIndex = that of DstPipe, recorded on the SrcPipe command buffer
        // (release, dstAccessMask 0) and again on the DstPipe one (acquire, srcAccessMask 0).
        void RecordEpilogueBarriers( std::span<const RDG::Barrier> barriers ) override;

        // ── RDG-PARALLEL-3: chunks recorded on task workers (UE FRDGBuilder parallel execute) ───────────────
        // Each chunk records into ONE primary command buffer of its own arena (VulkanRdgQueueObjects::
        // BeginChunkCommandBuffer) with its own recording context, pass state, labels and descriptor arena, its
        // render passes begun inline exactly as on the calling thread (a chunk never splits a render pass:
        // RDGBuilder cuts only between render pass instances). RDG-PARALLEL-10 (UE: a parallel-translated
        // command list is a primary payload, submitted in order): the calling thread splices it into the open
        // segment's submission in pass order (VulkanRdgSegmentRecorder::SpliceChunk), no vkCmdExecuteCommands.
        [[nodiscard]] bool SupportsParallelRecording() const override
        {
            return true;
        }
        Common::BoolResultStr BeginChunks( std::span<const RDG::RecordChunkDesc> chunks ) override;
        void                  BeginChunk( uint32_t chunk ) override;
        void                  EndChunk( uint32_t chunk ) override;
        Common::BoolResultStr ExecuteChunk( uint32_t chunk ) override;
        void                  DiscardChunk( uint32_t chunk ) override;
        void                  EndChunks() override;

    private:
        struct PreparedRenderPass; // below
        // Everything one chunk keeps while its worker records it: what the frame's state holds for the calling
        // thread (m_CommandBuffer -> Record.CommandBuffer, m_OpenRenderPass, m_PassSets, m_ProfilerScopes,
        // m_CpuScopes), and the debug labels its passes opened (EndChunk closes what a failed pass left open).
        struct Chunk
        {
            const VulkanRdgBackend*         Owner = nullptr;
            RDG::RecordChunkDesc            Desc;
            uint32_t                        Arena = 0; // its chunk arena on Desc.OnPipe; descriptors: Arena + 1
            VulkanRecordContext             Record;
            std::optional<RdgRenderPassKey> OpenRenderPass;
            VulkanRdgPassSetMemo            PassSets;
            std::vector<int32_t>            ProfilerScopes;
#if DESERT_DEV_INSTRUMENTS
            std::vector<std::unique_ptr<Common::Profiling::ScopedTimer>> CpuScopes;
#endif
            std::vector<const char*> Labels; // pass names (CompileResult outlives the chunk)
            // Its primary from BeginChunk until ExecuteChunk splices it (Record.CommandBuffer is nulled by a device
            // loss; this one is still ended by EndChunk).
            VkCommandBuffer CommandBuffer = VK_NULL_HANDLE;
            std::string     Failure; // its buffer could not be begun or ended: ExecuteChunk ends the frame with it
            // What the worker had bound before BeginChunk (a worker may record a chunk inside another's wait).
            Chunk*               PreviousChunk   = nullptr;
            VulkanRecordContext* PreviousContext = nullptr;
            uint32_t             PreviousArena   = 0;
        };
        // The chunk bound to the calling thread by BeginChunk, when it is this backend's; else null.
        [[nodiscard]] Chunk* BoundChunk() const noexcept
        {
            return t_Chunk != nullptr && t_Chunk->Owner == this ? t_Chunk : nullptr;
        }
        [[nodiscard]] VkCommandBuffer RecordingBuffer() const noexcept
        {
            const Chunk* const chunk = BoundChunk();
            return chunk != nullptr ? chunk->Record.CommandBuffer : m_CommandBuffer;
        }
        [[nodiscard]] std::optional<RdgRenderPassKey>& OpenRenderPassState() noexcept
        {
            Chunk* const chunk = BoundChunk();
            return chunk != nullptr ? chunk->OpenRenderPass : m_OpenRenderPass;
        }
        [[nodiscard]] VulkanRdgPassSetMemo& PassSetsState() noexcept
        {
            Chunk* const chunk = BoundChunk();
            return chunk != nullptr ? chunk->PassSets : m_PassSets;
        }
        [[nodiscard]] std::vector<int32_t>& ProfilerScopesState() noexcept
        {
            Chunk* const chunk = BoundChunk();
            return chunk != nullptr ? chunk->ProfilerScopes : m_ProfilerScopes;
        }
        std::vector<std::unique_ptr<Chunk>> m_Chunks; // reused across graphs; the first m_ChunkCount are this graph's
        uint32_t                            m_ChunkCount = 0;
        static inline thread_local Chunk*   t_Chunk      = nullptr;

        struct FramebufferEntry
        {
            VkRenderPass                                 RenderPass = VK_NULL_HANDLE;
            std::vector<VkImageView>                     Views;
            std::vector<std::weak_ptr<VulkanRdgTexture>> Textures; // dead once any of them is destroyed
            VkExtent2D                                   Extent{};
            uint32_t                                     Layers      = 1;
            VkFramebuffer                                Framebuffer = VK_NULL_HANDLE;
        };

        // What vkCmdBeginRenderPass of one pass needs (PrepareRecording / BeginRenderPass).
        struct PreparedRenderPass
        {
            // The planned pass it was resolved for, null when not prepared: a fault-adjusted copy of the pass
            // has another address and resolves again.
            const RDG::CompiledPass*  Source      = nullptr;
            VkRenderPass              RenderPass  = VK_NULL_HANDLE;
            VkFramebuffer             Framebuffer = VK_NULL_HANDLE;
            VkExtent2D                Extent{};
            std::vector<VkClearValue> Clears;
            RdgRenderPassKey          Compatibility; // becomes m_OpenRenderPass
        };

        void                            Release();
        Common::ResultStr<VkRenderPass> GetRenderPass( const RdgRenderPassKey& key );
        // Finds or creates the render pass, framebuffer and attachment views of @p pass (m_RenderPasses,
        // m_Framebuffers): on the calling thread alone, or under m_RenderPassLock.
        Common::BoolResultStr ResolveRenderPass( const RDG::CompiledPass& pass, PreparedRenderPass& out );
        // vkCmdBeginRenderPass, the open render pass key, and the whole-target viewport and scissor.
        void RecordBeginRenderPass( const PreparedRenderPass& prepared );

        const VulkanRdgDevice&      m_Device;
        VulkanRdgPool&              m_Pool;
        VulkanRdgMemoryRequirements m_Memory;
        VkCommandBuffer             m_CommandBuffer = VK_NULL_HANDLE; // the open segment's (or the graph tail's)
        RecordingListener           m_RecordingListener;
        PassBoundaryListener        m_PassBoundaryListener;
        // Every change of m_CommandBuffer goes through here, so the listener never records into a stale one.
        void                         SetRecording( VkCommandBuffer commandBuffer );
        VulkanRdgSegmentRecorder     m_Segments;    // B(3): segment command buffers, semaphores, submissions
        RDG::AsyncComputeFallbackLog m_FallbackLog; // B(4): the engine logger at Warning
        // RDG-FAULT1: the one place a pass fault is logged; per backend, so it outlives the per-frame graphs.
        RDG::PassFaultReporter m_FaultReporter;
        // RDG-RECORD-COMPILE-CACHE: the compiled plans of this backend's graphs; counts its frames in BeginFrame.
        RDG::CompileCache m_CompileCache;
        // Bound by BeginFrame for the frame being recorded (A1: transients and descriptors; B2: queues).
        uint32_t                     m_FrameSlot   = 0;
        const VulkanRdgQueueSet*     m_Queues      = nullptr;
        VulkanRdgTransientAllocator* m_Transients  = nullptr;
        VulkanRdgPassDescriptors*    m_Descriptors = nullptr;
        VulkanRdgPassSetMemo         m_PassSets; // the running pass exec's (BeginPass .. EndPass)
        std::string                  m_GraphName; // the graph between BeginGraph and EndGraph / AbandonGraph
        // The graph holds the shared transient heaps after an earlier graph
        // (VulkanRdgTransientAllocator::ReserveGraphHeaps): its first command buffer opens with a memory barrier.
        bool m_SharedHeapBarrierPending = false;
        void RecordSharedHeapBarrier( VkCommandBuffer commandBuffer );
        // A GPU row around the graph's commands outside every pass ("RDG Barriers: <graph> | <what>"); -1 when
        // there is no profiler. EndGraphCommandsScope ignores -1.
        int32_t BeginGraphCommandsScope( std::string_view what );
        void    EndGraphCommandsScope( int32_t handle );

        std::vector<std::shared_ptr<VulkanRdgTexture>> m_Textures; // by resource index, this graph only
        std::vector<std::shared_ptr<VulkanRdgBuffer>>  m_Buffers;
        std::vector<int32_t>                           m_ProfilerScopes; // open scope per nesting level
        // The GPU row of the AsyncCompute segment recording now ("RDG Async: <first> .. <last>"), -1 when none: the
        // segment's span on the compute queue, beside the per-pass rows, so its overlap with the graphics rows
        // between its fork and join is readable (and its sum against "GPU: Frame Total" is not a tiling).
        const RDG::CompileResult* m_Result = nullptr; // the graph between BeginGraph and EndGraph / AbandonGraph
        int32_t                   m_SegmentScope = -1;
        // The prologue segment's row (fork releases at the graph start), -1 when none is open.
        int32_t     m_PrologueScope = -1;
        std::string m_SegmentScopeName;
        // THE GRAPHICS QUEUE BETWEEN TWO OF ITS SUBMISSIONS (UE "stat gpu": a queue's Wait row beside its Busy
        // rows), -1 when none is open: opened at the end of a graphics segment that another graphics command
        // buffer of the graph follows, closed as that buffer starts recording. Its span is the graphics queue idle
        // across the submission boundary - the semaphore wait on an async run's join when the next buffer waits on
        // one, else the resubmission after a fork - which is otherwise the largest unnamed stretch of the graphics
        // timeline.
        int32_t     m_QueueBoundaryScope = -1;
        std::string m_QueueBoundaryScopeName;
        void        OpenQueueBoundaryScope( const RDG::PipeSegment& ending );
        void        CloseQueueBoundaryScope();
        // Per graph name, the STRUCTURE of the async segments last logged (BeginGraph): each segment's first and
        // last position and the positions its signal is waited at. A new "[RDG] ... async compute segments" line
        // only when that structure changes - never because a pass name carries this frame's counts.
        std::map<std::string, std::vector<uint32_t>, std::less<>> m_AsyncSegmentsLogged;
#if DESERT_DEV_INSTRUMENTS
        std::vector<std::unique_ptr<Common::Profiling::ScopedTimer>> m_CpuScopes; // CPU row of the same pass
#endif
        std::optional<RdgRenderPassKey>          m_OpenRenderPass;
        std::map<RdgRenderPassKey, VkRenderPass> m_RenderPasses;
        std::vector<FramebufferEntry>            m_Framebuffers;
        // By CompiledPass::Pass, this graph only (PrepareRecording; Release clears it).
        std::vector<PreparedRenderPass> m_PreparedRenderPasses;
        // A render pass resolved while passes record (BeginRenderPass of an unprepared pass).
        std::mutex m_RenderPassLock;
    };
} // namespace Desert::Graphic::API::Vulkan
