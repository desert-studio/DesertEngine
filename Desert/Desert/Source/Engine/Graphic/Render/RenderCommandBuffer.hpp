#pragma once

#include "RenderCommand.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>
#include <vector>

namespace Desert::Graphic::Render
{
    // Per-frame render command list backed by a reusable paged arena. Commands are placement-new'd into
    // arena pages instead of one heap allocation each (make_unique was ~256 debug-heap allocs + frees per
    // frame for the static-mesh path alone). Clear() runs the command destructors and rewinds the arena,
    // keeping the page memory for next frame — so steady-state frames do ZERO command allocations.
    //
    // Lifetime: command pointers stay valid until Clear() (pages are never freed/reallocated mid-frame).
    // Usage contract: Emplace* during collection, then ExecuteAll() once, then Clear() once (between frames).
    class RenderCommandBuffer
    {
    public:
        RenderCommandBuffer() = default;
        ~RenderCommandBuffer() { DestroyCommands(); }

        RenderCommandBuffer( const RenderCommandBuffer& )            = delete;
        RenderCommandBuffer& operator=( const RenderCommandBuffer& ) = delete;

        template <typename T, typename... Args>
        void Emplace( Args&&... args )
        {
            static_assert( std::is_base_of_v<RenderCommand, T>, "T must derive from RenderCommand" );
            void* mem = Allocate( sizeof( T ), alignof( T ) );
            m_Commands.push_back( new ( mem ) T( std::forward<Args>( args )... ) );
        }

        void ExecuteAll( Graphic::SceneRenderer& renderer )
        {
            for ( auto* cmd : m_Commands )
            {
                cmd->Execute( renderer );
            }
        }

        void Clear()
        {
            DestroyCommands();
            m_Commands.clear(); // keeps capacity for next frame
            m_CurrentPage = 0;  // rewind the arena; pages are reused, not freed
            m_PageOffset  = 0;
        }

    private:
        struct Page
        {
            std::unique_ptr<std::byte[]> Data;
            std::size_t                  Size = 0;
        };

        static constexpr std::size_t kPageSize = std::size_t{ 64 } * 1024;

        void DestroyCommands()
        {
            for ( auto* cmd : m_Commands )
                cmd->~RenderCommand(); // virtual: runs the concrete command's destructor (vectors, etc.)
        }

        void AddPage( std::size_t minSize )
        {
            const std::size_t size = minSize > kPageSize ? minSize : kPageSize;
            m_Pages.push_back( { std::make_unique<std::byte[]>( size ), size } );
        }

        void* Allocate( std::size_t size, std::size_t align )
        {
            if ( m_Pages.empty() )
                AddPage( size );

            for ( ;; )
            {
                Page&             page    = m_Pages[m_CurrentPage];
                const std::size_t aligned = ( m_PageOffset + ( align - 1 ) ) & ~( align - 1 );
                if ( aligned + size <= page.Size )
                {
                    m_PageOffset = aligned + size;
                    return page.Data.get() + aligned;
                }

                // Doesn't fit the current page — advance to the next (allocating one if needed).
                ++m_CurrentPage;
                m_PageOffset = 0;
                if ( m_CurrentPage >= m_Pages.size() )
                    AddPage( size );
            }
        }

        std::vector<Page>           m_Pages;
        std::size_t                 m_CurrentPage = 0;
        std::size_t                 m_PageOffset  = 0;
        std::vector<RenderCommand*> m_Commands;
    };

} // namespace Desert::Graphic::Render
