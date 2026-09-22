#pragma once

/**
 * ONE DRAG, ONE UNDO STEP — FOR THE HALF OF THE SEQUENCER THAT HAD NONE.
 *
 * Before this file, `SequencerPanel.cpp` did not mention `CommandHistory` once and both of
 * `GizmoController`'s undo guards read `!usePose`, so the whole POSE branch — the gizmo writing the
 * Animator's authoring buffer, and every key the `ControlKeyer` wrote out of it — was outside the undo
 * stack. Ctrl+Z after an afternoon of posing walked back through property edits and entity renames and
 * skipped the animation entirely.
 *
 * Report 05 §971 asks for "one undo transaction and one key per control per interaction". `ControlKeyer`
 * shipped the second half (A28: the keys are deferred to the release edge). This is the first half, and
 * it is deliberately driven by THE SAME EDGE, so the two cannot disagree about what one interaction is.
 *
 * ── WHY A SNAPSHOT AND NOT AN INVERSE COMMAND ────────────────────────────────────────────────────────
 *
 * The obvious undo for "a key was written at tick T" is "remove the key at tick T". It is wrong here, and
 * `TrackEditing.hpp` says why at `SetTransformKey`: THE WHOLE TRACK'S AUTO TANGENTS ARE REFRESHED after
 * the upsert, because the new key is a new neighbour for the two keys around it. An inverse that deletes
 * the key leaves those neighbours holding slopes computed against a key that no longer exists — the curve
 * does not come back, it comes back NEARLY, which is the middle-link shape this tree keeps paying for. A
 * by-value snapshot of the affected tracks restores derived state without needing to know it is derived.
 *
 * The same argument applies to the pose: `showKeyedPose` in the panel reloads the WHOLE authoring buffer
 * from the clip after a key lands, so the bones an interaction changed are not only the bone that was
 * dragged. The transaction therefore diffs the whole buffer rather than remembering "the dragged bone".
 *
 * ── WHAT IS STORED, AND WHY IT IS A DIFF AND NOT THE WHOLE CLIP ──────────────────────────────────────
 *
 * The full "before" copy is taken and held only while the interaction is open — one copy, not one per
 * history entry. What the command keeps is the CHANGED tracks and the CHANGED bones. A cooked clip is
 * 100 bones x hundreds of keys; storing it whole twice per entry against `CommandHistory::kMaxEntries`
 * (256) is hundreds of megabytes for a drag that moved one bone.
 *
 * ── WHY IT IS VOLATILE ───────────────────────────────────────────────────────────────────────────────
 *
 * The command holds an `Animator*` and an `AnimationClip*`, exactly as `CommandHistory::ByteCommand`
 * holds a field address, and for the same reason: the Animator is a `unique_ptr` member of
 * `AnimationComponent` (no `weak_ptr` exists to take) and the clip lives inside an `AnimationAsset` that
 * an eviction may unload. So it reports `IsVolatile()` and `CommandHistory::DropVolatile` drops it
 * whenever its target may have died. See the `PointerOwnership` register for the two rows.
 */

#include <Editor/Core/CommandHistory.hpp>

#include <Common/Core/ResultStr.hpp>

#include <Engine/Animation/AnimationClip.hpp>
#include <Engine/Animation/ClipSection.hpp>
#include <Engine/Animation/Pose.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace Desert::Animation
{
    class Animator;
}

namespace Desert::Editor
{
    /**
     * @brief Value equality over the STORED REPRESENTATION, reserved fields included.
     *
     * NOT `operator==` on the engine's types, and that is a decision. "Are these two keys the same key"
     * and "is there anything here an undo would have to put back" are different questions: a key whose
     * `ArriveWeight` differs is the same animation today (weights are RESERVED — `AnimationClip.hpp`
     * §969) and is still a byte an undo owes the animator. One function answering both questions is how a
     * comparison drifts away from one of them.
     *
     * EACH OVERLOAD DESTRUCTURES ITS ARGUMENT, AND THAT IS THE CENSUS. A structured binding names every
     * non-static data member of an aggregate and the arity is checked by the language, so adding a field
     * to `PositionKeyFrame` does not silently fall out of the comparison — it fails to compile, here,
     * with the field list in front of whoever added it. A `sizeof` pin would have done the same job on
     * this machine and been a coin toss on MSVC; this one is a language rule, not a layout.
     */
    [[nodiscard]] bool SameStoredValue( const Animation::BoneTransform& a, const Animation::BoneTransform& b );
    [[nodiscard]] bool SameStoredValue( const Animation::PositionKeyFrame& a,
                                        const Animation::PositionKeyFrame& b );
    [[nodiscard]] bool SameStoredValue( const Animation::RotationKeyFrame& a,
                                        const Animation::RotationKeyFrame& b );
    [[nodiscard]] bool SameStoredValue( const Animation::ScaleKeyFrame& a, const Animation::ScaleKeyFrame& b );
    [[nodiscard]] bool SameStoredValue( const Animation::BoneTrack& a, const Animation::BoneTrack& b );
    [[nodiscard]] bool SameStoredValue( const Animation::ScalarKey& a, const Animation::ScalarKey& b );
    /// A SECTION IS PART OF THE CLIP, so an interaction that edits one is an interaction on the clip and
    /// belongs in the same entry as the keys it was authored beside. The alternative — a second command
    /// class with a second transaction driving it — would give one Ctrl+Z two meanings depending on which
    /// widget the animator last touched, which is the state this file was written to end.
    [[nodiscard]] bool SameStoredValue( const Animation::ClipSection& a, const Animation::ClipSection& b );
    /// `LocalPose` is a class with a private vector, so it has no structured binding; it is compared
    /// through the two things it does promise — its length and its index-for-index bones (Pose.hpp).
    [[nodiscard]] bool SameStoredValue( const Animation::LocalPose& a, const Animation::LocalPose& b );

    /**
     * @brief ONE interaction's net effect on a pose buffer and a clip, as one undo entry.
     *
     * Undo/Redo put BOTH halves back. Restoring only the clip would leave the posed skeleton on screen
     * saying something the clip no longer contains, and restoring only the pose would leave the key
     * behind — the state on screen and the state in the file would disagree, which is exactly the class
     * of defect keying was supposed to end.
     */
    class ClipPoseCommand final : public ICommand
    {
    public:
        /// One bone whose authored transform moved during the interaction.
        struct BoneDelta
        {
            uint32_t                 Bone = 0;
            Animation::BoneTransform Before;
            Animation::BoneTransform After;
        };

        /// One track that was changed, created or (for completeness) removed. `HasBefore == false` means
        /// the interaction CREATED it — `ControlKeyer` appends a track for a subject the clip had none
        /// for — and undo must take it away again, not leave an empty one behind.
        struct TrackDelta
        {
            size_t               Index     = 0;
            bool                 HasBefore = false;
            bool                 HasAfter  = false;
            Animation::BoneTrack Before;
            Animation::BoneTrack After;
        };

        /**
         * @brief The clip's SECTION LIST, whole, before and after — and why it is not a per-section diff.
         *
         * The tracks above are diffed because a cooked clip is a hundred of them holding hundreds of keys
         * each. A section list is a handful of names, four numbers and a short weight channel, so the two
         * copies cost less than the bookkeeping a diff would need — and the operations are not per-element
         * anyway: `ReorderSection` swaps two, `RemoveSection` shifts every index after it, and a diff
         * keyed on index would have to describe both as "everything from here changed".
         *
         * `Changed` false is an interaction that did not touch a section; `Apply` then leaves the list
         * alone, which is what lets one entry carry a pose, a key and a section without the two halves
         * overwriting each other.
         */
        struct SectionEdit
        {
            bool                                Changed = false;
            std::vector<Animation::ClipSection> Before;
            std::vector<Animation::ClipSection> After;
        };

        ClipPoseCommand( Animation::Animator* animator, Animation::AnimationClip* clip,
                         std::vector<BoneDelta> bones, std::vector<TrackDelta> tracks, size_t poseSizeBefore,
                         size_t poseSizeAfter, size_t trackCountBefore, size_t trackCountAfter,
                         // NO DEFAULT ARGUMENT, and not for style: a defaulted `{}` here needs
                         // `SectionEdit`'s own member initialiser inside the enclosing class, which the
                         // language refuses. Passing it is also the honest shape — every caller knows
                         // whether the interaction touched a section, and a default would let one forget.
                         SectionEdit sections );

        bool Undo() override;
        bool Redo() override;

        bool IsVolatile() const override
        {
            return true;
        }

        std::string GetLabel() const override;

        /// For the suite: how much this entry is actually carrying. A transaction that pushed an entry
        /// holding nothing would satisfy every count-based assertion while undoing nothing.
        [[nodiscard]] size_t ChangedBones() const
        {
            return m_Bones.size();
        }
        [[nodiscard]] size_t ChangedTracks() const
        {
            return m_Tracks.size();
        }
        /// Whether this entry is carrying the section list at all — the same question as `ChangedTracks`
        /// above and for the same reason: an entry that pushed with `Changed` false would satisfy every
        /// count-based assertion about undo steps while restoring no section.
        [[nodiscard]] bool CarriesSections() const
        {
            return m_Sections.Changed;
        }

    private:
        [[nodiscard]] bool Apply( bool undo );

        Animation::Animator*      m_Animator = nullptr;
        Animation::AnimationClip* m_Clip     = nullptr;
        std::vector<BoneDelta>    m_Bones;
        std::vector<TrackDelta>   m_Tracks;
        size_t                    m_PoseSizeBefore   = 0;
        size_t                    m_PoseSizeAfter    = 0;
        size_t                    m_TrackCountBefore = 0;
        size_t                    m_TrackCountAfter  = 0;
        SectionEdit               m_Sections;
    };

    /**
     * @brief The interaction boundary, and the only thing that decides what "one undo step" means.
     *
     * TWO DRIVERS, BECAUSE THE TREE HAS TWO KINDS OF EDGE, and mixing them is how a transaction gets
     * closed by the wrong widget:
     *
     *   * `Observe` — for an interaction whose edges are in ANOTHER file. The bone gizmo's drag is held in
     *     `GizmoController` and published as one bit (`Core::GizmoState::PoseInteraction`); the panel
     *     hands that bit here once a frame, exactly as it already hands it to `ControlKeyer::Observe`.
     *   * `Begin`/`End` — for an edit whose edges are right at the call site: a button press, or a
     *     dope-sheet key drag that knows its own `IsItemActivated`/`IsItemDeactivated`.
     *
     * A transaction remembers which driver opened it, and `Observe` will not close one that `Begin`
     * opened. Without that, the frame after a dope-sheet drag begins reads `held == false` and commits
     * somebody else's transaction half-way through.
     *
     * ── THE BASELINE, AND THE FRAME-ORDER TRAP IT EXISTS FOR ─────────────────────────────────────────
     *
     * `Observe` cannot capture the "before" pose on the frame the bit rises: `GizmoController` writes the
     * pose and sets the bit in the same function, and the panel that reads the bit runs afterwards. So
     * while no interaction is open, `Observe` keeps LAST FRAME'S authoring pose, and a rising edge takes
     * its "before" from that. It costs one `LocalPose` copy per idle frame (a hundred bones is four
     * kilobytes) and it is the difference between undoing a drag and undoing all of it but the first
     * frame. The CLIP needs no such baseline: nothing writes it during a drag — that is what A28's
     * deferral guarantees — so the rising edge's copy of the tracks is the true before.
     */
    class PoseEditTransaction
    {
        enum class Driver : uint8_t
        {
            Edge,    ///< opened by `Observe` off the interaction bit
            Explicit ///< opened by `Begin` at a call site that will `End` it
        };

    public:
        /// Open a transaction the caller will close itself. Refuses (and says so) while one is open:
        /// nesting would have to decide whose `End` commits, which is `ControlKeyer::BeginInteraction`'s
        /// argument and the same answer.
        [[nodiscard]] Common::BoolResultStr Begin( Animation::Animator* animator, Animation::AnimationClip* clip );

        /// Close it and push AT MOST ONE history entry. Returns how many entries were pushed: 0 when
        /// nothing changed (a drag that moved nothing is not an undo step), 1 otherwise.
        [[nodiscard]] Common::ResultStr<uint32_t> End();

        /// Close it and push nothing. The abandoned edit; separate from `End` for the reason
        /// `ControlKeyer::CancelInteraction` is separate from `EndInteraction`.
        void Cancel();

        /**
         * @brief One call per frame for an edge-driven interaction. Returns entries pushed (0 or 1).
         *
         * Must be called AFTER the keyer's own `Observe` and after whatever reloads the pose from the
         * clip, or the release frame's keys and the pose they produced land outside the transaction they
         * belong to.
         */
        [[nodiscard]] Common::ResultStr<uint32_t> Observe( Animation::Animator*      animator,
                                                           Animation::AnimationClip* clip, bool held );

        [[nodiscard]] bool Open() const
        {
            return m_Open;
        }

        /// Open, AND opened by a `Begin` that is supposed to close it. The panel sweeps these: an explicit
        /// driver is always a widget being held, so one still open while the UI reports no active item has
        /// lost its closer (the field stopped being drawn because a combo above it changed the branch),
        /// and leaving it open would swallow every later edit into one enormous undo step. The edge-driven
        /// one must NOT be swept that way -- a viewport gizmo is not an item, so it looks identical.
        [[nodiscard]] bool OpenExplicitly() const
        {
            return m_Open && m_Driver == Driver::Explicit;
        }

    private:
        Animation::Animator*              m_Animator = nullptr;
        Animation::AnimationClip*         m_Clip     = nullptr;
        bool                              m_Open     = false;
        Driver                            m_Driver   = Driver::Edge;
        bool                              m_Held     = false; ///< last frame's bit, for the edges
        Animation::LocalPose                m_PoseBefore;
        std::vector<Animation::BoneTrack>   m_TracksBefore;
        std::vector<Animation::ClipSection> m_SectionsBefore;
        /// Last frame's authoring pose while nothing is open. See the baseline note above.
        Animation::LocalPose m_Baseline;
        bool                 m_BaselineValid = false;
    };

    /// `Begin` + `End` around one statement, for the instantaneous edits (a "Key" button, a lane's "+").
    /// A guard rather than a `RunOnce(lambda)` because the edit in between is ImGui code that wants to
    /// keep talking to the panel's own members.
    class ScopedPoseEdit
    {
    public:
        ScopedPoseEdit( PoseEditTransaction& transaction, Animation::Animator* animator,
                        Animation::AnimationClip* clip );
        ~ScopedPoseEdit();

        ScopedPoseEdit( const ScopedPoseEdit& )            = delete;
        ScopedPoseEdit& operator=( const ScopedPoseEdit& ) = delete;

    private:
        PoseEditTransaction& m_Transaction;
        bool                 m_Opened = false;
    };
} // namespace Desert::Editor
