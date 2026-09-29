#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Editor/Core/Control/ControlProtocol.hpp>

#include <array>
#include <cstdint>
#include <optional>

namespace Desert::Editor::Control
{
    /**
     * @brief A mouse drag the control channel plays through ImGui's OWN input queue.
     *
     * Why the queue and not a call into the gizmo: the defects this exists to find live between the
     * pointer and the widget — which window is hovered, which gizmo claims the press, whether one gesture
     * becomes one undo record. A drag that bypassed ImGui would prove none of that. The events enter
     * between the platform backend's NewFrame and ImGui::NewFrame (VulkanImGui::Begin), after the real
     * cursor's own position event, so the synthetic one is the last word for that frame.
     *
     * One event per frame, because a widget reads the pointer once per frame: hover at `from` (so the
     * gizmo is hot before the press, as with a hand), press, `steps` moves to `to`, release.
     */
    struct PointerStep
    {
        float X    = 0.0f; ///< ImGui screen coordinates (points)
        float Y    = 0.0f;
        bool  Down = false;
        /// The one rising edge of the drag: the frame the layers must also see as a mouse-press event,
        /// because entity/bone picking listens to that event and not to ImGui's button state.
        bool Press = false;
    };

    /// Where a target's image sits on screen this frame, in ImGui points, and which platform viewport
    /// holds it. Published by the panel that draws the image; `Frame` is ImGui's frame count at that time.
    struct PointerTargetRect
    {
        float    X          = 0.0f;
        float    Y          = 0.0f;
        float    Width      = 0.0f;
        float    Height     = 0.0f;
        uint32_t ViewportId = 0;
        int32_t  Frame      = -1;
    };

    class PointerDrag
    {
    public:
        static constexpr uint32_t kMaxSteps = 240;

        /// @p from / @p to are in the target's image PIXELS (what shot-viewport writes); @p pixelsPerPoint
        /// converts them to ImGui points (the framebuffer scale, 2 on a Retina display).
        [[nodiscard]] static Common::ResultStr<PointerDrag> Plan( const PointerTargetRect&    target,
                                                                  const std::array<float, 4>& fromTo,
                                                                  uint32_t steps, float pixelsPerPoint )
        {
            if ( steps == 0 || steps > kMaxSteps )
                return Common::MakeFormattedError<PointerDrag>(
                     "a drag takes 1..{} steps, not {}: zero would press and release on one spot, and "
                     "more would hold the channel for seconds.",
                     kMaxSteps, steps );
            if ( target.Width <= 0.0f || target.Height <= 0.0f || pixelsPerPoint <= 0.0f )
                return Common::MakeFormattedError<PointerDrag>(
                     "the target's image is {}x{} points at {} pixels per point; there is nothing to drag on.",
                     target.Width, target.Height, pixelsPerPoint );

            const float widthPx  = target.Width * pixelsPerPoint;
            const float heightPx = target.Height * pixelsPerPoint;
            for ( std::size_t i = 0; i < 4; ++i )
            {
                const float limit = ( i % 2 == 0 ) ? widthPx : heightPx;
                if ( fromTo[i] < 0.0f || fromTo[i] >= limit )
                    return Common::MakeFormattedError<PointerDrag>(
                         "drag point {} ({}, {}) lies outside the target's {}x{} pixels; a press outside "
                         "the image would land on some other widget and the reply would still say done.",
                         i / 2 == 0 ? "from" : "to", fromTo[i / 2 * 2], fromTo[i / 2 * 2 + 1], widthPx, heightPx );
            }

            PointerDrag drag;
            drag.m_FromX      = target.X + fromTo[0] / pixelsPerPoint;
            drag.m_FromY      = target.Y + fromTo[1] / pixelsPerPoint;
            drag.m_ToX        = target.X + fromTo[2] / pixelsPerPoint;
            drag.m_ToY        = target.Y + fromTo[3] / pixelsPerPoint;
            drag.m_Steps      = steps;
            drag.m_ViewportId = target.ViewportId;
            return Common::MakeSuccess( drag );
        }

        /// Hover, press, `steps` moves, release.
        [[nodiscard]] uint32_t FrameCount() const noexcept
        {
            return m_Steps + 3;
        }

        [[nodiscard]] PointerStep At( const uint32_t frame ) const noexcept
        {
            if ( frame <= 1 )
                return { m_FromX, m_FromY, frame == 1, frame == 1 };
            if ( frame >= m_Steps + 2 )
                return { m_ToX, m_ToY, false, false };
            const float t = static_cast<float>( frame - 1 ) / static_cast<float>( m_Steps );
            return { m_FromX + ( m_ToX - m_FromX ) * t, m_FromY + ( m_ToY - m_FromY ) * t, true, false };
        }

        [[nodiscard]] uint32_t ViewportId() const noexcept
        {
            return m_ViewportId;
        }

    private:
        float    m_FromX = 0.0f, m_FromY = 0.0f, m_ToX = 0.0f, m_ToY = 0.0f;
        uint32_t m_Steps      = 1;
        uint32_t m_ViewportId = 0;
    };

    /**
     * @brief The one drag in flight and the targets it can aim at.
     *
     * Process-wide for the reason ImGuizmo's context is: the panels that publish a target, the layer that
     * arms a drag and the ImGui backend that plays it have no object in common. At most one drag runs;
     * arming a second while one plays is refused, never queued.
     */
    class PointerInjection
    {
    public:
        static void PublishTarget( const Subject subject, const PointerTargetRect& rect ) noexcept
        {
            Targets()[Index( subject )] = rect;
        }

        /// The target published THIS frame or the one before, or nothing: a rect left by a panel that is
        /// no longer drawn would aim the press at whatever took its place.
        [[nodiscard]] static std::optional<PointerTargetRect> FreshTarget( const Subject subject,
                                                                           const int32_t currentFrame ) noexcept
        {
            const PointerTargetRect& rect = Targets()[Index( subject )];
            if ( rect.Frame < 0 || currentFrame - rect.Frame > 1 )
                return std::nullopt;
            return rect;
        }

        [[nodiscard]] static Common::BoolResultStr Arm( const PointerDrag& drag )
        {
            if ( State().Drag )
                return Common::MakeError( "a drag is already playing; its reply has not been sent yet." );
            State().Drag  = drag;
            State().Frame = 0;
            return Common::MakeSuccess( true );
        }

        /// The event for this frame, or nothing. Called once per frame by the ImGui backend. Stays pending
        /// for ONE call after the release so the frame that draws the release is behind the reply.
        [[nodiscard]] static std::optional<PointerStep> NextStep() noexcept
        {
            auto& state = State();
            if ( !state.Drag )
                return std::nullopt;
            if ( state.Frame >= state.Drag->FrameCount() )
            {
                state.Drag.reset();
                return std::nullopt;
            }
            return state.Drag->At( state.Frame++ );
        }

        [[nodiscard]] static uint32_t ViewportId() noexcept
        {
            const auto& drag = State().Drag;
            return drag.has_value() ? drag->ViewportId() : 0;
        }

        [[nodiscard]] static bool Playing() noexcept
        {
            return State().Drag.has_value();
        }

        static void Reset() noexcept
        {
            State()   = {};
            Targets() = {};
        }

    private:
        struct Playback
        {
            std::optional<PointerDrag> Drag;
            uint32_t                   Frame = 0;
        };

        static constexpr std::size_t kSubjectCount = std::size( kSubjects );

        [[nodiscard]] static std::size_t Index( const Subject subject ) noexcept
        {
            return static_cast<std::size_t>( subject );
        }

        static Playback& State() noexcept
        {
            static Playback state;
            return state;
        }

        static std::array<PointerTargetRect, kSubjectCount>& Targets() noexcept
        {
            static std::array<PointerTargetRect, kSubjectCount> targets{};
            return targets;
        }
    };
} // namespace Desert::Editor::Control
