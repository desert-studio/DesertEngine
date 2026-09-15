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
} // namespace Desert::Animation
