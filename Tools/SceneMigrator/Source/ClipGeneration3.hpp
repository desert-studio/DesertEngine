#pragma once

// `.anim` GENERATION 3 — ANIM v4, per-bone channels — AS THE MIGRATOR ALONE READS IT (ANIM-I8a).
//
// The engine's AnimationClip holds ONE Timeline::Sequence since ANIM v5 (Engine/Animation/Timeline/Hosts.hpp).
// The types below are the generation-3 clip and its file layout, moved here VERBATIM from the engine
// (AnimationClip.hpp, ClipSection.hpp, Assets/Serialization/Animation.hpp, AnimationClipBuild.cpp) so that
// exactly one reader of generation 3 exists and it lives in the tool that lifts it: the engine has none
// (the project keeps no legacy reader). `LiftClip` (ClipLift.cpp) turns a clip into its sequence and
// `VerifyLift` (ClipMigration.cpp) proves the lift bit for bit before a file is rewritten.

#include <Engine/Animation/KeyInterpolation.hpp>
#include <Engine/Animation/Pose.hpp>
#include <Engine/Animation/TimeModel.hpp>
#include <Engine/Animation/Timeline/Section.hpp>
#include <Engine/Animation/Timeline/Sequence.hpp>
#include <Engine/Assets/Serialization/ImportSourceInfo.hpp>

#include <Common/Content/TextAssetHeader.hpp>
#include <Common/Core/ResultStr.hpp>
#include <Common/Core/Serialization/GlmReflection.hpp>
#include <Common/Json/Json.hpp>

#include <glm/glm.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/compatibility.hpp>
#include <glm/gtx/quaternion.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::Migration::ClipGen3
{
    using namespace Desert::Animation;

    /**
     * @brief One section of a clip: a range, the tracks it speaks for, a blend type and a weight channel.
     */
    struct ClipSection
    {
        /// What an animator calls it in the Sequencer. Not a key: sections are matched by RANGE and by
        /// track name, never by name, so two sections may share one and nothing downstream cares.
        std::string Name;

        /// Inclusive on both ends, in ticks on the clip's `TickRate`. Inclusive because a tick is a point
        /// and not an interval: a half-open range would make the last tick of a section belong to the next
        /// one, and "the section ends at frame 30" would show frame 30 from somewhere else.
        FrameNumber Start;
        FrameNumber End;

        SectionBlendType Blend = SectionBlendType::Absolute;

        /**
         * @brief The track names this section speaks for. EMPTY MEANS EVERY TRACK IN THE CLIP.
         *
         * Not a shorthand: it is what a clip-wide section IS, and spelling it as a list of every name
         * would make the section a second copy of the track list that goes stale the first time the keyer
         * adds a track. The migration from generation 2 writes exactly this — one section, no names — so
         * a converted file says "all of it, absolutely, at full weight" in four fields.
         */
        std::vector<std::string> Tracks;

        /**
         * @brief How much of this section reaches the pose, over time. EMPTY MEANS 1, NOT 0.
         *
         * A `ScalarKey` channel and not a float, because a weight that cannot be keyed is a fade nobody
         * can author — and because `LiftChannel`/`ApplyChannel` already speak this type, so the curve view
         * edits a section weight with no new authoring surface.
         *
         * Empty is "no fade authored", which is full weight. That is a different statement from a channel
         * holding a single key of 0, and the two must not be spelled the same way: an empty list is what
         * every migrated file has, and reading it as silence would mute the whole corpus.
         */
        std::vector<ScalarKey> Weight;

        [[nodiscard]] bool Covers( FrameNumber tick ) const
        {
            return !( tick < Start ) && !( End < tick );
        }

        [[nodiscard]] bool Speaks( const std::string& trackName ) const
        {
            if ( Tracks.empty() )
            {
                return true;
            }
            for ( const std::string& owned : Tracks )
            {
                if ( owned == trackName )
                {
                    return true;
                }
            }
            return false;
        }

        /// The weight at `at`. 1 for an unkeyed channel; see the field note for why that is not 0.
        [[nodiscard]] float WeightAt( FrameTime at, FrameRate tickRate ) const;
    };

    /**
     * @brief Apply a section to one authored value, against the rest pose the bone would otherwise hold.
     *
     * @param authored  what the track says at this tick.
     * @param reference the pose an unanimated bone holds — `Animator::SampleLocalTransform`'s `rig.Rest`,
     *                  which under a retarget is the SOURCE rig's retarget pose and not its bind pose.
     *                  Passing the wrong one here is invisible at weight 1 and wrong at every other.
     *
     * AT WEIGHT 1 THIS RETURNS `authored` BIT-FOR-BIT on an `Absolute` section, and that is a guarantee
     * rather than an optimisation: every clip in the corpus migrates to one full-weight Absolute section,
     * so a `mix( reference, authored, 1.0F )` — which is `a + 1.0F * ( b - a )` and NOT `b` for floats —
     * would move every key in the repository by an amount no test asserting "about equal" could see.
     */
    [[nodiscard]] BoneTransform ApplySection( const ClipSection& section, const BoneTransform& authored,
                                              const BoneTransform& reference, float weight );

    /**
     * ── AUTHORING A SECTION, AS OPPOSED TO SAMPLING ONE ──────────────────────────────────────────────
     *
     * These live beside the data and not in `SequencerPanel.cpp` for the reason
     * `scripts/CI/UnreachedSources.sh` keeps naming: that file is compiled by no suite, so a rule stated
     * inside it cannot be checked. `TrackEditing.hpp` is the same decision about a track, and this is the
     * same decision about a section; the panel is left holding only the two facts it alone knows — which
     * clip and which section the animator is pointing at.
     *
     * THEY TAKE THE LIST AND THE DURATION, NOT THE CLIP. `AnimationClip.hpp` includes this header, so a
     * signature naming the clip would be a cycle; and the list plus the length is genuinely everything a
     * range rule needs, which is why the cycle is worth avoiding rather than breaking.
     *
     * EVERY ONE OF THEM REFUSES IN WORDS. A section edit that silently did nothing — an out-of-range
     * index, a start past its end, a rename of a section a second window just deleted — is the empty
     * successful answer this tree has paid for repeatedly; the panel puts the sentence in front of
     * whoever pressed the button.
     */

    /// Where a new section goes and what it says. `duration` is the clip's length in ticks; the range is
    /// checked against it rather than clamped to it, because a button that quietly authored a DIFFERENT
    /// range from the one it was asked for is how a section comes to disagree with the ruler above it.
    [[nodiscard]] Common::BoolResultStr AddSection( std::vector<ClipSection>& sections, std::string name,
                                                    FrameNumber start, FrameNumber end, SectionBlendType blend,
                                                    FrameNumber duration );

    /**
     * @brief Move both ends of a section at once, KEEPING ITS LENGTH, or refuse.
     *
     * Separate from `SetSectionRange` because a drag of the body and a drag of an edge are two different
     * authoring gestures and only one of them is allowed to change the length. Refusing at the clip's
     * ends (rather than clamping and shortening) is what makes a section survive being pushed against
     * tick 0 — clamping only the start is how a dragged section silently loses frames.
     */
    [[nodiscard]] Common::BoolResultStr MoveSection( std::vector<ClipSection>& sections, size_t index,
                                                     int32_t deltaTicks, FrameNumber duration );

    /// Both ends, checked: `0 <= start <= end <= duration`. The inclusive end is the format's (see the
    /// field note), so `end == duration` is legal and `end == start` is a one-tick section, not an empty one.
    [[nodiscard]] Common::BoolResultStr SetSectionRange( std::vector<ClipSection>& sections, size_t index,
                                                         FrameNumber start, FrameNumber end,
                                                         FrameNumber duration );

    [[nodiscard]] Common::BoolResultStr RemoveSection( std::vector<ClipSection>& sections, size_t index );

    /**
     * @brief Move a section one place along the list. THE LIST ORDER IS THE PRIORITY ORDER.
     *
     * `AnimationClip::SectionFor` takes the LAST section that covers a tick and speaks for the track, so
     * position in this vector is the only thing that decides an overlap. Without a way to change it the
     * animator's only remedy for "the wrong one wins" is to delete and re-add, which loses the weight
     * curve they authored — so this is not a convenience, it is the handle on the one rule the list has.
     *
     * @param delta -1 raises (earlier in the list, LOWER priority), +1 lowers. Anything else is refused:
     *              a multi-step move is a drag, and a drag is a sequence of these.
     */
    [[nodiscard]] Common::BoolResultStr ReorderSection( std::vector<ClipSection>& sections, size_t index,
                                                        int delta );

    /**
     * @brief Add or remove one track name from what a section speaks for.
     *
     * THE TWO SPELLINGS OF "EVERY TRACK" ARE COLLAPSED TO ONE, HERE. An empty `Tracks` means every track
     * (see the field note), and a list naming every track of the clip means the same thing today and a
     * STALE thing tomorrow — the first track the keyer adds is outside it. Rather than let a file carry
     * both spellings, checking the last unchecked box clears the list. The alternative is a section that
     * looks clip-wide in the inspector and stops being clip-wide the moment a bone is keyed.
     *
     * @param allTracks every track name the clip currently has — the set "all of them" is measured
     *                  against. An `on` for a name not in it is refused: a section speaking for a track
     *                  the clip does not have is a typo that only shows up as silence.
     */
    [[nodiscard]] Common::BoolResultStr SetSectionSpeaksFor( ClipSection& section, const std::string& track,
                                                             bool on, const std::vector<std::string>& allTracks );

    /// Back to the clip-wide spelling: `Tracks` EMPTY. Not "tick every box", which is the stale copy above.
    void SetSectionSpeaksForEveryTrack( ClipSection& section );

    /**
     * @brief Upsert one key of the weight channel. An existing key on `tick` keeps its shape.
     *
     * CLAMPED TO [0, 1], and that is a decision rather than defensive arithmetic. `ApplySection` will
     * happily scale an additive offset by 1.5, but "how much of this section reaches the pose" is a
     * proportion — over-driving a layer is a gain on the layer, a different control with a different
     * name, and putting it on this channel would mean the same slider reads as a proportion in one place
     * and a multiplier in another. A non-finite value is refused rather than clamped: it is not a number
     * that was too big, it is a number that is not one.
     *
     * NEW KEYS ARE LINEAR. A weight fade is a ramp; a cubic weight overshoots past 1 and past 0 between
     * its keys, which on an Absolute section reads as the pose flying past the authored one.
     */
    [[nodiscard]] Common::BoolResultStr SetSectionWeightKey( ClipSection& section, FrameNumber tick, float value );

    [[nodiscard]] Common::BoolResultStr RemoveSectionWeightKey( ClipSection& section, size_t keyIndex );

    /// Drop the fade entirely: an EMPTY channel, which is full weight and not silence (see the field note).
    void ClearSectionWeight( ClipSection& section );

    // A KEY SITS ON A TICK, NOT AT A FLOAT NUMBER OF SECONDS. `float Time` used to live here, and the
    // ordering predicates below were the whole reason it hurt: `lower_bound` over floats decides which two
    // keys bracket the playhead, so "the playhead is exactly on this key" depended on two float paths
    // producing bit-identical values. On the tick grid the comparison is an integer one.
    struct PositionKeyFrame
    {
        FrameNumber Tick;
        glm::vec3   Position = glm::vec3( 0.0f );

        /// The shape of the segment ENDING at this key, and the slopes that shape it. The convention that
        /// the later key owns the rule is taken from `UIAnimKey::Easing` rather than invented beside it.
        KeyInterp   Interp        = KeyInterp::Linear;
        TangentMode Mode          = TangentMode::Auto;
        glm::vec3   ArriveTangent = glm::vec3( 0.0f ); // value units per SECOND, one per component
        glm::vec3   LeaveTangent  = glm::vec3( 0.0f );
        glm::vec3   ArriveWeight  = glm::vec3( 0.0f ); // RESERVED: unweighted ships first (§969)
        glm::vec3   LeaveWeight   = glm::vec3( 0.0f );

        bool operator<( const PositionKeyFrame& other ) const
        {
            return Tick < other.Tick;
        }
        bool operator<( FrameNumber tick ) const
        {
            return Tick < tick;
        }
    };

    struct RotationKeyFrame
    {
        FrameNumber Tick;
        glm::quat   Rotation = glm::quat( 1.0f, 0.0f, 0.0f, 0.0f );

        /// NO TANGENTS, and the reason is the maths: a cubic through quaternions leaves the unit sphere,
        /// and the curve that does not is `squad`, which is a different construction. `Constant` and
        /// `Linear` are both meaningful for a rotation, and holding a pose is what `Constant` is for.
        KeyInterp Interp = KeyInterp::Linear;

        bool operator<( const RotationKeyFrame& other ) const
        {
            return Tick < other.Tick;
        }
        bool operator<( FrameNumber tick ) const
        {
            return Tick < tick;
        }
    };

    struct ScaleKeyFrame
    {
        FrameNumber Tick;
        glm::vec3   Scale = glm::vec3( 1.0f );

        KeyInterp   Interp        = KeyInterp::Linear;
        TangentMode Mode          = TangentMode::Auto;
        glm::vec3   ArriveTangent = glm::vec3( 0.0f );
        glm::vec3   LeaveTangent  = glm::vec3( 0.0f );
        glm::vec3   ArriveWeight  = glm::vec3( 0.0f );
        glm::vec3   LeaveWeight   = glm::vec3( 0.0f );

        bool operator<( const ScaleKeyFrame& other ) const
        {
            return Tick < other.Tick;
        }
        bool operator<( FrameNumber tick ) const
        {
            return Tick < tick;
        }
    };

    // THE BONE NAME IS THE ONLY BINDING KEY. A `uint32_t BoneIndex` used to sit beside it, uninitialised, and
    // Animator::ResolveTrack has never once read it — it builds name -> track and binds by name, because a
    // clip and the character it drives come from different files with different bone orders. The index was a
    // second answer to a question only the name answers, and the Sequencer's "New Clip" left it unset all the
    // way into the .anim file.
    struct BoneTrack
    {
        std::string BoneName;

        std::vector<PositionKeyFrame> PositionKeys;
        std::vector<RotationKeyFrame> RotationKeys;
        std::vector<ScaleKeyFrame>    ScaleKeys;

        /**
         * @brief The track's value at `animationTime`, in the three quantities it is stored in.
         *
         * THE MATRIX IS GONE FROM THE MIDDLE. This used to be `GetTransform`, composing the interpolated
         * P/R/S into a mat4 — which `Animator::SampleLocalTransform` handed on and layer composition
         * immediately decomposed again, twice per bone per layer. A round trip with no consumer of the
         * matrix in it, on the hottest path the animation system has.
         */
        /// THE TICK RATE IS AN ARGUMENT, and it has to be: a tangent is value-per-second, so turning one
        /// into a position inside a segment needs to know how long that segment is in seconds. The track
        /// does not own the rate — the clip does — so it is passed rather than duplicated here.
        [[nodiscard]] BoneTransform Sample( FrameTime at, FrameRate tickRate ) const
        {
            BoneTransform out;
            out.Translation = GetInterpolatedPosition( at, tickRate );
            out.Rotation    = GetInterpolatedRotation( at );
            out.Scale       = GetInterpolatedScale( at, tickRate );
            return out;
        }

        /// True when the track carries anything at all. A track with three empty channels is a name with no
        /// animation behind it, and the pose it would produce is the bind pose — which the caller already
        /// has, and which is why every sampler checked this before reading.
        [[nodiscard]] bool HasKeys() const
        {
            return !PositionKeys.empty() || !RotationKeys.empty() || !ScaleKeys.empty();
        }

        [[nodiscard]] glm::vec3 GetInterpolatedPosition( FrameTime at, FrameRate tickRate ) const
        {
            if ( PositionKeys.empty() )
            {
                return glm::vec3( 0.0f );
            }
            if ( PositionKeys.size() == 1 )
            {
                return PositionKeys[0].Position;
            }

            // The bracketing pair is found by INTEGER comparison on the tick; only the fraction between
            // them is a float, and it is bounded by one key interval rather than by the clip's length.
            const auto it = std::lower_bound( PositionKeys.begin(), PositionKeys.end(), at.Frame );

            if ( it == PositionKeys.begin() )
            {
                return PositionKeys.front().Position;
            }
            if ( it == PositionKeys.end() )
            {
                return PositionKeys.back().Position;
            }

            const auto prev = it - 1;
            const auto next = it;

            const auto span = static_cast<double>( next->Tick.Value - prev->Tick.Value );
            if ( span <= 0.0 )
            {
                // Two keys on the same tick. On floats this was a divide by zero producing an infinity or
                // a NaN that flowed into the pose; on the tick grid it is a state the file can hold and
                // the answer is the later key, which is what a sampler at that tick means.
                return next->Position;
            }
            const auto factor = static_cast<float>( ( at.AsTicks() - prev->Tick.Value ) / span );

            // ONE CALL PER COMPONENT, and no temporary channel built to make them: a tangent is a slope
            // and a slope is a scalar, which is also why UE stores a transform control as nine scalar
            // channels rather than three vector ones.
            const double spanSeconds =
                 span * static_cast<double>( tickRate.Denominator ) / static_cast<double>( tickRate.Numerator );
            glm::vec3 out;
            for ( int c = 0; c < 3; ++c )
            {
                out[c] = EvaluateSegment( prev->Position[c], prev->LeaveTangent[c], next->Position[c],
                                          next->ArriveTangent[c], next->Interp, spanSeconds, factor );
            }
            return out;
        }

        [[nodiscard]] glm::quat GetInterpolatedRotation( FrameTime at ) const
        {
            if ( RotationKeys.empty() )
            {
                return glm::quat( 1.0f, 0.0f, 0.0f, 0.0f );
            }
            if ( RotationKeys.size() == 1 )
            {
                return RotationKeys[0].Rotation;
            }

            // The bracketing pair is found by INTEGER comparison on the tick; only the fraction between
            // them is a float, and it is bounded by one key interval rather than by the clip's length.
            const auto it = std::lower_bound( RotationKeys.begin(), RotationKeys.end(), at.Frame );

            if ( it == RotationKeys.begin() )
            {
                return RotationKeys.front().Rotation;
            }
            if ( it == RotationKeys.end() )
            {
                return RotationKeys.back().Rotation;
            }

            const auto prev = it - 1;
            const auto next = it;

            const auto span = static_cast<double>( next->Tick.Value - prev->Tick.Value );
            if ( span <= 0.0 )
            {
                // Two keys on the same tick. On floats this was a divide by zero producing an infinity or
                // a NaN that flowed into the pose; on the tick grid it is a state the file can hold and
                // the answer is the later key, which is what a sampler at that tick means.
                return next->Rotation;
            }
            const auto factor = static_cast<float>( ( at.AsTicks() - prev->Tick.Value ) / span );

            // CONSTANT OR SLERP, and there is no third branch to add later without adding `squad` with
            // it. A rotation key states its shape like every other key; `Cubic` is refused where a clip
            // is BUILT (AnimationClipBuild), so it cannot reach here and be quietly treated as linear.
            if ( next->Interp == KeyInterp::Constant )
            {
                return prev->Rotation;
            }
            return glm::slerp( prev->Rotation, next->Rotation, factor );
        }

        [[nodiscard]] glm::vec3 GetInterpolatedScale( FrameTime at, FrameRate tickRate ) const
        {
            if ( ScaleKeys.empty() )
            {
                return glm::vec3( 1.0f );
            }
            if ( ScaleKeys.size() == 1 )
            {
                return ScaleKeys[0].Scale;
            }

            // The bracketing pair is found by INTEGER comparison on the tick; only the fraction between
            // them is a float, and it is bounded by one key interval rather than by the clip's length.
            const auto it = std::lower_bound( ScaleKeys.begin(), ScaleKeys.end(), at.Frame );

            if ( it == ScaleKeys.begin() )
            {
                return ScaleKeys.front().Scale;
            }
            if ( it == ScaleKeys.end() )
            {
                return ScaleKeys.back().Scale;
            }

            const auto prev = it - 1;
            const auto next = it;

            const auto span = static_cast<double>( next->Tick.Value - prev->Tick.Value );
            if ( span <= 0.0 )
            {
                // Two keys on the same tick. On floats this was a divide by zero producing an infinity or
                // a NaN that flowed into the pose; on the tick grid it is a state the file can hold and
                // the answer is the later key, which is what a sampler at that tick means.
                return next->Scale;
            }
            const auto factor = static_cast<float>( ( at.AsTicks() - prev->Tick.Value ) / span );

            // ONE CALL PER COMPONENT, and no temporary channel built to make them: a tangent is a slope
            // and a slope is a scalar, which is also why UE stores a transform control as nine scalar
            // channels rather than three vector ones.
            const double spanSeconds =
                 span * static_cast<double>( tickRate.Denominator ) / static_cast<double>( tickRate.Numerator );
            glm::vec3 out;
            for ( int c = 0; c < 3; ++c )
            {
                out[c] = EvaluateSegment( prev->Scale[c], prev->LeaveTangent[c], next->Scale[c],
                                          next->ArriveTangent[c], next->Interp, spanSeconds, factor );
            }
            return out;
        }
    };

    // Animation notify / event: a named marker at a time (same unit as Duration / key times). Fires once when
    // playback crosses it; the Animator queues crossed notifies and the ECS dispatches them to scripts.
    struct AnimationNotify
    {
        std::string Name;
        FrameNumber Tick;
        // The notify track (row) it is drawn on in the Animation Editor, UE's Notify Tracks. Authoring-only:
        // playback fires by tick, whatever the row.
        int32_t Track = 0;
        // UE's UAnimNotifyState is the same marker with a LENGTH, so it is one field here and not a second
        // list: 0 is an instant notify (fires once), more is a state active on [Tick, Tick + DurationTicks)
        // that Begins when playback enters that span and Ends when it leaves it.
        FrameNumber DurationTicks;

        [[nodiscard]] bool IsState() const
        {
            return DurationTicks.Value > 0;
        }
    };

    /// Whether a notify STATE is active at @p at ticks. Half-open [Tick, Tick + Duration), so the Begin
    /// tick agrees with NotifyCrossed's (before, after] (a state starting where an instant notify sits
    /// begins on the frame that notify fires) and a state reaching the clip's end ends when a
    /// non-looping clip stops there.
    [[nodiscard]] inline bool NotifyStateActiveAt( const AnimationNotify& notify, const double at )
    {
        const auto begin = static_cast<double>( notify.Tick.Value );
        return notify.IsState() && at >= begin && at < begin + static_cast<double>( notify.DurationTicks.Value );
    }

    /// What one notify did during a step of playback: UE's Notify (instant) / NotifyBegin / NotifyEnd.
    enum class NotifyEventKind : uint8_t
    {
        Fire,
        Begin,
        End,
    };

    struct NotifyEvent
    {
        std::string     Name;
        NotifyEventKind Kind = NotifyEventKind::Fire;

        bool operator==( const NotifyEvent& ) const = default;
    };

    /**
     * @brief Advance a clip's notify states from @p before to @p after ticks and append the events.
     *
     * @p active is the list of states active at @p before, BY VALUE (name, tick, duration) and not by
     * index: an edit to the notify list between two steps then Ends the state that was edited away and
     * Begins the one that replaced it, instead of an index silently pointing at a different notify.
     *
     * Ends come first, then Begins and instant Fires in notify order. With @p forwardPlayback the
     * (before, after] interval is swept too, so an instant notify fires and a state shorter than the
     * frame still reports Begin + End; without it (a scrub, reverse playback) only the difference of the
     * two active sets is reported and instant notifies stay silent — a scrub is not playback.
     */
    void StepNotifyStates( const std::vector<AnimationNotify>& notifies, std::vector<AnimationNotify>& active,
                           double before, double after, bool forwardPlayback, bool looped,
                           std::vector<NotifyEvent>& out );

    /**
     * @brief A named float curve carried by a clip: UE's FFloatCurve. The keys are the scalar keys every
     *        other channel here uses, with their Constant / Linear / Cubic interpolation.
     */
    struct AnimationCurve
    {
        std::string            Name;
        std::vector<ScalarKey> Keys; // sorted by tick

        /// The value at @p at. Held flat before the first key and after the last, like UE's default
        /// extrapolation. A curve with no keys has no value: callers ask the clip, which refuses by name.
        [[nodiscard]] float Evaluate( FrameTime at, FrameRate tickRate ) const;
    };

    /**
     * @brief Whether playback that moved from tick @p before to tick @p after crossed a notify at @p at.
     *
     * The covered interval is (before, after]; on a loop wrap it is (before, duration) then [0, after]. One
     * frame is assumed not to skip a whole loop. ONE RULE, TWO CONSUMERS: the Animator fires by it and the
     * Animation Editor lights a notify by it, so the marker that flashes is the one a script heard.
     */
    [[nodiscard]] inline bool NotifyCrossed( const double at, const double before, const double after,
                                             const bool looped )
    {
        return looped ? ( at > before || at <= after ) : ( at > before && at <= after );
    }

    class AnimationClip
    {
    public:
        std::string AnimationName;

        /// Length of the clip, in ticks on `TickRate`'s grid.
        FrameNumber DurationTicks;

        /// The resolution the keys are counted at, and the grid an artist edits on. TWO numbers, because
        /// they answer two questions — see TimeModel.hpp. `TicksPerSecond`, a float that every shipped
        /// clip set to 1.0 so that "tick" meant "second", is what they replace.
        FrameRate TickRate    = PROJECT_TICK_RATE;
        FrameRate DisplayRate = DEFAULT_DISPLAY_RATE;
        // 0 = "no rig claimed", and it needed an initialiser: a default-constructed clip read back
        // whatever was on the heap, and this number is what the animation system matches a skeleton on —
        // so an unset one does not fail to match, it matches something arbitrary. Its neighbours all had
        // one; this field was the exception.
        uint64_t SkeletonSignature = 0;

        // Named tracks, in the order the source file listed them. THIS IS NOT INDEXED BY BONE: it used to be
        // scattered by a serialised bone index, which left unnamed holes wherever the source rig was sparse
        // and made the vector's length a property of the exporter. Playback resolves by name
        // (Animator::ResolveTrack), so position here means nothing and is not allowed to pretend otherwise.
        std::vector<BoneTrack> Tracks;

        /**
         * @brief Bumped whenever `Tracks` is REPLACED. The only honest key for a per-clip track cache.
         *
         * The address of the vector's storage is not one, and believing it was left a real hole in
         * `Animator::TrackBinding`: an unload frees a one-element track list and the reload allocates
         * another of the same size, so malloc hands back the identical block and BOTH `Tracks.data()` and
         * `Tracks.size()` come out unchanged across a complete replacement. The cache then kept a binding
         * built against the OLD list — bone names mapped to the wrong tracks, and a bone the new list
         * animates mapped to nothing at all. It is not a crash (the binding now stores indices, so it
         * cannot dangle) and that is exactly why it would have gone unnoticed: a character that plays the
         * wrong track after an eviction looks like bad animation data.
         *
         * NOT SERIALIZED: it describes this process's copy of the list, not the file.
         */
        uint32_t TrackRevision = 0;

        std::vector<AnimationNotify> Notifies; // sorted-by-tick markers fired during playback

        /// Anim curves (UE: the clip's float curves), read by name through Animator::GetCurveValue.
        std::vector<AnimationCurve> Curves;

        [[nodiscard]] const AnimationCurve* FindCurve( std::string_view name ) const
        {
            const auto it = std::find_if( Curves.begin(), Curves.end(),
                                          [name]( const AnimationCurve& curve ) { return curve.Name == name; } );
            return it != Curves.end() ? &*it : nullptr;
        }

        /**
         * @brief The clip's sections. EMPTY IS LEGAL AND MEANS "one Absolute section at full weight".
         *
         * Every `.anim` of generation 3 states at least one; empty is what an in-memory clip built by an
         * importer or a test has, and it is the IDENTITY of the section blend rather than a second answer
         * beside it — `SampleTrack` below returns the authored value in both cases, and the suite pins
         * that the two are bit-identical rather than trusting this sentence.
         */
        std::vector<ClipSection> Sections;

        /**
         * @brief The section speaking for @p track at @p at, or null when none does.
         *
         * THE LATER SECTION WINS. Two sections covering one track at one tick are two statements about a
         * single stored value, and there is no composition to do because they share this clip's flat
         * `Tracks` list — see ClipSection.hpp for why real layering is the format step after this one.
         */
        [[nodiscard]] const ClipSection* SectionFor( const std::string& trackName, FrameNumber at ) const
        {
            const ClipSection* found = nullptr;
            for ( const ClipSection& section : Sections )
            {
                if ( section.Covers( at ) && section.Speaks( trackName ) )
                {
                    found = &section;
                }
            }
            return found;
        }

        /**
         * @brief The track's value at @p at AS THE CLIP'S SECTIONS SAY IT REACHES THE POSE.
         *
         * THE ONE SEAM. `BoneTrack::Sample` answers "what does this curve say"; this answers "what does
         * the clip put on the bone", and those stopped being the same question the moment a section could
         * carry a blend type. Playback calls this one (`Animator::SampleLocalTransform`); the curve view
         * and the keyer call `Sample`, because they are editing the curve and not watching the character.
         *
         * @param reference what an unanimated bone would hold — see `ApplySection`.
         */
        [[nodiscard]] BoneTransform SampleTrack( const BoneTrack& track, FrameTime at,
                                                 const BoneTransform& reference ) const
        {
            const BoneTransform authored = track.Sample( at, TickRate );
            const ClipSection*  section  = SectionFor( track.BoneName, at.Frame );
            if ( section == nullptr )
            {
                // NO SECTION SPEAKS FOR IT, so nothing has said what its value means and the only honest
                // reading is the one the format had before sections existed. This is also the path every
                // sectionless in-memory clip takes, which is why it must return `authored` itself.
                return authored;
            }
            return ApplySection( *section, authored, reference, section->WeightAt( at, TickRate ) );
        }

        /// The clip's length in seconds, for the callers whose question really is about seconds — a
        /// crossfade duration, a UI readout, the normalized fraction the AnimGraph gates exit time on.
        /// Derived rather than stored: a second copy of the length is a second answer to it.
        [[nodiscard]] double DurationSeconds() const
        {
            return FrameTimeToSeconds( FrameTime{ DurationTicks, 0.0F }, TickRate );
        }

    };
    // EVERY SCALAR IN THIS FILE CARRIES AN INITIALISER. These structs are what reflect-cpp writes into a
    // `.anim`, so a field left indeterminate is not a runtime accident that the next assignment repairs — it
    // is bytes on disk that outlive the process. glm's vector/quaternion default constructors leave their
    // components indeterminate too, which is why the Value members are spelled out as well.
    // `Tick`, NOT `Time`, AND THE RENAME IS THE POINT. The field it replaces was a float in a unit the
    // format could not state: every shipped clip set `TicksPerSecond` to 1, so a "tick" was a second and
    // 210 of the corpus's 267 key times were fractional. A tick is now a whole number on the clip's own
    // `TickRate` grid, which makes "are these two keys at the same time" a `==` instead of a tolerance.
    //
    // The rename also does the work a version byte cannot do alone: a v0 file has `Time` and no `Tick`, so
    // reading it with DefaultIfMissing yields tick 0 for every key — which is why the loader REFUSES a v0
    // file outright instead of reading one.
    /**
     * @brief The shape of the segment a key is the LATER end of, plus the slopes that shape it.
     *
     * A6 / generation 2. `Interp` and `Mode` are stored as int for the same reason every enum in this
     * file is (stable, tolerant serialization); the two weight fields are RESERVED AND ZERO — weighted
     * tangents are a second evaluator, not a field, and the place for them exists so that adding them
     * later is a code change and not a migration (report 05 §969).
     *
     * EVERY KEY IN EVERY SHIPPED CLIP STATES THESE EXPLICITLY, and that is the condition the version step
     * was granted on rather than a nicety. A lenient read would happily invent them, and an
     * invented default is indistinguishable from an authored one for ever after — so the migration writes
     * them into the corpus and `Tests/Engine/AnimationClipCorpus` reads the files back to check that it
     * did. A version that changed only the number a file states about ITSELF, and nothing it says about
     * its contents, would be versioning for its own sake.
     */
    struct KeyShape
    {
        int   Interp       = 1; // KeyInterp::Linear — what every clip did before per-key interpolation
        int   Mode         = 0; // TangentMode::Auto
        float ArriveWeight = 0.0f;
        float LeaveWeight  = 0.0f;
    };

    struct KeyPosition
    {
        int32_t   Tick  = 0;
        glm::vec3 Value = glm::vec3( 0.0f );
        KeyShape  Shape;
        // Value units per SECOND, one per component — a tangent is a slope and a slope is a scalar. See
        // Engine/Animation/KeyInterpolation.hpp for why the unit is seconds and not ticks.
        glm::vec3 ArriveTangent = glm::vec3( 0.0f );
        glm::vec3 LeaveTangent  = glm::vec3( 0.0f );
    };

    /**
     * @brief A rotation key. IT CARRIES NO TANGENTS, and the reason is the maths rather than the schedule.
     *
     * A cubic through quaternions is not a rotation: the Bezier of four quaternions leaves the unit
     * sphere, and the curve that does not is `squad`, which builds its own control quaternions from the
     * neighbours and is a different construction with a different authoring surface. Storing tangent
     * fields here would be four numbers nothing could read — and the shape enum is still present, because
     * `Constant` and `Linear` are both meaningful for a rotation and holding a pose is exactly what
     * `Constant` is for.
     */
    struct KeyRotation
    {
        int32_t   Tick  = 0;
        glm::quat Value = glm::quat( 1.0f, 0.0f, 0.0f, 0.0f );
        KeyShape  Shape;
    };

    struct KeyScale
    {
        int32_t   Tick  = 0;
        glm::vec3 Value = glm::vec3( 1.0f );
        KeyShape  Shape;
        glm::vec3 ArriveTangent = glm::vec3( 0.0f );
        glm::vec3 LeaveTangent  = glm::vec3( 0.0f );
    };

    // NO BONE INDEX. It was here, uninitialised, and the Sequencer wrote whatever the stack held into every
    // clip it saved; the loader then sized the track array from it. The bone NAME is what playback binds on,
    // it is what an animation and a character actually share, and an empty name is a refusal the loader can
    // see — which is the whole property an index of 0 could never have.
    struct ChannelData
    {
        std::string BoneName;

        std::vector<KeyPosition> Positions;
        std::vector<KeyRotation> Rotations;
        std::vector<KeyScale>    Scales;
    };

    // Animation notify / event: a named marker at a time (same unit as Duration / key times) that fires once
    // when playback crosses it — dispatched to the entity's scripts as OnAnimationNotify(name). Footstep,
    // hit-frame, "spawn VFX", etc.
    struct NotifyData
    {
        std::string Name;
        int32_t     Tick  = 0;
        int32_t     Track = 0; // the Animation Editor row (UE Notify Track); playback ignores it
        // UE's Notify State: 0 is an instant notify, more is a span [Tick, Tick + DurationTicks) that
        // begins and ends. Required like every field here (the reader is strict); ANV3 met no notify in the
        // corpus, so no file had to be rewritten for it.
        int32_t DurationTicks = 0;
    };

    // An exact rational rate, mirroring Animation::FrameRate. A SEPARATE STRUCT and not that type because
    // this one is the FILE: reflect-cpp writes these two fields, and a runtime type is free to grow
    // members that have no business on disk.
    struct FrameRateData
    {
        int32_t Numerator   = 24000;
        int32_t Denominator = 1;
    };

    /**
     * @brief One key of a section's WEIGHT channel. A scalar, with the same shape vocabulary as every
     *        other key in this file.
     *
     * A separate struct from `KeyPosition` rather than a reuse with one component, because the file is
     * where the difference is readable: a weight is one number, and a vec3 holding it would leave two
     * fields on disk that no reader may look at.
     */
    struct SectionWeightKey
    {
        int32_t  Tick  = 0;
        float    Value = 1.0f;
        KeyShape Shape;
        float    ArriveTangent = 0.0f;
        float    LeaveTangent  = 0.0f;
    };

    /**
     * @brief A named float curve of the clip — UE's FFloatCurve (anim curve).
     *
     * Its keys are the SAME scalar key a section weight stores, because both are one number with the key
     * shape vocabulary of this file; a second key struct would be a second reading of the same bytes.
     */
    struct CurveData
    {
        std::string                   Name;
        std::vector<SectionWeightKey> Keys;
    };

    /**
     * @brief A section of the clip. GENERATION 3, and report 05 §938's "from day one".
     *
     * `Blend` is an int for the reason every enum in this file is: the values are a format, and an int is
     * what survives a value being appended. `Tracks` EMPTY MEANS EVERY TRACK — see
     * Engine/Animation/ClipSection.hpp, which carries the argument; the short version is that a clip-wide
     * section spelled as a list of every name is a second copy of the channel list that goes stale the
     * first time a track is added.
     */
    struct SectionData
    {
        std::string Name;
        int32_t     StartTick = 0;
        int32_t     EndTick   = 0;
        int32_t     Blend     = 0; // SectionBlendType::Absolute

        std::vector<std::string>      Tracks;
        std::vector<SectionWeightKey> Weight;
    };

    /**
     * @brief The `.anim` file, and the ONE definition of it.
     *
     * IT HAS ITS OWN VERSION SEQUENCE, and that was a correction to this task's brief rather than a design
     * flourish. The scene carries `kSceneVersion`, but a `.anim` is not a scene: it has never had a version
     * field, `Tools/SceneMigrator` collected three extensions and none of them was this one, and a scene
     * names a clip BY NAME — so nothing in a `.desce` changes when a clip's time model does. Taking a scene
     * step for it would have sent every `.desce` in the repository through a migration that had nothing to
     * do with them, and stamped each one with a conversion that never happened to it.
     */
    struct AnimationAssetData
    {
        /// The text asset header (T7e, ANIM 4), FIRST so the registry reads it without parsing the keys: Kind
        /// "Animation", the GUID that IS the clip's identity and its handle (AnimationAsset's constructor), and
        /// the format under `ANIM`. Generations 0-3 stated a top-level `Version` instead (absent meaning 0, never
        /// "current"); ReadAnimationJson refuses them by name and Tools/SceneMigrator raises them. Absent only on
        /// data never written - WriteAnimationJson mints it then.
        std::optional<Common::Content::TextAssetHeaderSerialized> Header;

        std::string Name;

        /// The resolution `Tick` values are counted at. Stored per file so a `.anim` is self-describing:
        /// a future change of the project rate is then a difference two numbers can state, not an
        /// assumption every reader shares.
        FrameRateData TickRate;

        /// The grid an artist sees and snaps to. Separate from TickRate on purpose — see TimeModel.hpp.
        FrameRateData DisplayRate{ 30, 1 };

        /// Length of the clip in ticks on `TickRate`'s grid.
        int32_t DurationTicks = 0;

        uint64_t                 SkeletonSignature = 0; // 0 = "no rig claimed", as on AnimationClip
        std::vector<ChannelData> Channels;
        // New field — clips cooked before notifies existed load with rfl::DefaultIfMissing (empty list).
        std::vector<NotifyData>  Notifies;

        /**
         * @brief The clip's sections. GENERATION 3 WRITES AT LEAST ONE, ALWAYS.
         *
         * `DefaultIfMissing` would give a generation-2 file an empty list, which the runtime reads as "one
         * implicit Absolute section at full weight" — the behaviour that file already had. That is
         * PRECISELY why the version step exists rather than being skipped: an implicit reading is
         * indistinguishable from an authored one for ever after, and §938's whole point is that a file
         * should STATE its blend type. So the migration writes the section and
         * `Tests/Engine/AnimationClipCorpus` reads the files back to check that it did.
         */
        std::vector<SectionData> Sections;

        // Anim curves (ANV3). Required (the reader is strict): a clip without curves STATES an empty list.
        // The corpus was rewritten with `"Curves": []` in the same change and ANIM stays 4, because no
        // value any file held changes meaning — the step adds a statement, it does not reinterpret one.
        std::vector<CurveData> Curves;

        // THE SOURCE THE CLIP WAS IMPORTED FROM (THM-FIXJ; UE: UAnimSequence::AssetImportData): Reimport of the
        // clip re-imports this file, beside the clip, with its record's options. Absent on a hand-authored clip;
        // a save of an imported clip keeps it (SaveClipToFile). No ANIM step: no value any file held changes
        // meaning - the one imported clip of the corpus was given the statement in the same change.
        std::optional<Assets::Serialization::ImportSourceInfo> Import;
    };

    /**
     * @brief Give @p data the section it BEHAVES AS, if it states none. EVERY PRODUCER OF A `.anim` CALLS
     *        THIS, and that is what makes "a generation-3 file states its blend type" true of files rather
     *        than of intentions.
     *
     * There are three producers — the importer, `SaveClipToFile` and the migrator — and each of them
     * built its `AnimationAssetData` its own way. A default written out three times is a default that
     * disagrees with itself on the third change; written once, a file that says nothing is impossible to
     * produce rather than merely unlikely.
     *
     * `Tests/Engine/AnimationClipCorpus` reads the shipped files back and checks they say it, for the same
     * reason it checks `KeyShape`: an implicit reading and an authored one are indistinguishable for ever
     * after, so the condition the version step was granted on has to be visible in the bytes.
     */
    inline void EnsureStatedSections( AnimationAssetData& data )
    {
        if ( !data.Sections.empty() )
        {
            return;
        }
        SectionData whole;
        whole.Name      = "Whole clip";
        whole.StartTick = 0;
        whole.EndTick   = data.DurationTicks;
        whole.Blend     = 0; // Absolute — the value every clip written before sections existed behaved as
        // Tracks EMPTY = every track, and Weight EMPTY = full weight. Both are the identity of the blend,
        // so this section is exactly what the file already did — which is the property a migration must
        // have: it states the behaviour a file had, it does not choose a new one.
        data.Sections.push_back( std::move( whole ) );
    }

    /// The generation-3 builder, verbatim from the engine's AnimationClipBuild.cpp (refusals included).
    [[nodiscard]] Common::ResultStr<AnimationClip> BuildClip( const AnimationAssetData& data );
} // namespace Desert::Migration::ClipGen3

namespace Desert::Animation::Timeline
{
    /**
     * @brief `.anim` generation 3 → the clip's sequence. LOSSLESS: refuses rather than drops.
     *
     * Refuses by name: two BoneTracks with one bone name; a ClipSection naming a track the clip lacks.
     * Acceptance: for every bone track and every tick in [0, Duration], `EvaluatePose` on the result equals
     * `AnimationClip::SampleTrack` on the source BIT FOR BIT; every curve equals `AnimationCurve::Evaluate`
     * (Migration::VerifyLift checks it on every file the migrator lifts).
     */
    [[nodiscard]] Common::ResultStr<Sequence> LiftClip( const Migration::ClipGen3::AnimationClip& generation3 );
} // namespace Desert::Animation::Timeline
