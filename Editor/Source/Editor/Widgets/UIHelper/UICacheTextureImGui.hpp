#pragma once

#include <Editor/Widgets/UIHelper/ImageTextureIds.hpp>

#include <Engine/Graphic/Image.hpp>

#include <ImGui/imgui.h>

#include <vulkan/vulkan.h>

#include <cstddef>
#include <memory>

namespace Desert::Editor::UI
{
    /**
     * @brief Hands an engine image to Dear ImGui as a texture id, caching one descriptor set per
     *        live image.
     *
     * THE ENGINE USED TO OWN THE OTHER HALF OF THIS, AND THAT IS THE DEFECT THIS SHAPE REMOVES. There was
     * an abstract `Desert::Graphic::UICacheTexture` in `Engine/Graphic/`, whose static `Create()` did
     * `std::make_unique<ImGui::UICacheTextureImGui>()` — so the engine's own factory named an ImGui type,
     * and every binary that linked the engine linked the toolkit with it.
     *
     * WHY THE ABSTRACTION IS GONE RATHER THAN INVERTED. The obvious repair is to keep the interface in the
     * engine and let the editor REGISTER the implementation into it. That interface had exactly one
     * implementation and exactly one consumer, and after the move both of them are in the Editor: a
     * registration hook would leave the engine holding a nullable slot that only the editor can ever fill,
     * i.e. an engine-side API whose correct state in every non-editor process is "empty". Deleting it makes
     * ImGui's absence from the engine structural instead of conditional — there is no seam left to fill in
     * wrongly. `Desert/Tests/Engine/ImGuiBoundary` is what holds that.
     *
     * ONE ENTRY PER LIVE IMAGE, RELEASED WITH IT (AM3). The entries are `ImageTextureIds` — owned by the
     * image through a `weak_ptr` and checked against its resource generation — so an image that dies, is
     * re-created at another size, or has its sampler recreated gives its descriptor set back instead of
     * leaving it to be found again under a recycled `VkImageView`. The set is freed DEFERRED, through
     * `VulkanAllocator::RT_FreeDescriptorSets`, once the frame ring has come round and no frame in flight
     * can still be sampling it. The pool is the ImGui layer's: it binds it on attach and forgets every entry
     * before destroying it on detach, which frees the sets with it.
     */
    class UICacheTextureImGui
    {
    public:
        // THE ONE CACHE. It is process-wide (one descriptor set per image, whichever panel asks), so it is one
        // object every UIHelper points at rather than a function-static behind a member function.
        static UICacheTextureImGui& Get();

        /// The pool ImGui_ImplVulkan_AddTexture allocates from; the sets are freed back into it.
        void BindPool( VkDescriptorPool pool );

        /// The pool is about to be destroyed, freeing every set in it: forget them all without a free.
        void ReleasePool();

        ImTextureID AddTextureCache( const std::shared_ptr<Graphic::Image2D>& image );

        /// Give back the sets of every image destroyed since the last call. Every frame, before the UI is
        /// built. Returns how many were queued for release.
        std::size_t RetireReleased();

    private:
        void Retire( ImTextureID id ) const;

        ImageTextureIds<Graphic::Image2D, ImTextureID> m_Ids;
        VkDescriptorPool                               m_Pool = VK_NULL_HANDLE;
    };
} // namespace Desert::Editor::UI
