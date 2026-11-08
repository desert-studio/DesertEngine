#pragma once

#include <Common/Core/Memory/FrameArena.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <new>
#include <type_traits>
#include <utility>

// CPU-RDG-ALLOC (UE: TRDGLambdaPass, the pass lambda FRDGBuilder::AddPass places in FRDGAllocator). A move-only
// type-erased callable whose target never takes a heap box per frame:
//   * a target that fits kInlineBytes (and moves without throwing) lives inside the object;
//   * a larger one spills into the FrameArena the function was bound to (ArenaFunction( arena )), so a frame's
//     node bodies cost bump allocations of the graph's arena, which Reset gives back as a whole;
//   * only an unbound function (no arena) spills a large target onto the heap, and frees it itself.
// The function OWNS its target wherever it lives: its destructor (or a reassignment) runs the target's destructor,
// so every capture (a shared_ptr, a moved vector) is released when the function dies - for a node body, when the
// graph's arena destroys the pass closure holding it at Reset. The arena never runs it a second time: the spilled
// storage registers no destructor of its own.
// Calling an empty function aborts (std::function throws bad_function_call; nothing here throws).
namespace Common::Memory
{
    template <class Signature>
    class ArenaFunction;

    template <class R, class... Args>
    class ArenaFunction<R( Args... )> final
    {
        // Declared first: the constructors' default template arguments name it.
        template <class F>
        static constexpr bool IsTarget = !std::is_same_v<std::remove_cvref_t<F>, ArenaFunction> &&
                                         !std::is_same_v<std::remove_cvref_t<F>, std::nullptr_t> &&
                                         std::is_invocable_r_v<R, std::decay_t<F>&, Args...>;

    public:
        // A node body capturing `this`, a pipeline shared_ptr and a few scalars fits; a push struct or a moved
        // vector pair spills into the arena.
        static constexpr size_t kInlineBytes = 64u;

        ArenaFunction() noexcept = default;
        ArenaFunction( std::nullptr_t ) noexcept
        {
        }
        // Bound to @p arena: a target too large for the inline buffer is placed in it. The arena must outlive the
        // function (a node of the frame graph, added before the graph resets).
        explicit ArenaFunction( FrameArena& arena ) noexcept : m_Arena( &arena )
        {
        }
        template <class F, class = std::enable_if_t<IsTarget<F>>>
        ArenaFunction( F&& target ) noexcept
        {
            Emplace( std::forward<F>( target ) );
        }
        template <class F, class = std::enable_if_t<IsTarget<F>>>
        ArenaFunction( FrameArena& arena, F&& target ) noexcept : m_Arena( &arena )
        {
            Emplace( std::forward<F>( target ) );
        }

        ~ArenaFunction() noexcept
        {
            Clear();
        }

        ArenaFunction( const ArenaFunction& )            = delete;
        ArenaFunction& operator=( const ArenaFunction& ) = delete;

        ArenaFunction( ArenaFunction&& other ) noexcept : m_Arena( other.m_Arena )
        {
            TakeTarget( other );
        }
        // The binding stays this object's when it has one: a node built over the graph's arena keeps spilling
        // into it whatever it is assigned from.
        ArenaFunction& operator=( ArenaFunction&& other ) noexcept
        {
            if ( this == &other )
                return *this;
            Clear();
            if ( m_Arena == nullptr )
                m_Arena = other.m_Arena;
            TakeTarget( other );
            return *this;
        }
        ArenaFunction& operator=( std::nullptr_t ) noexcept
        {
            Clear();
            return *this;
        }
        // Call-site syntax of std::function: `node.Record = [...]( ... ) { ... };` - spills into the bound arena.
        template <class F, class = std::enable_if_t<IsTarget<F>>>
        ArenaFunction& operator=( F&& target ) noexcept
        {
            Clear();
            Emplace( std::forward<F>( target ) );
            return *this;
        }

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return m_Ops != nullptr;
        }

        // Like std::function, const to call and the target invoked as a non-const lvalue (a mutable lambda).
        R operator()( Args... args ) const noexcept
        {
            if ( m_Ops == nullptr )
                std::abort();
            return m_Ops->Invoke( Target(), std::forward<Args>( args )... );
        }

    private:
        template <class T>
        static constexpr bool FitsInline =
             sizeof( T ) <= kInlineBytes && alignof( T ) <= alignof( std::max_align_t ) &&
             std::is_nothrow_move_constructible_v<T>;

        enum class Storage : uint8_t
        {
            Inline, // m_Inline
            Arena,  // m_Spilled, in m_Arena: only the destructor runs
            Heap    // m_Spilled, from the aligned nothrow operator new: destroyed and freed
        };

        struct Ops
        {
            R ( *Invoke )( void* target, Args... args );
            // Inline only: move-constructs the target at @p to and destroys the one at @p from.
            void ( *Relocate )( void* from, void* to ) noexcept;
            void ( *Destroy )( void* target ) noexcept;
            Storage Where;
        };

        template <class T>
        static R InvokeAs( void* target, Args... args )
        {
            return std::invoke( *static_cast<T*>( target ), std::forward<Args>( args )... );
        }
        template <class T>
        static void RelocateAs( void* from, void* to ) noexcept
        {
            T* const source = static_cast<T*>( from );
            ::new ( to ) T( std::move( *source ) );
            source->~T();
        }
        template <class T>
        static void DestroyAs( void* target ) noexcept
        {
            static_cast<T*>( target )->~T();
        }
        template <class T>
        static void DestroyAndFreeAs( void* target ) noexcept
        {
            static_cast<T*>( target )->~T();
            ::operator delete( target, std::align_val_t{ alignof( T ) } );
        }

        template <class T>
        static constexpr Ops kInlineOps{ &InvokeAs<T>, &RelocateAs<T>, &DestroyAs<T>, Storage::Inline };
        template <class T>
        static constexpr Ops kArenaOps{ &InvokeAs<T>, nullptr, &DestroyAs<T>, Storage::Arena };
        template <class T>
        static constexpr Ops kHeapOps{ &InvokeAs<T>, nullptr, &DestroyAndFreeAs<T>, Storage::Heap };

        template <class F>
        void Emplace( F&& target ) noexcept
        {
            using T = std::decay_t<F>;
            if constexpr ( FitsInline<T> )
            {
                ::new ( static_cast<void*>( m_Inline ) ) T( std::forward<F>( target ) );
                m_Ops = &kInlineOps<T>;
            }
            else
            {
                void* storage = nullptr;
                if ( m_Arena != nullptr )
                    storage = m_Arena->Allocate( sizeof( T ), alignof( T ) );
                else
                    storage = ::operator new( sizeof( T ), std::align_val_t{ alignof( T ) }, std::nothrow );
                if ( storage == nullptr )
                    std::abort(); // out of memory: the ArenaAllocator::allocate rule, never a bad_alloc
                ::new ( storage ) T( std::forward<F>( target ) );
                m_Spilled = storage;
                if ( m_Arena != nullptr )
                    m_Ops = &kArenaOps<T>;
                else
                    m_Ops = &kHeapOps<T>;
            }
        }

        // @p other's target becomes this one's; @p other is left empty. This function is empty on entry.
        void TakeTarget( ArenaFunction& other ) noexcept
        {
            if ( other.m_Ops == nullptr )
                return;
            if ( other.m_Ops->Where == Storage::Inline )
                other.m_Ops->Relocate( static_cast<void*>( other.m_Inline ), static_cast<void*>( m_Inline ) );
            else
                m_Spilled = other.m_Spilled;
            m_Ops           = other.m_Ops;
            other.m_Ops     = nullptr;
            other.m_Spilled = nullptr;
        }

        void Clear() noexcept
        {
            if ( m_Ops == nullptr )
                return;
            m_Ops->Destroy( Target() );
            m_Ops     = nullptr;
            m_Spilled = nullptr;
        }

        [[nodiscard]] void* Target() const noexcept
        {
            if ( m_Ops->Where == Storage::Inline )
                return const_cast<void*>( static_cast<const void*>( m_Inline ) );
            return m_Spilled;
        }

        alignas( std::max_align_t ) std::byte m_Inline[kInlineBytes];
        void*       m_Spilled = nullptr;
        const Ops*  m_Ops     = nullptr;
        FrameArena* m_Arena   = nullptr;
    };
} // namespace Common::Memory
