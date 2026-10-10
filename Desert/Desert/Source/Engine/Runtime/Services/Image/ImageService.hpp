// THIS FILE HAD NO INCLUDE GUARD. It works only because nothing has yet included it twice in one
// translation unit; a second include is a class redefinition, i.e. a compile error that arrives the day
// somebody adds an unrelated include somewhere else. Every other header in this directory has one.
#pragma once

#include <Common/Core/HandlePool.hpp>

#include <Engine/Runtime/ImageHandle.hpp>
#include <Engine/Graphic/Image.hpp>

namespace Desert::Runtime
{
    class ImageService
    {
    public:
        [[nodiscard]] ImageHandle Register( std::shared_ptr<Graphic::Image>&& image, ImageHandle::Type type );
        void                      Unregister( const ImageHandle& handle );
        // Puts @p image behind the live @p handle, which keeps naming the slot: every holder of the handle
        // resolves to the new image from the next Resolve/Share on. The old image is released through its own
        // destructor, which defers the device objects by frame (`VulkanAllocator::RT_DestroyImage`), and a
        // frame graph that already imported it holds it by `Share`. False, and nothing changes, when the
        // handle is empty or stale -- the image it named is gone, so there is nothing to replace.
        [[nodiscard]] bool        Replace( const ImageHandle& handle, std::shared_ptr<Graphic::Image>&& image );
        Graphic::Image*           Resolve( const ImageHandle& handle ) const;
        // The owning pointer behind @p handle (null exactly when Resolve is): for a holder that must keep the
        // image alive or track it (the frame graph's import), never to bypass Resolve's staleness check.
        [[nodiscard]] std::shared_ptr<Graphic::Image> Share( const ImageHandle& handle ) const;

        // All registered images (for global operations like recreating samplers on a filter change).
        [[nodiscard]] const std::vector<std::shared_ptr<Graphic::Image>>& All() const
        {
            return m_Images;
        }

        // Drop every registered image. The other thirteen services have this; this one did not, and it is
        // the one holding the VkImages directly.
        void Clear();

    private:
        Common::Core::HandlePool                     m_HandlePool;
        std::vector<std::shared_ptr<Graphic::Image>> m_Images;
        // The generation each slot was last handed out under, parallel to m_Images. It is what lets
        // Resolve refuse a stale handle instead of answering with the slot's new occupant — see Resolve.
        // Parallel to the images rather than folded into a struct because m_Images is handed out whole by
        // All(), which Renderer::RecreateImageSamplers walks.
        std::vector<uint32_t> m_Generations;
    };
} // namespace Desert::Runtime
