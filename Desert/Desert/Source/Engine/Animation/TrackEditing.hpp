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
 */

#include <Engine/Animation/AnimationClip.hpp>

namespace Desert::Animation
{
    /// Which of a bone track's three channels an edit is about.
    enum class TrackChannel : uint8_t
    {
        Position,
        Rotation,
        Scale,
    };

    /**
     * @brief Recompute every `Auto` key's tangents across the whole track.
     *
     * Position and scale only: a rotation key carries no tangents, because a cubic through quaternions is
     * not a rotation (see `RotationKeyFrame`). Each vec3 channel is three scalar passes — a tangent is a
     * slope and a slope is a scalar.
     */
    void RefreshTangents( BoneTrack& track, FrameRate tickRate );

    /**
     * @brief One vec3 channel's component, as the scalar keys the tangent maths and the curve view both use.
     *
     * A CURVE VIEW THAT BUILT ITS OWN SCALARS WOULD BE A SECOND STATEMENT OF WHAT A CHANNEL IS. The tangent
     * rules, the evaluator and the picture an animator drags all have to be about the same numbers, and the
     * cheapest way to guarantee that is for there to be one function that produces them. This is the one the
     * auto pass already used, made public rather than copied.
     *
     * Rotation returns EMPTY, and that is the answer rather than a gap: a rotation key is a quaternion with
     * no tangents (see `RotationKeyFrame`), and its four components are not four curves an animator can read.
     * Euler channels would be — and they would be a format change, so they are not smuggled in through a view.
     *
     * @param component 0, 1 or 2 (x, y, z). Anything else returns empty rather than reading past the vector.
     */
    [[nodiscard]] std::vector<ScalarKey> LiftChannel( const BoneTrack& track, TrackChannel channel,
                                                      int component );

    /**
     * @brief Write edited scalars back into one channel component: value and both tangents, per key.
     *
     * REFUSES A SIZE MISMATCH instead of writing what fits. Lift-edit-apply is a chain, and the failure this
     * project keeps meeting is the middle link quietly dropping something — here that would be a curve view
     * that inserted a key into its working copy and wrote the first N back over the wrong ticks. The ticks are
     * checked too, for the same reason: this operation moves VALUES, and a retime is a different one.
     *
     * Returns false and changes nothing when the shapes disagree.
     */
    [[nodiscard]] bool ApplyChannel( BoneTrack& track, TrackChannel channel, int component,
                                     const std::vector<ScalarKey>& scalars );

    /**
     * @brief Insert a key at `tick` holding what the channel ALREADY says there, seeded with its slope.
     *
     * Returns false when a key is already on that tick — an upsert is a different operation with a
     * different meaning, and silently overwriting the key an animator is standing on is not what a button
     * called "add" should do.
     *
     * The inserted key is `User`: its tangents were taken from the curve, and the auto pass must not
     * immediately replace them with the ones the neighbours imply, which is what would move the pose.
     */
    [[nodiscard]] bool InsertKeyFromCurve( BoneTrack& track, TrackChannel channel, FrameNumber tick,
                                           FrameRate tickRate );

    /**
     * @brief The FIRST key of an EMPTY channel: record the pose the animator is already looking at.
     *
     * `InsertKeyFromCurve` refuses an empty channel deliberately — there is no curve to read there — but
     * a lane's "+" button exists precisely to key a channel from scratch, and the answer it gave was
     * `glm::vec3( 0.0f )` / an identity quaternion / a scale of 1. That is the SAME defect §936 names,
     * eleven lines below the call site that was fixed for it: the first press of "+" on an untouched
     * channel teleported the bone to the origin and called it a keyframe.
     *
     * The honest first key is the bone's CURRENT LOCAL POSE — what the animator sees on screen, so
     * recording it moves nothing. Its tangents are flat because one key has no neighbours to imply a
     * slope, which is also what `AutoSetTangents` gives an endpoint.
     *
     * Returns false when the channel is NOT empty: then the operation is `InsertKeyFromCurve`, and this
     * function silently overwriting authored keys with a decomposed matrix is exactly what it must not do.
     */
    [[nodiscard]] bool InsertFirstKeyFromPose( BoneTrack& track, TrackChannel channel, FrameNumber tick,
                                               const glm::mat4& localPose );

    /**
     * @brief Make @p tick say @p pose, in all three channels. AN UPSERT, and the only one in the tree.
     *
     * The two functions above each answer a button an animator presses ONCE: `InsertKeyFromCurve` refuses
     * an occupied tick and `InsertFirstKeyFromPose` refuses a non-empty channel, and both refusals are
     * right for "add a key here". Keying is the other operation — "the value at this tick is now THIS",
     * repeated every time the animator nudges the thing — and a refusal on the second nudge would mean the
     * FIRST nudge is the one that sticks, which is the worst of the three possible behaviours.
     *
     * AN EXISTING KEY KEEPS ITS SHAPE: interp, tangent mode and both tangents survive the write. Same
     * reason `AutoSetTangents` leaves a `User` key alone — the slope is the animator's work, and changing a
     * value is not permission to discard it. A NEW key gets Cubic/Auto on position and scale and Linear on
     * rotation, which is `InsertFirstKeyFromPose`'s convention rather than a second one: "the first key"
     * and "a key" must not differ in shape.
     *
     * THE WHOLE TRACK'S AUTO TANGENTS ARE REFRESHED afterwards (§969 item 1), because the inserted key is
     * a new neighbour for the two keys around it.
     *
     * Refuses a non-finite pose and changes nothing: a NaN written into a clip is a NaN in every pose
     * sampled from it afterwards, and it would be blamed on the sampler.
     */
    [[nodiscard]] bool SetTransformKey( BoneTrack& track, FrameNumber tick, const BoneTransform& pose,
                                        FrameRate tickRate );
} // namespace Desert::Animation
