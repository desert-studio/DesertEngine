#pragma once

#include <Common/Core/Memory/FrameArena.hpp>

#include <cstddef>
#include <type_traits>
#include <vector>

// CPU-RDG-ALLOC (UE: FMemStack::Get() + FMemMark, the scratch FRDGBuilder's compile and execute take their
// per-frame arrays from). The working arrays of one Compile or one Execute - the compiler's per-pass and
// per-subresource tables, the execute's resource views, late-fault roots and record chunks - live in the graph's
// scratch arena under a ScratchScope: one bump allocation each, all released at once when the scope ends, and
// the arena's blocks are kept, so a steady frame compiles and executes without touching the heap.
//
// A ScratchAllocator binds, when it is CONSTRUCTED, to the scope open on the constructing thread (none: the
// global heap). So a container made inside the scope takes every later allocation from that scope's arena, and
// a nested or copied container built inside the scope does too (a copy asks the current scope, not the heap).
// Rules a user keeps:
//   * a ScratchVector never outlives the scope it was made in (declare it after the ScratchScope);
//   * a ScratchVector made on the scope's thread is never GROWN on another thread (the arena has no lock) -
//     another thread may read and write its elements; a container grown on a worker is made on that worker
//     (no scope open there: the heap) or stays a std::vector;
//   * nothing allocates from the arena in a nested scope's lifetime but the nested scope's own containers.
namespace Desert::Graphic::RDG
{
    namespace Detail
    {
        // The arena of the ScratchScope open on this thread; null outside every scope.
        inline thread_local Common::Memory::FrameArena* t_RdgScratchArena = nullptr;
    } // namespace Detail

    // Opens @p arena as this thread's scratch until the scope ends, which rewinds the arena to where the scope
    // began (running nothing: scratch holds no registered destructors) and restores the enclosing scope.
    class ScratchScope
    {
    public:
        explicit ScratchScope( Common::Memory::FrameArena& arena ) noexcept
             : m_Previous( Detail::t_RdgScratchArena ), m_Mark( arena )
        {
            Detail::t_RdgScratchArena = &arena;
        }
        ~ScratchScope() noexcept
        {
            // The mark (a member) rewinds after this body: every container of the scope has died by then.
            Detail::t_RdgScratchArena = m_Previous;
        }
        ScratchScope( const ScratchScope& )            = delete;
        ScratchScope& operator=( const ScratchScope& ) = delete;

    private:
        Common::Memory::FrameArena*            m_Previous;
        Common::Memory::FrameArena::ScopedMark m_Mark;
    };

    template <class T>
    class ScratchAllocator
    {
    public:
        using value_type                             = T;
        using propagate_on_container_copy_assignment = std::false_type;
        using propagate_on_container_move_assignment = std::true_type;
        using propagate_on_container_swap            = std::true_type;
        using is_always_equal                        = std::false_type;

        ScratchAllocator() noexcept : m_Arena( Detail::t_RdgScratchArena )
        {
        }
        template <class U>
        ScratchAllocator( const ScratchAllocator<U>& other ) noexcept : m_Arena( other.GetArena() ) // NOLINT
        {
        }

        [[nodiscard]] T* allocate( size_t count ) noexcept
        {
            return Inner().allocate( count );
        }
        void deallocate( T* memory, size_t count ) noexcept
        {
            Inner().deallocate( memory, count );
        }
        // A copy binds to the scope open where it is made (the arena inside the compile, the heap outside it).
        [[nodiscard]] ScratchAllocator select_on_container_copy_construction() const noexcept
        {
            return ScratchAllocator();
        }

        [[nodiscard]] Common::Memory::FrameArena* GetArena() const noexcept
        {
            return m_Arena;
        }

    private:
        [[nodiscard]] Common::Memory::ArenaAllocator<T> Inner() const noexcept
        {
            if ( m_Arena == nullptr )
                return Common::Memory::ArenaAllocator<T>();
            return Common::Memory::ArenaAllocator<T>( *m_Arena );
        }

        Common::Memory::FrameArena* m_Arena = nullptr;
    };

    template <class T, class U>
    [[nodiscard]] bool operator==( const ScratchAllocator<T>& a, const ScratchAllocator<U>& b ) noexcept
    {
        return a.GetArena() == b.GetArena();
    }

    template <class T>
    using ScratchVector = std::vector<T, ScratchAllocator<T>>;
} // namespace Desert::Graphic::RDG
