#pragma once

#include <Engine/Graphic/ViewMemory.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>

// The layout rules of an asset document that shows a LARGE preview beside its details: a left column that
// is the viewport and a right column that is the properties, with a draggable divider between them (the
// pattern of Unreal's Material Editor: Viewport and Details side by side, the viewport filling its column).
//
// Kept apart from ImGui on purpose. Every rule here is arithmetic on widths and a scale, which is exactly
// the part a person cannot see break until the window is some particular shape — so it is tested
// (Desert/Tests/Editor/PreviewInput) and the panel only feeds it ImGui's numbers.
namespace Desert::Editor::PreviewPane
{
    // UE opens the viewport larger than the details; 60 % of the document is the owner's ask.
    inline constexpr float kDefaultSplit = 0.6f;

    // Below these a column stops being usable, so the divider cannot drag past them. A picture narrower
    // than its toolbar is not a preview, and a details column narrower than a label and a slider is not
    // one either.
    inline constexpr float kMinPreviewWidth = 160.0f;
    inline constexpr float kMinDetailsWidth = 240.0f;

    struct Split
    {
        float Preview = 0.0f;
        float Details = 0.0f;
    };

    /**
     * @brief Divide @p available (the document's width minus the divider) at @p fraction.
     *
     * The fraction is what the person chose; the widths are what the window can give. While both minimums
     * fit, the preview is the fraction clamped so that NEITHER column goes under its minimum. When the
     * window is narrower than the two minimums together, the minimums themselves cannot be honoured, and
     * the width is shared in their proportion instead — both columns shrink together and neither collapses
     * to zero, which is what a person dragging the window small expects to see.
     */
    [[nodiscard]] constexpr Split SplitWidth( const float available, const float fraction,
                                              const float minPreview = kMinPreviewWidth,
                                              const float minDetails = kMinDetailsWidth ) noexcept
    {
        // A NaN compares false with everything, so `!( x > 0 )` is also the NaN test.
        if ( !( available > 0.0f ) )
            return {};

        if ( available < minPreview + minDetails )
        {
            const float preview = available * minPreview / ( minPreview + minDetails );
            return { preview, available - preview };
        }

        // Clamped, not replaced: a drag past either end is a fraction past 0 or 1, and it means "as far as it
        // goes", not "back to the default". Only a NaN (a fraction nobody chose) falls back.
        const float chosen  = ( fraction == fraction ) ? std::clamp( fraction, 0.0f, 1.0f ) : kDefaultSplit;
        const float wanted  = available * chosen;
        const float preview = std::clamp( wanted, minPreview, available - minDetails );
        return { preview, available - preview };
    }

    /**
     * @brief The fraction the divider stands at after the person drags it by @p delta pixels.
     *
     * Re-derived from the CLAMPED split rather than from the raw drag, so a drag past a minimum stops the
     * divider at it instead of storing a fraction the window then silently ignores: dragging back moves the
     * divider on the first pixel, not after the overshoot has been paid back.
     */
    [[nodiscard]] constexpr float DragSplit( const float available, const float fraction, const float delta,
                                             const float minPreview = kMinPreviewWidth,
                                             const float minDetails = kMinDetailsWidth ) noexcept
    {
        if ( !( available > 0.0f ) )
            return fraction;
        const Split now  = SplitWidth( available, fraction, minPreview, minDetails );
        const Split next = SplitWidth( available, ( now.Preview + delta ) / available, minPreview, minDetails );
        return next.Preview / available;
    }

    /**
     * @brief The size the preview RENDERS at for a pane of @p width x @p height UI units.
     *
     * The pane's own size times the framebuffer scale, so the image is drawn one texel per screen pixel —
     * never a fixed square stretched over whatever the pane happens to be. A size no target can be built at
     * (a pane collapsed to nothing, a negative width during a drag, a non-finite number, a side past the
     * engine's ceiling) comes back as the zero extent, which the caller treats as "skip this resize and keep
     * the targets you have" — the same rule SceneRenderer::Resize applies, asked here so the preview does
     * not even build its first targets from such a size.
     */
    [[nodiscard]] inline Graphic::ViewExtent RenderExtent( const float width, const float height,
                                                           const float framebufferScale ) noexcept
    {
        const float scale = ( framebufferScale > 0.0f && std::isfinite( framebufferScale ) ) ? framebufferScale : 1.0f;
        const float w     = width * scale;
        const float h     = height * scale;
        if ( !std::isfinite( w ) || !std::isfinite( h ) || w < 1.0f || h < 1.0f ||
             w > static_cast<float>( Graphic::kMaxViewExtentSide ) ||
             h > static_cast<float>( Graphic::kMaxViewExtentSide ) )
            return {};
        const Graphic::ViewExtent extent{ static_cast<uint32_t>( w ), static_cast<uint32_t>( h ) };
        return Graphic::IsUsableViewExtent( extent ) ? extent : Graphic::ViewExtent{};
    }
} // namespace Desert::Editor::PreviewPane
