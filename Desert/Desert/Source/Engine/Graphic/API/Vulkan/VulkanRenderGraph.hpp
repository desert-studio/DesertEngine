#pragma once

#include <Common/Core/Memory/FrameArena.hpp>
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
#include <source_location>
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

    // GPU CRASH BREADCRUMBS (FPS-GPU-MARKER; UE FGPUBreadcrumbs / r.GPUCrashDebugging, NVIDIA Aftermath markers).
    // One host-visible, host-coherent buffer of two words per queue (graphics, async compute): every graph pass
    // writes its serial into its queue's STARTED word at TOP_OF_PIPE before its commands and into the FINISHED
    // word at BOTTOM_OF_PIPE after them (vkCmdWriteBufferMarkerAMD: legal inside a render pass and in a secondary
    // buffer, unlike vkCmdFillBuffer). Serials are the graph's base + CompiledPass::Pass, so on each queue they
    // grow in GPU order (graphs submit in BeginGraph order, passes are never reordered), whatever thread records.
    // A ring of kRingSize names maps a serial back to its pass. On a loss the [DeviceLost] report (DeviceLost::
    // SetBreadcrumbReader -> Read) prints per queue the last pass started, the last finished, and the passes in
    // between - the faulting pass is among them. Disabled (RenderPipeline.GpuCrashBreadcrumbs 0): no marker.
    class VulkanGpuBreadcrumbs
    {
    public:
        static constexpr uint32_t kRingSize  = 8192; // power of two; > the passes of the frames in flight
        static constexpr uint32_t kNameBytes = 64;
        static constexpr uint32_t kNoSerial  = 0;
        // FPS-DEVLOST8: the device queues a one-off submit's words are kept for (graphics, compute, transfer).
        static constexpr uint32_t kOneOffQueues = 3;

        VulkanGpuBreadcrumbs() = default;
        ~VulkanGpuBreadcrumbs();
        VulkanGpuBreadcrumbs( const VulkanGpuBreadcrumbs& )            = delete;
        VulkanGpuBreadcrumbs& operator=( const VulkanGpuBreadcrumbs& ) = delete;

        // FPS-DEVLOST4: which of the device's routes the breadcrumbs take (each its DeviceCaps row, enabled).
        struct Routes
        {
            bool BufferMarker = false; // VK_AMD_buffer_marker: the per-queue words
            bool HostImport   = false; // VK_EXT_external_memory_host: the words in application-owned memory
            bool Checkpoints  = false; // VK_NV_device_diagnostic_checkpoints: the driver's per-queue record
        };
        // At least one of BufferMarker / Checkpoints. A failed host import is not an error: the words go to a
        // driver-owned host-coherent allocation and WordsHome() says so (and why) - logged once by the caller.
        Common::BoolResultStr Init( VkPhysicalDevice physicalDevice, VkDevice device, VmaAllocator allocator,
                                    const VulkanResourceSharing& sharing, const Routes& routes ) noexcept;
        // The graph's queues (compute is VK_NULL_HANDLE when the async pipe runs on the graphics queue) and, for
        // the one-off submits and the checkpoint report (FPS-DEVLOST8: every queue anything is submitted to), the
        // device's compute and transfer queues (either may equal another queue: reported once). Before the first
        // frame, on the frame thread.
        void SetQueues( VkQueue graphics, VkQueue compute, VkQueue deviceCompute, VkQueue transfer ) noexcept;
        void SetEnabled( bool enabled ) noexcept;
        // FPS-DEVLOST4: once per frame after the slot fence. Says ONCE that the GPU's words reached the host on a
        // healthy run (the tool works) - or, kLiveProbeFrames frames after enabling with every word still 0, that
        // they never do (the tool is broken on this device, and a loss report's "(none yet)" means nothing).
        void ObserveLive() noexcept;
        // Where the words live, for the startup line and the report.
        [[nodiscard]] const std::string& WordsHome() const noexcept
        {
            return m_WordsHome;
        }
        // Calling thread, before any pass of the graph records: reserves the graph's serials and names its passes.
        // The graph's base serial, or kNoSerial when disabled (then the graph records no marker).
        [[nodiscard]] uint32_t BeginGraph( std::span<const RDG::CompiledPass> passes ) noexcept;
        void MarkStarted( VkCommandBuffer commandBuffer, RDG::Pipe pipe, uint32_t serial ) const noexcept;
        void MarkFinished( VkCommandBuffer commandBuffer, RDG::Pipe pipe, uint32_t serial ) const noexcept;
        // FPS-DEVLOST6, ONE-OFF SUBMITS (UE: a breadcrumb on every queue submit, not only graph passes): an upload
        // or one-shot flush recorded outside any graph (CommandBufferAllocator::OneOffMarker; context is the
        // VulkanGpuBreadcrumbs), called just before its vkEndCommandBuffer, any thread. Reserves a serial named
        // "one-off <file>:<line>" (@p where: the caller) and records, after the buffer's commands, a TOP_OF_PIPE
        // marker (the front end reached the end of the buffer) and a BOTTOM_OF_PIPE one (its commands completed)
        // into the submit words of @p queue - graphics, the device's compute or its transfer queue (FPS-DEVLOST8;
        // the words buffer is CONCURRENT over all three families, VulkanResourceSharing) - plus the NV
        // checkpoints. Their own words: a one-off is not in the graphs' serial order on its queue.
        static void MarkOneOff( void* context, VkCommandBuffer commandBuffer, VkQueue queue,
                                const std::source_location& where ) noexcept;
        // The report, from the host words and vkGetQueueCheckpointDataNV (serial << 1 | finished per checkpoint;
        // the one device call it makes, legal on a lost device - that is what the extension is for).
        [[nodiscard]] std::string Describe() const;
        // DeviceLost::BreadcrumbReader: context is the VulkanGpuBreadcrumbs.
        [[nodiscard]] static std::string Read( const void* context );

    private:
        struct Entry
        {
            std::atomic<uint32_t> Serial{ kNoSerial }; // published after OnPipe and Name
            RDG::Pipe             OnPipe = RDG::Pipe::Graphics;
            char                  Name[kNameBytes]{};
            bool                  OneOff = false; // FPS-DEVLOST6: a one-off submit's serial, not a graph pass's
        };
        [[nodiscard]] std::string NameOf( uint32_t serial, RDG::Pipe& pipe ) const;

        [[nodiscard]] Common::BoolResultStr MakeWordsImported( VkPhysicalDevice          physicalDevice,
                                                               const VkBufferCreateInfo& info ) noexcept;
        [[nodiscard]] Common::BoolResultStr MakeWordsAllocated( const VkBufferCreateInfo& info ) noexcept;
        [[nodiscard]] std::string           DescribeCheckpoints() const;

        static constexpr uint32_t kLiveProbeFrames = 300;

        VkDevice                       m_Device         = VK_NULL_HANDLE;
        VmaAllocator                   m_Allocator      = nullptr;
        VkBuffer                       m_Buffer         = VK_NULL_HANDLE;
        VmaAllocation                  m_Allocation     = nullptr;        // the VMA route
        VkDeviceMemory                 m_HostMemory     = VK_NULL_HANDLE; // the host-import route
        void*                          m_HostBlock      = nullptr;        // its application-owned block
        const volatile uint32_t*       m_Words          = nullptr; // [queue * 2 + 0] started, [+ 1] finished
        std::string                    m_WordsHome      = "none (no VK_AMD_buffer_marker)";
        PFN_vkCmdWriteBufferMarkerAMD  m_WriteMarker    = nullptr;
        PFN_vkCmdSetCheckpointNV       m_SetCheckpoint  = nullptr;
        PFN_vkGetQueueCheckpointDataNV m_GetCheckpoints = nullptr;
        VkQueue                        m_GraphicsQueue  = VK_NULL_HANDLE;
        VkQueue                        m_ComputeQueue   = VK_NULL_HANDLE;
        // FPS-DEVLOST8: the one-off words' and the checkpoint report's queues: [0] graphics, [1] the device's
        // compute, [2] its transfer; VK_NULL_HANDLE where it is the same queue as an earlier one.
        std::array<VkQueue, kOneOffQueues> m_OneOffQueues{};
        // FPS-DEVLOST6: [first, first + count) of the serials, never through kNoSerial; any thread (graphs and
        // one-off submits).
        [[nodiscard]] uint32_t ReserveSerials( uint32_t count ) noexcept;

        std::atomic<bool>              m_Enabled{ false };
        std::atomic<bool>              m_EverEnabled{ false };
        std::atomic<uint32_t>          m_NextSerial{ 1 };
        uint32_t                       m_LiveFrames  = 0; // ObserveLive's thread only
        bool                           m_LiveSaid    = false;
        std::unique_ptr<Entry[]>       m_Ring;
    };

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
        // FPS-GPU-MARKER: the pass breadcrumbs; null without VK_AMD_buffer_marker (and in a device suite).
        VulkanGpuBreadcrumbs* Breadcrumbs = nullptr;
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
        // FPS-SET-MEMO: a cached entry's last-bindable frame (m_CacheFrame), shared with every holder of the set
        // outside the cache (the bound-sets memo below). kStampFreed once EvictCached freed the set: a holder
        // that finds it so never binds the set again. Touch (a bind) and eviction race only through this word
        // (compare-exchange), so a set is either refreshed for this frame or freed, never both.
        using CachedStamp = std::shared_ptr<std::atomic<uint64_t>>;
        static constexpr uint64_t kStampFreed = ~uint64_t{ 0 };
        // Marks @p stamp's set bindable in @p frame; false when the cache already freed it.
        [[nodiscard]] static bool TouchCached( const CachedStamp& stamp, uint64_t frame ) noexcept;

        struct CachedSet
        {
            VkDescriptorSet  Set  = VK_NULL_HANDLE;
            VkDescriptorPool Pool = VK_NULL_HANDLE;
            bool             Hit  = false; // written before: bind it as it is and write nothing into it
            CachedStamp      Stamp;        // a hit's entry stamp (null for a fresh set until PublishCached)
        };
        Common::ResultStr<CachedSet> AcquireCached( VkDescriptorSetLayout layout, std::span<const uint64_t> key );
        // After the WriteBatch that wrote @p fresh (Hit false) has flushed: makes it findable under @p key and
        // returns its entry's stamp. When another thread published an equal key first, that entry stays, @p fresh
        // is freed once no frame in flight can bind it, and the stamp is null (the set must not outlive the exec).
        CachedStamp PublishCached( std::span<const uint64_t> key, const CachedSet& fresh );

        // FPS-SET-MEMO (UE FMeshDrawCommand / FMeshDrawShaderBindings: a draw's bindings are built when their
        // inputs change and every later frame only binds them). THE BOUND-SETS MEMO: the sets one graphics
        // draw combination resolved to, kept ACROSS frames under the full content of what decided them - the
        // shader (address and code generation), the pipeline and set layouts, the bindless table's set, the
        // material part (set, copy record identity and version, copied bindings, push route) and the block's
        // signature (per entry: name hash, slot, kind, access, the physical identity and range or sampler it
        // names; VulkanRendererAPI::BindGraphicsPassState builds it). The pass set memo (VulkanRdgPassSetMemo)
        // lives one pass exec, so before this every frame resolved, keyed and looked up every combination of
        // every pass again (~150 "set memo miss" per frame on FIELD_Barn) for sets the content cache already
        // held. Every kept set is a content-cache entry, held by its stamp: a hit touches each stamp, and a set
        // the cache freed (unused for the frames in flight) makes the entry a miss and drops it. Equal keys mean
        // the resolution and the writes would produce exactly these sets, so a hit binds what a miss would.
        // Thread-safe (sharded locks): chunk workers look up and keep concurrently.
        // @p key -> the kept sets into @p sets; false (and @p sets untouched) when not kept or no longer live.
        [[nodiscard]] bool FindBound( std::span<const uint64_t> key, std::vector<VkDescriptorSet>& sets );
        // Keeps @p sets (each held by its @p stamps entry; null for the bindless table's set, which is in the
        // key) under @p key.
        void KeepBound( std::span<const uint64_t> key, std::span<const VkDescriptorSet> sets,
                        std::span<const CachedStamp> stamps );
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
            // FPS-CPU2-VKFRAME: a set was allocated from Pools[0 .. Current] since the last reset. Pools past
            // Current and the pools of an arena no chunk drew from hold nothing: resetting them changes nothing.
            bool Used = false;
        };

        Common::ResultStr<VkDescriptorPool> AddPool( SlotPools& slot );
        // One pool shape for every pool of this object; @p flags: FREE_DESCRIPTOR_SET for the cache's pools.
        Common::ResultStr<VkDescriptorPool> CreatePool( VkDescriptorPoolCreateFlags flags );
        // BeginFrameSlot: frees the cached and retired sets last bound before the oldest frame in flight.
        void EvictCached();

        struct CachedSetEntry
        {
            std::vector<uint64_t> Key;
            VkDescriptorSet       Set  = VK_NULL_HANDLE;
            VkDescriptorPool      Pool = VK_NULL_HANDLE;
            CachedStamp           Stamp; // m_CacheFrame of the last exec that bound it (kStampFreed: freed)
        };
        // FPS-SET-MEMO: one bound-sets memo entry (FindBound / KeepBound).
        struct BoundSetsEntry
        {
            std::vector<uint64_t>        Key;
            std::vector<VkDescriptorSet> Sets;
            std::vector<CachedStamp>     Stamps;
            uint64_t                     LastHit = 0; // m_CacheFrame of the last FindBound / KeepBound
        };
        struct BoundSetsShard
        {
            std::mutex                                                Lock;
            std::unordered_map<uint64_t, std::vector<BoundSetsEntry>> Buckets; // by the key's hash
        };
        // EvictCached: drops the entries no draw found for this many frames (their sets age out of the cache).
        static constexpr uint64_t kBoundSetsRetainFrames = 8;
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
        std::array<BoundSetsShard, kCachedSetShards> m_BoundShards; // FPS-SET-MEMO
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
    // came from is reset with the frame slot. (The key names the block by its pass index, which restarts with
    // every graph: a memo kept past EndPass could hand graph B's pass 3 the sets of graph A's pass 3. Reuse ACROSS
    // passes of equal content is the descriptor cache's (WriteRdgPassSets), keyed by what the sets hold.)
    // CPU-DRAW-BIND (UE FMeshDrawCommandStateCache: O(1) per draw, nothing allocated): an open-addressing table
    // over flat entry / binding / set arrays. Clear() empties the arrays (capacity kept) and bumps the table's
    // generation, so a pass boundary frees nothing and touches no slot; a lookup hashes the key once. One memo per
    // recording context (the backend's own, each chunk's), never shared between threads.
    class VulkanRdgPassSetMemo
    {
    public:
        // What the sets were written from. Block: the pass's declared block (RDG::PassBindings::GetBlock); Shader
        // and Layout: the recording pipeline's (the reflection the block was resolved against, the set layouts the
        // sets were allocated for); MaterialSet / MaterialBindings: the material set whose written slots were
        // copied into set 0 (null / empty when nothing is copied); MaterialPush and BlockPushBytes: the
        // push-constant routes the resolution checked. Shader is an address compared as an identity, never
        // dereferenced. FPS-CPU-DEBT-NEXT (CPU-DEBT-OPEN #1; UE FMeshDrawShaderBindings keyed by what the bindings
        // hold, not by the material): the material's identity is NOT part of the key. All the sets read of a
        // material is its set's slots (MaterialSet + MaterialBindings, constant while the pass records: the
        // prepare wrote them before the graph recorded) and whether it pushes (MaterialPush); two materials with
        // no copied slots and the same push route resolve and write the same sets, so their draws share one entry.
        // A caller passes MaterialSet null whenever MaterialBindings is empty (nothing of the set is copied).
        struct Key
        {
            RDG::BindingBlockRef      Block;
            std::uintptr_t            Shader         = 0;
            VkPipelineLayout          Layout         = VK_NULL_HANDLE;
            VkDescriptorSet           MaterialSet    = VK_NULL_HANDLE;
            std::span<const uint32_t> MaterialBindings;
            bool                      MaterialPush   = false;
            size_t                    BlockPushBytes = 0;
        };

        // The sets written for @p key in this pass exec (possibly none: a layout with no sets), or nullopt when
        // the combination was not written yet. The span is valid until the next Keep or Clear.
        [[nodiscard]] std::optional<std::span<const VkDescriptorSet>> Find( const Key& key ) const noexcept;
        // The sets just written for @p key (they and its MaterialBindings are copied); returns the kept copy,
        // valid until the next Keep or Clear.
        std::span<const VkDescriptorSet> Keep( const Key& key, std::span<const VkDescriptorSet> sets ) noexcept;
        // A pass exec begins or ends: nothing written for another pass is bound by this one. O(1), frees nothing.
        void Clear() noexcept;

    private:
        struct Entry
        {
            RDG::BindingBlockRef Block;
            std::uintptr_t       Shader         = 0;
            VkPipelineLayout     Layout         = VK_NULL_HANDLE;
            VkDescriptorSet      MaterialSet    = VK_NULL_HANDLE;
            bool                 MaterialPush   = false;
            size_t               BlockPushBytes = 0;
            uint64_t             Hash           = 0;
            uint32_t             BindingsFirst  = 0; // into m_Bindings
            uint32_t             BindingsCount  = 0;
            uint32_t             SetsFirst      = 0; // into m_Sets
            uint32_t             SetsCount      = 0;
        };
        // A table slot: Entry is live only while Generation equals the memo's m_Generation.
        struct Slot
        {
            uint32_t Generation = 0;
            uint32_t Entry      = 0;
        };
        [[nodiscard]] static uint64_t HashOf( const Key& key ) noexcept;
        [[nodiscard]] bool            Matches( const Entry& entry, uint64_t hash, const Key& key ) const noexcept;
        [[nodiscard]] std::span<const VkDescriptorSet> SetsOf( const Entry& entry ) const noexcept;
        void                                           Insert( uint32_t entry ) noexcept; // into m_Slots
        void                                           Grow() noexcept; // doubles m_Slots, re-inserts the live

        std::vector<Entry>           m_Entries;
        std::vector<uint32_t>        m_Bindings; // every entry's MaterialBindings, back to back
        std::vector<VkDescriptorSet> m_Sets;     // every entry's sets, back to back
        std::vector<Slot>            m_Slots;    // power of two (or empty); at most half live
        uint32_t                     m_Generation = 1;
        mutable uint32_t             m_LastHit    = ~0u; // consecutive draws of one block hit the same entry
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

    // FPS-UPLOAD-RING #10 (UE: FRHIUploadBuffer / the per-frame upload ring a QueueBufferUpload is staged in). The
    // bytes of every graph upload (VulkanRdgBackend::UploadBuffer) are copied into a persistently mapped,
    // host-coherent TRANSFER_SRC buffer of the frame slot being recorded, and the upload records a
    // vkCmdCopyBuffer region from there - one copy per destination per pass with all its regions, where
    // vkCmdUpdateBuffer put every byte into the command buffer in 64 KiB pieces.
    //
    // LIFETIME: a slot's blocks are reused only by BeginFrameSlot of the same slot, which the frame loop calls
    // after that slot's fence (VulkanRendererAPI::BeginRdgFrame -> VulkanRdgBackend::BeginFrame); every graph of
    // the frame is submitted with the slot's submissions, so the copies that read a block have finished by then.
    // The host writes are visible to the copy by the queue submission (host-coherent memory, no flush), and the
    // copy is a TRANSFER-stage write, the stage and access vkCmdUpdateBuffer had: the graph's barriers around
    // the upload are unchanged.
    //
    // OVERFLOW: a request that does not fit the slot's current block opens a fresh block (error as value when the
    // allocator refuses one); the next BeginFrameSlot of that slot folds its blocks into one of their total size,
    // so a frame that overflowed once fits in one block from then on.
    class VulkanRdgUploadRing final
    {
    public:
        explicit VulkanRdgUploadRing( const VulkanRdgDevice& device ) : m_Device( device )
        {
        }
        ~VulkanRdgUploadRing();
        VulkanRdgUploadRing( const VulkanRdgUploadRing& )            = delete;
        VulkanRdgUploadRing& operator=( const VulkanRdgUploadRing& ) = delete;

        // Where one upload's bytes go: Mapped (host) is Offset bytes into Buffer (device).
        struct Span
        {
            VkBuffer   Buffer = VK_NULL_HANDLE;
            uint64_t   Offset = 0;
            std::byte* Mapped = nullptr;
        };

        // The slot the next allocations belong to; its earlier blocks are free again (the slot's fence was
        // waited). Not concurrent with Allocate.
        void BeginFrameSlot( uint32_t slot ) noexcept;
        // @p bytes of the current slot's ring. Thread-safe: graph chunks record uploads on task workers.
        [[nodiscard]] Common::ResultStr<Span> Allocate( uint64_t bytes ) noexcept;

    private:
        struct Block
        {
            VmaAllocator      Allocator  = nullptr;
            VkBuffer          Buffer     = VK_NULL_HANDLE;
            VmaAllocation     Allocation = nullptr;
            std::byte*        Mapped     = nullptr;
            uint64_t          Size       = 0;
            ResourceOwnership Ledger;

            Block() = default;
            ~Block();
            Block( const Block& )            = delete;
            Block& operator=( const Block& ) = delete;
        };
        struct Slot
        {
            std::vector<std::unique_ptr<Block>> Blocks;   // the last one is being filled
            uint64_t                            Head = 0; // first free byte of Blocks.back()
            uint64_t                            Want = 0; // the size the next first block is made with
        };
        static constexpr uint64_t kFirstBlockBytes = 1ull << 20;
        static constexpr uint64_t kAlignment       = 16;

        // A mapped host-coherent TRANSFER_SRC block of @p bytes into @p out, or the reason it could not be made.
        [[nodiscard]] Common::BoolResultStr MakeBlock( uint64_t bytes, std::unique_ptr<Block>& out ) const noexcept;

        const VulkanRdgDevice& m_Device;
        std::mutex             m_Lock;
        std::vector<Slot>      m_Slots;
        uint32_t               m_Slot = 0;
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
        // FPS-UPLOAD-RING #10: the bytes are staged in the frame slot's upload ring (VulkanRdgUploadRing) and the
        // copy is queued as one region of the pass's batch for that destination (PendingUpload), recorded by
        // FlushUploads - at the end of the pass, or before any other command this backend records into the pass.
        // Outside any render pass.
        Common::BoolResultStr UploadBuffer( uint32_t resource, uint64_t at,
                                            std::span<const std::byte> bytes ) override;
        Common::BoolResultStr CopyBuffer( uint32_t source, uint64_t sourceOffset, uint32_t target,
                                          uint64_t targetOffset, uint64_t bytes ) override;
        // vkCmdFillBuffer, outside any render pass.
        Common::BoolResultStr FillBuffer( uint32_t resource, uint64_t offset, uint64_t bytes,
                                          uint32_t value ) override;

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
        RDG::IPhysicalTexture*                 FindPhysicalTexture( uint32_t resource ) const override;
        RDG::IPhysicalBuffer*                  FindPhysicalBuffer( uint32_t resource ) const override;

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
        // FPS-UPLOAD-RING #10: one staged upload region not yet recorded. The copies of one pass are unordered
        // against each other (the builder joins only disjoint ranges into one upload pass), so the regions are
        // grouped by (staging block, destination) and each group is ONE vkCmdCopyBuffer.
        struct PendingUpload
        {
            VkBuffer     Source = VK_NULL_HANDLE;
            VkBuffer     Target = VK_NULL_HANDLE;
            VkBufferCopy Region{};
        };
        // Records @p pending into @p commandBuffer (grouped as above) and empties it; a null buffer only empties
        // it (its graph recorded nothing more, as every other command of the pass).
        static void FlushUploads( VkCommandBuffer commandBuffer, std::vector<PendingUpload>& pending ) noexcept;
        // Everything one chunk keeps while its worker records it: what the frame's state holds for the calling
        // thread (m_CommandBuffer -> Record.CommandBuffer, m_OpenRenderPass, m_PassSets, m_ProfilerScopes,
        // m_CpuScope), and the debug labels its passes opened (EndChunk closes what a failed pass left open).
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
            // CPU-RDG-RECORD: its open pass's CPU row, in place (no allocation per pass); passes never nest.
            std::optional<Common::Profiling::ScopedTimer> CpuScope;
#endif
            std::vector<const char*> Labels; // pass names (CompileResult outlives the chunk)
            // FPS-GPU-MARKER: the breadcrumb serial of its open pass (kNoSerial: none) and that pass's pipe.
            uint32_t  OpenCrumb     = VulkanGpuBreadcrumbs::kNoSerial;
            RDG::Pipe OpenCrumbPipe = RDG::Pipe::Graphics;
            // Its primary from BeginChunk until ExecuteChunk splices it (Record.CommandBuffer is nulled by a device
            // loss; this one is still ended by EndChunk).
            VkCommandBuffer CommandBuffer = VK_NULL_HANDLE;
            std::string     Failure; // its buffer could not be begun or ended: ExecuteChunk ends the frame with it
            // What the worker had bound before BeginChunk (a worker may record a chunk inside another's wait).
            Chunk*               PreviousChunk   = nullptr;
            VulkanRecordContext* PreviousContext = nullptr;
            uint32_t             PreviousArena   = 0;
            // CPU-FRAME-ARENA (MEM-7): the barrier and render-pass scratch of its passes (ScratchArena). Reset by
            // BeginChunks; every user takes it under a ScopedMark, so it is empty between two calls.
            Common::Memory::FrameArena Scratch;
            // FPS-UPLOAD-RING: its open pass's staged upload regions (PendingUploadsState); capacity reused.
            std::vector<PendingUpload> Uploads;
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
        // CPU-FRAME-ARENA (MEM-7; UE: FMemStack per thread): the scratch of the calling thread's recording - its
        // chunk's arena on a worker, the backend's own on the thread that records without a chunk (the same split
        // as RecordingBuffer). Taken under a FrameArena::ScopedMark: it is empty again when the taker returns.
        [[nodiscard]] Common::Memory::FrameArena& ScratchArena() noexcept
        {
            Chunk* const chunk = BoundChunk();
            return chunk != nullptr ? chunk->Scratch : m_Scratch;
        }
        [[nodiscard]] std::vector<int32_t>& ProfilerScopesState() noexcept
        {
            Chunk* const chunk = BoundChunk();
            return chunk != nullptr ? chunk->ProfilerScopes : m_ProfilerScopes;
        }
        std::vector<std::unique_ptr<Chunk>> m_Chunks; // reused across graphs; the first m_ChunkCount are this graph's
        uint32_t                            m_ChunkCount = 0;
        Common::Memory::FrameArena          m_Scratch; // ScratchArena of the thread recording without a chunk
        // FPS-UPLOAD-RING: the staged upload regions of the open pass of the calling thread's recording - its
        // chunk's on a worker (the same split as RecordingBuffer).
        [[nodiscard]] std::vector<PendingUpload>& PendingUploadsState() noexcept
        {
            Chunk* const chunk = BoundChunk();
            return chunk != nullptr ? chunk->Uploads : m_Uploads;
        }
        std::vector<PendingUpload> m_Uploads;
        // FPS-GPU-MARKER: this graph's breadcrumb base (kNoSerial: no markers), and the open pass's serial and
        // pipe on the thread recording without a chunk (Chunk::OpenCrumb on a worker).
        uint32_t  m_CrumbBase     = VulkanGpuBreadcrumbs::kNoSerial;
        uint32_t  m_OpenCrumb     = VulkanGpuBreadcrumbs::kNoSerial;
        RDG::Pipe m_OpenCrumbPipe = RDG::Pipe::Graphics;
        static inline thread_local Chunk*   t_Chunk      = nullptr;

        struct FramebufferEntry
        {
            VkRenderPass                                 RenderPass = VK_NULL_HANDLE;
            std::vector<VkImageView>                     Views;
            std::vector<std::weak_ptr<VulkanRdgTexture>> Textures; // dead once any of them is destroyed
            VkExtent2D                                   Extent{};
            uint32_t                                     Layers      = 1;
            VkFramebuffer                                Framebuffer = VK_NULL_HANDLE;

            // A texture of it destroyed by the pool: its views may be reused by a new image.
            [[nodiscard]] bool Dead() const noexcept;
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
        // FPS-UPLOAD-RING: the frame slot's staging for UploadBuffer; BeginFrame turns it to the slot.
        VulkanRdgUploadRing m_UploadRing;
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
        // CPU-RDG-RECORD (F6): a chunk was spliced last and the segment has no buffer recording on this thread
        // (VulkanRdgSegmentRecorder::SpliceChunk). Every calling-thread entry point that records calls
        // ResumeSegment first, which begins the continuation then; a failed begin leaves no buffer recording
        // and is reported by EndPipeSegment (m_ResumeFailure), the passes in between see no command buffer.
        bool        m_ContinuationPending = false;
        std::string m_ResumeFailure;
        void        ResumeSegment();
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
        // Holds the boundary row's end stamp on a join's wait stages (BeginPipeSegment).
        void RecordJoinWaitBarrier( const RDG::PipeSegment& segment, VkCommandBuffer commandBuffer ) noexcept;
        // Per graph name, the STRUCTURE of the async segments last logged (BeginGraph): each segment's first and
        // last position and the positions its signal is waited at. A new "[RDG] ... async compute segments" line
        // only when that structure changes - never because a pass name carries this frame's counts.
        std::map<std::string, std::vector<uint32_t>, std::less<>> m_AsyncSegmentsLogged;
#if DESERT_DEV_INSTRUMENTS
        // CPU row of the open pass, in place (CPU-RDG-RECORD: no allocation per pass); passes never nest.
        std::optional<Common::Profiling::ScopedTimer> m_CpuScope;
#endif
        std::optional<RdgRenderPassKey>          m_OpenRenderPass;
        std::map<RdgRenderPassKey, VkRenderPass> m_RenderPasses;
        // FPS-CPU-DEBT-TAIL (UE FRenderTargetPool / framebuffer cache): by FramebufferHash of (render pass, views,
        // extent, layers); ResolveRenderPass compares the entries of its hash and drops a found dead one, and
        // BeginFrame sweeps the dead ones once per frame (never per graph).
        std::unordered_multimap<uint64_t, FramebufferEntry> m_Framebuffers;
        // By CompiledPass::Pass, this graph only (PrepareRecording; Release clears it).
        std::vector<PreparedRenderPass> m_PreparedRenderPasses;
        // A render pass resolved while passes record (BeginRenderPass of an unprepared pass).
        std::mutex m_RenderPassLock;
    };
} // namespace Desert::Graphic::API::Vulkan
