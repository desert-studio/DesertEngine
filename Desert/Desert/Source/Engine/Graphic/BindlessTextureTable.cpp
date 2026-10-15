#include <Engine/Graphic/BindlessTextureTable.hpp>

#include <Common/Core/Core.hpp>

#include <algorithm>

namespace Desert::Graphic
{
    static_assert( BindlessSamplerFor( BindlessSamplerState( kBindlessSamplerStateCount - 1 ) ).Index ==
                        kBindlessSamplerStateCount - 1,
                   "BindlessSamplerFor and BindlessSamplerState must be inverses" );

    BindlessSlotAllocator::BindlessSlotAllocator( uint32_t capacity )
         : m_Capacity( capacity ), m_IsLive( capacity, false )
    {
        DESERT_VERIFY( capacity > 1, "BindlessSlotAllocator: capacity {} leaves no slot beside the reserved 0",
                       capacity );
    }

    std::optional<uint32_t> BindlessSlotAllocator::Allocate()
    {
        uint32_t slot = 0;
        if ( !m_Free.empty() )
        {
            // Lowest first keeps the live range dense, so the variable-count set a growth allocates stays small.
            const auto lowest = std::min_element( m_Free.begin(), m_Free.end() );
            slot              = *lowest;
            *lowest           = m_Free.back();
            m_Free.pop_back();
        }
        else if ( m_Next < m_Capacity )
            slot = m_Next++;
        else
            return std::nullopt;
        m_IsLive[slot] = true;
        ++m_Live;
        return slot;
    }

    void BindlessSlotAllocator::Release( uint32_t slot, uint64_t frame )
    {
        DESERT_VERIFY( slot != 0 && slot < m_Capacity && m_IsLive[slot],
                       "BindlessSlotAllocator: release of slot {} that is not live (capacity {})", slot,
                       m_Capacity );
        m_IsLive[slot] = false;
        --m_Live;
        m_Parked.push_back( { slot, frame } );
    }

    uint32_t BindlessSlotAllocator::Retire( uint64_t frame, uint32_t framesInFlight )
    {
        uint32_t retired = 0;
        std::erase_if( m_Parked,
                       [&]( const Parked& parked )
                       {
                           if ( frame < parked.Frame + framesInFlight + 1 )
                               return false;
                           m_Free.push_back( parked.Slot );
                           ++retired;
                           return true;
                       } );
        return retired;
    }

    void BindlessSlotAllocator::Grow( uint32_t newCapacity )
    {
        DESERT_VERIFY( newCapacity >= m_Capacity, "BindlessSlotAllocator: grow from {} to the smaller {}",
                       m_Capacity, newCapacity );
        m_Capacity = newCapacity;
        m_IsLive.resize( newCapacity, false );
    }
} // namespace Desert::Graphic
