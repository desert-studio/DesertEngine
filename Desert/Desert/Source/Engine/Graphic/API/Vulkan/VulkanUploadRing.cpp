#include <Engine/Graphic/API/Vulkan/VulkanUploadRing.hpp>

#include <algorithm>

namespace Desert::Graphic::API::Vulkan
{
    namespace
    {
        constexpr uint64_t AlignUp( const uint64_t value, const uint64_t alignment ) noexcept
        {
            return ( value + alignment - 1 ) / alignment * alignment;
        }

        constexpr double ToMiB( const uint64_t bytes ) noexcept
        {
            return static_cast<double>( bytes ) / ( 1024.0 * 1024.0 );
        }
    } // namespace

    VulkanUploadRing& VulkanUploadRing::Get() noexcept
    {
        // A function static, not a Common::Singleton: the leases of the last textures may be dropped after
        // VulkanContext::Shutdown (Destroy); this object outlives them all, and an empty ring retires into
        // nothing.
        static VulkanUploadRing s_Ring;
        return s_Ring;
    }

    bool VulkanUploadRing::CreateLocked() noexcept
    {
        VmaAllocator vma = VulkanAllocator::GetVMAAllocator();
        if ( vma == VK_NULL_HANDLE )
            return false;
        const VkBufferCreateInfo info{ .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                       .size        = kCapacity,
                                       .usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                       .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
        VmaAllocationCreateInfo  alloc{};
        // SYSTEM MEMORY, WRITTEN ONCE IN ORDER BY THE CPU, READ ONCE BY THE COPY ENGINE: AUTO_PREFER_HOST keeps it
        // out of the 256 MB BAR heap (DEVICE_LOCAL | HOST_VISIBLE) that SEQUENTIAL_WRITE alone could pick on a
        // resizable-BAR device, where 192 MB of staging would crowd the per-frame uniform rings.
        alloc.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        alloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT |
                      VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
        alloc.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        VmaAllocationInfo made{};
        const VkResult    result = vmaCreateBuffer( vma, &info, &alloc, &m_Buffer, &m_Allocation, &made );
        if ( result != VK_SUCCESS || made.pMappedData == nullptr )
        {
            LOG_ERROR( "[Upload] ring: the {} MB host staging buffer was not made (VkResult {}); every texture "
                       "upload takes the per-level path, staged by the main thread",
                       kCapacity / ( 1024ull * 1024ull ), static_cast<int>( result ) );
            if ( result == VK_SUCCESS )
                vmaDestroyBuffer( vma, m_Buffer, m_Allocation );
            m_Buffer     = VK_NULL_HANDLE;
            m_Allocation = nullptr;
            return false;
        }
        vmaSetAllocationName( vma, m_Allocation, "UploadRing" );
        m_Mapped                    = static_cast<std::byte*>( made.pMappedData );
        VkMemoryPropertyFlags flags = 0;
        vmaGetMemoryTypeProperties( vma, made.memoryType, &flags );
        LOG_INFO( "[Upload] ring: {} x {} MB host staging, memory type {} ({})", kSlices,
                  kSliceBytes / ( 1024ull * 1024ull ), made.memoryType,
                  ( flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT ) != 0 ? "DEVICE_LOCAL: BAR" : "system memory" );
        return true;
    }

    VulkanUploadRing::Reservation VulkanUploadRing::Reserve( const uint64_t size ) noexcept
    {
        const uint64_t         asked = AlignUp( std::max<uint64_t>( size, 1 ), kAlignment );
        const std::scoped_lock lock( m_Mutex );
        if ( m_Buffer == VK_NULL_HANDLE && !m_Refused && !CreateLocked() )
            m_Refused = true;
        if ( m_Refused || asked > kCapacity )
        {
            ++m_Stats.FullCount;
            LogLocked();
            return Full{ size, m_Stats.BytesInFlight, kCapacity, true };
        }

        // [m_Head, tail) is free when the head is behind the oldest live slice, [m_Head, end) + [0, tail) when it
        // is ahead of it; head == tail with a slice alive is a ring with no byte left.
        bool     fits    = false;
        uint64_t offset  = 0;
        uint64_t charged = asked;
        if ( m_Live.empty() )
        {
            m_Head = 0;
            fits   = true;
        }
        else
        {
            const uint64_t tail = m_Live.front().Offset;
            if ( m_Head > tail )
            {
                if ( m_Head + asked <= kCapacity )
                {
                    offset = m_Head;
                    fits   = true;
                }
                else if ( asked <= tail )
                {
                    // The wrap: the bytes from the head to the end stay unused until this slice retires.
                    charged = asked + ( kCapacity - m_Head );
                    fits    = true;
                }
            }
            else if ( m_Head + asked <= tail )
            {
                offset = m_Head;
                fits   = true;
            }
        }
        if ( !fits )
        {
            ++m_Stats.FullCount;
            LogLocked();
            return Full{ size, m_Stats.BytesInFlight, kCapacity, false };
        }

        const uint64_t id = ++m_NextId;
        m_Live.push_back( Live{ id, offset, charged, false } );
        m_Head = offset + asked;
        m_Stats.BytesInFlight += charged;
        m_Stats.PeakBytesInFlight = std::max( m_Stats.PeakBytesInFlight, m_Stats.BytesInFlight );
        ++m_Stats.Reservations;
        LogLocked();

        Slice slice;
        slice.Buffer = m_Buffer;
        slice.Offset = offset;
        slice.Size   = size;
        slice.Mapped = m_Mapped + offset;
        slice.Lease  = std::shared_ptr<const void>( nullptr, [this, id]( const void* ) { Retire( id ); } );
        return slice;
    }

    void VulkanUploadRing::Retire( const uint64_t id ) noexcept
    {
        const std::scoped_lock lock( m_Mutex );
        RetireLocked( id );
        LogLocked();
    }

    void VulkanUploadRing::RetireLocked( const uint64_t id ) noexcept
    {
        const auto it =
             std::find_if( m_Live.begin(), m_Live.end(), [id]( const Live& live ) { return live.Id == id; } );
        if ( it == m_Live.end() || it->Retired )
            return; // Destroy ran first: the slice went with the buffer
        it->Retired = true;
        m_Stats.BytesInFlight -= it->Size;
        while ( !m_Live.empty() && m_Live.front().Retired )
            m_Live.pop_front();
        if ( m_Live.empty() )
            m_Head = 0; // nothing alive: the next slice starts at 0, the longest run the ring has
    }

    VulkanUploadRing::Stats VulkanUploadRing::GetStats() const noexcept
    {
        const std::scoped_lock lock( m_Mutex );
        return m_Stats;
    }

    void VulkanUploadRing::LogLocked() noexcept
    {
        // THE '[Upload] ring' LINE: what the ring holds for the GPU and how often a reservation did not fit. At
        // most every two seconds and only when a count moved, so a streaming walk logs its curve, an idle frame
        // nothing.
        const auto now = std::chrono::steady_clock::now();
        if ( now - m_LastLog < std::chrono::seconds( 2 ) )
            return;
        if ( m_Stats.Reservations == m_Logged.Reservations && m_Stats.FullCount == m_Logged.FullCount &&
             m_Stats.BytesInFlight == m_Logged.BytesInFlight )
            return;
        m_LastLog = now;
        m_Logged  = m_Stats;
        LOG_INFO( "[Upload] ring: {:.1f} MB in flight of {} MB (peak {:.1f} MB), {} slices, {} full",
                  ToMiB( m_Stats.BytesInFlight ), kCapacity / ( 1024ull * 1024ull ),
                  ToMiB( m_Stats.PeakBytesInFlight ), m_Stats.Reservations, m_Stats.FullCount );
    }

    void VulkanUploadRing::Destroy() noexcept
    {
        const std::scoped_lock lock( m_Mutex );
        if ( m_Buffer != VK_NULL_HANDLE )
            if ( VmaAllocator vma = VulkanAllocator::GetVMAAllocator(); vma != VK_NULL_HANDLE )
                vmaDestroyBuffer( vma, m_Buffer, m_Allocation );
        m_Buffer     = VK_NULL_HANDLE;
        m_Allocation = nullptr;
        m_Mapped     = nullptr;
        m_Refused    = true; // no ring is made again after the device's teardown
        m_Live.clear();
        m_Head                = 0;
        m_Stats.BytesInFlight = 0;
    }
} // namespace Desert::Graphic::API::Vulkan
