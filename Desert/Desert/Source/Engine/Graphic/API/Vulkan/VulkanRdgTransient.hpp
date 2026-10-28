#pragma once

#include <Engine/Graphic/API/Vulkan/VulkanRenderGraph.hpp>
#include <Engine/Graphic/ResourceLedger.hpp>

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
    // Responsibility: per graph, one VkDeviceMemory block per TransientHeapDesc (vmaAllocateMemory
    // with MemoryTypeBits = the heap's intersection, size = Bytes, alignment = Alignment), and images / buffers
    // created with the same create info VulkanRdgMemoryRequirements measured (so the plan's size is the size
    // bound) and bound with vmaBindImageMemory2 / vmaBindBufferMemory2 at the allocation's offset.
    // THE HEAP CACHE (BISTRO-MEM-2 p3, UE FRHITransientHeapCache + FRHITransientResourceAllocator): a graph holds
    // its heaps only from ReserveHeaps to EndGraph, then forfeits them to ONE cache shared by every graph and
    // frame slot; the next graph takes the smallest cached heap that fits without wasting more than half of it.
    // Transient memory is therefore the largest graph's plan (plus what is in flight), not the sum over every
    // graph of every slot (p2: 29 heaps, 770 MB on Bistro). A heap whose last user may still be executing is
    // taken only by a graphics-only graph, after its last user in queue submission order: the backend records
    // one full memory barrier before that graph's first command (TakeCrossGraphBarrier), as UE's aliasing
    // barrier against the previous graph. A graph with an async-compute segment, and a heap an async-compute
    // graph gave up, wait for the fence (another queue is not ordered by that barrier). Placed images/buffers
    // are cached ON THE HEAP by (offset, desc, usage), so a stable sequence of graphs reuses its VkImages; a
    // placed resource unused for kGarbageCollectLatency frames, and a heap no graph took for as long, are
    // destroyed once fenced.
    // Who calls it: VulkanRdgBackend::BeginGraph / EndGraph / AbandonGraph, and the renderer's frame loop
    // (BeginFrameSlot, right after the frames-in-flight fence wait - where VulkanRdgPool::BeginFrame is called
    // today). Never: free memory a submitted, unwaited frame may use; bind two live resources of the
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
        [[nodiscard]] RDG::TransientAllocatorStats                GetStats() const override;

        // ReserveHeaps for a graph whose compile result is known: @p asyncCompute = it has an AsyncCompute
        // segment (it may then take only fenced heaps). The interface's ReserveHeaps assumes it does.
        Common::BoolResultStr ReserveGraphHeaps( std::string_view                        graph,
                                                 std::span<const RDG::TransientHeapDesc> heaps,
                                                 bool                                    asyncCompute );
        // True once after a ReserveGraphHeaps that took a heap an unfenced graph used: the caller records a full
        // memory barrier (all commands, memory write -> read/write) before the graph's first command.
        [[nodiscard]] bool TakeCrossGraphBarrier() noexcept
        {
            const bool needed   = m_CrossGraphBarrier;
            m_CrossGraphBarrier = false;
            return needed;
        }

    private:
        // UE RHI transient allocator's GarbageCollectLatency: frames a cached heap or a placed resource may stay
        // unused before it is destroyed.
        static constexpr uint64_t kGarbageCollectLatency = 16;
        struct PlacedTexture
        {
            uint64_t                          Offset     = 0;
            uint32_t                          AccessMask = 0;
            std::shared_ptr<VulkanRdgTexture> Resource;
            uint64_t                          LastGraph = 0; // m_GraphSerial of its last hand-out
            uint64_t                          LastFrame = 0;
        };
        struct PlacedBuffer
        {
            uint64_t                         Offset     = 0;
            uint32_t                         AccessMask = 0;
            RDG::BufferDesc                  Desc;
            std::shared_ptr<VulkanRdgBuffer> Resource;
            uint64_t                         LastGraph = 0;
            uint64_t                         LastFrame = 0;
        };
        struct Heap
        {
            RDG::TransientHeapDesc Desc; // what was allocated (Bytes / Alignment may exceed the plan's)
            VmaAllocation          Memory     = nullptr;
            uint32_t               MemoryType = 0;
            // The heap's ledger row (TransientHeap, owner Device): opened with the memory, closed when it is
            // freed.
            ResourceOwnership          Ledger;
            uint64_t                   LastFrame = 0;     // frame counter of its last graph
            bool                       AsyncUser = false; // its last graph had an AsyncCompute segment
            std::vector<PlacedTexture> Textures;
            std::vector<PlacedBuffer>  Buffers;
        };

        [[nodiscard]] bool Fenced( uint64_t frame ) const noexcept
        {
            return frame + m_SlotCount <= m_Frame;
        }
        void Free( Heap& heap ) const;
        // The smallest cached heap fitting @p want (same class, an allowed memory type, at most twice its bytes)
        // that this graph may take, moved out of the cache into @p out; false if none.
        [[nodiscard]] bool TakeCached( const RDG::TransientHeapDesc& want, bool asyncCompute, Heap& out );
        // Fills @p out (a heap is move-only: it holds its ledger row).
        [[nodiscard]] Common::BoolResultStr AllocateHeap( std::string_view graph, const RDG::TransientHeapDesc& desc,
                                                          Heap& out ) const;
        [[nodiscard]] Common::ResultStr<Heap*> OpenHeap( const RDG::Allocation& allocation,
                                                         RDG::MemoryClass memoryClass, std::string_view name );

        const VulkanRdgDevice& m_Device;
        uint64_t               m_SlotCount = 1;
        std::vector<Heap>      m_Cache;                     // heaps no graph holds
        std::vector<Heap>      m_Open;                      // the open graph's heaps, by the plan's heap index
        bool                   m_Begun             = false; // BeginFrameSlot was called at least once
        bool                   m_GraphOpen         = false; // between ReserveHeaps and EndGraph
        bool                   m_OpenAsync         = false; // the open graph has an AsyncCompute segment
        bool                   m_CrossGraphBarrier = false;
        uint64_t               m_Frame             = 0; // BeginFrameSlot count
        uint64_t               m_GraphSerial       = 0; // ReserveHeaps count
    };
} // namespace Desert::Graphic::API::Vulkan
