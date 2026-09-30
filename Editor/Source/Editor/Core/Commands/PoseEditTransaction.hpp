#pragma once

/**
 * THE GESTURES THAT AUTHOR A POSE, AND HOW EACH BECOMES ONE UNDO STEP.
 *
 * The undo of animation DATA is one command: `SequenceEditCommand` over the owner's `Timeline::Sequence`
 * (SequenceEdit.hpp), with an optional authoring-pose half. This file holds what drives it from the
 * editor's surfaces, and the one pose that is not in a Sequence:
 *
 *  - `ControlPoseCommand` / `RecordControlDrag` / `RotateControlRecorded` — a control's authored pose lives
 *    in its `ControlHierarchy`, so it is its own command (see the class note for why not a byte command).
 *  - `ControlGizmoGesture`, `BoneGizmoGesture` — ImGuizmo reports only "held this frame"; these turn a
 *    press-drag-release into exactly one entry.
 *  - `KeyBonePose`, `KeyControlsRecorded`, `ControlAutoKey` — keying, each keyed interaction one
 *    `SequenceEditTransaction`, so the key and the pose it came from undo together.
 *
 * Every entry is BY VALUE (a snapshot of the changed tracks), never an inverse: `SetTransformKey` refreshes
 * the neighbours' auto tangents, so "remove the key at T" would leave them holding slopes computed against
 * a key that no longer exists. The suite is ClipEditUndo.
 */

#include <Editor/Core/CommandHistory.hpp>
#include <Editor/Core/Commands/SequenceEdit.hpp>

#include <Common/Core/ResultStr.hpp>

#include <Engine/Animation/AnimationClip.hpp>
#include <Engine/Animation/Pose.hpp>
#include <Engine/Animation/Rig/ControlHierarchy.hpp>
#include <Engine/Animation/Rig/ControlKeyer.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Desert::Animation
{
    class Animator;
}

namespace Desert::Editor
{

    /**
     * @brief ONE CONTROL-RIG DRAG, AS ONE UNDO ENTRY — the third thing in this editor that authors a pose.
     *
     * IT IS A SECOND COMMAND AND NOT A SECOND USE OF `SequenceEditCommand`, because the subject is neither
     * of that one's two halves: a control's authored `Pose` lives in a `ControlHierarchy`, not in the
     * Animator's authoring buffer and not in a clip track. A drag writes it and nothing else.
     *
     * ── WHY IT IS NOT `CommandHistory::Push` ─────────────────────────────────────────────────────────
     *
     * `BoneTransform` is three trivially copyable members, so the byte command WOULD take it — and would
     * be wrong. `ControlHierarchy::SetPose` DIRTIES the control and everything downstream of it (T5.1's
     * laziness is the whole design), and a memcpy into the member restores the value while leaving every
     * dependent global cached at the dragged position. The control would snap back and the skeleton would
     * not follow, which reads as a rig defect rather than an undo one. This command goes through
     * `SetPose` and reports its refusal.
     *
     * ── WHAT IT REPLACES ─────────────────────────────────────────────────────────────────────────────
     *
     * `LightGizmoRenderer::m_ControlPoseAtGrab` and `m_ControlDragOwner` were WRITTEN AND NEVER READ, and
     * the comment above them said "kept beside the drag so one undo entry per completed drag is pushed".
     * No entry was ever pushed: control drags were outside the undo stack exactly as the whole pose
     * branch was before A29. A comment is not the code; this is the line that does the thing.
     */
    class ControlPoseCommand final : public ICommand
    {
    public:
        ControlPoseCommand( Animation::ControlHierarchy* hierarchy, uint32_t control,
                            Animation::BoneTransform before, Animation::BoneTransform after );

        bool Undo() override;
        bool Redo() override;

        /// A raw pointer into a rig that an asset eviction or a re-attach can replace under it, so the
        /// same guard `SequenceEditCommand` takes and for the same reason. See the `PointerOwnership` row.
        bool IsVolatile() const override
        {
            return true;
        }

        std::string GetLabel() const override;

    private:
        bool Apply( const Animation::BoneTransform& pose );

        Animation::ControlHierarchy* m_Hierarchy = nullptr;
        uint32_t                     m_Control   = 0;
        Animation::BoneTransform     m_Before;
        Animation::BoneTransform     m_After;
    };

    /**
     * @brief Push one entry for a FINISHED control drag, from the pose the grab captured.
     *
     * @return entries pushed: 0 when the drag moved nothing — a click that grabbed and released is not
     *         an undo step, and this editor already pays for histories full of empty entries.
     *
     * Takes the "after" from the hierarchy itself rather than from the caller: the drag's last `Update`
     * is what wrote it, and a caller passing its own copy is a second opinion about what the drag did.
     */
    [[nodiscard]] Common::ResultStr<uint32_t> RecordControlDrag( Animation::ControlHierarchy*    hierarchy,
                                                                 uint32_t                        control,
                                                                 const Animation::BoneTransform& before );

    /**
     * @brief Turn a control about one of its LOCAL axes (Animation::RotateControlLocal) and record the turn as
     *        exactly one undo entry.
     *
     * The palette's "Control Rig / Rotate selected <axis> <deg>" is this call. It lives here and not inside
     * EditorLayer so that "one entry per turn" is measured by a suite (ClipEditUndo) instead of asserted by a
     * comment in a file no suite compiles.
     *
     * @return entries pushed (1; 0 only for a zero-degree turn, which moved nothing).
     */
    [[nodiscard]] Common::ResultStr<uint32_t> RotateControlRecorded( Animation::ControlHierarchy* hierarchy,
                                                                     uint32_t control, int axis, float degrees );

    /**
     * @brief The viewport axis gizmo's gesture boundary: ONE undo entry per press-drag-release.
     *
     * ImGuizmo reports only "held this frame", and every held frame writes the control. Recording per frame
     * would make one drag forty undo steps; recording nothing would make it none. This remembers the pose at
     * the rising edge and records it against the control's pose at the falling edge — the same pairing the
     * shape drag uses (RecordControlDrag), held by value.
     *
     * A gesture whose rig or control changed underneath it (selection moved, rig rebuilt) is ABANDONED on
     * release rather than recorded: an entry addressed into a different rig is worth less than no entry.
     */
    class ControlGizmoGesture
    {
    public:
        /// Call once per frame after the gizmo reported whether it is held and BEFORE this frame's write is
        /// applied, so the rising edge captures the pose the gesture started from.
        /// @return entries pushed this frame: 1 on the release of a gesture that moved the control, else 0.
        [[nodiscard]] Common::ResultStr<uint32_t> Step( Animation::ControlHierarchy* hierarchy, uint32_t control,
                                                        bool held );

        [[nodiscard]] bool Active() const noexcept
        {
            return m_Active;
        }

        void Abandon() noexcept
        {
            m_Active = false;
        }

    private:
        bool                         m_Active    = false;
        Animation::ControlHierarchy* m_Hierarchy = nullptr;
        uint32_t                     m_Control   = Animation::ControlHierarchy::INVALID;
        Animation::BoneTransform     m_Before;
    };


    /**
     * @brief The Animation Editor's bone gizmo gesture: ONE undo entry per press-drag-release.
     *
     * The bone-gizmo twin of `ControlGizmoGesture`. ImGuizmo reports only "held this frame" and every held
     * frame writes the bone, so the edges live here, where a suite can drive them, rather than in the
     * document's draw code: an explicit transaction opens on the rising edge over the pose the drag starts
     * from, and closes on the falling edge, pushing at most one entry. A press that moves nothing pushes
     * nothing — `SequenceEditTransaction::End` compares by value.
     *
     * The pose source is a callback because the document must only switch its preview into posing on a
     * real press; asking for the animator every frame the gizmo is merely drawn would take the preview off
     * the clip.
     */
    class BoneGizmoGesture
    {
    public:
        /// Call once per frame after the gizmo reported whether it is held and BEFORE this frame's write is
        /// applied. @p startPose is called on the rising edge only and returns the animator whose authoring
        /// pose the drag starts from (nullptr refuses the gesture).
        /// @return entries pushed this frame: 1 on the release of a gesture that changed the pose, else 0.
        [[nodiscard]] Common::ResultStr<uint32_t> Step( SequenceEditTransaction& transaction, bool held,
                                                        const std::function<Animation::Animator*()>& startPose,
                                                        Animation::AnimationClip*                    clip );

        [[nodiscard]] bool Active() const noexcept
        {
            return m_Active;
        }

        /// Forget the gesture without recording it; the owner cancels the transaction itself.
        void Abandon() noexcept
        {
            m_Active = false;
        }

    private:
        bool m_Active = false;
    };

    /**
     * @brief THE PERSONA "+ Key" BUTTON: one bone of the authoring pose, keyed into the clip, as ONE undo step.
     *
     * The bone's WHOLE transform (location, rotation, scale) is upserted into the track named after the
     * skeleton bone at `tick`, creating the track when the clip has none for it. It goes through the same
     * transaction as a drag so that Ctrl+Z puts back the clip AND the pose by value, neighbours' auto
     * tangents included (`SetTransformKey` refreshes the whole track).
     *
     * @return undo entries pushed (1, or 0 when the clip already held exactly this key). Refuses a null
     *         animator or clip, a bone outside the skeleton, a non-finite pose and a transaction already open.
     */

    [[nodiscard]] Common::ResultStr<uint32_t> KeyBonePose( SequenceEditTransaction&      transaction,
                                                           Animation::Animator*      animator,
                                                           Animation::AnimationClip* clip, uint32_t bone,
                                                           Animation::FrameNumber tick );

    /**
     * @brief The Sequencer's "Key (S)": key every control in @p controls at `target.Tick`, as ONE undo entry.
     *
     * The keys go through the keyer's button path (`ControlWriteSource::Authored`, not `Observe`), so the
     * auto-key mode cannot silence a key the animator asked for. The transaction brackets the writes, so
     * one press of S over five selected controls is one Ctrl+Z, and that is measured by ClipEditUndo rather
     * than claimed by the panel, which no suite compiles.
     *
     * @return keys written. Refuses a target with no hierarchy or no clip, an empty selection, and a
     *         transaction already open (a key pressed mid-drag belongs to the drag, which records it).
     */
    [[nodiscard]] Common::ResultStr<uint32_t> KeyControlsRecorded( SequenceEditTransaction&               transaction,
                                                                   Animation::Animator*               animator,
                                                                   Animation::ControlKeyer&           keyer,
                                                                   const Animation::ControlKeyTarget& target,
                                                                   std::span<const uint32_t>          controls );

    /**
     * @brief Auto-key for a CONTROL gesture: exactly one key and one undo entry per press-drag-release.
     *
     * The edge rule is the keyer's (`ControlKeyer::Observe`); what this adds is the two facts the panel
     * would otherwise compute in a file no suite compiles: "did the control move since last frame", and the
     * undo transaction around the release frame — the only frame on which `Observe` writes the clip.
     * Hand it a keyer of its own: `Observe` keeps last frame's pointer bit, so one keyer observed for a bone
     * and for a control in the same frame would see two edges per frame.
     */
    class ControlAutoKey
    {
    public:
        /// One call per frame. @p held is `GizmoState::ControlInteraction()`.
        ///
        /// A gesture is also opened by `LastControlEdit()` naming this rig and @p control: that is how the
        /// palette's "Rotate selected", a nudge and a Details edit — none of which holds the gizmo bit —
        /// reach the keyer, through the same edge the mouse uses. Such a gesture is held for the frame the
        /// edit is first seen and released on the next, and its key entry is JOINED with the control's
        /// own pose entry when that is still the top of the stack: one command, one Ctrl+Z.
        /// A pose change outside any gesture (the playhead writing the clip onto the controls) keys nothing.
        /// @return undo entries pushed: 1 on the release frame of a gesture that moved the control, else 0.
        [[nodiscard]] Common::ResultStr<uint32_t>
        Step( SequenceEditTransaction& transaction, Animation::Animator* animator, Animation::ControlKeyer& keyer,
              const Animation::ControlKeyTarget& target, uint32_t control, bool held );

    private:
        bool                     m_Held    = false;
        uint32_t                 m_Control = Animation::ControlHierarchy::INVALID;
        Animation::BoneTransform m_Last;
        /// `LastControlEdit().Generation` already seen; empty until the first Step, so an edit made
        /// before this Sequencer existed is not taken for a new one.
        std::optional<uint64_t> m_SeenEdit;
        /// The history revision right after the control's entry of the edit inside this gesture.
        std::optional<uint64_t> m_JoinRevision;
    };

    /// THE ONE "A CONTROL WAS CHANGED" SIGNAL. Every control edit that becomes an undo entry goes through
    /// `RecordControlDrag` (the gizmo release, a nudge, the palette's rotate, the Control Rig panel's
    /// pose field), and that is where this is stamped — so the auto-keyer listens to one place instead of
    /// to each tool.
    struct ControlEdit
    {
        uint64_t                           Generation = 0; ///< 0 = no edit yet; +1 per recorded edit
        const Animation::ControlHierarchy* Hierarchy  = nullptr;
        uint32_t                           Control    = Animation::ControlHierarchy::INVALID;
        uint64_t                           Revision   = 0; ///< CommandHistory::Revision() after its entry
    };

    [[nodiscard]] ControlEdit LastControlEdit();

} // namespace Desert::Editor
