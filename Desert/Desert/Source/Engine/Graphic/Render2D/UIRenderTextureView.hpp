#pragma once

#include <Engine/Core/ViewBudget.hpp>
#include <Engine/Graphic/ViewMemory.hpp>

#include <cstdint>

namespace Desert::Graphic::Render2D
{
    /// The view a render-texture element asks the budget for, and the one UIRenderTextureCache then builds.
    struct UIRenderTextureViewRequest
    {
        Engine::ViewBudget::Demand Who = Engine::ViewBudget::Demand::UserSurface;
        ViewProfile                Profile{};
        ViewExtent                 Extent{};
    };

    /**
     * @brief What a render-texture element of @p width x @p height asks for.
     *
     * ONE ANSWER FOR THE QUESTION AND THE BUILD: the cache asks MayCreateView with these fields and builds the
     * SceneRenderer from the same ones, so the forecast the budget checks is the view that gets built.
     * UserSurface: an author put the element on a canvas and a player is looking at its rect, so it may take
     * the last byte, as the Details preview may. Header-only so Tests/Engine/UIRenderTexture pins it.
     */
    [[nodiscard]] constexpr UIRenderTextureViewRequest RequestUIRenderTextureView( const uint32_t width,
                                                                                   const uint32_t height ) noexcept
    {
        return { Engine::ViewBudget::Demand::UserSurface, kPreviewViewProfile, ViewExtent{ width, height } };
    }
} // namespace Desert::Graphic::Render2D
