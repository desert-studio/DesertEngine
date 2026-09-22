#pragma once

/**
 * A CONTROL-RIG DRAG THAT DOES NOT NEED A MOUSE.
 *
 * `ControlDrag::Begin/Update/End` (Engine/Animation/Rig/ControlManipulator.hpp) is proved by a suite and
 * named by a census, and until this file **no frame had ever contained its result**: the only caller is
 * `LightGizmoRenderer::RenderControlRig`, which runs it off `ImGui::IsMouseClicked` and
 * `ImGui::GetMousePos` — and synthetic input is closed on this machine at BOTH doors (osascript has no
 * assistive access, `CGEventPost` is refused), so nothing without a human at the keyboard could put the
 * feature on screen. A feature that cannot be photographed is a feature nobody has checked.
 *
 * THE ANSWER IS NOT TO FAKE THE MOUSE, IT IS TO GIVE THE GESTURE A NAME. The command palette is the
 * editor's dictionary and the control channel's vocabulary at the same time (`CommandPalette.hpp`), so a
 * drag expressed as "nudge the selected control 80 px right" is reachable by a person searching for it
 * AND by an unattended run — and it goes through `ControlDrag`, the same object the mouse grabs, so what
 * the picture shows is the real path and not a second one written for the test.
 *
 * ── WHY A REQUEST AND NOT A DIRECT CALL ──────────────────────────────────────────────────────────────
 *
 * The palette command runs in `OnUpdate`. The drag needs a `ManipulatorView` — the camera, the entity's
 * world transform and the viewport rectangle folded into one matrix — and the `ControlHierarchy` behind
 * the selected entity's rig. Both belong to the viewport that is drawing the overlay, and they are only
 * true DURING its `OnUIRender`. A command that reached into a viewport to fetch them would be holding a
 * matrix from the previous frame and a rectangle from before the last resize; this queues one nudge and
 * lets the surface that owns those two things perform it, which is the same shape `SubjectOpenRequests`
 * uses for exactly the same reason.
 *
 * ── THE THREE STATES, AND WHY "APPLIED" IS NOT "DONE" ────────────────────────────────────────────────
 *
 * The control channel releases a reply only after a presented frame on which nothing this command queued
 * was still outstanding. A nudge consumed during `OnUIRender` of frame N is NOT VISIBLE ON FRAME N: the
 * overlay's shapes were projected before it ran, and the skinned mesh was evaluated in that frame's
 * `OnUpdate`, which is earlier still. So `Take()` moves the request to `Applied` rather than clearing
 * it, and `Tick()` — one call per frame, at the top of `OnUpdate` — retires it one frame later. The
 * first frame that reports settled is therefore the first frame that actually DRAWS the moved control.
 * Without this the reply would be released on a picture of the state before the command, which is the
 * one thing this channel promises never to do.
 *
 * `Tick()` also drops a request that NOBODY consumed (no viewport in Control mode, no control selected)
 * after it has been offered one full frame, with a log line naming it. An unconsumable request that
 * stayed pending would hold the reply gate until it timed out, and the timeout would blame the wrong
 * thing.
 */

#include <Common/Core/Logger.hpp>
#include <Common/Core/ResultStr.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>

namespace Desert::Editor::Core
{
    class ControlNudgeRequests
    {
    public:
        /// Queue one nudge, in VIEWPORT PIXELS. Refuses while one is outstanding: two nudges in flight
        /// would be two drags of one control in one frame, and the second `ControlDrag::Begin` would be
        /// refused anyway — better to say so at the door than to lose the second silently.
        [[nodiscard]] static Common::BoolResultStr Request( const glm::vec2& pixelDelta )
        {
            State& state = Get();
            if ( state.Current != Phase::Idle )
            {
                return Common::MakeError<bool>(
                     "a control nudge is already in flight; one drag at a time, because a second grab of "
                     "the same control in one frame is what ControlDrag::Begin refuses" );
            }
            state.Current = Phase::Queued;
            state.Delta   = pixelDelta;
            state.Age     = 0;
            return Common::MakeSuccess<bool>( true );
        }

        /// Is anything this command queued still outstanding? Asked by the quiescence census.
        [[nodiscard]] static bool HasPending()
        {
            return Get().Current != Phase::Idle;
        }

        /// The queued delta, taken by the surface that can perform it. `nullopt` when nothing is queued
        /// — including when one has already been taken this frame, so two viewports drawing the same
        /// rig cannot each perform it.
        [[nodiscard]] static std::optional<glm::vec2> Take()
        {
            State& state = Get();
            if ( state.Current != Phase::Queued )
            {
                return std::nullopt;
            }
            state.Current = Phase::Applied;
            state.Age     = 0;
            return state.Delta;
        }

        /// One call per frame, at the top of `OnUpdate`. See the file note for both jobs it does.
        static void Tick()
        {
            State& state = Get();
            switch ( state.Current )
            {
                case Phase::Idle:
                    return;
                case Phase::Applied:
                    // The frame that drew it has been presented. Done.
                    state.Current = Phase::Idle;
                    return;
                case Phase::Queued:
                    ++state.Age;
                    if ( state.Age <= 1 )
                    {
                        return;
                    }
                    // OFFERED A WHOLE FRAME AND NOBODY TOOK IT. Said, not swallowed: the only causes are
                    // "no viewport is in Control mode on this entity" and "no control is selected", and a
                    // person who asked for a nudge and got silence has no way to tell those apart.
                    LOG_WARN( "[Animation] the queued control nudge ({}, {}) px was not performed: no "
                              "viewport is drawing a control rig with a control selected.",
                              state.Delta.x, state.Delta.y );
                    state.Current = Phase::Idle;
                    return;
            }
        }

    private:
        enum class Phase : uint8_t
        {
            Idle,
            Queued,  ///< waiting for a viewport that can perform it
            Applied, ///< performed; the frame that DRAWS it has not been presented yet
        };

        struct State
        {
            Phase     Current = Phase::Idle;
            glm::vec2 Delta   = glm::vec2( 0.0F );
            int       Age     = 0;
        };

        static State& Get()
        {
            static State s_State;
            return s_State;
        }
    };
} // namespace Desert::Editor::Core
