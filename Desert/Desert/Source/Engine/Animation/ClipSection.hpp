#pragma once

/**
 * A SECTION: A RANGE OF THE CLIP, THE TRACKS IT SPEAKS FOR, HOW IT REACHES THE POSE, AND HOW MUCH OF IT.
 *
 * Report 05 §938, and the reason it is here before anything needs it: *"put the blend-type enum in from
 * day one"*. A clip in this engine is the whole timeline — one value per track per tick, no statement
 * about what that value MEANS — and the cost of adding that statement later is not the enum, it is every
 * file already written without it. So the file says it now, and `kAnimationVersion` 3 is the step that
 * makes it say it.
 *
 * ── THE WARNING WORTH REPEATING (report 05 §972), AND WHAT WE DID ABOUT IT ────────────────────────────
 *
 * §972: section weight on an *Absolute* rig section blends POSES; on an *Additive* one it blends CONTROL
 * VALUES. Same slider, visibly different results, and UE ships both.
 *
 * WE BLEND IN VALUE SPACE IN BOTH CASES, and this is the sentence the documentation has to carry:
 *
 *     A section at 50 % is 50 % of the VALUE the tracks hold — half the offset on an Additive section,
 *     half the way from the rest pose to the authored pose on an Absolute one. It is NOT a 50 % blend of
 *     the two POSES the rig would evaluate to.
 *
 * The difference is visible the moment a control has a parent chain: a pose blend interpolates where the
 * bones END UP, so a half-weighted arm swings through the arc between two poses; a value blend
 * interpolates the NUMBERS, so a half-weighted arm is what the rig produces from half the authored angle.
 * With one bone they agree. With a chain they do not, and the second is the one an animator can predict
 * from the curve they are looking at — which is the property that makes a weight curve editable at all.
 *
 * ── WHY `Absolute` AND `Additive` ARE BOTH IMPLEMENTED, AND WHY THERE IS NO THIRD ────────────────────
 *
 * §938 says the enum may ship with one value. Two ship, because with only `Absolute` the WEIGHT CHANNEL
 * would be the only thing distinguishing a section from no section at all, and a one-valued enum beside a
 * live weight is a field that records a decision nobody can make. `Additive` is what makes the enum load
 * bearing: on it the tracks are OFFSETS from the rest pose, so an identity value means "no change" — the
 * exact opposite of what an identity value means on an `Absolute` section, where it means "go to the
 * origin". One stored number, two meanings, and the enum is the only thing that can tell them apart.
 *
 * `Override` is NOT here. In UE it means "this section replaces what is under it rather than blending
 * with it", and "what is under it" requires two sections to hold their own keys for one track. Ours hold
 * NAMES and share one flat `AnimationClip::Tracks` (see below), so nothing can be under anything: an
 * `Override` value would be a synonym for `Absolute` with a different spelling. It arrives with the step
 * that gives a section its own keys, and not before.
 *
 * ── WHAT A SECTION DOES NOT DO YET, STATED SO THE NEXT STEP IS NOT A SURPRISE ────────────────────────
 *
 * Sections REFER to tracks by name; they do not own keys. Two sections covering one track at one tick are
 * therefore two statements about one value, and the later one wins (see `AnimationClip::SampleTrack`).
 * Real layering — two sections each with their own curve for the same control, composited — is the format
 * step after this one, and it is the step that moves `Tracks` inside a section. That move is why the enum
 * had to exist first: a file written today states its blend type, so the layering step converts files
 * that SAY what they meant instead of guessing.
 */

#include <Engine/Animation/KeyInterpolation.hpp>
#include <Engine/Animation/Pose.hpp>
#include <Engine/Animation/TimeModel.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace Desert::Animation
{
    /**
     * @brief How a section's tracks reach the pose. STORED, NEVER INFERRED.
     *
     * The values are written to disk as integers, so their ORDER IS PART OF THE FORMAT: append only.
     */
    enum class SectionBlendType : uint8_t
    {
        /// The track's value IS the pose. Weight blends from the rest pose towards it.
        Absolute = 0,
        /// The track's value is an OFFSET applied on top of the rest pose. Weight scales the offset, so
        /// an identity value is a no-op at any weight.
        Additive = 1,
    };

    [[nodiscard]] inline const char* SectionBlendName( SectionBlendType blend ) noexcept
    {
        switch ( blend )
        {
            case SectionBlendType::Absolute:
                return "Absolute";
            case SectionBlendType::Additive:
                return "Additive";
        }
        return "Absolute";
    }

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
} // namespace Desert::Animation
