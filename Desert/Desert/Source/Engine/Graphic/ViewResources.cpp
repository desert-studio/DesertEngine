#include <Engine/Graphic/ViewResources.hpp>

#include <algorithm>
#include <atomic>
#include <format>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

// Compiled on its own by the ViewResources suite, without the engine's forced precompiled header, so every
// include this file needs is spelled out above and nothing here reaches Vulkan, the log or the engine context.

namespace Desert::Graphic
{
    namespace
    {
        // One object for the live list, its lock and the active pointer, so they are constructed and
        // destroyed together. The frame context is NOT here: see FrameContext().
        struct RegistryState
        {
            std::mutex                  Lock;
            std::vector<ViewResources*> Live;
        };

        // THE ACTIVE VIEW IS PER RECORDING THREAD (RenderTask): the render task records a scene view inside its
        // ActiveViewScope while the game thread records outside every view (or a preview inside its own); one
        // process-wide pointer sent the other thread's per-frame writes into the wrong copies. null = the frame
        // context.
        thread_local ViewResources* t_ActiveView = nullptr;

        RegistryState& State()
        {
            static RegistryState state;
            return state;
        }
    } // namespace

    ViewResourceKey ViewResourceKey::Allocate() noexcept
    {
        // Starts at 1: zero is the "no key" value of a default-constructed key.
        static std::atomic<uint64_t> next{ 1 };
        return ViewResourceKey( next.fetch_add( 1, std::memory_order_relaxed ) );
    }

    ViewResources::ViewResources( std::string name ) : m_Name( std::move( name ) )
    {
        ViewResourceRegistry::Register( *this );
    }

    ViewResources::ViewResources( FrameContextTag, std::string name )
         : m_Registered( false ), m_Name( std::move( name ) )
    {
    }

    ViewResources::~ViewResources()
    {
        if ( m_Registered )
            ViewResourceRegistry::Unregister( *this );
        // m_Frames goes with the object; every copy's destructor defers its own GPU release.
    }

    IViewResourceCopy& ViewResources::Acquire( const ViewResourceKey key, const uint32_t frameIndex,
                                               IViewResourceCopyFactory& factory )
    {
        if ( !key.IsValid() )
            throw std::invalid_argument( "ViewResources::Acquire: unassigned ViewResourceKey in view '" + m_Name +
                                         "'" );

        if ( frameIndex >= m_Frames.size() )
            m_Frames.resize( static_cast<size_t>( frameIndex ) + 1 );

        std::unique_ptr<IViewResourceCopy>& slot = m_Frames[frameIndex][key.Value()];
        if ( !slot )
        {
            slot = factory.CreateViewCopy( m_Name, frameIndex );
            if ( !slot )
            {
                m_Frames[frameIndex].erase( key.Value() );
                throw std::logic_error( std::format(
                     "ViewResources::Acquire: factory returned no copy for key {}, frame {}, view '{}'",
                     key.Value(), frameIndex, m_Name ) );
            }
        }
        return *slot;
    }

    IViewResourceCopy* ViewResources::Find( const ViewResourceKey key, const uint32_t frameIndex ) const
    {
        if ( frameIndex >= m_Frames.size() )
            return nullptr;
        const auto it = m_Frames[frameIndex].find( key.Value() );
        return it == m_Frames[frameIndex].end() ? nullptr : it->second.get();
    }

    uint32_t ViewResources::Forget( const ViewResourceKey key )
    {
        uint32_t dropped = 0;
        for ( CopyMap& frame : m_Frames )
            dropped += static_cast<uint32_t>( frame.erase( key.Value() ) );
        return dropped;
    }

    void ViewResources::Clear()
    {
        m_Frames.clear();
    }

    uint32_t ViewResources::CopyCount() const noexcept
    {
        size_t count = 0;
        for ( const CopyMap& frame : m_Frames )
            count += frame.size();
        return static_cast<uint32_t>( count );
    }

    uint32_t ViewResourceRegistry::LiveCount()
    {
        RegistryState&                    state = State();
        const std::lock_guard<std::mutex> guard( state.Lock );
        return static_cast<uint32_t>( state.Live.size() );
    }

    uint32_t ViewResourceRegistry::Forget( const ViewResourceKey key )
    {
        RegistryState&                    state = State();
        const std::lock_guard<std::mutex> guard( state.Lock );
        uint32_t                          dropped = FrameContext().Forget( key );
        for ( ViewResources* view : state.Live )
            dropped += view->Forget( key );
        return dropped;
    }

    ViewResources& ViewResourceRegistry::FrameContext()
    {
        // A function-local static, deliberately not inside RegistryState: constructing it registers nothing
        // (the tag constructor), so it cannot recurse into State(). Its copies are given back by Clear() at
        // renderer shutdown, before the allocator's queue is drained; by static destruction it is empty.
        static ViewResources frameContext( ViewResources::FrameContextTag{}, "frame context" );
        return frameContext;
    }

    ViewResources& ViewResourceRegistry::Active()
    {
        return t_ActiveView != nullptr ? *t_ActiveView : FrameContext();
    }

    void ViewResourceRegistry::Register( ViewResources& view )
    {
        RegistryState&                    state = State();
        const std::lock_guard<std::mutex> guard( state.Lock );
        state.Live.push_back( &view );
    }

    void ViewResourceRegistry::Unregister( ViewResources& view )
    {
        RegistryState&                    state = State();
        const std::lock_guard<std::mutex> guard( state.Lock );
        std::erase( state.Live, &view );
        // A view destroyed while it is the active one must not leave a dangling target behind: the next
        // write outside a view would land in freed memory.
        if ( t_ActiveView == &view )
            t_ActiveView = nullptr;
    }

    ViewResources* ViewResourceRegistry::ExchangeActive( ViewResources* view )
    {
        ViewResources* const previous = t_ActiveView;
        t_ActiveView                  = view;
        return previous;
    }

    void ViewResourceRegistry::RestoreActive( ViewResources* previous )
    {
        RegistryState&                    state = State();
        const std::lock_guard<std::mutex> guard( state.Lock );
        // The view that was active before the scope opened may have been destroyed inside it; restoring it
        // then would re-arm a dangling pointer, so a view no longer on the live list restores the frame context.
        const bool stillLive = previous != nullptr &&
                               std::find( state.Live.begin(), state.Live.end(), previous ) != state.Live.end();
        t_ActiveView = stillLive ? previous : nullptr;
    }

    ActiveViewScope::ActiveViewScope( ViewResources& view ) noexcept
         : m_Previous( ViewResourceRegistry::ExchangeActive( &view ) )
    {
    }

    ActiveViewScope::~ActiveViewScope() noexcept
    {
        ViewResourceRegistry::RestoreActive( m_Previous );
    }
} // namespace Desert::Graphic
