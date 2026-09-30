#pragma once

/**
 * ONE UNDO PATH FOR EVERY EDIT OF ANIMATION DATA: A TRANSACTION OVER THE OWNER'S `Timeline::Sequence`.
 *
 * UE edits a skeletal clip, a widget animation and a level sequence the same way: `FScopedTransaction`
 * opens, `UMovieScene::Modify()` records the object, the edit happens, the transaction closes and one
 * undo entry holds the object before and after. Every host here keeps its data in ONE `Timeline::Sequence`
 * (AnimationClip.hpp, Components.hpp `UIAnimData`, `.dseq`), so there is one command and one transaction
 * over it — not a "clip pose" command beside a "UI clip" command, each with its own field census that a new
 * field could fall out of.
 *
 * ── WHAT IS STORED ────────────────────────────────────────────────────────────────────────────────────
 *
 * The owner's Sequence is copied whole when the transaction OPENS (one copy, held only while it is open).
 * At close, the entry keeps the sequence's HEADER (every member but `Tracks`, i.e. host, rates, range and
 * bindings) before and after when it changed, and only the TRACKS that differ, by index. A cooked clip is a
 * hundred Transform tracks of hundreds of keys; two whole copies per entry against
 * `CommandHistory::kMaxEntries` would be hundreds of megabytes for a drag that moved one bone.
 *
 * WHY A SNAPSHOT AND NOT AN INVERSE: writing a key refreshes the whole channel's auto tangents
 * (TrackEditing.hpp `SetTransformKey`), a section edit shifts every index after it, and a UI lane has two
 * keys on one tick as an ordinary state. An inverse addressed by tick or index describes something that
 * may no longer be there; a by-value snapshot restores derived state without knowing it is derived.
 *
 * `Sequence::Revision` is NOT restored: it describes this process's copy (the Evaluator's binding cache is
 * keyed by it), so every Undo/Redo BUMPS it — a restored binding list is a structural edit.
 *
 * ── THE POSE HALF (clip hosts only) ──────────────────────────────────────────────────────────────────
 *
 * A clip edit in the Sequencer or the Animation Editor also moves the Animator's AUTHORING POSE (the
 * gizmo writes it, a key reloads it). Restoring only the sequence would leave the posed skeleton saying
 * something the clip no longer contains, so the same entry carries the changed bones when the transaction
 * was opened with an animator. One entry, one Ctrl+Z, both halves.
 *
 * ── THE OWNER, AND WHY THE ENTRY IS VOLATILE ─────────────────────────────────────────────────────────
 *
 * `SequenceOwner` resolves the sequence on every Undo/Redo rather than holding it. `OwnerOf(clip*)` and
 * `OwnerOf(UIAnimData*)` capture a raw address (an evicted AnimationAsset, an entt pool that relocated), so
 * they are VOLATILE and `CommandHistory::DropVolatile` / `DropFor(identity)` take them. An owner that
 * resolves through an asset handle or an entity UUID is not, and says so with `Volatile = false`. An entry
 * carrying the pose half is volatile whatever its owner: the Animator is a unique_ptr member of a component.
 *
 * ── THE CENSUS ───────────────────────────────────────────────────────────────────────────────────────
 *
 * `SameStoredValue` destructures every aggregate of the Timeline data model (Sequence, Binding, Track,
 * Section, the six channels, ScalarKey, EventKey, the two non-channel section contents). A structured
 * binding names every non-static member and its arity is a language rule, so a field added to any of them
 * fails to compile HERE — it cannot fall quietly out of "did this interaction change anything".
 */

#include <Editor/Core/CommandHistory.hpp>

#include <Common/Core/ResultStr.hpp>

#include <Engine/Animation/AnimationClip.hpp>
#include <Engine/Animation/Pose.hpp>
#include <Engine/Animation/Timeline/Sequence.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace Desert::Animation
{
    class Animator;
}

namespace Desert::ECS
{
    struct UIAnimData;
}

namespace Desert::Editor
{
    // ── The census: value equality over the STORED representation, reserved fields included ──────────
    [[nodiscard]] bool SameStoredValue( const Animation::ScalarKey& a, const Animation::ScalarKey& b );
    [[nodiscard]] bool SameStoredValue( const Animation::Timeline::FloatChannel& a,
                                        const Animation::Timeline::FloatChannel& b );
    [[nodiscard]] bool SameStoredValue( const Animation::Timeline::VectorChannel& a,
                                        const Animation::Timeline::VectorChannel& b );
    [[nodiscard]] bool SameStoredValue( const Animation::Timeline::RotationChannel& a,
                                        const Animation::Timeline::RotationChannel& b );
    [[nodiscard]] bool SameStoredValue( const Animation::Timeline::TransformChannel& a,
                                        const Animation::Timeline::TransformChannel& b );
    [[nodiscard]] bool SameStoredValue( const Animation::Timeline::BoolChannel& a,
                                        const Animation::Timeline::BoolChannel& b );
    [[nodiscard]] bool SameStoredValue( const Animation::Timeline::EventKey& a,
                                        const Animation::Timeline::EventKey& b );
    [[nodiscard]] bool SameStoredValue( const Animation::Timeline::EventChannel& a,
                                        const Animation::Timeline::EventChannel& b );
    /// The channel alternative is part of the value: a different alternative is a different value.
    [[nodiscard]] bool SameStoredValue( const Animation::Timeline::Channel& a,
                                        const Animation::Timeline::Channel& b );
    [[nodiscard]] bool SameStoredValue( const Animation::Timeline::AnimationSectionContent& a,
                                        const Animation::Timeline::AnimationSectionContent& b );
    [[nodiscard]] bool SameStoredValue( const Animation::Timeline::CameraCutSectionContent& a,
                                        const Animation::Timeline::CameraCutSectionContent& b );
    [[nodiscard]] bool SameStoredValue( const Animation::Timeline::Section& a,
                                        const Animation::Timeline::Section& b );
    [[nodiscard]] bool SameStoredValue( const Animation::Timeline::Binding& a,
                                        const Animation::Timeline::Binding& b );
    [[nodiscard]] bool SameStoredValue( const Animation::Timeline::Track& a, const Animation::Timeline::Track& b );
    /// Everything but `Tracks` (compared per track by the transaction) and `Revision` (not stored).
    [[nodiscard]] bool SameStoredHeader( const Animation::Timeline::Sequence& a,
                                         const Animation::Timeline::Sequence& b );
    /// The whole sequence, `Revision` excluded. What an "is this clip dirty" check asks.
    [[nodiscard]] bool SameStoredValue( const Animation::Timeline::Sequence& a,
                                        const Animation::Timeline::Sequence& b );
    [[nodiscard]] bool SameStoredValue( const Animation::BoneTransform& a, const Animation::BoneTransform& b );
    /// `LocalPose` is a class with a private vector: compared by length and index-for-index bones.
    [[nodiscard]] bool SameStoredValue( const Animation::LocalPose& a, const Animation::LocalPose& b );

    /**
     * @brief Whose Sequence an edit is on, resolved at every use (UE: the transacted UObject).
     */
    struct SequenceOwner
    {
        /// What `CommandHistory::DropFor` matches (the clip, the UIAnimData, the asset). Compared, never read.
        const void* Identity = nullptr;
        /// The live sequence, or null when the owner is gone (the entry then refuses to apply).
        std::function<Animation::Timeline::Sequence*()> Resolve;
        /// Runs after Undo/Redo wrote the sequence: runtime state derived from it (a UI clip's Player) goes.
        std::function<void()> AfterRestore;
        /// True when `Resolve` dereferences a captured raw address (see the file note).
        bool        Volatile = true;
        std::string Name;
    };

    /// A clip's sequence by address. Volatile; `Identity` is the clip (what an Animation Editor's DropFor passes).
    [[nodiscard]] SequenceOwner OwnerOf( Animation::AnimationClip* clip );
    /// A UI animation's sequence by address. Volatile; restoring drops its `Playback` so the next frame
    /// re-creates the Player from the restored range (Components.hpp `UIAnimData::Playback`).
    [[nodiscard]] SequenceOwner OwnerOf( ECS::UIAnimData* animation );

    /**
     * @brief ONE interaction's net effect on one owner's Sequence (and, for a clip, the authoring pose).
     */
    class SequenceEditCommand final : public ICommand
    {
    public:
        /// A track that changed, was created (HasBefore false) or removed (HasAfter false).
        struct TrackDelta
        {
            size_t                     Index     = 0;
            bool                       HasBefore = false;
            bool                       HasAfter  = false;
            Animation::Timeline::Track Before;
            Animation::Timeline::Track After;
        };

        /// One bone of the authoring pose that moved.
        struct BoneDelta
        {
            uint32_t                 Bone = 0;
            Animation::BoneTransform Before;
            Animation::BoneTransform After;
        };

        /// The pose half. Present only for a transaction opened with an animator.
        struct PoseEdit
        {
            Animation::Animator*   Animator = nullptr;
            std::vector<BoneDelta> Bones;
            size_t                 SizeBefore = 0;
            size_t                 SizeAfter  = 0;
        };

        /// The header (the Sequence with `Tracks` left empty), before and after, when it changed.
        struct HeaderEdit
        {
            Animation::Timeline::Sequence Before;
            Animation::Timeline::Sequence After;
        };

        SequenceEditCommand( SequenceOwner owner, std::optional<HeaderEdit> header, std::vector<TrackDelta> tracks,
                             size_t trackCountBefore, size_t trackCountAfter, std::optional<PoseEdit> pose );

        bool Undo() override;
        bool Redo() override;

        bool IsVolatile() const override
        {
            return m_Owner.Volatile || m_Pose.has_value();
        }

        [[nodiscard]] const void* EditedObject() const override
        {
            return m_Owner.Identity;
        }

        std::string GetLabel() const override;

        /// The animator whose authoring pose Undo/Redo write, or null: `DropPoseRecordsFor` matches on it.
        [[nodiscard]] const Animation::Animator* PosedAnimator() const
        {
            return m_Pose.has_value() ? m_Pose->Animator : nullptr;
        }

        // For the suite: what the entry actually carries. An entry holding nothing would satisfy every
        // count-based assertion about undo steps while undoing nothing.
        [[nodiscard]] size_t ChangedTracks() const
        {
            return m_Tracks.size();
        }
        [[nodiscard]] size_t ChangedBones() const
        {
            return m_Pose.has_value() ? m_Pose->Bones.size() : 0;
        }
        [[nodiscard]] bool CarriesHeader() const
        {
            return m_Header.has_value();
        }

    private:
        [[nodiscard]] bool Apply( bool undo );

        SequenceOwner             m_Owner;
        std::optional<HeaderEdit> m_Header;
        std::vector<TrackDelta>   m_Tracks;
        size_t                    m_TrackCountBefore = 0;
        size_t                    m_TrackCountAfter  = 0;
        std::optional<PoseEdit>   m_Pose;
    };

    /**
     * @brief The interaction boundary: one open transaction = at most one `SequenceEditCommand`.
     *
     * TWO DRIVERS, ONE TRANSACTION. `Begin`/`End` at a call site that owns both edges (an ImGui widget's
     * `IsItemActivated` / `IsItemDeactivatedAfterEdit`, a button press), and `Observe` once per frame for an
     * interaction whose edges arrive as a published "held" bit (the bone gizmo, the control keyer's release).
     * Every frame between the edges edits the live sequence; the close diffs it against the open's copy, so a
     * drag of forty frames is ONE entry (UE: the transaction spans the drag).
     */
    class SequenceEditTransaction
    {
        enum class Driver : uint8_t
        {
            Edge,    ///< opened by `Observe` off the interaction bit
            Explicit ///< opened by `Begin` at a call site that will `End` it
        };

    public:
        /// Open on @p owner; @p animator (clip hosts) adds the pose half. Refuses while one is open (nesting
        /// would have to decide whose End commits) and an owner that does not resolve.
        [[nodiscard]] Common::BoolResultStr Begin( SequenceOwner owner, Animation::Animator* animator = nullptr );

        /// Close and push AT MOST ONE entry: 0 when nothing stored changed (a click that missed is not an
        /// undo step), 1 otherwise.
        [[nodiscard]] Common::ResultStr<uint32_t> End();

        /// Close and push nothing — the abandoned edit.
        void Cancel();

        /**
         * @brief One call per frame for an edge-driven interaction. Returns entries pushed (0 or 1).
         *
         * Call AFTER the keyer's own `Observe` and after whatever reloads the pose from the clip, or the
         * release frame's keys land outside the transaction they belong to. THE RISING EDGE TAKES LAST
         * FRAME'S POSE: the manipulator writes the pose and raises the bit in the same function, so "the
         * buffer right now" has already moved; the baseline kept while nothing is open is the true before.
         */
        [[nodiscard]] Common::ResultStr<uint32_t> Observe( const SequenceOwner& owner,
                                                           Animation::Animator* animator, bool held );

        [[nodiscard]] bool Open() const
        {
            return m_Open;
        }

        /// Open AND opened by a `Begin`: the panel sweeps these when no item is active (the closer stopped
        /// being drawn). The edge-driven one must not be swept — a viewport gizmo is not an item.
        [[nodiscard]] bool OpenExplicitly() const
        {
            return m_Open && m_Driver == Driver::Explicit;
        }

        /// The owner identity this transaction is open on, or null — so a sweep does not commit it against
        /// a different owner the next frame resolved.
        [[nodiscard]] const void* Subject() const
        {
            return m_Open ? m_Owner.Identity : nullptr;
        }

    private:
        SequenceOwner                 m_Owner;
        Animation::Animator*          m_Animator = nullptr;
        bool                          m_Open     = false;
        Driver                        m_Driver   = Driver::Edge;
        bool                          m_Held     = false; ///< last frame's bit, for the edges
        Animation::Timeline::Sequence m_Before;
        Animation::LocalPose          m_PoseBefore;
        /// Last frame's authoring pose while nothing is open (the rising-edge before).
        Animation::LocalPose m_Baseline;
        bool                 m_BaselineValid = false;
    };

    /// `Begin` + `End` around one statement (a `+` button, "Delete key", a section menu item). A guard, not a
    /// `RunOnce(lambda)`, because the edit in between is ImGui code talking to the panel's own members.
    class ScopedSequenceEdit
    {
    public:
        ScopedSequenceEdit( SequenceEditTransaction& transaction, SequenceOwner owner,
                            Animation::Animator* animator = nullptr );
        ~ScopedSequenceEdit();

        ScopedSequenceEdit( const ScopedSequenceEdit& )            = delete;
        ScopedSequenceEdit& operator=( const ScopedSequenceEdit& ) = delete;

    private:
        SequenceEditTransaction& m_Transaction;
        bool                     m_Opened = false;
    };

    /// Forgets every entry whose pose half writes @p animator (a preview animator that died while its clip
    /// lives on with its other records). Returns how many went.
    size_t DropPoseRecordsFor( const Animation::Animator* animator );
} // namespace Desert::Editor
