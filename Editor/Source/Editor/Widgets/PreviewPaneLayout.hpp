#pragma once

#include <Engine/Graphic/ViewMemory.hpp>
#include <glm/glm.hpp>

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
        const float scale =
             ( framebufferScale > 0.0f && std::isfinite( framebufferScale ) ) ? framebufferScale : 1.0f;
        const float w = width * scale;
        const float h = height * scale;
        if ( !std::isfinite( w ) || !std::isfinite( h ) || w < 1.0f || h < 1.0f ||
             w > static_cast<float>( Graphic::kMaxViewExtentSide ) ||
             h > static_cast<float>( Graphic::kMaxViewExtentSide ) )
            return {};
        const Graphic::ViewExtent extent{ static_cast<uint32_t>( w ), static_cast<uint32_t>( h ) };
        return Graphic::IsUsableViewExtent( extent ) ? extent : Graphic::ViewExtent{};
    }

    // ── Framing: the subject whole, centred, with air around it ──────────────────────────────────────────

    // The orientation a preview opens at (and returns to on Reset View): a three-quarter view from a little
    // above. The fit is solved at THIS orientation, not the current one, so orbiting never dollies the camera.
    inline constexpr float kFramingYaw   = -0.6f; // radians
    inline constexpr float kFramingPitch = 0.4f;  // radians, positive = camera above the subject

    // The share of the pane's NARROWER side left empty on EACH side of the fitted subject (UE's Material
    // Editor viewport shows the sphere whole with a clear margin): the subject spans 1 - 2 * 0.1 = 80 % of it.
    inline constexpr float kFrameMargin = 0.1f;

    struct FramedSubject
    {
        // A ball is bounded by its own radius from every direction, so it fits by the cone tangent to it;
        // everything else fits by the corners of its box, which is tighter than its bounding sphere.
        bool      Round      = false;
        float     Radius     = 1.0f;
        glm::vec3 HalfExtent = glm::vec3( 0.5f );
    };

    /**
     * @brief The orbit distance at which @p subject fits a pane of @p aspect (width / height) whole.
     *
     * The camera's field of view is VERTICAL, so the horizontal one follows from the aspect:
     * tan(h/2) = aspect * tan(v/2). The subject must fit BOTH, which means the narrower side binds — a tall
     * narrow pane is fitted by its width, a wide short one by its height. Fitting by the vertical field alone
     * (the rule of the square thumbnail this replaced) pushes a subject out of the sides of every pane taller
     * than it is wide.
     *
     * The margin is applied in tangent space, i.e. as a share of the SCREEN, because screen position is
     * proportional to the tangent of the angle off the axis.
     */
    [[nodiscard]] inline float FitDistance( const FramedSubject& subject, const float yaw, const float pitch,
                                            const float verticalFovRadians, const float aspect,
                                            const float margin = kFrameMargin ) noexcept
    {
        // An aspect nobody can draw at (a pane collapsed to nothing, a NaN mid-drag) is fitted as a square
        // rather than producing an infinite or negative distance the camera would be placed at.
        const float usableAspect = ( aspect > 0.0f && std::isfinite( aspect ) ) ? aspect : 1.0f;
        const float fill         = std::clamp( 1.0f - 2.0f * margin, 0.05f, 1.0f );
        const float tanV         = std::tan( verticalFovRadians * 0.5f ) * fill;
        const float tanH         = tanV * usableAspect;

        if ( subject.Round )
            return subject.Radius / std::sin( std::atan( std::min( tanV, tanH ) ) );

        //   corner depth  = d + dot(c, forward)
        //   inside while |dot(c, right)| <= depth * tanH  and  |dot(c, up)| <= depth * tanV
        //   => d >= max(|dot(c, right)| / tanH, |dot(c, up)| / tanV) - dot(c, forward)
        const float     cp = std::cos( pitch );
        const glm::vec3 forward{ -cp * std::sin( yaw ), -std::sin( pitch ), -cp * std::cos( yaw ) };
        const glm::vec3 right = glm::normalize(
             glm::cross( forward, std::abs( forward.y ) > 0.99f ? glm::vec3( 0, 0, 1 ) : glm::vec3( 0, 1, 0 ) ) );
        const glm::vec3 up = glm::normalize( glm::cross( right, forward ) );

        float needed = 0.0f;
        for ( int corner = 0; corner < 8; ++corner )
        {
            const glm::vec3 c( ( corner & 1 ) != 0 ? subject.HalfExtent.x : -subject.HalfExtent.x,
                               ( corner & 2 ) != 0 ? subject.HalfExtent.y : -subject.HalfExtent.y,
                               ( corner & 4 ) != 0 ? subject.HalfExtent.z : -subject.HalfExtent.z );
            const float lateral =
                 std::max( std::abs( glm::dot( c, right ) ) / tanH, std::abs( glm::dot( c, up ) ) / tanV );
            needed = std::max( needed, lateral - glm::dot( c, forward ) );
        }
        // Never inside the content's own sphere, whatever a flat box's corners allow.
        return std::max( needed, subject.Radius );
    }

    // ── The viewport toolbar: wraps, never clips ─────────────────────────────────────────────────────────

    /**
     * @brief Whether an item @p itemWidth wide starts a NEW toolbar line rather than following the items
     * already @p usedOnLine wide on the current one, in a row @p available wide.
     *
     * The first item of a line never wraps (there is nothing to wrap away from; an item wider than the whole
     * row is narrowed by the caller instead). Everything after it wraps as soon as it would cross the edge,
     * so a narrow pane shows its toolbar on two or three lines instead of cutting the last controls off.
     */
    [[nodiscard]] constexpr bool WrapsToNextLine( const float usedOnLine, const float itemWidth,
                                                  const float spacing, const float available ) noexcept
    {
        return usedOnLine > 0.0f && usedOnLine + spacing + itemWidth > available;
    }
} // namespace Desert::Editor::PreviewPane
