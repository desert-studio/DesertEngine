#pragma once

#include <Engine/Graphic/API/Vulkan/VulkanDevice.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanContext.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanUtils/VulkanHelper.hpp>
#include <Engine/Graphic/MappedMemory.hpp>

#include <VulkanAllocator/vk_mem_alloc.h>

#include <cstddef>
#include <functional>

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
        uint32_t      FrameIndex;
    };

    struct ImageDeletionEntry
    {
        VkImage                  Image;
        VmaAllocation            Allocation;
        VkImageView              ImageView;
        VkSampler                Sampler;
        std::vector<VkImageView> MipImageViews;
        uint32_t                 FrameIndex;
    };

    struct FramebufferDeletionEntry
    {
        VkFramebuffer Framebuffer;
        uint32_t      FrameIndex;
    };

    struct RenderPassDeletionEntry
    {
        VkRenderPass RenderPass;
        uint32_t     FrameIndex;
    };

    struct DescriptorPoolDeletionEntry
    {
        VkDescriptorPool DescriptorPool;
        uint32_t         FrameIndex;
    };

    class VulkanAllocator
    {
    public:
        ~VulkanAllocator();

        Common::ResultStr<VmaAllocation> RT_AllocateImage( const std::string&       tag,
                                                        const VkImageCreateInfo& imageCreateInfo,
                                                        VmaMemoryUsage usage, VkImage& outImage );

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
        void RT_DestroyImage( VkImage image, VmaAllocation allocation, VkImageView imageView = VK_NULL_HANDLE,
                              VkSampler sampler = VK_NULL_HANDLE,
                              const std::vector<VkImageView>& mipImageViews = {} );
        void RT_DestroyFramebuffer( VkFramebuffer framebuffer );
        void RT_DestroyRenderPass( VkRenderPass renderPass );
        // Takes every set allocated from the pool with it, so it waits for the frame that last bound one of
        // them exactly as a buffer does: a material or a view destroyed mid-session may still have its sets
        // in a command buffer the GPU has not finished.
        void RT_DestroyDescriptorPool( VkDescriptorPool descriptorPool );

        /// The per-frame drain, called once per present: destroys what the ring has come back round to.
        void ProcessDeletionQueue();

        /// Destroys EVERYTHING still queued, whatever frame it was queued on, and answers how many.
        /// For teardown only — see the argument at the definition. The caller must have idled the device.
        std::size_t DrainDeletionQueue();

        /// How many deferred destructions are still owed. Nonzero after the last frame means leaked.
        [[nodiscard]] std::size_t QueuedCount() const;

        /// A mapping you cannot write through without having asked whether it exists — see
        /// Engine/Graphic/MappedMemory.hpp for the whole argument, and for the ten `memcpy`s into a
        /// possibly-null pointer that this return type makes uncompilable rather than merely loud.
        ///
        /// The mapping unmaps itself, so there is no `UnmapMemory` to pair with this call any more: the
        /// pairing was hand-written at thirteen sites and an early return past one of them leaked a
        /// mapping with nothing to say so.
        NO_DISCARD MappedMemory MapMemory( VmaAllocation allocation );

        void Init( const std::shared_ptr<VulkanLogicalDevice>& device, VkInstance instance );

        void Shutdown();

        static VmaAllocator& GetVMAAllocator();

        VulkanAllocator() = default;

    private:
        // The unmap MappedMemory holds as its release hook. Private and static: nothing outside this
        // class may unmap an allocation, because the only object that knows a mapping is still live is
        // the MappedMemory that owns it, and a second unmap of the same allocation is undefined
        // behaviour in VMA. It takes `void*` rather than `VmaAllocation` so that MappedMemory — which
        // lives in Engine/Graphic and must not know Vulkan — can hold its address.
        static void UnmapAllocation( void* allocation );

        /// The one body behind ProcessDeletionQueue and DrainDeletionQueue. They differ ONLY in which
        /// queued frames they take, and that is the whole reason they share this: two copies of four
        /// erase loops is where a fifth resource kind gets destroyed on one path and leaked on the other.
        std::size_t DestroyQueued( const std::function<bool( uint32_t )>& takeFrame );

        friend class Common::Singleton<VulkanAllocator>;

        /// The device this allocator was created for. See Init() for why it is held rather than looked up.
        VkDevice m_Device = VK_NULL_HANDLE;

        std::vector<BufferDeletionEntry>      m_BufferDeletionQueue;
        std::vector<ImageDeletionEntry>       m_ImageDeletionQueue;
        std::vector<FramebufferDeletionEntry> m_FramebufferDeletionQueue;
        std::vector<RenderPassDeletionEntry>  m_RenderPassDeletionQueue;
        std::vector<DescriptorPoolDeletionEntry> m_DescriptorPoolDeletionQueue;
    };
} // namespace Desert::Graphic::API::Vulkan