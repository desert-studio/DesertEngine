#include <Engine/Graphic/API/Vulkan/VulkanRdgTransient.hpp>

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
                Destroy( graph );
            DestroyRetired( m_Device.Allocator, slot );
        }
    }

    void VulkanRdgTransientAllocator::DestroyRetired( VmaAllocator allocator, Slot& slot )
    {
        // Resources first: nothing may stay bound to memory that is freed.
        slot.RetiredTextures.clear();
        slot.RetiredBuffers.clear();
        for ( VmaAllocation memory : slot.RetiredHeaps )
            vmaFreeMemory( allocator, memory );
        slot.RetiredHeaps.clear();
    }

    void VulkanRdgTransientAllocator::Destroy( GraphHeaps& graph )
    {
        graph.Textures.clear();
        graph.Buffers.clear();
        for ( Heap& heap : graph.Heaps )
        {
            if ( heap.Memory != nullptr )
                vmaFreeMemory( m_Device.Allocator, heap.Memory );
        }
        graph.Heaps.clear();
    }

    void VulkanRdgTransientAllocator::Retire( Slot& slot, GraphHeaps& graph, uint32_t heap )
    {
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
        if ( graph.Heaps[heap].Memory != nullptr )
            slot.RetiredHeaps.push_back( graph.Heaps[heap].Memory );
        graph.Heaps[heap].Memory = nullptr;
    }

    void VulkanRdgTransientAllocator::BeginFrameSlot( uint32_t slot )
    {
        m_Slot        = slot % static_cast<uint32_t>( m_Slots.size() );
        m_Begun       = true;
        m_Current     = -1;
        Slot& current = m_Slots[m_Slot];
        // The caller waited for this slot's previous submission: what it retired is unused now.
        DestroyRetired( m_Device.Allocator, current );
        // A graph this slot's previous frame did not record has nothing in flight: its heaps go now.
        std::erase_if( current.Graphs,
                       [&]( GraphHeaps& graph )
                       {
                           if ( graph.UsedThisFrame )
                               return false;
                           Destroy( graph );
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
    }

    Common::ResultStr<VulkanRdgTransientAllocator::Heap>
    VulkanRdgTransientAllocator::AllocateHeap( const RDG::TransientHeapDesc& desc ) const
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
            return Common::MakeFormattedError<Heap>( "transient heap of {} bytes (memory types {:#x}): "
                                                     "vmaAllocateMemory failed ({})",
                                                     desc.Bytes, desc.MemoryTypeBits, static_cast<int>( result ) );
        heap.Desc       = desc;
        heap.MemoryType = info.memoryType;
        return Common::MakeSuccess( heap );
    }

    Common::BoolResultStr
    VulkanRdgTransientAllocator::ReserveHeaps( std::string_view                        graph,
                                               std::span<const RDG::TransientHeapDesc> heaps )
    {
        if ( !m_Begun )
            return Common::MakeFormattedError( "graph '{}': BeginFrameSlot was never called", graph );
        if ( m_Current >= 0 )
            return Common::MakeFormattedError( "graph '{}': graph '{}' did not end (EndGraph)", graph,
                                               m_Slots[m_Slot].Graphs[m_Current].Graph );
        Slot&        slot  = m_Slots[m_Slot];
        const auto   found = std::find_if( slot.Graphs.begin(), slot.Graphs.end(), [&]( const GraphHeaps& entry )
                                           { return entry.Graph == graph && !entry.UsedThisFrame; } );
        const size_t index = static_cast<size_t>( found - slot.Graphs.begin() );
        if ( found == slot.Graphs.end() )
            slot.Graphs.push_back( GraphHeaps{ std::string( graph ) } );
        GraphHeaps& entry   = slot.Graphs[index];
        entry.UsedThisFrame = true;

        // Heaps the plan no longer has are retired with everything placed in them.
        for ( uint32_t extra = static_cast<uint32_t>( heaps.size() ); extra < entry.Heaps.size(); ++extra )
            Retire( slot, entry, extra );
        entry.Heaps.resize( std::min( entry.Heaps.size(), heaps.size() ) );
        for ( uint32_t i = 0; i < heaps.size(); ++i )
        {
            const RDG::TransientHeapDesc& want = heaps[i];
            if ( want.Bytes == 0 )
                return Common::MakeFormattedError( "graph '{}': transient heap {} is empty", graph, i );
            if ( i < entry.Heaps.size() )
            {
                const Heap& have = entry.Heaps[i];
                const bool  fits = have.Memory != nullptr && have.Desc.Class == want.Class &&
                                  ( want.MemoryTypeBits & ( 1u << have.MemoryType ) ) != 0 &&
                                  have.Desc.Bytes >= want.Bytes && have.Desc.Alignment >= want.Alignment;
                if ( fits )
                    continue;
                // Grown (or of another class / type): the old heap may still be read by nothing in flight on
                // this slot, but it is retired, not freed, so no bound resource outlives its memory.
                Retire( slot, entry, i );
            }
            Common::ResultStr<Heap> allocated = AllocateHeap( want );
            if ( !allocated )
                return Common::MakeFormattedError( "graph '{}': {}", graph, allocated.GetError() );
            if ( i < entry.Heaps.size() )
                entry.Heaps[i] = allocated.GetValue();
            else
                entry.Heaps.push_back( allocated.GetValue() );
        }
        m_Current = static_cast<int32_t>( index );
        return Common::MakeSuccess( true );
    }

    Common::ResultStr<const VulkanRdgTransientAllocator::Heap*>
    VulkanRdgTransientAllocator::OpenHeap( const RDG::Allocation& allocation, RDG::MemoryClass memoryClass,
                                           std::string_view name ) const
    {
        using Out = const Heap*;
        if ( m_Current < 0 )
            return Common::MakeFormattedError<Out>( "'{}': placed outside ReserveHeaps .. EndGraph", name );
        const GraphHeaps& graph = m_Slots[m_Slot].Graphs[m_Current];
        if ( allocation.Heap >= graph.Heaps.size() )
            return Common::MakeFormattedError<Out>( "'{}': heap {} of {} in graph '{}'", name, allocation.Heap,
                                                    graph.Heaps.size(), graph.Graph );
        const Heap& heap = graph.Heaps[allocation.Heap];
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
        Common::ResultStr<std::shared_ptr<VulkanRdgTexture>> created = VulkanRdgTexture::CreatePlaced(
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
        Common::ResultStr<std::shared_ptr<VulkanRdgBuffer>> created = VulkanRdgBuffer::CreatePlaced(
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
            // A retired heap is still reserved until its slot is re-begun.
            for ( VmaAllocation memory : slot.RetiredHeaps )
            {
                VmaAllocationInfo info{};
                vmaGetAllocationInfo( m_Device.Allocator, memory, &info );
                stats.ReservedBytes += info.size;
                ++stats.HeapCount;
            }
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
