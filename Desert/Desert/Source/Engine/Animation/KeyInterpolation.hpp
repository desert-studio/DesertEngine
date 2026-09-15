#pragma once

/**
 * WHAT HAPPENS BETWEEN TWO KEYS, AND WHO DECIDES IT.
 *
 * Report 05 §931 item 3 gives the key struct — "value + arrive/leave tangent + interp mode + tangent
 * mode", defaulting to Cubic/Auto — and item 4 gives the part that is easy to underestimate: auto-tangents
 * are NOT "compute a slope". They are the monotone clamp, the flat-extremum rule, the flat endpoints, and
 * a recompute of the WHOLE channel on every edit (§969 item 1: "underestimating this produces curves that
 * overshoot and animators who do not trust the tool").
 *
 * THE UNIT A TANGENT IS EXPRESSED IN — value units per SECOND — and this file is the only place that
 * needs to say so, because it is the only place that converts. Three candidates existed and two are
 * traps:
 *
 *   * per TICK would rescale every tangent in the repository the day the project tick rate changed, and
 *     A5 made that rate a number a file states rather than a constant everybody shares;
 *   * per NORMALISED SEGMENT would change a key's own tangent whenever a NEIGHBOUR moved, so dragging one
 *     key would silently reshape the curve two keys away;
 *   * per second is invariant under both. It is also not a third space: seconds are already the boundary
 *     unit the clock converts from, and the normalised parameter stays inside `EvaluateSegment` where it
 *     belongs.
 *
 * UNWEIGHTED SHIPS FIRST (§969 item 2). The weight fields exist and are zero; there is no weighted branch
 * in the evaluator, because a weighted tangent is "a fork in the evaluator, not a field" — a Cardano cubic
 * solve per sample, a per-side predicate, and an incompatibility with auto tangents. The place for them is
 * reserved in the format so adding them later is not a migration.
 */

#include <Engine/Animation/TimeModel.hpp>

#include <cstdint>
#include <vector>

namespace Desert::Animation
{
    /**
     * @brief How the segment ENDING at a key is shaped.
     *
     * ON THE KEY AND NOT ON THE TRACK, which is the whole of T4.2: before this, a clip had exactly one
     * rule — `glm::lerp` / `glm::slerp`, unconditionally — so an animator could not hold a pose and then
     * ease out of it without inserting keys to fake the curve.
     */
    enum class KeyInterp : uint8_t
    {
        Constant, ///< hold the previous key's value until this one; the stepped look, and pose-holding
        Linear,   ///< straight line between the two values — what every clip in this engine did
        Cubic,    ///< a Bézier shaped by the two keys' tangents
    };

    /**
     * @brief Where a key's tangents come from.
     *
     * `Auto` is recomputed from the neighbours on every edit; `User` is a slope the animator set and the
     * auto pass must not touch; `Break` is `User` with the two sides independent, which is how a corner is
     * authored. The distinction is not cosmetic: an auto pass that overwrote a user tangent would undo the
     * animator's work on the next unrelated edit anywhere in the channel.
     */
    enum class TangentMode : uint8_t
    {
        Auto,
        User,
        Break,
    };

    [[nodiscard]] const char* ToString( KeyInterp interp );
    [[nodiscard]] const char* ToString( TangentMode mode );

    /**
     * @brief One scalar channel key, as the tangent maths sees it.
     *
     * A vec3 key is three of these, one per component, because a tangent is a slope and a slope is a
     * scalar. That is also why UE stores a transform control as nine scalar channels (report 05 §931
     * item 7) rather than as three vector ones.
     */
    struct ScalarKey
    {
        FrameNumber Tick;
        float       Value = 0.0F;

        /// Value units per SECOND — see the file comment. Zero is flat, which is what an extremum gets.
        float ArriveTangent = 0.0F;
        float LeaveTangent  = 0.0F;

        /// RESERVED AND UNUSED. Weighted tangents are a second evaluator, not a field; these exist so that
        /// adding them later is a code change and not a file-format migration (§969 item 2).
        float ArriveWeight = 0.0F;
        float LeaveWeight  = 0.0F;

        KeyInterp   Interp = KeyInterp::Linear;
        TangentMode Mode   = TangentMode::Auto;
    };

    /**
     * @brief The value between two neighbouring keys. THE ONLY FUNCTION PLAYBACK CALLS.
     *
     * Takes the two keys' numbers rather than the channel, so sampling allocates nothing and a vec3 key
     * costs three calls with no temporary channel built per component. `t` is the normalised position
     * inside the segment, and `spanSeconds` is what turns the per-second tangents into that parameter's
     * units — the one conversion in this file.
     *
     * `interp` is the interpolation of the LATER key, because a segment is shaped by the key it arrives
     * at; that is the same convention `UIAnimKey::Easing` already uses ("Easing shapes the segment ENDING
     * at this key"), and matching it is deliberate — see the report in Docs/Animation/Shots/A6.
     */
    [[nodiscard]] float EvaluateSegment( float startValue, float startLeaveTangent, float endValue,
                                         float endArriveTangent, KeyInterp interp, double spanSeconds,
                                         float t );

    /**
     * @brief Recompute the tangents of every `Auto` key in the channel. Authoring-time, whole-channel.
     *
     * WHOLE-CHANNEL BECAUSE A TANGENT IS NOT A PROPERTY OF ITS KEY. It is computed from the neighbours, so
     * moving one key changes the curve on both sides of it; UE recomputes the channel at six different
     * call sites for exactly this reason (§969 item 1). Doing it per-edited-key is the version that
     * produces curves which look right until the second edit.
     *
     * The rules, in the order they apply:
     *   * the first and last key are FLAT. A curve that leaves its last key with a slope is a curve that
     *     overshoots past the end of the clip, where there is nothing to come back from;
     *   * a LOCAL EXTREMUM is flat. Without it a peak is an overshoot: the curve sails past the highest
     *     key the animator authored and comes back, which is the single most reported curve-editor bug;
     *   * otherwise the slope through the neighbours, CLAMPED so that neither adjacent segment can
     *     overshoot — the Fritsch-Carlson limit of three times the smaller adjacent secant.
     *
     * `User` and `Break` keys are left exactly as they are.
     */
    void AutoSetTangents( std::vector<ScalarKey>& keys, FrameRate tickRate );

    /**
     * @brief The slope the channel already has at `tick`, for seeding a key being inserted there (§936).
     *
     * SO THAT INSERTING A KEY NEVER CHANGES THE POSE. A key added with zero tangents flattens the curve
     * through the point it was added at, which moves every frame around it — the animator asked to record
     * where the curve IS, and the tool answered by reshaping it. Returns 0 for an empty channel and for a
     * position outside the keyed range, where there is no slope to read.
     */
    [[nodiscard]] float SlopeAt( const std::vector<ScalarKey>& keys, FrameNumber tick, FrameRate tickRate );
} // namespace Desert::Animation
