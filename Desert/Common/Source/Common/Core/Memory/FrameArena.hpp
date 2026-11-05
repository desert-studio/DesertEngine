#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

// CPU-FRAME-ARENA (UE: FMemStack / FRDGAllocator). A linear allocator for memory whose owners all die together
// (one render graph's records, names and pass execs; one recording chunk's barrier and render-pass scratch):
//   * Allocate bumps a pointer in the current block; ArenaAllocator::deallocate does nothing.
//   * New<T> constructs an object in the arena and registers its destructor; Reset (or Rewind to a Mark) runs
//     the registered destructors newest first, then rewinds every block.
//   * Blocks are kept across Reset, so a steady frame takes nothing from the heap once its peak fits; Reset
//     folds a chain of several blocks into ONE block of their total size, so a steady frame works in one block.
//   * Exhaustion grows a new block (double the last, at least the request); nothing throws. Allocate answers
//     nullptr only when the system refuses a block (out of memory).
// Not thread-safe: one owner thread at a time (a view's graph on the render thread, a chunk on its worker).
namespace Common::Memory
{
    class FrameArena final
    {
        struct Block;
        struct DestructorNode;

    public:
        static constexpr size_t kDefaultBlockBytes = 16u * 1024u;
        static constexpr size_t kMaxGrowBytes      = 4u * 1024u * 1024u; // the doubling stops here
        // A request (size or alignment) above this is refused with nullptr: the block arithmetic below
        // (at + bytes, bytes + alignment, header + bytes) cannot wrap under it.
        static constexpr size_t kMaxRequestBytes = ~size_t{ 0 } / 4u;

        struct Stats
        {
            size_t   UsedBytes     = 0; // allocated since the last Reset, alignment padding included
            size_t   PeakBytes     = 0; // the most in use at once since construction (Rewind and Reset note it)
            size_t   ReservedBytes = 0; // the bytes of every block held
            uint32_t Blocks        = 0;
        };

        // Where Rewind returns to (UE: FMemMark). Taken and rewound in stack order on the owner thread.
        struct Mark
        {
            Block*          At          = nullptr;
            size_t          Used        = 0;
            DestructorNode* Destructors = nullptr;
            size_t          UsedBytes   = 0;
        };

        // A Mark rewound when the scope ends: a function's scratch (UE: FMemMark on FMemStack::Get()).
        class ScopedMark
        {
        public:
            explicit ScopedMark( FrameArena& arena ) noexcept : m_Arena( arena ), m_Mark( arena.TakeMark() )
            {
            }
            ~ScopedMark() noexcept
            {
                m_Arena.Rewind( m_Mark );
            }
            ScopedMark( const ScopedMark& )            = delete;
            ScopedMark& operator=( const ScopedMark& ) = delete;

        private:
            FrameArena& m_Arena;
            Mark        m_Mark;
        };

        explicit FrameArena( size_t blockBytes = kDefaultBlockBytes ) noexcept
             : m_NextBlockBytes( blockBytes < 256u ? 256u : blockBytes )
        {
        }
        ~FrameArena() noexcept
        {
            RunDestructors( nullptr );
            FreeBlocks();
        }
        FrameArena( const FrameArena& )            = delete;
        FrameArena& operator=( const FrameArena& ) = delete;

        // @p alignment is a power of two. Never nullptr unless the system refuses a new block or the request is
        // above kMaxRequestBytes.
        [[nodiscard]] void* Allocate( size_t bytes, size_t alignment ) noexcept
        {
            if ( bytes > kMaxRequestBytes || alignment > kMaxRequestBytes )
                return nullptr;
            if ( bytes == 0 )
                bytes = 1;
            for ( Block* block = m_Current; block != nullptr; block = block->Next )
            {
                // Blocks after the current one are empty (an allocation only moves forward; Reset / Rewind
                // empty everything after the point they return to).
                const std::uintptr_t base = reinterpret_cast<uintptr_t>( DataOf( block ) );
                const std::uintptr_t at   = AlignUp( base + block->Used, alignment );
                if ( at + bytes <= base + block->Size )
                {
                    m_UsedBytes += at + bytes - ( base + block->Used );
                    block->Used = at + bytes - base;
                    m_Current   = block;
                    return reinterpret_cast<void*>( at );
                }
            }
            Block* const grown = AppendBlock( bytes + alignment );
            if ( grown == nullptr )
                return nullptr;
            const std::uintptr_t base = reinterpret_cast<uintptr_t>( DataOf( grown ) );
            const std::uintptr_t at   = AlignUp( base, alignment );
            m_UsedBytes += at + bytes - base;
            grown->Used = at + bytes - base;
            m_Current   = grown;
            return reinterpret_cast<void*>( at );
        }

        // Constructs a T in the arena; its destructor runs at the Reset / Rewind that releases it (none is
        // registered for a trivially destructible T). nullptr when the arena could not grow: nothing was built.
        template <class T, class... Args>
        [[nodiscard]] T* New( Args&&... args ) noexcept
        {
            if constexpr ( std::is_trivially_destructible_v<T> )
            {
                void* const storage = Allocate( sizeof( T ), alignof( T ) );
                if ( storage == nullptr )
                    return nullptr;
                return ::new ( storage ) T( std::forward<Args>( args )... );
            }
            else
            {
                // The node first: once the object exists its destructor is registered, whatever happens next.
                void* const node = Allocate( sizeof( DestructorNode ), alignof( DestructorNode ) );
                if ( node == nullptr )
                    return nullptr;
                void* const storage = Allocate( sizeof( T ), alignof( T ) );
                if ( storage == nullptr )
                    return nullptr;
                T* const object = ::new ( storage ) T( std::forward<Args>( args )... );
                m_Destructors   = ::new ( node ) DestructorNode{ &DestroyAs<T>, object, m_Destructors };
                return object;
            }
        }

        [[nodiscard]] Mark TakeMark() const noexcept
        {
            Mark mark;
            mark.At          = m_Current;
            mark.Used        = m_Current != nullptr ? m_Current->Used : size_t{ 0 };
            mark.Destructors = m_Destructors;
            mark.UsedBytes   = m_UsedBytes;
            return mark;
        }

        // Runs the destructors registered after @p mark (newest first) and returns the arena to it.
        void Rewind( const Mark& mark ) noexcept
        {
            // A ScopedMark's scratch counts towards the peak too, before the rewind gives it back.
            if ( m_UsedBytes > m_PeakBytes )
                m_PeakBytes = m_UsedBytes;
            RunDestructors( mark.Destructors );
            Block* const from = mark.At != nullptr ? mark.At->Next : m_First;
            for ( Block* block = from; block != nullptr; block = block->Next )
                block->Used = 0;
            if ( mark.At != nullptr )
            {
                mark.At->Used = mark.Used;
                m_Current     = mark.At;
            }
            else
                m_Current = m_First;
            m_UsedBytes = mark.UsedBytes;
        }

        // Runs every registered destructor (newest first) and empties the arena; no Mark may be live. A chain of
        // several blocks is replaced by one block of their total size (kept as it is if the system refuses it).
        void Reset() noexcept
        {
            Rewind( Mark{} ); // notes the peak first
            if ( m_First == nullptr || m_First->Next == nullptr )
                return;
            size_t total = 0;
            for ( const Block* block = m_First; block != nullptr; block = block->Next )
                total += block->Size;
            Block* const single = MakeBlock( total );
            if ( single == nullptr )
                return;
            FreeBlocks();
            m_First   = single;
            m_Last    = single;
            m_Current = single;
        }

        [[nodiscard]] Stats GetStats() const noexcept
        {
            Stats stats;
            stats.UsedBytes = m_UsedBytes;
            stats.PeakBytes = m_UsedBytes > m_PeakBytes ? m_UsedBytes : m_PeakBytes;
            for ( const Block* block = m_First; block != nullptr; block = block->Next )
            {
                stats.ReservedBytes += block->Size;
                ++stats.Blocks;
            }
            return stats;
        }

    private:
        // The header in front of a block's bytes; its alignment keeps the bytes max_align_t aligned.
        struct alignas( alignof( std::max_align_t ) ) Block
        {
            Block* Next = nullptr;
            size_t Size = 0; // usable bytes after the header
            size_t Used = 0;
        };
        struct DestructorNode
        {
            void ( *Destroy )( void* ) noexcept;
            void*           Object;
            DestructorNode* Next;
        };

        template <class T>
        static void DestroyAs( void* object ) noexcept
        {
            static_cast<T*>( object )->~T();
        }
        [[nodiscard]] static std::byte* DataOf( Block* block ) noexcept
        {
            return reinterpret_cast<std::byte*>( block + 1 );
        }
        [[nodiscard]] static std::uintptr_t AlignUp( std::uintptr_t value, size_t alignment ) noexcept
        {
            return ( value + ( alignment - 1u ) ) & ~static_cast<std::uintptr_t>( alignment - 1u );
        }
        [[nodiscard]] static Block* MakeBlock( size_t bytes ) noexcept
        {
            void* const memory = std::malloc( sizeof( Block ) + bytes );
            if ( memory == nullptr )
                return nullptr;
            Block* const block = ::new ( memory ) Block{};
            block->Size        = bytes;
            return block;
        }
        [[nodiscard]] Block* AppendBlock( size_t atLeast ) noexcept
        {
            size_t bytes = m_NextBlockBytes;
            if ( bytes < atLeast )
                bytes = atLeast;
            Block* const block = MakeBlock( bytes );
            if ( block == nullptr )
                return nullptr;
            if ( m_Last != nullptr )
                m_Last->Next = block;
            else
                m_First = block;
            m_Last = block;
            if ( m_NextBlockBytes < kMaxGrowBytes )
                m_NextBlockBytes *= 2u;
            return block;
        }
        void RunDestructors( const DestructorNode* until ) noexcept
        {
            while ( m_Destructors != nullptr && m_Destructors != until )
            {
                DestructorNode* const node = m_Destructors;
                m_Destructors              = node->Next;
                node->Destroy( node->Object );
            }
        }
        void FreeBlocks() noexcept
        {
            Block* block = m_First;
            while ( block != nullptr )
            {
                Block* const next = block->Next;
                std::free( block );
                block = next;
            }
            m_First   = nullptr;
            m_Last    = nullptr;
            m_Current = nullptr;
        }

        Block*          m_First          = nullptr;
        Block*          m_Last           = nullptr;
        Block*          m_Current        = nullptr;
        DestructorNode* m_Destructors    = nullptr;
        size_t          m_UsedBytes      = 0;
        size_t          m_PeakBytes      = 0;
        size_t          m_NextBlockBytes = kDefaultBlockBytes;
    };

    // A standard allocator over a FrameArena: containers whose storage dies with the arena's next Reset. A
    // default-constructed one (no arena) is the global heap. A COPY of an arena container leaves the arena
    // (select_on_container_copy_construction answers the heap), so a copy kept past the Reset stays valid.
    template <class T>
    class ArenaAllocator
    {
    public:
        using value_type                             = T;
        using propagate_on_container_copy_assignment = std::false_type;
        using propagate_on_container_move_assignment = std::true_type;
        using propagate_on_container_swap            = std::true_type;
        using is_always_equal                        = std::false_type;

        ArenaAllocator() noexcept = default;
        explicit ArenaAllocator( FrameArena& arena ) noexcept : m_Arena( &arena )
        {
        }
        template <class U>
        ArenaAllocator( const ArenaAllocator<U>& other ) noexcept : m_Arena( other.GetArena() ) // NOLINT
        {
        }

        // No exception path: the heap branch asks the nothrow operator new, and either branch's refusal (out of
        // memory - a container cannot take a null allocation) ends the process here, never as a bad_alloc.
        [[nodiscard]] T* allocate( size_t count ) noexcept
        {
            const size_t bytes = count * sizeof( T );
            void*        memory = nullptr;
            if ( m_Arena == nullptr )
            {
                if constexpr ( alignof( T ) > __STDCPP_DEFAULT_NEW_ALIGNMENT__ )
                    memory = ::operator new( bytes, std::align_val_t{ alignof( T ) }, std::nothrow );
                else
                    memory = ::operator new( bytes, std::nothrow );
            }
            else
                memory = m_Arena->Allocate( bytes, alignof( T ) );
            if ( memory == nullptr )
                std::abort();
            return static_cast<T*>( memory );
        }
        void deallocate( T* memory, size_t count ) noexcept
        {
            if ( m_Arena != nullptr )
                return; // released by the arena's Reset / Rewind
            if constexpr ( alignof( T ) > __STDCPP_DEFAULT_NEW_ALIGNMENT__ )
                ::operator delete( memory, count * sizeof( T ), std::align_val_t{ alignof( T ) } );
            else
                ::operator delete( memory, count * sizeof( T ) );
        }
        [[nodiscard]] ArenaAllocator select_on_container_copy_construction() const noexcept
        {
            return ArenaAllocator();
        }

        [[nodiscard]] FrameArena* GetArena() const noexcept
        {
            return m_Arena;
        }

    private:
        FrameArena* m_Arena = nullptr;
    };

    template <class T, class U>
    [[nodiscard]] bool operator==( const ArenaAllocator<T>& a, const ArenaAllocator<U>& b ) noexcept
    {
        return a.GetArena() == b.GetArena();
    }

    template <class T>
    using ArenaVector = std::vector<T, ArenaAllocator<T>>;
    using ArenaString = std::basic_string<char, std::char_traits<char>, ArenaAllocator<char>>;
} // namespace Common::Memory
