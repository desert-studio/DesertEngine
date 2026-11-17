#include "AllocationCounter.hpp"

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <new>

#if defined( _WIN32 )
#include <malloc.h>
#endif

namespace Common::Memory
{
    namespace
    {
        // One cache line per thread: the owner is the only writer, so it adds with a load + store (no locked
        // read-modify-write); the reader (the profiler) loads. Constant-initialised, so usable before any static
        // constructor runs - operator new is called before main.
        struct alignas( 64 ) Slot
        {
            std::atomic<uint64_t> Allocations{ 0 };
            std::atomic<uint64_t> Bytes{ 0 };
            std::atomic<uint64_t> Frees{ 0 };
        };

        constexpr uint32_t    kSlots = 1024; // threads that ever allocated; later ones share the overflow slot
        Slot                  g_Slots[kSlots];
        Slot                  g_Overflow; // shared: fetch_add
        std::atomic<uint32_t> g_SlotsClaimed{ 0 };
        // Trivially constructed and destroyed: valid on a thread's first allocation and after its TLS teardown.
        thread_local Slot* t_Slot = nullptr;

        Slot& LocalSlot() noexcept
        {
            Slot* slot = t_Slot;
            if ( slot != nullptr )
                return *slot;
            const uint32_t index = g_SlotsClaimed.fetch_add( 1u, std::memory_order_relaxed );
            slot                 = &g_Overflow;
            if ( index < kSlots )
                slot = &g_Slots[index];
            t_Slot = slot;
            return *slot;
        }

        void Bump( std::atomic<uint64_t>& counter, const bool shared, const uint64_t amount ) noexcept
        {
            if ( shared )
            {
                counter.fetch_add( amount, std::memory_order_relaxed );
                return;
            }
            counter.store( counter.load( std::memory_order_relaxed ) + amount, std::memory_order_relaxed );
        }

        void CountAllocation( const std::size_t bytes ) noexcept
        {
            Slot&      slot   = LocalSlot();
            const bool shared = &slot == &g_Overflow;
            Bump( slot.Allocations, shared, 1u );
            Bump( slot.Bytes, shared, static_cast<uint64_t>( bytes ) );
        }

        void CountFree( const void* const pointer ) noexcept
        {
            if ( pointer == nullptr )
                return;
            Slot& slot = LocalSlot();
            Bump( slot.Frees, &slot == &g_Overflow, 1u );
        }

        void* RawAllocate( std::size_t bytes ) noexcept
        {
            if ( bytes == 0 )
                bytes = 1; // a distinct non-null pointer for a zero-size request
            return std::malloc( bytes );
        }

        void* RawAllocateAligned( std::size_t bytes, const std::size_t alignment ) noexcept
        {
            if ( bytes == 0 )
                bytes = 1;
#if defined( _WIN32 )
            return ::_aligned_malloc( bytes, alignment );
#else
            void* memory = nullptr;
            if ( ::posix_memalign( &memory, alignment, bytes ) != 0 )
                return nullptr;
            return memory;
#endif
        }

        void RawFreeAligned( void* const memory ) noexcept
        {
#if defined( _WIN32 )
            ::_aligned_free( memory );
#else
            std::free( memory );
#endif
        }

        // The throwing forms: the standard's new_handler loop; with no handler the process ends here (this engine
        // has no exception path for out-of-memory).
        void* AllocateOrEnd( const std::size_t bytes, const std::size_t alignment ) noexcept
        {
            for ( ;; )
            {
                void* memory = nullptr;
                if ( alignment == 0 )
                    memory = RawAllocate( bytes );
                else
                    memory = RawAllocateAligned( bytes, alignment );
                if ( memory != nullptr )
                {
                    CountAllocation( bytes );
                    return memory;
                }
                const std::new_handler handler = std::get_new_handler();
                if ( handler == nullptr )
                    std::terminate();
                handler();
            }
        }

        void* AllocateOrNull( const std::size_t bytes, const std::size_t alignment ) noexcept
        {
            void* memory = nullptr;
            if ( alignment == 0 )
                memory = RawAllocate( bytes );
            else
                memory = RawAllocateAligned( bytes, alignment );
            if ( memory != nullptr )
                CountAllocation( bytes );
            return memory;
        }

        void Free( void* const memory ) noexcept
        {
            CountFree( memory );
            std::free( memory );
        }

        void FreeAligned( void* const memory ) noexcept
        {
            CountFree( memory );
            RawFreeAligned( memory );
        }
    } // namespace

    AllocationCounts ReadAllocationCounts() noexcept
    {
        AllocationCounts total;
        uint32_t         claimed = g_SlotsClaimed.load( std::memory_order_relaxed );
        if ( claimed > kSlots )
            claimed = kSlots;
        const auto add = [&total]( const Slot& slot )
        {
            total.Allocations += slot.Allocations.load( std::memory_order_relaxed );
            total.Bytes += slot.Bytes.load( std::memory_order_relaxed );
            total.Frees += slot.Frees.load( std::memory_order_relaxed );
        };
        for ( uint32_t index = 0; index < claimed; ++index )
            add( g_Slots[index] );
        add( g_Overflow );
        return total;
    }
} // namespace Common::Memory

// THE PROCESS' REPLACEMENT GLOBAL OPERATORS. They allocate where the defaults did (malloc - mimalloc through its
// redirect in Release/Shipping on Windows; _aligned_malloc / posix_memalign for the aligned forms) and count.
namespace
{
    std::size_t AlignOf( const std::align_val_t alignment ) noexcept
    {
        return static_cast<std::size_t>( alignment );
    }
} // namespace

void* operator new( std::size_t bytes )
{
    return Common::Memory::AllocateOrEnd( bytes, 0 );
}
void* operator new[]( std::size_t bytes )
{
    return Common::Memory::AllocateOrEnd( bytes, 0 );
}
void* operator new( std::size_t bytes, const std::nothrow_t& ) noexcept
{
    return Common::Memory::AllocateOrNull( bytes, 0 );
}
void* operator new[]( std::size_t bytes, const std::nothrow_t& ) noexcept
{
    return Common::Memory::AllocateOrNull( bytes, 0 );
}
void* operator new( std::size_t bytes, std::align_val_t alignment )
{
    return Common::Memory::AllocateOrEnd( bytes, AlignOf( alignment ) );
}
void* operator new[]( std::size_t bytes, std::align_val_t alignment )
{
    return Common::Memory::AllocateOrEnd( bytes, AlignOf( alignment ) );
}
void* operator new( std::size_t bytes, std::align_val_t alignment, const std::nothrow_t& ) noexcept
{
    return Common::Memory::AllocateOrNull( bytes, AlignOf( alignment ) );
}
void* operator new[]( std::size_t bytes, std::align_val_t alignment, const std::nothrow_t& ) noexcept
{
    return Common::Memory::AllocateOrNull( bytes, AlignOf( alignment ) );
}

void operator delete( void* memory ) noexcept
{
    Common::Memory::Free( memory );
}
void operator delete[]( void* memory ) noexcept
{
    Common::Memory::Free( memory );
}
void operator delete( void* memory, std::size_t ) noexcept
{
    Common::Memory::Free( memory );
}
void operator delete[]( void* memory, std::size_t ) noexcept
{
    Common::Memory::Free( memory );
}
void operator delete( void* memory, const std::nothrow_t& ) noexcept
{
    Common::Memory::Free( memory );
}
void operator delete[]( void* memory, const std::nothrow_t& ) noexcept
{
    Common::Memory::Free( memory );
}
void operator delete( void* memory, std::align_val_t ) noexcept
{
    Common::Memory::FreeAligned( memory );
}
void operator delete[]( void* memory, std::align_val_t ) noexcept
{
    Common::Memory::FreeAligned( memory );
}
void operator delete( void* memory, std::size_t, std::align_val_t ) noexcept
{
    Common::Memory::FreeAligned( memory );
}
void operator delete[]( void* memory, std::size_t, std::align_val_t ) noexcept
{
    Common::Memory::FreeAligned( memory );
}
void operator delete( void* memory, std::align_val_t, const std::nothrow_t& ) noexcept
{
    Common::Memory::FreeAligned( memory );
}
void operator delete[]( void* memory, std::align_val_t, const std::nothrow_t& ) noexcept
{
    Common::Memory::FreeAligned( memory );
}
