#pragma once

/**
 * ONE INTERACTION, ONE UNDO STEP — FOR THE OTHER TIMELINE, THE ONE OVER A UI ELEMENT.
 *
 * `SequencerPanel::DrawUITracks` edits a `UIAnimComponent` ON THE ENTITY. It is not an
 * `AnimationClip`, it has no `Animator` and it has no `ControlKeyer`, so neither half of
 * `PoseEditTransaction` — an authoring pose buffer and a list of `BoneTrack`s — is about this subject
 * and that transaction cannot be pointed at it. Before this file the whole lane editor was outside the
 * undo stack: add a lane, drag a key, retype a colour, and Ctrl+Z walked past all of it into whatever
 * property edit came before.
 *
 * ── WHY A SNAPSHOT AND NOT AN INVERSE COMMAND (a DIFFERENT reason from the pose one) ─────────────────
 *
 * `PoseEditTransaction.hpp` refuses an inverse because writing a key REFRESHES THE WHOLE TRACK'S AUTO
 * TANGENTS, so "delete the key I added" leaves its neighbours holding slopes computed against a key
 * that is gone. UI lanes have no tangents at all: `UIAnimKey::Easing` is authored per key and
 * `UICanvasRenderer2D::ApplyAnimClip` reads nothing but the two keys around the playhead. So that
 * argument does NOT carry over, and it was checked rather than assumed.
 *
 * The inverse is refused here for a different reason, and it is about IDENTITY:
 *
 *   * A LANE IS KEPT SORTED BY TIME. Both editing paths — the lane's `+` and a key drag — re-sort the
 *     vector, so a key's INDEX is derived from the whole lane's contents. An inverse addressed by index
 *     describes a key that may no longer be there.
 *   * A KEY HAS NO IDENTITY TO ADDRESS IT BY INSTEAD. `+` appends at the playhead with no dedupe, so
 *     two keys at exactly the same time are an ordinary authored state — and then "remove the key at
 *     time T" names two of them and can pick the wrong one. There is no id to fall back on, because
 *     `UIAnimKey` is three plain fields and the format (v3) stores no id.
 *
 * And the SIZE argument that forced the pose command to store a diff is simply absent: a UI clip is a
 * handful of lanes holding a handful of keys — `UIAnimKey` is 32 bytes — where a cooked skeletal clip is
 * a hundred tracks of hundreds of keys. Two whole copies of the authored clip per history entry is a
 * few kilobytes at worst, so the bookkeeping a diff would need buys nothing and can be wrong.
 *
 * ── WHAT IS STORED: THE CONTENT, NEVER THE TRANSPORT ─────────────────────────────────────────────────
 *
 * `UIClipContent` below is the authored half of `UIAnimData`: `Tracks`, `Duration`, `Loop`. The other
 * two fields are deliberately NOT in it:
 *
 *   * `Time` is the playhead and is runtime-only — `Components.hpp` says so and the serializer proves
 *     it (`UIAnimComponentSer` has no such field). Scrubbing is not an edit, and an undo that rewound
 *     the playhead would be undoing something nobody did.
 *   * `Playing` IS serialized, and it is still out, because inside this panel it is TRANSPORT: three
 *     separate places set it false as a side effect of the mouse being down (the Time slider, a key
 *     drag, the playhead), and exactly one of those is inside a transaction. An undo that RESUMED
 *     PLAYBACK because the drag had paused it would be the panel's own side effect coming back as if
 *     it were the animator's decision. The Play/Pause button is how playback is chosen, and it is not
 *     an undo step. THIS IS A DECISION, not an omission — the census below is what makes it visible.
 *
 * THE CENSUS IS A STRUCTURED BINDING, exactly as `PoseEditTransaction`'s is: `SameStoredValue` and
 * `Capture` destructure their aggregate, so a field added to `UIAnimKey` or `UIAnimData` does not fall
 * quietly out of the comparison or out of the snapshot — it fails to compile, here, with the field list
 * in front of whoever added it.
 *
 * ── WHY IT IS VOLATILE ───────────────────────────────────────────────────────────────────────────────
 *
 * The command holds a `UIAnimData*` into a live component, exactly as `CommandHistory::ByteCommand`
 * holds a field address, and it is the same pointer with the same hazard: entt relocates a pool when it
 * grows, so the address can die without the ENTITY dying. So it reports `IsVolatile()` and
 * `CommandHistory::DropVolatile` — called by every structural command and by every selection change —
 * drops it. See the `PointerOwnership` register for the row.
 */

#include <Editor/Core/CommandHistory.hpp>

#include <Common/Core/ResultStr.hpp>

#include <Engine/ECS/Components.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace Desert::Editor
{
    /// Value equality over the STORED representation of one UI keyframe, every field included.
    [[nodiscard]] bool SameStoredValue( const ECS::UIAnimKey& a, const ECS::UIAnimKey& b );
    /// Same, for a lane: its property AND its keys in order. Order is part of the value here — the lane
    /// is kept sorted, so two lanes holding the same keys in a different order is a state no editing
    /// path produces and one an undo would have to put back exactly.
    [[nodiscard]] bool SameStoredValue( const ECS::UIAnimTrack& a, const ECS::UIAnimTrack& b );

    /**
     * @brief The authored half of a UI clip, by value. See the file note for what is left out and why.
     */
    struct UIClipContent
    {
        std::vector<ECS::UIAnimTrack> Tracks;
        float                         Duration = 1.0F;
        bool                          Loop     = false;
    };

    /// Read the authored content out of a live clip. The playhead and the transport are not read.
    [[nodiscard]] UIClipContent CaptureUIClip( const ECS::UIAnimData& clip );

    /// Write authored content back into a live clip, leaving the playhead and the transport alone.
    void RestoreUIClip( ECS::UIAnimData& clip, const UIClipContent& content );

    [[nodiscard]] bool SameStoredValue( const UIClipContent& a, const UIClipContent& b );

    /**
     * @brief ONE interaction's net effect on one UI clip, as one undo entry.
     */
    class UIClipCommand final : public ICommand
    {
    public:
        UIClipCommand( ECS::UIAnimData* clip, UIClipContent before, UIClipContent after );

        bool Undo() override;
        bool Redo() override;

        bool IsVolatile() const override
        {
            return true;
        }

        std::string GetLabel() const override;

        /// For the suite: how many lanes this entry actually differs in. An entry that pushed with
        /// nothing in it would satisfy every count-based assertion about undo steps while undoing
        /// nothing — the hole §8.4 is about.
        [[nodiscard]] size_t ChangedTracks() const;

    private:
        bool Apply( const UIClipContent& state );

        ECS::UIAnimData* m_Clip = nullptr;
        UIClipContent    m_Before;
        UIClipContent    m_After;
    };

    /**
     * @brief The interaction boundary: the only thing that decides what "one undo step" means here.
     *
     * ONE DRIVER, NOT TWO, and that is the difference from `PoseEditTransaction`. That one needs an
     * `Observe` because the bone gizmo's drag edges live in another file and arrive as a published bit.
     * Every edit in `DrawUITracks` is an ImGui widget whose own edges are right at the call site
     * (`IsItemActivated` / `IsItemDeactivatedAfterEdit`, or a button press that is instantaneous), so an
     * edge-driven second driver here would be a mechanism with no caller — and a transaction that can be
     * closed by two things has to decide which one wins.
     */
    class UIClipEditTransaction
    {
    public:
        /// Open a transaction the caller will close. Refuses while one is open: nesting would have to
        /// decide whose `End` commits.
        [[nodiscard]] Common::BoolResultStr Begin( ECS::UIAnimData* clip );

        /// Close it and push AT MOST ONE history entry. Returns how many were pushed: 0 when the
        /// interaction changed no authored content (a drag that moved nothing is not an undo step), 1
        /// otherwise.
        [[nodiscard]] Common::ResultStr<uint32_t> End();

        /// Close it and push nothing — the abandoned edit.
        void Cancel();

        [[nodiscard]] bool Open() const
        {
            return m_Open;
        }

        /// The clip this transaction is open on, or null. The panel sweeps a transaction left open by a
        /// widget that stopped being drawn (a lane deleted under its own key editor), and it must not
        /// commit that one against a DIFFERENT clip the next frame resolved.
        [[nodiscard]] const ECS::UIAnimData* Subject() const
        {
            return m_Clip;
        }

    private:
        ECS::UIAnimData* m_Clip = nullptr;
        bool             m_Open = false;
        UIClipContent    m_Before;
    };

    /// `Begin` + `End` around one statement, for the instantaneous edits (a lane's `+`, "Delete key").
    /// A guard rather than a `RunOnce(lambda)` because the edit in between is ImGui code that wants to
    /// keep talking to the panel's own members — the same shape and the same reason as `ScopedPoseEdit`.
    class ScopedUIClipEdit
    {
    public:
        ScopedUIClipEdit( UIClipEditTransaction& transaction, ECS::UIAnimData* clip );
        ~ScopedUIClipEdit();

        ScopedUIClipEdit( const ScopedUIClipEdit& )            = delete;
        ScopedUIClipEdit& operator=( const ScopedUIClipEdit& ) = delete;

    private:
        UIClipEditTransaction& m_Transaction;
        bool                   m_Opened = false;
    };
} // namespace Desert::Editor
