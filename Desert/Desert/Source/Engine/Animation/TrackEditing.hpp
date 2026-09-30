#pragma once

/**
 * EDITING A TRACK, AS OPPOSED TO SAMPLING ONE.
 *
 * Two operations, and both exist because the Sequencer was doing them wrong in a way that only a curve
 * makes visible:
 *
 *   * RECOMPUTING THE TANGENTS after any edit. A tangent is computed from a key's NEIGHBOURS, so moving,
 *     adding or deleting one key changes the curve on both sides of it. UE recomputes the whole channel
 *     at six separate call sites for this reason (report 05 §969 item 1), and doing it per-edited-key is
 *     the version that looks right until the second edit.
 *   * INSERTING A KEY WITHOUT MOVING THE POSE (§936). The panel's "Add Key @ Playhead" buttons used to
 *     insert `glm::vec3( 0.0f )` — a position key AT THE ORIGIN, which yanks the bone across the scene
 *     the moment it is pressed. A key added at the playhead means "record what the curve says here".
 *
 * It lives beside the clip rather than inside the panel because these are rules about a track, and a
 * second editor (a curve view, T4.3) must not have to restate them.
 *
 * ── WHERE THE KEYS LIVE (ANIM-I8b) ────────────────────────────────────────────────────────────────────
 *
 * UE's split: IAnimationDataModel holds the data, IAnimationDataController edits it (SetBoneTrackKeys /
 * UpdateBoneTrackKeys). Here the model is the clip's `Timeline::Sequence`, and a bone's keys are the
 * `TransformChannel` of a section of that bone's Transform track (Bone binding, locator = bone name,
 * property ""). There is no second key list. Two levels:
 *
 *   * CHANNEL rules (`RefreshTangents` .. `MoveKey`) — what an edit does to one `TransformChannel`;
 *   * SEQUENCE edits (`SetBoneKey`, `InsertBoneKey`, `RemoveBoneKey`, `MoveBoneKey`) — find or create the
 *     binding, the track and the section to key, apply the channel rule, and bump `Sequence.Revision`.
 *     The revision bump is part of the edit, never the caller's chore: the Animator's binding cache is
 *     keyed by it (Animator.cpp, ClipBinding), and an edit that forgot it would be played from a stale
 *     table.
 *
 * THE SECTION A KEY LANDS IN: the topmost section (highest `Row`, then last in the list — the one the fold
 * evaluates last, so the one whose value is SEEN) covering the tick. No section covers it → a new Absolute
 * full-weight section over the whole clip [Start, End] on a row BELOW every existing one, so it fills the
 * gaps and hides no authored section. A tick outside the clip is refused: lengthening a clip is a clip edit.
 */

#include <Engine/Animation/AnimationClip.hpp>
#include <Engine/Animation/Timeline/Channel.hpp>
#include <Engine/Animation/Timeline/Sequence.hpp>
#include <Engine/Animation/Timeline/Track.hpp>

#include <Common/Core/ResultStr.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace Desert::Animation
{
    /// Which of a Transform channel's three parts an edit is about.
    enum class TrackChannel : uint8_t
    {
        Position,
        Rotation,
        Scale,
    };

    // ── Channel rules ────────────────────────────────────────────────────────────────────────────────

    /// Any key in any component of the channel.
    [[nodiscard]] bool HasKeys( const Timeline::TransformChannel& channel );

    /**
     * @brief Recompute every `Auto` key's tangents across translation and scale (§969 item 1).
     *
     * Rotation carries no tangents (a cubic through quaternions is not a rotation; Channel.hpp). A component
     * with one key is a constant: its `Auto` key gets flat tangents and a `User`/`Break` key keeps its slope —
     * the slope starts to matter the moment a second key arrives, and it must still be there then.
     */
    void RefreshTangents( Timeline::TransformChannel& channel, FrameRate tickRate );

    /**
     * @brief One component's scalar keys — the numbers the tangent maths, the evaluator and the curve view
     * all read. Rotation returns EMPTY: a quaternion's components are not curves an animator can read.
     * @param component 0, 1 or 2 (x, y, z); anything else returns empty.
     */
    [[nodiscard]] std::vector<ScalarKey> LiftChannel( const Timeline::TransformChannel& channel,
                                                      TrackChannel part, int component );

    /**
     * @brief Write edited scalars back into one component: value, interp, mode and both tangents per key.
     * REFUSES (false, nothing changed) a size or tick mismatch — this moves VALUES; a retime is `MoveKey`.
     */
    [[nodiscard]] bool ApplyChannel( Timeline::TransformChannel& channel, TrackChannel part, int component,
                                     const std::vector<ScalarKey>& scalars );

    /**
     * @brief Insert a key at `tick` holding what the part ALREADY says there, seeded with its slope (§936),
     * as `User` so the auto pass does not reshape the curve it was read from. false when the part has no
     * keys (there is no curve to read — `InsertFirstKeyFromPose`) or a key already sits on the tick.
     */
    [[nodiscard]] bool InsertKeyFromCurve( Timeline::TransformChannel& channel, TrackChannel part,
                                           FrameNumber tick, FrameRate tickRate );

    /**
     * @brief The FIRST key of an EMPTY part: the bone's current local pose, so recording it moves nothing.
     * Flat tangents (one key has no neighbours). false when the part is not empty or the matrix does not
     * decompose.
     */
    [[nodiscard]] bool InsertFirstKeyFromPose( Timeline::TransformChannel& channel, TrackChannel part,
                                               FrameNumber tick, const glm::mat4& localPose );

    /**
     * @brief Make @p tick say @p pose in all three parts — AN UPSERT, the keying operation.
     *
     * An existing key keeps its shape (interp, tangent mode, tangents); a new key is Cubic/Auto on
     * translation and scale and Linear on rotation (`InsertFirstKeyFromPose`'s convention). Auto tangents are
     * refreshed afterwards. Refuses a non-finite pose and changes nothing.
     */
    [[nodiscard]] bool SetTransformKey( Timeline::TransformChannel& channel, FrameNumber tick,
                                        const BoneTransform& pose, FrameRate tickRate );

    /// Delete the part's key at @p tick from every component that has one; refresh tangents. false = none.
    [[nodiscard]] bool RemoveKey( Timeline::TransformChannel& channel, TrackChannel part, FrameNumber tick,
                                  FrameRate tickRate );

    /**
     * @brief Retime the part's key @p from → @p to in every component that has one, keeping its shape.
     * false (nothing changed) when no key is on @p from or any component already has a key on @p to — a
     * move that merged two keys would silently delete one.
     */
    [[nodiscard]] bool MoveKey( Timeline::TransformChannel& channel, TrackChannel part, FrameNumber from,
                                FrameNumber to, FrameRate tickRate );

    // ── Sequence edits (the controller) ──────────────────────────────────────────────────────────────

    /// The Transform track of the Bone binding whose locator is @p bone, or null.
    [[nodiscard]] const Timeline::Track* FindBoneTrack( const Timeline::Sequence& sequence, std::string_view bone );
    [[nodiscard]] Timeline::Track*       FindBoneTrack( Timeline::Sequence& sequence, std::string_view bone );

    /// The bone's Transform track, its binding and track created (no section) when missing — Revision++ then.
    [[nodiscard]] Timeline::Track& AddBoneTrack( Timeline::Sequence& sequence, const std::string& bone );

    /// Any key in any section of the track.
    [[nodiscard]] bool HasKeys( const Timeline::Track& track );

    /// The channel a key at @p tick lands in (see the file note), or null when no section covers the tick.
    [[nodiscard]] Timeline::TransformChannel* KeyedChannelAt( Timeline::Track& track, FrameNumber tick );

    /// `KeyedChannelAt`, or a new Absolute section over the whole clip below every existing row. Revision++
    /// when a section is created.
    [[nodiscard]] Timeline::TransformChannel& ChannelForKey( Timeline::Sequence& sequence, Timeline::Track& track,
                                                             FrameNumber tick );

    /// Upsert @p pose at @p tick on @p bone's track (binding, track and section created as needed). Revision++.
    [[nodiscard]] Common::BoolResultStr SetBoneKey( Timeline::Sequence& sequence, const std::string& bone,
                                                    FrameNumber tick, const BoneTransform& pose );

    /// `InsertKeyFromCurve` on the section keyed at @p tick; refuses a bone with no track. Revision++.
    [[nodiscard]] Common::BoolResultStr InsertBoneKey( Timeline::Sequence& sequence, std::string_view bone,
                                                       TrackChannel part, FrameNumber tick );

    /// `RemoveKey` on the topmost section holding a @p part key on @p tick. Revision++.
    [[nodiscard]] Common::BoolResultStr RemoveBoneKey( Timeline::Sequence& sequence, std::string_view bone,
                                                       TrackChannel part, FrameNumber tick );

    /// `MoveKey` on the topmost section holding a @p part key on @p from; @p to must lie in the clip. Revision++.
    [[nodiscard]] Common::BoolResultStr MoveBoneKey( Timeline::Sequence& sequence, std::string_view bone,
                                                     TrackChannel part, FrameNumber from, FrameNumber to );
} // namespace Desert::Animation
