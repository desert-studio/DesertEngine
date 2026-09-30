#pragma once

/**
 * A SECTION: A RANGE OF ONE TRACK, WITH ITS OWN KEYS, A BLEND TYPE AND A WEIGHT.
 *
 * This is the step ClipSection.hpp announced ("the step that moves `Tracks` inside a section"): a
 * `ClipSection` REFERS to tracks by name and shares the clip's one flat key list, so two sections could
 * never be two statements composited. Here a section OWNS its content, so real layering is expressible,
 * and `SectionBlendType` is REUSED from ClipSection.hpp — the enum and its on-disk integers are one home.
 *
 * ── HOW SECTIONS OF ONE TRACK COMBINE (the only blend rule in the core) ──────────────────────────────
 *
 *     acc = the channel's Default (the rest value the host states)
 *     for each section covering t, in ascending (Row, then list order):
 *         w = WeightAt( section, t )
 *         Absolute:  acc = ( w == 1 ) ? v : mix( acc, v, w )      — bit-exact at full weight
 *         Additive:  acc = acc + w * v      (rotation: acc * slerp( identity, v, w ); scale: acc * mix(1, v, w))
 *
 * VALUE SPACE, as ClipSection.hpp's §972 note decided and for the same reason: a half-weighted section is
 * half the VALUE its keys hold, which is what an animator can read off the curve. The `w == 1` short
 * circuit is NOT an optimisation: `mix( a, b, 1.0F )` is `a + 1.0F * ( b - a )` and is not `b`, and the
 * migration of every clip (one Absolute section at full weight) must be the identity bit for bit.
 *
 * With one Absolute full-weight section per track this rule reduces to ClipSection's "the later section
 * wins", so a generation-3 file lifts without changing a sampled value.
 */

#include <Engine/Animation/ClipSection.hpp>
#include <Engine/Animation/KeyInterpolation.hpp>
#include <Engine/Animation/TimeModel.hpp>
#include <Engine/Animation/Timeline/Binding.hpp>
#include <Engine/Animation/Timeline/Channel.hpp>

#include <Common/Content/AssetEnvelope.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace Desert::Animation::Timeline
{
    using Animation::SectionBlendType;

    /**
     * @brief LevelSequence's Animation section: plays a `.anim` on the track's (skeletal entity) binding.
     *
     * The clip is referenced BY GUID — never by name or path, which is how the AnimGraph's `State::Clip`
     * refers to clips today and why a rename breaks it. The clip's own sequence is evaluated at
     * `StartOffset + ( t - section.Start ) * PlayRate`, wrapped when `Loop`, and the resulting pose enters
     * the entity's pose under this section's blend type and weight.
     */
    struct AnimationSectionContent
    {
        Common::Content::AssetGuid Clip;
        FrameNumber                StartOffset; ///< in the CLIP's ticks
        double                     PlayRate = 1.0;
        bool                       Loop     = false;
    };

    /// Camera Cut: from this section's start the viewport looks through `Camera` (an Entity binding).
    struct CameraCutSectionContent
    {
        BindingGuid Camera;
    };

    /// What a section holds. A track's sections all hold the same alternative (`Validate` refuses a mix).
    using SectionContent = std::variant<Channel, AnimationSectionContent, CameraCutSectionContent>;

    /**
     * @brief One section.
     *
     * INVARIANTS:
     *   * `Start <= End`; the range is INCLUSIVE on both ends (a tick is a point — ClipSection's reasoning);
     *   * keys MAY lie outside the range — they shape the curve entering it (UE keeps them too) — but the
     *     section contributes nothing outside [Start, End];
     *   * an empty `Weight` means 1, not 0 (every migrated section has it empty);
     *   * Camera Cut sections are always Absolute at full weight and never overlap on one row.
     */
    struct Section
    {
        FrameNumber            Start;
        FrameNumber            End;
        SectionBlendType       Blend = SectionBlendType::Absolute;
        std::vector<ScalarKey> Weight;
        /// Vertical row inside the track; lower rows evaluate first (see the fold above).
        int32_t Row = 0;
        /// The Sequencer's label; not a key.
        std::string    Name;
        SectionContent Content;

        [[nodiscard]] bool Covers( FrameNumber tick ) const
        {
            return !( tick < Start ) && !( End < tick );
        }
    };

    /// The weight at @p at: 1 for an empty channel, the keyed value otherwise (not clamped: > 1 is legal,
    /// UE allows it, and a clamp would be a second rule nobody authored).
    [[nodiscard]] float WeightAt( const Section& section, FrameTime at, FrameRate tickRate );
} // namespace Desert::Animation::Timeline
