#include "ImageService.hpp"

#include <Common/Core/Logger.hpp>

namespace Desert::Runtime
{
    ImageHandle ImageService::Register( std::shared_ptr<Graphic::Image>&& image, ImageHandle::Type type )
    {
        const auto& handle = m_HandlePool.Allocate();

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

    void ImageService::Unregister( const ImageHandle& handle )
    {
        // AN EMPTY HANDLE NAMES NO IMAGE, AND ASKING TO RELEASE NOTHING IS NOT AN ERROR. Without this
        // line an empty handle (index 0, generation 0) falls into the stale-handle branch below and is
        // reported as "something is holding an image handle past the image's life" — which would now be
        // printed on every environment rebake, because an Environment's three cubes are released
        // together, unconditionally, by both `SkyboxRenderer` and `MaterialSkybox::ReleaseEnvironment`,
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
        if ( handle.Value.Index >= m_Images.size() )
            return;
        if ( handle.Value.Index < m_Generations.size() &&
             m_Generations[handle.Value.Index] != handle.Value.Generation )
        {
            LOG_ERROR( "[ImageService] Unregister was given a handle for slot {} generation {}, but that "
                       "slot now holds generation {}. The handle is stale; nothing was released. Something "
                       "is holding an image handle past the image's life.",
                       handle.Value.Index, handle.Value.Generation, m_Generations[handle.Value.Index] );
            return;
        }

        m_HandlePool.Release( handle.Value );
        m_Images[handle.Value.Index].reset();
        if ( handle.Value.Index < m_Generations.size() )
            m_Generations[handle.Value.Index] = 0U;
    }

    void ImageService::Clear()
    {
        // The vector is INDEXED by handle, so the handle pool has to be reset with it or the next
        // Allocate() would hand out an index into a table that no longer describes anything.
        m_Images.clear();
        m_Generations.clear();
        m_HandlePool = Common::Core::HandlePool{};
    }

    Graphic::Image* ImageService::Resolve( const ImageHandle& handle ) const
    {
        if ( handle.Value.Index >= m_Images.size() )
            return nullptr;

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
            return nullptr;
        }

        return m_Images[handle.Value.Index].get();
    }

} // namespace Desert::Runtime
