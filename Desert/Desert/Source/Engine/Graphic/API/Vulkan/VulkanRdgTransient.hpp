#pragma once

#include <Engine/Graphic/API/Vulkan/VulkanRenderGraph.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The render graph's transient memory on Vulkan (RDG-ALIAS A1): per-frame-slot heaps that the aliasing plan
// places images and buffers into (UE: FRDGTransientResourceAllocator over RHI transient heaps).
namespace Desert::Graphic::API::Vulkan
{
    // RDG-CONTRACTS A(1). The Vulkan ITransientAllocator; REPLACES VulkanRdgPool for every non-extracted
    // transient (the pool survives only for extracted transients, which need a dedicated allocation).
    // Responsibility: per frame slot and graph, one VkDeviceMemory block per TransientHeapDesc (vmaAllocateMemory
    // with MemoryTypeBits = the heap's intersection, size = Bytes, alignment = Alignment), and images / buffers
    // created with the same create info VulkanRdgMemoryRequirements measured (so the plan's size is the size
    // bound) and bound with vmaBindImageMemory2 / vmaBindBufferMemory2 at the allocation's offset. Placed
    // resources are cached per (slot, graph, heap, offset, desc, usage) so an unchanged plan reuses its VkImage
    // objects; a changed plan destroys the stale ones at the slot's next BeginFrameSlot.
    // Who calls it: VulkanRdgBackend::BeginGraph / EndGraph / AbandonGraph, and the renderer's frame loop
    // (BeginFrameSlot, right after the frames-in-flight fence wait - where VulkanRdgPool::BeginFrame is called
    // today). Never: free or rebind memory a submitted, unwaited frame may use; bind two live resources of the
    // same graph to intersecting bytes unless the plan's lifetimes are disjoint.
    class VulkanRdgTransientAllocator final : public RDG::ITransientAllocator
    {
    public:
        VulkanRdgTransientAllocator( const VulkanRdgDevice& device, uint32_t frameSlots );
        ~VulkanRdgTransientAllocator() override;
        VulkanRdgTransientAllocator( const VulkanRdgTransientAllocator& )            = delete;
        VulkanRdgTransientAllocator& operator=( const VulkanRdgTransientAllocator& ) = delete;

        void                                                      BeginFrameSlot( uint32_t slot ) override;
        Common::BoolResultStr                                     ReserveHeaps( std::string_view                        graph,
                                                                                std::span<const RDG::TransientHeapDesc> heaps ) override;
        Common::ResultStr<std::shared_ptr<RDG::IPhysicalTexture>> PlaceTexture( const RDG::Allocation&  allocation,
                                                                                const RDG::TextureDesc& desc,
                                                                                uint32_t                accessMask,
                                                                                std::string_view name ) override;
        Common::ResultStr<std::shared_ptr<RDG::IPhysicalBuffer>>  PlaceBuffer( const RDG::Allocation& allocation,
                                                                               const RDG::BufferDesc& desc,
                                                                               uint32_t               accessMask,
                                                                               std::string_view name ) override;
        void                                                      EndGraph( std::string_view graph ) override;
        RDG::TransientAllocatorStats                              GetStats() const override;

    private:
        struct Heap
        {
            RDG::TransientHeapDesc Desc; // what was allocated (Bytes / Alignment may exceed the plan's)
            VmaAllocation          Memory     = nullptr;
            uint32_t               MemoryType = 0;
        };
        struct PlacedTexture
        {
            uint32_t                          Heap       = 0;
            uint64_t                          Offset     = 0;
            uint32_t                          AccessMask = 0;
            RDG::TextureDesc                  Desc;
            std::shared_ptr<VulkanRdgTexture> Resource;
            bool                              UsedThisFrame = false;
        };
        struct PlacedBuffer
        {
            uint32_t                         Heap       = 0;
            uint64_t                         Offset     = 0;
            uint32_t                         AccessMask = 0;
            RDG::BufferDesc                  Desc;
            std::shared_ptr<VulkanRdgBuffer> Resource;
            bool                             UsedThisFrame = false;
        };
        // One graph's heaps in one slot. A graph name recorded twice in one frame gets a second entry: its
        // heaps must not be the first recording's (the second's acquire barriers cannot see the first's accesses).
        struct GraphHeaps
        {
            std::string                Graph;
            bool                       UsedThisFrame = false;
            std::vector<Heap>          Heaps;
            std::vector<PlacedTexture> Textures;
            std::vector<PlacedBuffer>  Buffers;
        };
        struct Slot
        {
            std::vector<GraphHeaps> Graphs;
            // Grown-out heaps and the resources bound to them, destroyed at this slot's next BeginFrameSlot.
            std::vector<VmaAllocation>                     RetiredHeaps;
            std::vector<std::shared_ptr<VulkanRdgTexture>> RetiredTextures;
            std::vector<std::shared_ptr<VulkanRdgBuffer>>  RetiredBuffers;
        };

        void                           Retire( Slot& slot, GraphHeaps& graph, uint32_t heap );
        void                           Destroy( GraphHeaps& graph );
        static void                    DestroyRetired( VmaAllocator allocator, Slot& slot );
        Common::ResultStr<Heap>        AllocateHeap( const RDG::TransientHeapDesc& desc ) const;
        Common::ResultStr<const Heap*> OpenHeap( const RDG::Allocation& allocation, RDG::MemoryClass memoryClass,
                                                 std::string_view name ) const;

        const VulkanRdgDevice& m_Device;
        std::vector<Slot>      m_Slots;
        uint32_t               m_Slot    = 0;
        bool                   m_Begun   = false; // BeginFrameSlot was called at least once
        int32_t                m_Current = -1;    // the open graph's index in the slot's Graphs, -1 between graphs
    };
} // namespace Desert::Graphic::API::Vulkan
