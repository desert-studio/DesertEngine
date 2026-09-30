#pragma once

#include <Common/Core/Profiler.hpp>
#include <Common/Core/ResultStr.hpp>

#include <Engine/Graphic/API/Vulkan/VulkanRdgQueues.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>

#include <VulkanAllocator/vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <compare>
#include <functional>
#include <map>
#include <optional>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

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

    struct VulkanRdgDevice
    {
        VkDevice     Device    = VK_NULL_HANDLE;
        VmaAllocator Allocator = nullptr;
        // The packed depth+stencil format this device picked (ImageFormat::DEPTH24STENCIL8).
        VkFormat DepthStencilFormat = VK_FORMAT_UNDEFINED;

        // Debug labels exist only on an instance with VK_EXT_debug_utils; null means no labels.
        PFN_vkCmdBeginDebugUtilsLabelEXT CmdBeginLabel = nullptr;
        PFN_vkCmdEndDebugUtilsLabelEXT   CmdEndLabel   = nullptr;
        // GPU timestamps per pass; null when the build has no GPU profiler (Shipping).
        Common::Profiling::IGpuProfilerSink* Profiler = nullptr;
    };

    // Usage flags a transient is created with, from the graph's derived usage (bit i = 1 << Access i).
    VkImageUsageFlags  RdgImageUsage( uint32_t accessMask );
    VkBufferUsageFlags RdgBufferUsage( uint32_t accessMask );

    // The Vulkan translation of the graph's own state enums; one table each, no second copy.
    VkPipelineStageFlags RdgVulkanStages( RDG::PipelineStageFlags stages );
    VkAccessFlags        RdgVulkanAccess( RDG::MemoryAccessFlags access );
    VkImageLayout        RdgVulkanLayout( RDG::ImageLayout layout );
    // The inverse of RdgVulkanLayout, for an image imported from its own layout record. A layout the graph
    // has no name for is refused rather than mapped to a near neighbour.
    std::optional<RDG::ImageLayout> RdgLayoutFromVulkan( VkImageLayout layout );

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

        RDG::BackendKind GetBackendKind() const override
        {
            return RDG::BackendKind::Vulkan;
        }

        // A view of @p range (kAllRemaining resolved against the description), created once and cached.
        // A range of one layer of a 2D texture is a 2D view; more layers make an array view; the whole of a
        // cube or a volume is a cube / 3D view. An attachment view is always 2D / 2D array.
        Common::ResultStr<VkImageView> GetView( RDG::SubresourceRange range         = RDG::SubresourceRange::All(),
                                                bool                  forAttachment = false );

        VkImage GetImage() const
        {
            return m_Image;
        }
        VkFormat GetFormat() const
        {
            return m_Format;
        }
        VkImageAspectFlags GetAspect() const
        {
            return m_Aspect;
        }
        const RDG::TextureDesc& GetDesc() const
        {
            return m_Desc;
        }
        // The same description and usage: an image created for one can stand in for the other.
        bool     Matches( const RDG::TextureDesc& desc, uint32_t accessMask ) const;
        uint32_t GetAccessMask() const
        {
            return m_AccessMask;
        }

    private:
        VulkanRdgTexture() = default;

        VkDevice           m_Device     = VK_NULL_HANDLE;
        VmaAllocator       m_Allocator  = nullptr; // null for a wrapped image
        VmaAllocation      m_Allocation = nullptr;
        VkImage            m_Image      = VK_NULL_HANDLE;
        VkFormat           m_Format     = VK_FORMAT_UNDEFINED;
        VkImageAspectFlags m_Aspect     = 0;
        RDG::TextureDesc   m_Desc;
        uint32_t           m_AccessMask = 0;
        std::map<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t>, VkImageView> m_Views;
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
        // imports it because its own frames-in-flight release waits for them.
        static std::shared_ptr<VulkanRdgBuffer> Wrap( VkBuffer buffer, uint64_t size );
        ~VulkanRdgBuffer() override;

        RDG::BackendKind GetBackendKind() const override
        {
            return RDG::BackendKind::Vulkan;
        }

        VkBuffer GetBuffer() const
        {
            return m_Buffer;
        }
        uint64_t GetSize() const
        {
            return m_Size;
        }
        uint32_t GetAccessMask() const
        {
            return m_AccessMask;
        }
        // Null unless host-visible. Reading it is only meaningful after the submission's fence.
        const void* GetMapped() const
        {
            return m_Mapped;
        }
        // Makes device writes visible to the host on non-coherent memory; no-op on coherent memory.
        void InvalidateForHost() const;

    private:
        VulkanRdgBuffer() = default;

        VmaAllocator  m_Allocator  = nullptr;
        VmaAllocation m_Allocation = nullptr;
        VkBuffer      m_Buffer     = VK_NULL_HANDLE;
        uint64_t      m_Size       = 0;
        uint32_t      m_AccessMask = 0;
        void*         m_Mapped     = nullptr;
    };

    // Physical resources for transients, WITHOUT aliasing: every transient gets its own image or buffer.
    // An entry is handed out again for the same description and usage, so an unchanged graph gets the same
    // images every frame and whatever descriptors point at them stay valid.
    //
    // Entries are kept per frame slot: a slot's resources are reused only when the caller has waited for
    // that slot's previous frame (the renderer's frames-in-flight fence), so the first barrier of a reused
    // image - from UNDEFINED, waiting on nothing - never races the GPU still reading it from an earlier
    // frame. An entry not handed out during a slot's whole previous frame is destroyed at the next
    // BeginFrame of that slot; an entry still held by an extraction target is never handed out twice.
    class VulkanRdgPool
    {
    public:
        VulkanRdgPool( const VulkanRdgDevice& device, uint32_t frameSlots );

        // The caller has waited for @p slot's previous submission.
        void BeginFrame( uint32_t slot );

        // Keeps @p texture alive until the next BeginFrame of the current slot. The caller has waited for that
        // slot's previous submission by then, and fences are waited in submission order, so every frame that
        // could have recorded the texture's views has completed. The engine image's graph handle goes here when
        // the image is released or resized (IVulkanImage::DropGraphTexture), instead of destroying views a
        // frame in flight may still reference.
        void Retire( std::shared_ptr<RDG::IPhysicalTexture> texture );

        Common::ResultStr<std::shared_ptr<VulkanRdgTexture>>
        AcquireTexture( const RDG::TextureDesc& desc, uint32_t accessMask, std::string_view name );
        Common::ResultStr<std::shared_ptr<VulkanRdgBuffer>>
        AcquireBuffer( const RDG::BufferDesc& desc, uint32_t accessMask, std::string_view name );

        size_t GetTextureCount() const;
        size_t GetBufferCount() const;

    private:
        template <class T>
        struct Entry
        {
            std::shared_ptr<T> Resource;
            uint64_t           LastFrame = 0; // frame counter of the last hand-out
        };
        struct Slot
        {
            std::vector<Entry<VulkanRdgTexture>> Textures;
            std::vector<Entry<VulkanRdgBuffer>>  Buffers;
            std::vector<std::shared_ptr<RDG::IPhysicalTexture>> Retired; // released at the slot's next BeginFrame
            uint64_t                             PreviousFrame = 0; // frame counter of the slot's last BeginFrame
        };

        const VulkanRdgDevice& m_Device;
        std::vector<Slot>      m_Slots;
        uint32_t               m_Slot  = 0;
        uint64_t               m_Frame = 1;
    };

    // RDG-CONTRACTS A(3). Descriptor sets for graph resources, allocated from a pool per frame slot that is reset
    // when the slot is re-begun. The one way a pass binds a transient to a pipeline: allocate a set for the
    // pipeline's layout inside exec, write the pass's bindings into it, bind it, forget it.
    // Never: return a set to a caller outside the exec lambda that allocated it; write a set with a view of a
    // resource the pass did not declare (the binding comes from PassContext::GetTexture, which refuses those).
    class VulkanRdgPassDescriptors
    {
    public:
        VulkanRdgPassDescriptors( VkDevice device, uint32_t frameSlots );
        ~VulkanRdgPassDescriptors();
        VulkanRdgPassDescriptors( const VulkanRdgPassDescriptors& )            = delete;
        VulkanRdgPassDescriptors& operator=( const VulkanRdgPassDescriptors& ) = delete;

        Common::ResultStr<VkDescriptorSet> Allocate( VkDescriptorSetLayout layout );
        // @p type is the layout binding's type: SAMPLED_IMAGE, STORAGE_IMAGE or COMBINED_IMAGE_SAMPLER (with
        // @p sampler) - a layout alone does not say whether an image in GENERAL is sampled or stored.
        Common::BoolResultStr WriteTexture( VkDescriptorSet set, uint32_t binding, VkDescriptorType type,
                                            const RDG::TextureBinding& texture, RDG::SubresourceRange range,
                                            VkImageLayout layout, VkSampler sampler = VK_NULL_HANDLE );
        // @p type: UNIFORM_BUFFER or STORAGE_BUFFER; the whole buffer is bound.
        Common::BoolResultStr WriteBuffer( VkDescriptorSet set, uint32_t binding, VkDescriptorType type,
                                           const RDG::BufferBinding& buffer );
        // The frame loop, after the slot's fence: resets that slot's pool.
        void BeginFrameSlot( uint32_t slot );

    private:
        struct SlotPools
        {
            std::vector<VkDescriptorPool> Pools; // Pools[Current] allocates; a full one is followed by a new one
            size_t                        Current = 0;
        };

        Common::ResultStr<VkDescriptorPool> AddPool( SlotPools& slot );

        VkDevice               m_Device = VK_NULL_HANDLE;
        std::vector<SlotPools> m_Slots; // one per frame slot
        uint32_t               m_Slot = 0;
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
        mutable std::map<std::string, RDG::MemoryRequirements> m_Cache;
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
        VkCommandBuffer GetRecordingCommandBuffer() const
        {
            return m_CommandBuffer;
        }
        // The engine's command recorder (VulkanRendererAPI, the RHI command list every renderer's Record()
        // goes through) records a pass into the command buffer of the segment the pass runs on, as a pass
        // context would: it is told the backend's recording command buffer each time that changes (a
        // segment begins or ends, the graph begins, is abandoned or hands its submissions over).
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
        void                  BeginPass( const RDG::CompiledPass& pass ) override;
        void                  RecordBarriers( std::span<const RDG::Barrier> barriers ) override;
        Common::BoolResultStr BeginRenderPass( const RDG::CompiledPass& pass ) override;
        void                  EndRenderPass() override;
        void                  EndPass( const RDG::CompiledPass& pass ) override;
        Common::BoolResultStr EndGraph( std::span<const RDG::Barrier> finalBarriers ) override;
        void                  AbandonGraph() override;

        // The compatibility key of the render pass this backend has open on its command buffer, empty between
        // passes. Pipelines bound by an exec lambda are resolved against it
        // (VulkanRendererAPI::BindGraphicsPipeline).
        const std::optional<RdgRenderPassKey>& GetOpenRenderPass() const
        {
            return m_OpenRenderPass;
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

        // The allocator BeginFrame bound: every Execute follows a BeginFrame, so a call before the first one
        // is a programming error and aborts.
        RDG::ITransientAllocator&     GetTransientAllocator() override;
        RDG::PipeCapabilities         GetPipeCapabilities() const override;
        RDG::AsyncComputeFallbackLog& GetAsyncComputeFallbackLog() override;
        Common::BoolResultStr         BeginPipeSegment( const RDG::PipeSegment& segment ) override;
        Common::BoolResultStr         EndPipeSegment( const RDG::PipeSegment& segment ) override;
        // Vulkan 1.0 queue family ownership transfer: a VkImage/BufferMemoryBarrier with srcQueueFamilyIndex =
        // the family of SrcPipe and dstQueueFamilyIndex = that of DstPipe, recorded on the SrcPipe command buffer
        // (release, dstAccessMask 0) and again on the DstPipe one (acquire, srcAccessMask 0).
        void RecordEpilogueBarriers( std::span<const RDG::Barrier> barriers ) override;

    private:
        struct FramebufferEntry
        {
            VkRenderPass                                 RenderPass = VK_NULL_HANDLE;
            std::vector<VkImageView>                     Views;
            std::vector<std::weak_ptr<VulkanRdgTexture>> Textures; // dead once any of them is destroyed
            VkExtent2D                                   Extent{};
            uint32_t                                     Layers      = 1;
            VkFramebuffer                                Framebuffer = VK_NULL_HANDLE;
        };

        void                            Release();
        Common::ResultStr<VkRenderPass> GetRenderPass( const RdgRenderPassKey& key );

        const VulkanRdgDevice&      m_Device;
        VulkanRdgPool&              m_Pool;
        VulkanRdgMemoryRequirements m_Memory;
        VkCommandBuffer              m_CommandBuffer = VK_NULL_HANDLE; // the open segment's (or the graph tail's)
        RecordingListener            m_RecordingListener;
        // Every change of m_CommandBuffer goes through here, so the listener never records into a stale one.
        void                         SetRecording( VkCommandBuffer commandBuffer );
        VulkanRdgSegmentRecorder     m_Segments;    // B(3): segment command buffers, semaphores, submissions
        RDG::AsyncComputeFallbackLog m_FallbackLog; // B(4): the engine logger at Warning
        // Bound by BeginFrame for the frame being recorded (A1: transients and descriptors; B2: queues).
        uint32_t                     m_FrameSlot   = 0;
        const VulkanRdgQueueSet*     m_Queues      = nullptr;
        VulkanRdgTransientAllocator* m_Transients  = nullptr;
        VulkanRdgPassDescriptors*    m_Descriptors = nullptr;
        std::string                  m_GraphName; // the graph between BeginGraph and EndGraph / AbandonGraph

        std::vector<std::shared_ptr<VulkanRdgTexture>> m_Textures; // by resource index, this graph only
        std::vector<std::shared_ptr<VulkanRdgBuffer>>  m_Buffers;
        std::vector<int32_t>                           m_ProfilerScopes; // open scope per nesting level
#if DESERT_DEV_INSTRUMENTS
        std::vector<std::unique_ptr<Common::Profiling::ScopedTimer>> m_CpuScopes; // CPU row of the same pass
#endif
        std::optional<RdgRenderPassKey>          m_OpenRenderPass;
        std::map<RdgRenderPassKey, VkRenderPass> m_RenderPasses;
        std::vector<FramebufferEntry>            m_Framebuffers;
    };
} // namespace Desert::Graphic::API::Vulkan
