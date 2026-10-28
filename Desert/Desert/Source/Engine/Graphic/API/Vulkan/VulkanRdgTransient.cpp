#include <Engine/Graphic/API/Vulkan/VulkanRdgTransient.hpp>

#include <algorithm>

namespace Desert::Graphic::API::Vulkan
{
    VulkanRdgTransientAllocator::VulkanRdgTransientAllocator( const VulkanRdgDevice& device, uint32_t frameSlots )
         : m_Device( device ), m_SlotCount( std::max( 1u, frameSlots ) )
    {
    }

    VulkanRdgTransientAllocator::~VulkanRdgTransientAllocator()
    {
        // The owner destroys the allocator after the device went idle (like VulkanRdgPool).
        for ( Heap& heap : m_Open )
            Free( heap );
        for ( Heap& heap : m_Cache )
            Free( heap );
    }

    void VulkanRdgTransientAllocator::Free( Heap& heap ) const
    {
        // Resources first: nothing may stay bound to memory that is freed.
        heap.Textures.clear();
        heap.Buffers.clear();
        if ( heap.Memory != nullptr )
            vmaFreeMemory( m_Device.Allocator, heap.Memory );
        heap.Memory = nullptr;
        heap.Ledger = ResourceOwnership{}; // closes the row with the memory
    }

    bool VulkanRdgTransientAllocator::TakeCached( const RDG::TransientHeapDesc& want, bool asyncCompute, Heap& out )
    {
        auto best = m_Cache.end();
        for ( auto it = m_Cache.begin(); it != m_Cache.end(); ++it )
        {
            const RDG::TransientHeapDesc& have = it->Desc;
            const bool fits = have.Class == want.Class && ( want.MemoryTypeBits & ( 1u << it->MemoryType ) ) != 0 &&
                              have.Bytes >= want.Bytes && have.Bytes / 2 <= want.Bytes && have.Alignment >= want.Alignment;
            // Another queue is not ordered by the barrier TakeCrossGraphBarrier asks for: a heap an async-compute
            // graph used, or one an async-compute graph would use, waits for its last user's fence.
            const bool ordered = Fenced( it->LastFrame ) || ( !asyncCompute && !it->AsyncUser );
            if ( fits && ordered && ( best == m_Cache.end() || have.Bytes < best->Desc.Bytes ) )
                best = it;
        }
        if ( best == m_Cache.end() )
            return false;
        if ( !Fenced( best->LastFrame ) )
            m_CrossGraphBarrier = true;
        out = std::move( *best );
        m_Cache.erase( best );
        return true;
    }

    void VulkanRdgTransientAllocator::BeginFrameSlot( uint32_t )
    {
        // The caller waited for this slot's previous submission: every frame up to m_Frame + 1 - m_SlotCount is
        // complete (Fenced).
        ++m_Frame;
        m_Begun = true;
        // A cached heap no graph took for kGarbageCollectLatency frames goes back to the driver; a placed
        // resource no graph asked for as long is destroyed (both are fenced: the latency exceeds the slots).
        const auto stale = [&]( uint64_t lastFrame )
        { return Fenced( lastFrame ) && m_Frame - lastFrame > kGarbageCollectLatency; };
        for ( Heap& heap : m_Cache )
        {
            if ( stale( heap.LastFrame ) )
            {
                Free( heap );
                continue;
            }
            std::erase_if( heap.Textures, [&]( const PlacedTexture& placed ) { return stale( placed.LastFrame ); } );
            std::erase_if( heap.Buffers, [&]( const PlacedBuffer& placed ) { return stale( placed.LastFrame ); } );
        }
        std::erase_if( m_Cache, []( const Heap& heap ) { return heap.Memory == nullptr; } );
    }

    Common::BoolResultStr VulkanRdgTransientAllocator::AllocateHeap( const RDG::TransientHeapDesc& desc,
                                                                     Heap&                         out ) const
    {
        // One vkAllocateMemory per heap (dedicated): the heap is the unit the plan's offsets are relative to.
        VkMemoryRequirements requirements{};
        requirements.size           = desc.Bytes;
        requirements.alignment      = std::max<uint64_t>( 1, desc.Alignment );
        requirements.memoryTypeBits = desc.MemoryTypeBits;
        VmaAllocationCreateInfo create{};
        create.flags          = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
        create.preferredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        Heap              heap;
        VmaAllocationInfo info{};
        const VkResult    result =
             vmaAllocateMemory( m_Device.Allocator, &requirements, &create, &heap.Memory, &info );
        if ( result != VK_SUCCESS )
            return Common::MakeFormattedError<bool>( "transient heap of {} bytes (memory types {:#x}): "
                                                     "vmaAllocateMemory failed ({})",
                                                     desc.Bytes, desc.MemoryTypeBits, static_cast<int>( result ) );
        heap.Desc       = desc;
        heap.MemoryType = info.memoryType;
        // Graph execution runs outside every attribution scope, and the heap serves every view's graph: the
        // device owns it.
        heap.Ledger =
             ResourceOwnership::Take( ResourceKind::TransientHeap, static_cast<std::size_t>( info.size ) );
        heap.Ledger.Claim( ResourceOwner::Device );
        out = std::move( heap );
        return BOOLSUCCESS;
    }

    Common::BoolResultStr
    VulkanRdgTransientAllocator::ReserveHeaps( std::string_view graph, std::span<const RDG::TransientHeapDesc> heaps )
    {
        // Without its compile result the graph may have an async-compute segment: only fenced heaps.
        return ReserveGraphHeaps( graph, heaps, true );
    }

    Common::BoolResultStr VulkanRdgTransientAllocator::ReserveGraphHeaps( std::string_view                        graph,
                                                                          std::span<const RDG::TransientHeapDesc> heaps,
                                                                          bool asyncCompute )
    {
        if ( !m_Begun )
            return Common::MakeFormattedError( "graph '{}': BeginFrameSlot was never called", graph );
        if ( m_GraphOpen )
            return Common::MakeFormattedError( "graph '{}': the previous graph did not end (EndGraph)", graph );
        ++m_GraphSerial;
        m_GraphOpen         = true;
        m_OpenAsync         = asyncCompute;
        m_CrossGraphBarrier = false;
        m_Open.clear();
        m_Open.reserve( heaps.size() );
        for ( uint32_t i = 0; i < heaps.size(); ++i )
        {
            const RDG::TransientHeapDesc& want = heaps[i];
            if ( want.Bytes == 0 )
                return Common::MakeFormattedError( "graph '{}': transient heap {} is empty", graph, i );
            Heap taken;
            if ( !TakeCached( want, asyncCompute, taken ) )
            {
                if ( const Common::BoolResultStr made = AllocateHeap( want, taken ); !made )
                    return Common::MakeFormattedError( "graph '{}': {}", graph, made.GetError() );
            }
            m_Open.push_back( std::move( taken ) );
        }
        return Common::MakeSuccess( true );
    }

    Common::ResultStr<VulkanRdgTransientAllocator::Heap*>
    VulkanRdgTransientAllocator::OpenHeap( const RDG::Allocation& allocation, RDG::MemoryClass memoryClass,
                                           std::string_view name )
    {
        using Out = Heap*;
        if ( !m_GraphOpen )
            return Common::MakeFormattedError<Out>( "'{}': placed outside ReserveHeaps .. EndGraph", name );
        if ( allocation.Heap >= m_Open.size() )
            return Common::MakeFormattedError<Out>( "'{}': heap {} of {} in the open graph", name, allocation.Heap,
                                                    m_Open.size() );
        Heap& heap = m_Open[allocation.Heap];
        if ( heap.Desc.Class != allocation.Class || allocation.Class != memoryClass )
            return Common::MakeFormattedError<Out>( "'{}': its memory class differs from heap {}'s", name,
                                                    allocation.Heap );
        if ( allocation.Offset + allocation.Size > heap.Desc.Bytes )
            return Common::MakeFormattedError<Out>( "'{}': bytes [{}, +{}) outside heap {} of {} bytes", name,
                                                    allocation.Offset, allocation.Size, allocation.Heap,
                                                    heap.Desc.Bytes );
        return Common::MakeSuccess( &heap );
    }

    Common::ResultStr<std::shared_ptr<RDG::IPhysicalTexture>>
    VulkanRdgTransientAllocator::PlaceTexture( const RDG::Allocation& allocation, const RDG::TextureDesc& desc,
                                               uint32_t accessMask, std::string_view name )
    {
        using Out                           = std::shared_ptr<RDG::IPhysicalTexture>;
        const Common::ResultStr<Heap*> open = OpenHeap( allocation, RDG::MemoryClass::Texture, name );
        if ( !open )
            return Common::MakeFormattedError<Out>( "{}", open.GetError() );
        Heap& heap = *open.GetValue();
        // Two transients sharing these bytes in one graph are two images: a cached one is reused only if no
        // other resource of this graph took it.
        for ( PlacedTexture& placed : heap.Textures )
        {
            if ( placed.LastGraph != m_GraphSerial && placed.Offset == allocation.Offset &&
                 placed.Resource->Matches( desc, accessMask ) )
            {
                placed.LastGraph = m_GraphSerial;
                placed.LastFrame = m_Frame;
                return Common::MakeSuccess( Out( placed.Resource ) );
            }
        }
        const Common::ResultStr<std::shared_ptr<VulkanRdgTexture>> created =
             VulkanRdgTexture::CreatePlaced( m_Device, desc, accessMask, heap.Memory, allocation.Offset, name );
        if ( !created )
            return Common::MakeFormattedError<Out>( "{}", created.GetError() );
        heap.Textures.push_back( PlacedTexture{ .Offset     = allocation.Offset,
                                                .AccessMask = accessMask,
                                                .Resource   = created.GetValue(),
                                                .LastGraph  = m_GraphSerial,
                                                .LastFrame  = m_Frame } );
        return Common::MakeSuccess( Out( created.GetValue() ) );
    }

    Common::ResultStr<std::shared_ptr<RDG::IPhysicalBuffer>>
    VulkanRdgTransientAllocator::PlaceBuffer( const RDG::Allocation& allocation, const RDG::BufferDesc& desc,
                                              uint32_t accessMask, std::string_view name )
    {
        using Out                           = std::shared_ptr<RDG::IPhysicalBuffer>;
        const Common::ResultStr<Heap*> open = OpenHeap( allocation, RDG::MemoryClass::Buffer, name );
        if ( !open )
            return Common::MakeFormattedError<Out>( "{}", open.GetError() );
        Heap& heap = *open.GetValue();
        for ( PlacedBuffer& placed : heap.Buffers )
        {
            if ( placed.LastGraph != m_GraphSerial && placed.Offset == allocation.Offset &&
                 placed.AccessMask == accessMask && placed.Desc.Bytes == desc.Bytes )
            {
                placed.LastGraph = m_GraphSerial;
                placed.LastFrame = m_Frame;
                return Common::MakeSuccess( Out( placed.Resource ) );
            }
        }
        const Common::ResultStr<std::shared_ptr<VulkanRdgBuffer>> created =
             VulkanRdgBuffer::CreatePlaced( m_Device, desc, accessMask, heap.Memory, allocation.Offset, name );
        if ( !created )
            return Common::MakeFormattedError<Out>( "{}", created.GetError() );
        heap.Buffers.push_back( PlacedBuffer{ .Offset     = allocation.Offset,
                                              .AccessMask = accessMask,
                                              .Desc       = desc,
                                              .Resource   = created.GetValue(),
                                              .LastGraph  = m_GraphSerial,
                                              .LastFrame  = m_Frame } );
        return Common::MakeSuccess( Out( created.GetValue() ) );
    }

    void VulkanRdgTransientAllocator::EndGraph( std::string_view )
    {
        // The graph forfeits its heaps (with their placed resources) to the cache: the next graph may take them.
        for ( Heap& heap : m_Open )
        {
            heap.LastFrame = m_Frame;
            heap.AsyncUser = m_OpenAsync;
            m_Cache.push_back( std::move( heap ) );
        }
        m_Open.clear();
        m_GraphOpen = false;
    }

    RDG::TransientAllocatorStats VulkanRdgTransientAllocator::GetStats() const
    {
        RDG::TransientAllocatorStats stats;
        for ( const std::vector<Heap>* heaps : { &m_Open, &m_Cache } )
        {
            for ( const Heap& heap : *heaps )
            {
                if ( heap.Memory == nullptr )
                    continue;
                stats.ReservedBytes += heap.Desc.Bytes;
                ++stats.HeapCount;
                stats.PlacedResources += static_cast<uint32_t>( std::count_if(
                     heap.Textures.begin(), heap.Textures.end(),
                     [&]( const PlacedTexture& placed ) { return placed.LastFrame == m_Frame; } ) );
                stats.PlacedResources += static_cast<uint32_t>(
                     std::count_if( heap.Buffers.begin(), heap.Buffers.end(),
                                    [&]( const PlacedBuffer& placed ) { return placed.LastFrame == m_Frame; } ) );
            }
        }
        return stats;
    }
} // namespace Desert::Graphic::API::Vulkan
