#pragma once

#include <Engine/Graphic/Image.hpp>

#include <memory>

namespace Desert::Editor::UI
{
    /**
     * @brief Hands an engine image to Dear ImGui as a texture id, caching one descriptor set per
     *        `VkImageView`.
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
     * The cache is keyed by `VkImageView` and is never cleared — see the note in
     * `Engine/Assets/AssetEviction.hpp` for why that makes evicting a live `Graphic::Image` unsafe today.
     */
    class UICacheTextureImGui
    {
    public:
        const void* AddTextureCache( const std::shared_ptr<Graphic::Image2D>& image );
    };
} // namespace Desert::Editor::UI
