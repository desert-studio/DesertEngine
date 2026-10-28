#include <Engine/Graphic/API/Vulkan/VulkanRdgTransient.hpp>
#include <format>

#include <algorithm>

namespace Desert::Graphic::API::Vulkan
{
    VulkanRdgTransientAllocator::VulkanRdgTransientAllocator( const VulkanRdgDevice& device, uint32_t frameSlots )
         : m_Device( device ), m_Slots( std::max( 1u, frameSlots ) )
    {
    }

    VulkanRdgTransientAllocator::~VulkanRdgTransientAllocator()
    {
        // The owner destroys the allocator after the device went idle (like VulkanRdgPool).
        for ( Slot& slot : m_Slots )
        {
            for ( GraphHeaps& graph : slot.Graphs )
                Release( slot, graph );
            slot.Graphs.clear();
            DestroyRetired( slot );
            for ( const Heap& retired : slot.RetiredHeaps )
                vmaFreeMemory( m_Device.Allocator, retired.Memory );
            slot.RetiredHeaps.clear();
            FreePool( slot );
        }
        for ( const SharedHeap& shared : m_Shared )
        {
            if ( shared.Held.Memory != nullptr )
                vmaFreeMemory( m_Device.Allocator, shared.Held.Memory );
        }
        m_Shared.clear();
    }

    void VulkanRdgTransientAllocator::DestroyRetired( Slot& slot )
    {
        slot.RetiredTextures.clear();
        slot.RetiredBuffers.clear();
    }

    void VulkanRdgTransientAllocator::FreePool( Slot& slot ) const
    {
        // Resources first (DestroyRetired): nothing may stay bound to memory that is freed.
        for ( const PooledHeap& pooled : slot.Pool )
            vmaFreeMemory( m_Device.Allocator, pooled.Pooled.Memory );
        slot.Pool.clear(); // closes the rows with the memory
    }

    void VulkanRdgTransientAllocator::Release( Slot& slot, GraphHeaps& graph )
    {
        if ( graph.Shared )
        {
            // The bytes are the slot's: only what this graph placed in them goes.
            for ( uint32_t heap = 0; heap < graph.SharedIndex.size(); ++heap )
                RetirePlaced( slot, graph, heap );
            graph.SharedIndex.clear();
            return;
        }
        for ( uint32_t heap = 0; heap < graph.Heaps.size(); ++heap )
            Retire( slot, graph, heap );
        graph.Heaps.clear();
    }

    void VulkanRdgTransientAllocator::RetireShared( uint32_t index )
    {
        // Every slot's graphs placed resources in it; the current slot's next BeginFrameSlot destroys them all
        // (the frames of the other slots that recorded them are earlier, and complete by then).
        Slot& current = m_Slots[m_Slot];
        for ( Slot& slot : m_Slots )
        {
            for ( GraphHeaps& graph : slot.Graphs )
            {
                if ( !graph.Shared )
                    continue;
                for ( uint32_t heap = 0; heap < graph.SharedIndex.size(); ++heap )
                {
                    if ( graph.SharedIndex[heap] == index )
                        RetirePlaced( current, graph, heap );
                }
            }
        }
        SharedHeap& shared = m_Shared[index];
        if ( shared.Held.Memory != nullptr )
            current.RetiredHeaps.push_back( std::move( shared.Held ) );
        shared.Held.Memory          = nullptr;
        shared.Held.Ledger          = ResourceOwnership{};
        shared.Held.OversizedFrames = 0;
        shared.OversizedFrames      = 0;
    }

    void VulkanRdgTransientAllocator::RetirePlaced( Slot& slot, GraphHeaps& graph, uint32_t heap )
    {
        // The resources bound to the heap live until the slot's next BeginFrameSlot (a recorded command or a
        // graph's physical reference may still name them).
        for ( PlacedTexture& placed : graph.Textures )
        {
            if ( placed.Heap == heap )
                slot.RetiredTextures.push_back( std::move( placed.Resource ) );
        }
        for ( PlacedBuffer& placed : graph.Buffers )
        {
            if ( placed.Heap == heap )
                slot.RetiredBuffers.push_back( std::move( placed.Resource ) );
        }
        std::erase_if( graph.Textures, [&]( const PlacedTexture& placed ) { return placed.Heap == heap; } );
        std::erase_if( graph.Buffers, [&]( const PlacedBuffer& placed ) { return placed.Heap == heap; } );
    }

    void VulkanRdgTransientAllocator::Retire( Slot& slot, GraphHeaps& graph, uint32_t heap )
    {
        // The heap's bytes go to the pool at once (RetirePlaced keeps its resources to the next BeginFrameSlot).
        RetirePlaced( slot, graph, heap );
        if ( graph.Heaps[heap].Memory != nullptr )
        {
            slot.Pool.push_back( PooledHeap{ .Pooled = std::move( graph.Heaps[heap] ), .IdleFrames = 0, .GivenUpThisFrame = true } );
            slot.Pool.back().Pooled.OversizedFrames = 0;
        }
        graph.Heaps[heap].Memory = nullptr;
        graph.Heaps[heap].Ledger = ResourceOwnership{};
    }

    bool VulkanRdgTransientAllocator::TakePooled( Slot& slot, const RDG::TransientHeapDesc& want, Heap& out )
    {
        auto best = slot.Pool.end();
        for ( auto it = slot.Pool.begin(); it != slot.Pool.end(); ++it )
        {
            const RDG::TransientHeapDesc& have = it->Pooled.Desc;
            const bool                    fits =
                 !it->GivenUpThisFrame && have.Class == want.Class && ( want.MemoryTypeBits & ( 1u << it->Pooled.MemoryType ) ) != 0 &&
                 have.Bytes >= want.Bytes && have.Bytes / 2 <= want.Bytes && have.Alignment >= want.Alignment;
            if ( fits && ( best == slot.Pool.end() || have.Bytes < best->Pooled.Desc.Bytes ) )
                best = it;
        }
        if ( best == slot.Pool.end() )
            return false;
        out = std::move( best->Pooled );
        slot.Pool.erase( best );
        return true;
    }

    void VulkanRdgTransientAllocator::BeginFrameSlot( uint32_t slot )
    {
        m_Slot        = slot % static_cast<uint32_t>( m_Slots.size() );
        m_Begun       = true;
        m_Current     = -1;
        Slot& current = m_Slots[m_Slot];
        // The caller waited for this slot's previous submission: what it retired is unused now.
        DestroyRetired( current );
        for ( const Heap& retired : current.RetiredHeaps )
            vmaFreeMemory( m_Device.Allocator, retired.Memory ); // closes its row with it
        current.RetiredHeaps.clear();
        // A shared heap the previous frame's graphs wanted at most half of (or not at all) for
        // kGarbageCollectLatency frames is given up (the next want allocates one of its size).
        for ( uint32_t index = 0; index < m_Shared.size(); ++index )
        {
            SharedHeap& shared = m_Shared[index];
            if ( shared.Held.Memory == nullptr )
                continue;
            const bool oversized   = shared.WantThisFrame <= shared.Held.Desc.Bytes / 2;
            shared.OversizedFrames = oversized ? shared.OversizedFrames + 1 : 0;
            if ( shared.OversizedFrames > kGarbageCollectLatency )
                RetireShared( index );
            shared.WantThisFrame = 0;
        }
        // A pooled heap no graph took for kGarbageCollectLatency frames of this slot goes back to the driver.
        for ( PooledHeap& pooled : current.Pool )
        {
            if ( ++pooled.IdleFrames > kGarbageCollectLatency )
                vmaFreeMemory( m_Device.Allocator, pooled.Pooled.Memory );
        }
        std::erase_if( current.Pool,
                       []( const PooledHeap& pooled ) { return pooled.IdleFrames > kGarbageCollectLatency; } );
        // A graph this slot's previous frame did not record has nothing in flight: its heaps go to the pool.
        std::erase_if( current.Graphs,
                       [&]( GraphHeaps& graph )
                       {
                           if ( graph.UsedThisFrame )
                               return false;
                           Release( current, graph );
                           return true;
                       } );
        for ( GraphHeaps& graph : current.Graphs )
        {
            // A placed resource the previous frame's plan did not use belongs to a changed plan.
            std::erase_if( graph.Textures, []( const PlacedTexture& placed ) { return !placed.UsedThisFrame; } );
            std::erase_if( graph.Buffers, []( const PlacedBuffer& placed ) { return !placed.UsedThisFrame; } );
            for ( PlacedTexture& placed : graph.Textures )
                placed.UsedThisFrame = false;
            for ( PlacedBuffer& placed : graph.Buffers )
                placed.UsedThisFrame = false;
            graph.UsedThisFrame = false;
        }
        // Every heap in the pool now was last used by this slot's previous frame, which has completed.
        for ( PooledHeap& pooled : current.Pool )
            pooled.GivenUpThisFrame = false;
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
    VulkanRdgTransientAllocator::ReserveHeaps( std::string_view                        graph,
                                               std::span<const RDG::TransientHeapDesc> heaps )
    {
        const Common::ResultStr<bool> reserved = ReserveGraphHeaps( graph, heaps, false );
        if ( !reserved )
            return Common::MakeFormattedError( "{}", reserved.GetError() );
        return Common::MakeSuccess( true );
    }

    Common::ResultStr<bool>
    VulkanRdgTransientAllocator::ReserveGraphHeaps( std::string_view                        graph,
                                                    std::span<const RDG::TransientHeapDesc> heaps, bool share )
    {
        if ( !m_Begun )
            return Common::MakeFormattedError<bool>( "graph '{}': BeginFrameSlot was never called", graph );
        if ( m_Current >= 0 )
            return Common::MakeFormattedError<bool>( "graph '{}': graph '{}' did not end (EndGraph)", graph,
                                                     m_Slots[m_Slot].Graphs[m_Current].Graph );
        Slot&        slot  = m_Slots[m_Slot];
        const auto   found = std::find_if( slot.Graphs.begin(), slot.Graphs.end(), [&]( const GraphHeaps& entry )
                                           { return entry.Graph == graph && !entry.UsedThisFrame && entry.Shared == share; } );
        const size_t index = static_cast<size_t>( found - slot.Graphs.begin() );
        if ( found == slot.Graphs.end() )
            slot.Graphs.push_back(
                 GraphHeaps{ .Graph = std::string( graph ), .Heaps = {}, .Textures = {}, .Buffers = {} } );
        GraphHeaps& entry   = slot.Graphs[index];
        entry.UsedThisFrame = true;
        entry.Shared        = share;
        if ( share )
        {
            const bool barrier = m_SharedUsed;
            if ( auto reserved = ReserveShared( slot, entry, heaps ); !reserved )
                return Common::MakeFormattedError<bool>( "{}", reserved.GetError() );
            m_SharedUsed = true;
            m_Current                = static_cast<int32_t>( index );
            return Common::MakeSuccess( barrier );
        }

        // Heaps the plan no longer has are retired with everything placed in them.
        for ( auto extra = static_cast<uint32_t>( heaps.size() ); extra < entry.Heaps.size(); ++extra )
            Retire( slot, entry, extra );
        entry.Heaps.resize( std::min( entry.Heaps.size(), heaps.size() ) );
        for ( uint32_t i = 0; i < heaps.size(); ++i )
        {
            const RDG::TransientHeapDesc& want = heaps[i];
            if ( want.Bytes == 0 )
                return Common::MakeFormattedError<bool>( "graph '{}': transient heap {} is empty", graph, i );
            if ( i < entry.Heaps.size() )
            {
                const Heap& have = entry.Heaps[i];
                const bool  fits = have.Memory != nullptr && have.Desc.Class == want.Class &&
                                  ( want.MemoryTypeBits & ( 1u << have.MemoryType ) ) != 0 &&
                                  have.Desc.Bytes >= want.Bytes && have.Desc.Alignment >= want.Alignment;
                // A heap the plan keeps wanting at most half of (the view shrank, a feature went off) is
                // traded for one of the plan's size after kGarbageCollectLatency frames: heaps never only grow.
                Heap& kept = entry.Heaps[i];
                if ( fits && want.Bytes > kept.Desc.Bytes / 2 )
                    kept.OversizedFrames = 0;
                if ( fits &&
                     ( want.Bytes > kept.Desc.Bytes / 2 || ++kept.OversizedFrames <= kGarbageCollectLatency ) )
                    continue;
                // Grown, oversized (or of another class / type): the heap goes to the slot's pool; the resources
                // bound to it are destroyed at the slot's next BeginFrameSlot, so none outlives its memory.
                Retire( slot, entry, i );
            }
            Heap allocated;
            if ( !TakePooled( slot, want, allocated ) )
            {
                if ( const Common::BoolResultStr made = AllocateHeap( want, allocated ); !made )
                    return Common::MakeFormattedError<bool>( "graph '{}': {}", graph, made.GetError() );
            }
            // Named for the graph that holds it (allocated or taken from the pool): the census's detailed map
            // (VulkanAllocator, BISTRO-MEM-2 p4) sums the heaps by the graph that wants them.
            vmaSetAllocationName( m_Device.Allocator, allocated.Memory,
                                  std::format( "RDG heap {}", graph ).c_str() );
            if ( i < entry.Heaps.size() )
                entry.Heaps[i] = std::move( allocated );
            else
                entry.Heaps.push_back( std::move( allocated ) );
        }
        m_Current = static_cast<int32_t>( index );
        return Common::MakeSuccess( false );
    }

    Common::BoolResultStr VulkanRdgTransientAllocator::ReserveShared( Slot& slot, GraphHeaps& entry,
                                                                     std::span<const RDG::TransientHeapDesc> heaps )
    {
        // The plan has fewer heaps than last time: what was placed in the others goes.
        for ( auto extra = static_cast<uint32_t>( heaps.size() ); extra < entry.SharedIndex.size(); ++extra )
            RetirePlaced( slot, entry, extra );
        entry.SharedIndex.resize( std::min( entry.SharedIndex.size(), heaps.size() ) );
        for ( uint32_t i = 0; i < heaps.size(); ++i )
        {
            const RDG::TransientHeapDesc& want = heaps[i];
            if ( want.Bytes == 0 )
                return Common::MakeFormattedError( "graph '{}': transient heap {} is empty", entry.Graph, i );
            // The key: the n-th heap of this class and memory types in the plan.
            uint32_t ordinal = 0;
            for ( uint32_t earlier = 0; earlier < i; ++earlier )
            {
                if ( heaps[earlier].Class == want.Class && heaps[earlier].MemoryTypeBits == want.MemoryTypeBits )
                    ++ordinal;
            }
            auto found = std::find_if( m_Shared.begin(), m_Shared.end(),
                                       [&]( const SharedHeap& shared )
                                       {
                                           return shared.Class == want.Class &&
                                                  shared.MemoryTypeBits == want.MemoryTypeBits &&
                                                  shared.Ordinal == ordinal;
                                       } );
            if ( found == m_Shared.end() )
            {
                m_Shared.push_back( SharedHeap{ .Class          = want.Class,
                                                   .MemoryTypeBits = want.MemoryTypeBits,
                                                   .Ordinal        = ordinal,
                                                   .Held           = {},
                                                   .WantThisFrame   = 0,
                                                   .OversizedFrames = 0 } );
                found = m_Shared.end() - 1;
            }
            const auto  shared = static_cast<uint32_t>( found - m_Shared.begin() );
            SharedHeap& held   = m_Shared[shared];
            held.WantThisFrame = std::max( held.WantThisFrame, want.Bytes );
            // The plan's heap i moved to another shared heap: what this graph placed for heap i is in other bytes.
            if ( i < entry.SharedIndex.size() && entry.SharedIndex[i] != shared )
                RetirePlaced( slot, entry, i );
            const bool fits = held.Held.Memory != nullptr && held.Held.Desc.Bytes >= want.Bytes &&
                              held.Held.Desc.Alignment >= want.Alignment &&
                              ( want.MemoryTypeBits & ( 1u << held.Held.MemoryType ) ) != 0;
            if ( !fits )
            {
                // Grown: every graph's resources in the old bytes go with them (RetireShared).
                if ( held.Held.Memory != nullptr )
                    RetireShared( shared );
                RDG::TransientHeapDesc desc = want;
                desc.Bytes                  = held.WantThisFrame; // the largest this frame's graphs asked for
                Heap allocated;
                if ( const Common::BoolResultStr made = AllocateHeap( desc, allocated ); !made )
                    return Common::MakeFormattedError( "graph '{}': {}", entry.Graph, made.GetError() );
                // The census's detailed map (VulkanAllocator, BISTRO-MEM-2 p4) sums the heaps by this name.
                vmaSetAllocationName( m_Device.Allocator, allocated.Memory, "RDG heap shared by the slot's graphs" );
                held.Held = std::move( allocated );
            }
            if ( i < entry.SharedIndex.size() )
                entry.SharedIndex[i] = shared;
            else
                entry.SharedIndex.push_back( shared );
        }
        return BOOLSUCCESS;
    }

    Common::ResultStr<const VulkanRdgTransientAllocator::Heap*>
    VulkanRdgTransientAllocator::OpenHeap( const RDG::Allocation& allocation, RDG::MemoryClass memoryClass,
                                           std::string_view name ) const
    {
        using Out = const Heap*;
        if ( m_Current < 0 )
            return Common::MakeFormattedError<Out>( "'{}': placed outside ReserveHeaps .. EndGraph", name );
        const Slot&       slot  = m_Slots[m_Slot];
        const GraphHeaps& graph = slot.Graphs[m_Current];
        const size_t      count = graph.Shared ? graph.SharedIndex.size() : graph.Heaps.size();
        if ( allocation.Heap >= count )
            return Common::MakeFormattedError<Out>( "'{}': heap {} of {} in graph '{}'", name, allocation.Heap,
                                                    count, graph.Graph );
        const Heap& heap =
             graph.Shared ? m_Shared[graph.SharedIndex[allocation.Heap]].Held : graph.Heaps[allocation.Heap];
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
        using Out                                 = std::shared_ptr<RDG::IPhysicalTexture>;
        const Common::ResultStr<const Heap*> heap = OpenHeap( allocation, RDG::MemoryClass::Texture, name );
        if ( !heap )
            return Common::MakeFormattedError<Out>( "{}", heap.GetError() );
        GraphHeaps& graph = m_Slots[m_Slot].Graphs[m_Current];
        // Two transients sharing these bytes in one frame are two images: a cached one is reused only if no
        // other resource of this frame took it.
        for ( PlacedTexture& placed : graph.Textures )
        {
            if ( !placed.UsedThisFrame && placed.Heap == allocation.Heap && placed.Offset == allocation.Offset &&
                 placed.Resource->Matches( desc, accessMask ) )
            {
                placed.UsedThisFrame = true;
                return Common::MakeSuccess( Out( placed.Resource ) );
            }
        }
        const Common::ResultStr<std::shared_ptr<VulkanRdgTexture>> created = VulkanRdgTexture::CreatePlaced(
             m_Device, desc, accessMask, heap.GetValue()->Memory, allocation.Offset, name );
        if ( !created )
            return Common::MakeFormattedError<Out>( "{}", created.GetError() );
        graph.Textures.push_back(
             PlacedTexture{ allocation.Heap, allocation.Offset, accessMask, desc, created.GetValue(), true } );
        return Common::MakeSuccess( Out( created.GetValue() ) );
    }

    Common::ResultStr<std::shared_ptr<RDG::IPhysicalBuffer>>
    VulkanRdgTransientAllocator::PlaceBuffer( const RDG::Allocation& allocation, const RDG::BufferDesc& desc,
                                              uint32_t accessMask, std::string_view name )
    {
        using Out                                 = std::shared_ptr<RDG::IPhysicalBuffer>;
        const Common::ResultStr<const Heap*> heap = OpenHeap( allocation, RDG::MemoryClass::Buffer, name );
        if ( !heap )
            return Common::MakeFormattedError<Out>( "{}", heap.GetError() );
        GraphHeaps& graph = m_Slots[m_Slot].Graphs[m_Current];
        for ( PlacedBuffer& placed : graph.Buffers )
        {
            if ( !placed.UsedThisFrame && placed.Heap == allocation.Heap && placed.Offset == allocation.Offset &&
                 placed.AccessMask == accessMask && placed.Desc.Bytes == desc.Bytes )
            {
                placed.UsedThisFrame = true;
                return Common::MakeSuccess( Out( placed.Resource ) );
            }
        }
        const Common::ResultStr<std::shared_ptr<VulkanRdgBuffer>> created = VulkanRdgBuffer::CreatePlaced(
             m_Device, desc, accessMask, heap.GetValue()->Memory, allocation.Offset, name );
        if ( !created )
            return Common::MakeFormattedError<Out>( "{}", created.GetError() );
        graph.Buffers.push_back(
             PlacedBuffer{ allocation.Heap, allocation.Offset, accessMask, desc, created.GetValue(), true } );
        return Common::MakeSuccess( Out( created.GetValue() ) );
    }

    void VulkanRdgTransientAllocator::EndGraph( std::string_view )
    {
        // Placed resources stay cached for this slot's next frame; only the open-graph mark ends.
        m_Current = -1;
    }

    RDG::TransientAllocatorStats VulkanRdgTransientAllocator::GetStats() const
    {
        RDG::TransientAllocatorStats stats;
        for ( const Slot& slot : m_Slots )
        {
            for ( const GraphHeaps& graph : slot.Graphs )
            {
                for ( const Heap& heap : graph.Heaps )
                {
                    if ( heap.Memory == nullptr )
                        continue;
                    stats.ReservedBytes += heap.Desc.Bytes;
                    ++stats.HeapCount;
                }
            }
            for ( const Heap& retired : slot.RetiredHeaps )
            {
                stats.ReservedBytes += retired.Desc.Bytes;
                ++stats.HeapCount;
            }
            // A pooled heap is reserved until the pool frees it.
            for ( const PooledHeap& pooled : slot.Pool )
            {
                stats.ReservedBytes += pooled.Pooled.Desc.Bytes;
                ++stats.HeapCount;
            }
        }
        for ( const SharedHeap& shared : m_Shared )
        {
            if ( shared.Held.Memory == nullptr )
                continue;
            stats.ReservedBytes += shared.Held.Desc.Bytes;
            ++stats.HeapCount;
        }
        for ( const GraphHeaps& graph : m_Slots[m_Slot].Graphs )
        {
            stats.PlacedResources += static_cast<uint32_t>(
                 std::count_if( graph.Textures.begin(), graph.Textures.end(),
                                []( const PlacedTexture& placed ) { return placed.UsedThisFrame; } ) );
            stats.PlacedResources += static_cast<uint32_t>(
                 std::count_if( graph.Buffers.begin(), graph.Buffers.end(),
                                []( const PlacedBuffer& placed ) { return placed.UsedThisFrame; } ) );
        }
        return stats;
    }
} // namespace Desert::Graphic::API::Vulkan
