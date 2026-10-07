#pragma once

/**
 * CHANNELS: TYPED KEYS WITH ONE KEY MODEL.
 *
 * Every VALUE channel stores `ScalarKey` (KeyInterpolation.hpp) — tick, value, interp, tangent mode,
 * arrive/leave tangent — and is evaluated by `EvaluateSegment`, the one function playback calls. There are
 * three key models in the tree today and this file is where two of them stop existing:
 *
 *   * `Position/ScaleKeyFrame` (a vec3 key with vec3 tangents) become three `FloatChannel`s — UE's
 *     transform is nine scalar channels for the same reason ScalarKey's comment gives: a tangent is a slope
 *     and a slope is a scalar;
 *   * `UIAnimKey` (time in float seconds, a vec4, a `UIEasing`) becomes ScalarKeys whose interp and
 *     tangents are set by an EASING PRESET (`ApplyEasingPreset` below). An easing is a way of AUTHORING a
 *     segment, not a second kind of segment; keeping it as a key field meant two evaluators and two curve
 *     editors for one idea.
 *
 * ── THE ONE EXCEPTION, STATED ─────────────────────────────────────────────────────────────────────────
 *
 * `EventChannel` keys are markers, not values: there is nothing between two events to interpolate, so a
 * ScalarKey there would carry a Value, tangents and an Interp that mean nothing (UE draws the same line:
 * FMovieSceneEventChannel has its own key type). An `EventKey` is `AnimationNotify` moved into the core.
 *
 * ── ROTATION ──────────────────────────────────────────────────────────────────────────────────────────
 *
 * Four ScalarKey channels, the quaternion's X/Y/Z/W, KEYED TOGETHER (same ticks, same interp on each
 * component) and evaluated as a whole: the two bracketing keys are rebuilt as quaternions and slerped,
 * which is exactly `RotationKeyFrame` today — so the migration of every clip in the corpus is bit-exact
 * rather than "close". `Cubic` on a rotation is refused by `Validate` until squad lands; an Euler view for
 * the curve editor is PRESENTATION, derived on display, never stored (Euler storage would unwind every
 * mocap clip through gimbal flips at import).
 *
 * ── EXTRAPOLATION ─────────────────────────────────────────────────────────────────────────────────────
 *
 * Held flat before the first key and after the last (UE's default, and `AnimationCurve::Evaluate`'s rule).
 * A channel with NO keys evaluates to its `Default` — the rest value the host states — never to 0.
 */

#include <Engine/Animation/KeyInterpolation.hpp>
#include <Engine/Animation/Pose.hpp>
#include <Engine/Animation/TimeModel.hpp>

#include <Common/Core/ResultStr.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace Desert::Animation::Timeline
{
    /// STORED AS AN INTEGER: append only.
    enum class ChannelKind : uint8_t
    {
        Float     = 0,
        Vector    = 1,
        Rotation  = 2,
        Transform = 3,
        Bool      = 4,
        Event     = 5,
    };

    [[nodiscard]] const char* ToString( ChannelKind kind );

    /// INVARIANT: `Keys` sorted by tick, no two keys on one tick.
    struct FloatChannel
    {
        std::vector<ScalarKey> Keys;
        float                  Default = 0.0F;
    };

    /// Three independent components (UE: they may diverge after an edit). Default comes from each component.
    struct VectorChannel
    {
        FloatChannel X;
        FloatChannel Y;
        FloatChannel Z;
    };

    /// INVARIANT: the four components have identical tick lists and identical Interp per key; Interp is
    /// Constant or Linear. Default is identity (W.Default = 1).
    struct RotationChannel
    {
        FloatChannel X;
        FloatChannel Y;
        FloatChannel Z;
        FloatChannel W{ {}, 1.0F };
    };

    /// A bone or an entity transform. Scale defaults to 1 on every component.
    struct TransformChannel
    {
        VectorChannel   Translation;
        RotationChannel Rotation;
        VectorChannel   Scale{ { {}, 1.0F }, { {}, 1.0F }, { {}, 1.0F } };
    };

    /// INVARIANT: every key Interp == Constant and Value is exactly 0 or 1.
    struct BoolChannel
    {
        FloatChannel Bits;
    };

    /// A marker. Duration > 0 is a STATE active on [Tick, Tick + Duration) — UE's AnimNotifyState, one
    /// field and not a second list, as AnimationNotify already decided.
    struct EventKey
    {
        FrameNumber Tick;
        FrameNumber Duration;
        std::string Name;
        /// The authoring row (UE notify track). Playback fires by tick, whatever the row.
        int32_t Row = 0;
    };

    /// INVARIANT: sorted by tick; several events on one tick are legal and fire in list order.
    struct EventChannel
    {
        std::vector<EventKey> Keys;
    };

    using Channel =
         std::variant<FloatChannel, VectorChannel, RotationChannel, TransformChannel, BoolChannel, EventChannel>;

    [[nodiscard]] ChannelKind KindOf( const Channel& channel );

    /// A channel of @p kind with its rest defaults — what a new section of a track of that kind starts as.
    [[nodiscard]] Channel MakeChannel( ChannelKind kind );

    // ── Sampling. `tickRate` is the owning sequence's: it turns per-second tangents into segment units. ──

    [[nodiscard]] float         Evaluate( const FloatChannel& channel, FrameTime at, FrameRate tickRate );
    [[nodiscard]] glm::vec3     Evaluate( const VectorChannel& channel, FrameTime at, FrameRate tickRate );
    [[nodiscard]] glm::quat     Evaluate( const RotationChannel& channel, FrameTime at, FrameRate tickRate );
    [[nodiscard]] BoneTransform Evaluate( const TransformChannel& channel, FrameTime at, FrameRate tickRate );
    [[nodiscard]] bool          Evaluate( const BoolChannel& channel, FrameTime at, FrameRate tickRate );

    /**
     * @brief The events playback crossed moving from @p from to @p to, appended to @p out in firing order.
     *
     * The interval is (from, to]; on a loop wrap (`wrapped`) it is (from, end] then [start, to] — the rule
     * `NotifyCrossed` states today, moved here so the Animator and the Sequencer keep sharing ONE rule.
     * A state event reports Begin when the interval enters its span and End when it leaves it.
     */
    enum class EventEdge : uint8_t
    {
        Instant,
        Begin,
        End,
    };
    struct CrossedEvent
    {
        const EventKey* Key  = nullptr;
        EventEdge       Edge = EventEdge::Instant;
    };
    void CollectCrossed( const EventChannel& channel, FrameTime from, FrameTime to, bool wrapped,
                         FrameNumber rangeStart, FrameNumber rangeEnd, std::vector<CrossedEvent>& out );

    // ── Authoring ─────────────────────────────────────────────────────────────────────────────────────

    /**
     * @brief How a segment eases — an AUTHORING PRESET that sets interp and tangents on real keys.
     *
     * The values mirror `ECS::UIEasing` one for one so the UI migration is a table, and `UIEasing` is
     * deleted by that migration: this is the one home of the idea. Stored nowhere — a key does not
     * remember which preset made it, exactly as a UE key does not remember "ease out".
     */
    enum class EasingPreset : uint8_t
    {
        Linear,
        QuadIn,
        QuadOut,
        QuadInOut,
        CubicIn,
        CubicOut,
        CubicInOut,
        BackOut,
        ElasticOut,
        BounceOut,
    };

    /**
     * @brief What applying a preset did to the channel.
     *
     * EXACT FOR EVERY PRESET THAT IS A CUBIC (Linear, Quad/Cubic In/Out, BackOut): a Hermite segment with
     * the right tangents IS the easing polynomial. The InOut presets are two cubics and insert ONE key at
     * the segment's middle tick. Elastic and Bounce are not cubics at all: they are baked into keys on the
     * display grid, and `MaxDeviation` reports how far the baked curve sits from the formula (normalised
     * 0..1 segment units) — an approximation is reported, never silent.
     */
    struct EasingResult
    {
        uint32_t InsertedKeys = 0;
        float    MaxDeviation = 0.0F;
    };

    /**
     * @brief Shape the segment that ENDS at `keys[endKey]` (the later key owns the segment, as everywhere).
     *
     * Refuses (error names the index) when `endKey` is 0 or out of range. The keys touched become
     * `TangentMode::User`, so a later `AutoSetTangents` cannot undo the preset.
     */
    [[nodiscard]] Common::ResultStr<EasingResult> ApplyEasingPreset( std::vector<ScalarKey>& keys, size_t endKey,
                                                                     EasingPreset preset, FrameRate tickRate,
                                                                     FrameRate displayRate );
} // namespace Desert::Animation::Timeline
