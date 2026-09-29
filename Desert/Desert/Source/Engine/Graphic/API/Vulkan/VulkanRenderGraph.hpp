#pragma once

#include <Common/Core/Profiler.hpp>
#include <Common/Core/ResultStr.hpp>

#include <Engine/Graphic/RDG/RDGBuilder.hpp>

#include <VulkanAllocator/vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <compare>
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
    VkImageUsageFlags  RdgImageUsage( uint32_t accessMask, bool depthFormat );
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
        std::vector<RdgAttachmentKey>   Colours; // by slot
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

    class VulkanRdgBuffer final : public RDG::IPhysicalBuffer
    {
    public:
        // Host-visible (and persistently mapped) when the usage includes HostRead: a readback target.
        static Common::ResultStr<std::shared_ptr<VulkanRdgBuffer>> Create( const VulkanRdgDevice& device,
                                                                           const RDG::BufferDesc& desc,
                                                                           uint32_t               accessMask,
                                                                           std::string_view       name );
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
            uint64_t                             PreviousFrame = 0; // frame counter of the slot's last BeginFrame
        };

        const VulkanRdgDevice& m_Device;
        std::vector<Slot>      m_Slots;
        uint32_t               m_Slot  = 0;
        uint64_t               m_Frame = 1;
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

        // The command buffer the next Execute records into (begun by the caller, submitted by the caller).
        void SetCommandBuffer( VkCommandBuffer commandBuffer )
        {
            m_CommandBuffer = commandBuffer;
        }

        // What an exec lambda records with, and what its bindings are. Refused for a context or binding
        // that does not come from this backend.
        static Common::ResultStr<VkCommandBuffer>   CommandBufferOf( const RDG::PassContext& context );
        static Common::ResultStr<VulkanRdgTexture*> TextureOf( const RDG::TextureBinding& binding );
        static Common::ResultStr<VulkanRdgBuffer*>  BufferOf( const RDG::BufferBinding& binding );

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

        std::shared_ptr<RDG::IPhysicalTexture> GetPhysicalTexture( uint32_t resource ) const override;
        std::shared_ptr<RDG::IPhysicalBuffer>  GetPhysicalBuffer( uint32_t resource ) const override;

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
        VkCommandBuffer             m_CommandBuffer = VK_NULL_HANDLE;

        std::vector<std::shared_ptr<VulkanRdgTexture>> m_Textures; // by resource index, this graph only
        std::vector<std::shared_ptr<VulkanRdgBuffer>>  m_Buffers;
        std::vector<int32_t>                           m_ProfilerScopes; // open scope per nesting level
#if DESERT_DEV_INSTRUMENTS
        std::vector<std::unique_ptr<Common::Profiling::ScopedTimer>> m_CpuScopes; // CPU row of the same pass
#endif
        bool                                     m_RenderPassOpen = false;
        std::map<RdgRenderPassKey, VkRenderPass> m_RenderPasses;
        std::vector<FramebufferEntry>            m_Framebuffers;
    };
} // namespace Desert::Graphic::API::Vulkan
