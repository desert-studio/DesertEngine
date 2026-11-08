// THIS FILE HAD NO INCLUDE GUARD. It works only because nothing has yet included it twice in one
// translation unit; a second include is a class redefinition, i.e. a compile error that arrives the day
// somebody adds an unrelated include somewhere else. Every other header in this directory has one.
#pragma once

#include <Common/Core/HandlePool.hpp>

#include <Engine/Runtime/ImageHandle.hpp>
#include <Engine/Graphic/Image.hpp>

#include <mutex>
#include <shared_mutex>

namespace Desert::Runtime
{
    /**
     * ONE LOCK, OWNED HERE. The service is process-wide and reached from more than one thread at once:
     *   - main thread: SkyCapture::Begin/Publish (SceneSkyLight::Begin), OwnedEnvironment::Replace, Texture2D
     *     create/destroy from scene load, editor panels (MaterialEditorPanel, SkyboxViewerDocument,
     *     PropertyEditorBuilder, TextureDnD), RuntimeLayer, Renderer::RecreateImageSamplers, ClearAll;
     *   - job-system workers: texture streaming / async asset loads build a Texture2D and Register its image
     *     (Texture2D::AdoptImage), and drop the superseded one (Unregister) -- concurrently with the main
     *     thread;
     *   - render thread (one frame of overlap with the main thread): Resolve/Share from SceneRenderer,
     *     SceneRendererFrameDeferred, Mesh/Terrain/Water/Decal/Cloud renderers, Render2D, UIMaterialCache,
     *     MaterialService/TextureService descriptor writes, AnimatedImageService, UI canvas resources.
     * Two concurrent Registers grew HandlePool's vector under each other (EXCEPTION_ACCESS_VIOLATION in
     * memmove, crash 20261107-204659), and a reader racing a resize read a freed buffer. So: Register /
     * Unregister / Replace / Clear take the lock exclusively, Resolve / Share / ForEachImage take it shared,
     * and no caller ever sees the raw vectors. An image whose last reference is dropped by Unregister /
     * Replace / Clear is released AFTER the lock is released, so its destructor never runs under the lock.
     * Resolve's raw pointer is valid while the caller keeps the owner (the Texture2D / Environment) alive,
     * and for the frames in flight after Unregister / Replace (the outgoing image retires through
     * RendererContext::RetireAfterFramesInFlight); Clear (shutdown) destroys at once. A holder that must
     * outlive both takes Share.
     */
    class ImageService
    {
    public:
        [[nodiscard]] ImageHandle Register( std::shared_ptr<Graphic::Image>&& image,
                                            ImageHandle::Type                 type ) noexcept;
        void                      Unregister( const ImageHandle& handle ) noexcept;
        // Puts @p image behind the live @p handle, which keeps naming the slot: every holder of the handle
        // resolves to the new image from the next Resolve/Share on. The old image is released through its own
        // destructor, which defers the device objects by frame (`VulkanAllocator::RT_DestroyImage`), and a
        // frame graph that already imported it holds it by `Share`. False, and nothing changes, when the
        // handle is empty or stale -- the image it named is gone, so there is nothing to replace.
        [[nodiscard]] bool Replace( const ImageHandle& handle, std::shared_ptr<Graphic::Image>&& image ) noexcept;
        Graphic::Image*    Resolve( const ImageHandle& handle ) const noexcept;
        // The owning pointer behind @p handle (null exactly when Resolve is): for a holder that must keep the
        // image alive or track it (the frame graph's import), never to bypass Resolve's staleness check.
        [[nodiscard]] std::shared_ptr<Graphic::Image> Share( const ImageHandle& handle ) const noexcept;

        // Visits every registered image under the shared lock (for global operations like recreating samplers
        // on a filter change). Replaces the old `All()`, which handed the raw vector out for the caller to walk
        // while another thread could reallocate it. @p visit gets a `Graphic::Image&` (empty slots skipped);
        // it must not call this service's writers (Register/Unregister/Replace/Clear) -- that would deadlock.
        template <typename Visitor>
        void ForEachImage( Visitor&& visit ) const noexcept
        {
            std::shared_lock lock( m_Lock );
            for ( const auto& image : m_Images )
            {
                if ( image )
                    visit( *image );
            }
        }

        // Drop every registered image. The other thirteen services have this; this one did not, and it is
        // the one holding the VkImages directly.
        void Clear() noexcept;

    private:
        // Under m_Lock, which the caller already holds (shared or exclusive).
        [[nodiscard]] bool IsLiveLocked( const ImageHandle& handle ) const noexcept;

        mutable std::shared_mutex                    m_Lock;
        Common::Core::HandlePool                     m_HandlePool;
        std::vector<std::shared_ptr<Graphic::Image>> m_Images;
        // The generation each slot was last handed out under, parallel to m_Images. It is what lets
        // Resolve refuse a stale handle instead of answering with the slot's new occupant — see Resolve.
        // Both vectors are guarded by m_Lock and never leave this class.
        std::vector<uint32_t> m_Generations;
    };
} // namespace Desert::Runtime
