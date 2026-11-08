#include "ImageService.hpp"

#include <Common/Core/Logger.hpp>
#include <Engine/Core/EngineContext.hpp>
#include <Engine/Graphic/RendererContext.hpp>

namespace Desert::Runtime
{
    namespace
    {
        // RESOLVE'S RAW POINTER OUTLIVES EVERY FRAME THAT MAY HAVE READ IT. A render worker of frame N-1
        // resolves a handle while the game thread of frame N unregisters or replaces its image: the outgoing
        // image is retired through the renderer's frame-retire queue (UE: deferred deletion), and dies at
        // once only with no renderer. Called after m_Lock is released.
        void RetireImage( std::shared_ptr<Graphic::Image>&& outgoing ) noexcept
        {
            if ( !outgoing )
                return;
            const auto ctx = EngineContext::GetInstance().GetRendererContext();
            if ( !ctx )
                return; // `outgoing` dies here
            ctx->RetireAfterFramesInFlight( [held = std::move( outgoing )]() mutable { held.reset(); } );
        }
    } // namespace
    ImageHandle ImageService::Register( std::shared_ptr<Graphic::Image>&& image, ImageHandle::Type type ) noexcept
    {
        // Exclusive: Allocate may grow the pool's vector and the two resizes below may move ours -- the
        // crash this lock exists for was two Registers (SkyCapture on the main thread, a streaming job)
        // growing HandlePool::m_Entries under each other.
        std::unique_lock lock( m_Lock );
        const auto       handle = m_HandlePool.Allocate();

        if ( handle.Index >= m_Images.size() )
            m_Images.resize( handle.Index + 1 );

        // The generation the slot was handed out under, kept BESIDE the image so Resolve can tell a live
        // handle from a stale one. See Resolve for what it cost not to have it.
        if ( handle.Index >= m_Generations.size() )
            m_Generations.resize( handle.Index + 1, 0U );
        m_Generations[handle.Index] = handle.Generation;

        m_Images[handle.Index] = std::move( image );

        return ImageHandle{ handle, type };
    }

    void ImageService::Unregister( const ImageHandle& handle ) noexcept
    {
        // AN EMPTY HANDLE NAMES NO IMAGE, AND ASKING TO RELEASE NOTHING IS NOT AN ERROR. Without this
        // line an empty handle (index 0, generation 0) falls into the stale-handle branch below and is
        // reported as "something is holding an image handle past the image's life" — which would now be
        // printed on every environment rebake, because an Environment's three cubes are released
        // together, unconditionally, by `SkyboxRenderer`,
        // and the procedural sky's radiance slot is routinely empty by design (SceneEnvironment.cpp).
        // A diagnostic that fires for a legal state is a diagnostic people learn to scroll past, and
        // the one below is load-bearing.
        if ( !handle.IsValid() )
            return;

        // THE POOL AND THE VECTOR USED TO DISAGREE, AND ONLY THE POOL CHECKED. `HandlePool::Release`
        // refuses an out-of-range index and refuses a generation mismatch — and the line beneath it reset
        // `m_Images[Index]` unconditionally, so a stale or double Unregister cleared whichever image had
        // since moved into that slot while the pool correctly did nothing. Two things that must agree,
        // one of which was checked.
        // The released image leaves the slot under the lock and is destroyed at function end, after the
        // inner scope has unlocked: its destructor never runs inside the service's lock.
        std::shared_ptr<Graphic::Image> released;
        uint32_t                        staleSlotGeneration = 0U;
        bool                            stale               = false;
        {
            std::unique_lock lock( m_Lock );
            if ( handle.Value.Index >= m_Images.size() )
                return;
            if ( handle.Value.Index < m_Generations.size() &&
                 m_Generations[handle.Value.Index] != handle.Value.Generation )
            {
                stale               = true;
                staleSlotGeneration = m_Generations[handle.Value.Index];
            }
            else
            {
                m_HandlePool.Release( handle.Value );
                released.swap( m_Images[handle.Value.Index] );
                if ( handle.Value.Index < m_Generations.size() )
                    m_Generations[handle.Value.Index] = 0U;
            }
        }

        RetireImage( std::move( released ) );

        if ( stale )
        {
            LOG_ERROR( "[ImageService] Unregister was given a handle for slot {} generation {}, but that "
                       "slot now holds generation {}. The handle is stale; nothing was released. Something "
                       "is holding an image handle past the image's life.",
                       handle.Value.Index, handle.Value.Generation, staleSlotGeneration );
        }
    }

    bool ImageService::Replace( const ImageHandle& handle, std::shared_ptr<Graphic::Image>&& image ) noexcept
    {
        if ( !image || !handle.IsValid() )
            return false;
        // The outgoing image is destroyed after the lock is released (see Unregister).
        std::shared_ptr<Graphic::Image> outgoing;
        {
            std::unique_lock lock( m_Lock );
            // IsLiveLocked answers false for exactly the empty, out-of-range and stale handles.
            if ( !IsLiveLocked( handle ) )
                return false;
            outgoing                     = std::move( m_Images[handle.Value.Index] );
            m_Images[handle.Value.Index] = std::move( image );
        }
        RetireImage( std::move( outgoing ) );
        return true;
    }

    void ImageService::Clear() noexcept
    {
        // The vector is INDEXED by handle, so the handle pool has to be reset with it or the next
        // Allocate() would hand out an index into a table that no longer describes anything.
        // The images are swapped out under the lock and destroyed after it (see Unregister).
        std::vector<std::shared_ptr<Graphic::Image>> outgoing;
        {
            std::unique_lock lock( m_Lock );
            outgoing.swap( m_Images );
            m_Generations.clear();
            m_HandlePool = Common::Core::HandlePool{};
        }
    }

    Graphic::Image* ImageService::Resolve( const ImageHandle& handle ) const noexcept
    {
        // No shared_ptr copy: the hot per-draw path pays one uncontended shared lock and nothing else.
        std::shared_lock lock( m_Lock );
        if ( !IsLiveLocked( handle ) )
            return nullptr;
        return m_Images[handle.Value.Index].get();
    }

    std::shared_ptr<Graphic::Image> ImageService::Share( const ImageHandle& handle ) const noexcept
    {
        std::shared_lock lock( m_Lock );
        if ( !IsLiveLocked( handle ) )
            return nullptr;
        return m_Images[handle.Value.Index];
    }

    bool ImageService::IsLiveLocked( const ImageHandle& handle ) const noexcept
    {
        if ( handle.Value.Index >= m_Images.size() )
            return false;

        // A STALE HANDLE USED TO RESOLVE TO WHOEVER MOVED INTO THE SLOT. This checked the index and
        // nothing else, while `HandlePool` recycles indices and bumps a generation on every release —
        // and four call sites already unregister images (SkyboxRenderer drops the previous environment's
        // three cubes on every re-bake). So an ImageHandle held across a re-bake returned a perfectly
        // valid pointer to a DIFFERENT image: the exact "plausible pointer instead of an error" that
        // AssetManager::AsRequestedType was written to close one layer up, still open one layer down.
        //
        // Load-bearing for eviction specifically: releasing an image is only safe if a handle to it
        // cannot come back as somebody else's.
        if ( handle.Value.Index < m_Generations.size() &&
             m_Generations[handle.Value.Index] != handle.Value.Generation )
        {
            return false;
        }

        return m_Images[handle.Value.Index] != nullptr;
    }

} // namespace Desert::Runtime
