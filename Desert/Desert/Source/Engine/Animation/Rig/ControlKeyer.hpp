#pragma once

/**
 * KEYING A CONTROL, AND THE TWO RULES THAT STOP IT EATING ITSELF.
 *
 * T5.3. Taking what a control says right now and writing it into a clip is four lines; the task is the
 * two behaviours around it, and both exist because the naive version is subtly wrong rather than slow.
 *
 * ── WHERE A CONTROL KEY LANDS: AN ORDINARY TRACK, AND NO FORMAT CHANGE ───────────────────────────────
 *
 * `AnimationClip::Tracks` is NOT indexed by bone and says so at its declaration: "Playback resolves by
 * name (Animator::ResolveTrack), so position here means nothing". `BoneTrack` is therefore a NAMED
 * TRANSFORM TRACK with three channels, and a control's animated value is a `BoneTransform` — the same
 * three quantities. A control's keys are a track whose name is the control's name, and `.anim` does not
 * move: `kAnimationVersion` stays at 2 and no clip in the corpus is touched.
 *
 * THE PRICE OF THAT IS ONE AMBIGUITY, AND IT IS REFUSED RATHER THAN RESOLVED. A track name is the only
 * binding key there is; UE can let a control and a bone share a name because `FRigElementKey` is
 * {name, TYPE} and ours is {name}. So a control named exactly like a bone of the skeleton would have its
 * keys bound straight onto that bone by `Animator::ResolveTrack` — driving the bone with a value that
 * means "offset from the control's parent space", which is wrong by the offset and by the whole parent
 * chain, and wrong in a way that looks like bad animation data rather than like a defect. `Key` refuses
 * that control by name and says which bone it collided with. A rename is the fix; a silent bind is not.
 *
 * ── SCALE: WHAT A9 ASKED T5.3 TO ANSWER BEFORE SHE BUILDS THE DRAG ───────────────────────────────────
 *
 * Two questions were left here by `ControlDrag`'s note ("no scale, and that is a decision").
 *
 *   1. WHAT DOES A SCALE CHANNEL KEY TO? `Pose.Scale`, into the track's ordinary `ScaleKeys`, as a VEC3
 *      with the same Cubic/Auto convention a bone's scale channel gets. Not a uniform scalar: the channel
 *      is three floats and a uniform-only knob would make two thirds of it unauthorable while pretending
 *      the clip could not express it.
 *   2. DOES A SCALED CONTROL SCALE ITS CHILDREN'S SPACES? YES, and this is NOT a decision T5.3 got to
 *      make — T5.1 already made it in one line: `m_Global = ParentSpace * Offset * Pose` composes
 *      matrices, and a child whose parent is a control reads `m_Global[parent]` as its space. The suite
 *      asserts the arithmetic rather than this paragraph.
 *
 * The consequence worth saying out loud, because it is the one that will produce a bug report: a
 * non-uniform scale under a rotation makes the child's parent space SHEARED, and `BoneTransform` cannot
 * hold shear — `FromMatrix` takes each axis's length as its scale and folds the rest into the rotation
 * silently. So a control keyed under a non-uniformly-scaled, rotated parent reads back a pose that is
 * close but not equal. That is a property of TRS keyframes, not of this file, and no keyer can fix it.
 *
 * ── RULE ONE: THE KEY IS DEFERRED TO THE END OF THE INTERACTION (report 05 §971) ─────────────────────
 *
 * §971 is explicit that this is not an optimisation: one undo transaction, ONE key per control, and the
 * value resolved before it is committed. The failure it prevents is not "too many keys in the same
 * place" — an upsert would collapse those — it is that an interaction can span TICKS. Drag a control
 * while the sequence is playing, or scrub during a drag, and a keyer fed per frame lays a key on every
 * tick the playhead passed, recording every intermediate position the pointer happened to be in as
 * authored animation. The animator's twenty-frame drag becomes twenty keys they have to delete, and the
 * curve through them is the path of their mouse.
 *
 * So: `BeginInteraction` opens one, `Write` during it only REMEMBERS which controls moved, and
 * `EndInteraction` writes exactly one key per remembered control, at ONE tick, from the value the
 * control ended at. Outside an interaction `Write` keys immediately — that is auto-key-on-change, report
 * 01 §823's broadcast, and it is the same function rather than a second mode.
 *
 * ── RULE TWO: PLAYBACK MUST NOT FEED THE KEYER (report 01 §823) ──────────────────────────────────────
 *
 * §823: "a 'never key' flag on the context so playback can write without keying — that one enum value is
 * what keeps the loop from feeding itself." The loop is real and short: `ApplyClipToControls` below reads
 * a clip and writes control poses, so every scrubbed frame would look exactly like an animator moving
 * every control, and the keyer would write back what it had just read. On a channel with tangents that is
 * not even idempotent — the re-keyed values reshape the curve that produced them.
 *
 * `ControlWriteSource` is that enum, and it is a parameter of the WRITE rather than a mode on the object,
 * so it cannot be left switched on by a path that returned early. `Playback` writes the pose and stops;
 * it does not even become pending, because a pending control would be keyed by the `EndInteraction` of an
 * interaction that happened to be open.
 *
 * ── WHY THE WRITE IS A FUNNEL AND `SetPose` IS NOT CALLED DIRECTLY ───────────────────────────────────
 *
 * A flag on a context only means something if everything that writes goes past it. `Write` is therefore
 * the authoring operation — it stores the pose AND decides about the key — while `ControlHierarchy::
 * SetPose` remains the storage operation underneath it. `ApplyClipToControls` uses the same funnel with
 * the flag raised, which is what makes the flag load-bearing rather than decorative: delete the
 * `Playback` branch and scrubbing re-keys every control on every frame, which is exactly what the suite
 * mutates to check.
 *
 * ── THE SAME DEFERRAL FOR A BONE, AND WHY THERE IS NOT A SECOND KEYER (A28) ──────────────────────────
 *
 * The Sequencer's Record mode keys BONES, not controls, and it had no interaction at all: every mouse-move
 * frame in which the posed bone differed called a keying routine of its own. The two rules above are not
 * about controls — they are about an INTERACTION and a named transform track — so a second class with a
 * second upsert in it would be this file's own warning in a new costume. `KeySubject` is therefore a
 * {kind, index} pair and the pending list holds subjects; everything else is shared.
 *
 * TWO ASYMMETRIES, BOTH DELIBERATE:
 *
 *   1. `WriteBone` DOES NOT TAKE A POSE. A control's storage is the hierarchy and the keyer owns that
 *      write, which is what makes `ControlWriteSource` load-bearing. A bone's storage is the Animator's
 *      authoring buffer, which the gizmo writes through `Animator::SetBoneLocalPose` — a funnel the keyer
 *      cannot stand in front of without duplicating that function's refusals. So the keyer READS the bone
 *      pose from `ControlKeyTarget::AuthoredPose` instead, at the moment it commits.
 *   2. `WriteBone` HAS NO `ControlWriteSource`. Report 01 §823's loop cannot form here and the line that
 *      says so is `Animator.hpp:157`: playback writes `m_EvaluatedPose`, the authoring buffer is
 *      "UNAFFECTED by it". A flag that no path can raise is a knob that hides the absence of the problem
 *      it claims to solve, so there is not one.
 *
 * ── WHAT THIS FILE IS NOT ────────────────────────────────────────────────────────────────────────────
 *
 * `ApplyClipToControls` is the READ side of keying and nothing more: it samples named tracks onto
 * controls. It is NOT T5.4 — there is no Animator change, no ECS change and no pose the rig contributes
 * to; the rig is still not a stage of the animation pipeline, and making it one is the next task.
 */

#include <Engine/Animation/AnimationClip.hpp>
#include <Engine/Animation/Rig/ControlHierarchy.hpp>

#include <Common/Core/ResultStr.hpp>

#include <cstdint>
#include <vector>

namespace Desert::Animation
{
    class Skeleton;

    /**
     * @brief Who is writing a control's pose. THE "NEVER KEY" FLAG of report 01 §823.
     *
     * Two values and no third: "do not care" is what UE's `EControlRigSetKey::DoNotCare` is, and it means
     * "consult a mode stored somewhere else" — a second place for the answer to live, which is how the
     * question "why did this key appear" stops being answerable.
     */
    enum class ControlWriteSource : uint8_t
    {
        Authored, ///< a person moved it. The keyer's business.
        Playback, ///< an evaluated clip put it there. NEVER KEY — it is what the keyer just read.
    };

    /**
     * @brief Which of a rig's two kinds of animatable thing a key is about.
     *
     * A track name is the only binding key there is (see the file note), so from the CLIP's point of view
     * these two are the same thing; the kind says only where the name and the value are read from.
     */
    enum class KeySubjectKind : uint8_t
    {
        Control, ///< name and pose come from the `ControlHierarchy`
        Bone,    ///< name comes from the `Skeleton`, pose from `ControlKeyTarget::AuthoredPose`
    };

    /// One thing an interaction is moving. A pair rather than a tagged index, because an index whose
    /// meaning depends on a bit stored elsewhere is how a control's keys end up on a bone.
    struct KeySubject
    {
        KeySubjectKind Kind  = KeySubjectKind::Control;
        uint32_t       Index = 0;

        [[nodiscard]] bool operator==( const KeySubject& other ) const
        {
            return Kind == other.Kind && Index == other.Index;
        }
    };

    /**
     * @brief What a key is written into, and where. PASSED PER CALL, NEVER STORED.
     *
     * The same reason `ControlHierarchy::Evaluate` takes the pose as an argument: a keyer holding a
     * pointer to a clip would owe the ownership register an argument about outliving an asset unload, and
     * the honest version of that argument is "it does not".
     */
    struct ControlKeyTarget
    {
        ControlHierarchy* Hierarchy = nullptr;
        const Skeleton*   Skeleton  = nullptr; ///< the one the rig was evaluated against, for the name check
        AnimationClip*    Clip      = nullptr;
        FrameNumber       Tick;

        /// Where a BONE subject's value is read from — the Animator's authoring buffer, not its bind pose
        /// and not its evaluated pose. Null is legal and means "this target keys controls only"; a bone
        /// write against such a target is refused rather than keyed from whatever the bind pose says,
        /// because a key holding the bind pose is the §936 defect (a keyframe that moves the bone).
        const LocalPose* AuthoredPose = nullptr;
    };

    /**
     * @brief The keyer's whole state: whether an interaction is open, and what moved during it.
     *
     * It deliberately holds NO hierarchy, NO clip and NO transform. A drag's captured pose lives in
     * `ControlDrag::PoseAtGrab`, the value keyed is read from the hierarchy at the end, and both of those
     * are somebody else's. What is left is the one thing that is genuinely the keyer's: the interaction.
     */
    class ControlKeyer
    {
    public:
        /**
         * @brief Open an interaction. Until it ends, a write remembers instead of keying.
         *
         * Refuses a second one rather than nesting. A nested interaction has to decide whose end commits,
         * and every answer to that is a rule nobody can predict from the outside; a manipulator that
         * begins twice has lost track of its own drag, and saying so is more useful than coping.
         */
        [[nodiscard]] Common::BoolResultStr BeginInteraction();

        [[nodiscard]] bool Interacting() const
        {
            return m_Interacting;
        }

        /// How many controls are waiting for the interaction to end. The count the deferral claim is
        /// asserted through: during a drag this is 1 and the clip is untouched.
        [[nodiscard]] size_t Pending() const
        {
            return m_Pending.size();
        }

        /**
         * @brief Put @p pose on @p control, and key it if that is what this write means.
         *
         * @return the number of controls keyed by THIS call: 1 when it keyed, 0 when it deferred or when
         *         the source forbids keying. A count rather than a bool because `EndInteraction` returns
         *         the same quantity and they have to be addable.
         *
         * Refuses an incomplete target, an unknown control, a tick outside the clip, and a control whose
         * name is a bone's (see the file note). A refusal writes NOTHING — not the pose either, because a
         * pose stored under a control that cannot be keyed is animation the animator will lose without
         * being told.
         */
        [[nodiscard]] Common::ResultStr<uint32_t> Write( const ControlKeyTarget& target, uint32_t control,
                                                         const BoneTransform& pose, ControlWriteSource source );

        /**
         * @brief The same operation for a BONE the animator has posed. See the file note for the two
         *        asymmetries (no pose argument, no write source) and why each of them is the honest shape.
         *
         * @return 1 when it keyed, 0 when it deferred. Refuses a target with no `AuthoredPose`, a bone
         *         index the skeleton does not have, and everything `Check` refuses for a control.
         */
        [[nodiscard]] Common::ResultStr<uint32_t> WriteBone( const ControlKeyTarget& target, uint32_t bone );

        /**
         * @brief Close the interaction and key every control that moved during it, once each.
         *
         * The value keyed is read from the hierarchy NOW, not remembered from the writes: that is the
         * "resolve before you commit" half of §971, and it is what makes a mid-drag space switch or a
         * parent moving under the control come out right — the key records where the control ended, not
         * the sequence of places the pointer put it.
         *
         * @return how many controls were keyed. Zero is a legitimate answer: an interaction in which
         *         nothing moved keys nothing.
         */
        [[nodiscard]] Common::ResultStr<uint32_t> EndInteraction( const ControlKeyTarget& target );

        /**
         * @brief Close the interaction and key NOTHING.
         *
         * The cancelled drag — Escape, or a drag that began on a control and was refused halfway. It is a
         * separate function rather than a flag on the end because "abandon" and "commit" are the two
         * things a caller must not be able to confuse, and a bool parameter at a call site is exactly how
         * they get confused.
         */
        void CancelInteraction();

    private:
        bool                    m_Interacting = false;
        std::vector<KeySubject> m_Pending;
    };

    /**
     * @brief Put every control where the clip says it is at `target.Tick`. THE READ SIDE OF KEYING.
     *
     * Writes through @p keyer with `ControlWriteSource::Playback`, which is the whole demonstration that
     * the flag does something: this function moves every control that has a track, and keys none of them.
     *
     * A control with no track of its own is LEFT WHERE IT IS rather than reset to identity — an unkeyed
     * control has no animation, and "no animation" is not "at the origin". That was the defect §936 names
     * in the Sequencer's own "+" button, in a different costume.
     *
     * @return how many controls were moved.
     */
    [[nodiscard]] Common::ResultStr<uint32_t> ApplyClipToControls( const ControlKeyTarget& target,
                                                                   ControlKeyer&           keyer );
} // namespace Desert::Animation
