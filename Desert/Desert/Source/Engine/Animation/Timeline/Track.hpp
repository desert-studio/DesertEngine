#pragma once

/**
 * A TRACK: ONE PROPERTY OF ONE BOUND OBJECT, OVER TIME.
 *
 * `Binding` names the object (Binding.hpp); `Property` names what on it the track drives. The property is
 * text, resolved by the host exactly once per binding revision (Evaluator.hpp), because the three hosts
 * speak three vocabularies and the core must link none of them:
 *
 *     host             binding kind   property examples                    track kind
 *     AnimationClip    Bone           "" (the bone's local transform)      Transform
 *                      Sequence       curve name ("Footstep_L")            Float
 *                      Sequence       "" (notifies)                        Event
 *     UI animation     Widget         "Offset" | "Size" | "Opacity" | "Color"   Vector | Float
 *     LevelSequence    Entity         "Transform" | reflected property path     any value kind
 *                      Entity         "" (skeletal)                        Animation
 *                      Sequence       ""                                   CameraCut | Event
 *
 * INVARIANTS (`Validate`): `Binding` is a binding of the owning sequence; every section's content is the
 * track's `Kind`; (Binding, Property, Kind) is unique within a sequence — two tracks for one property
 * would be two answers with no rule between them, sections ARE the rule.
 */

#include <Engine/Animation/Timeline/Binding.hpp>
#include <Engine/Animation/Timeline/Channel.hpp>
#include <Engine/Animation/Timeline/Section.hpp>

#include <Common/Core/ResultStr.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Desert::Animation::Timeline
{
    /// A channel kind, or one of the two non-channel section contents. STORED AS AN INTEGER: append only;
    /// the first six are `ChannelKind` value for value, so a channel track's kind converts without a table.
    enum class TrackKind : uint8_t
    {
        Float     = 0,
        Vector    = 1,
        Rotation  = 2,
        Transform = 3,
        Bool      = 4,
        Event     = 5,
        Animation = 6,
        CameraCut = 7,
    };

    [[nodiscard]] const char* ToString( TrackKind kind );
    [[nodiscard]] TrackKind   TrackKindOf( const SectionContent& content );

    struct Track
    {
        BindingGuid          Binding;
        std::string          Property;
        TrackKind            Kind = TrackKind::Float;
        std::vector<Section> Sections;
        /// Authoring switch (UE: track mute). A muted track evaluates to nothing — it is SKIPPED, not
        /// evaluated to its default, so muting a bone track shows the pose underneath.
        bool Muted = false;
    };

    /// A new section of the track's kind spanning [start, end], its content at rest defaults.
    [[nodiscard]] Section& AddSection( Track& track, FrameNumber start, FrameNumber end );

    // ── Section edits (UE: UMovieSceneSection::MoveSection / SetRange / SetRowIndex, the weight channel) ──
    //
    // ONE set for every host — the clip Sequencer, UI animation and LevelSequence edit sections through
    // these, never by writing `Start`/`Row`/`Weight` themselves, so the invariants `Validate` states hold
    // after every edit rather than being discovered at save. Every refusal leaves the track untouched.
    //
    // NO CLAMP TO THE PLAYBACK RANGE: a section may lie partly or wholly outside [Sequence.Start,
    // Sequence.End] (UE does the same, and `Validate` does not refuse it) — the range is where a player
    // loops, not where content may live. What IS refused: an end before its start, a tick that does not
    // fit the frame type, a negative row, and two Camera Cuts overlapping on one row.

    /// Shifts the section AND EVERY KEY IT OWNS (content keys, event ticks, weight keys) by @p deltaTicks,
    /// so its length and its curve's shape relative to it are kept (UE: MoveSection moves the channels).
    /// An Animation section's StartOffset is in the CLIP's ticks and relative to the section: unchanged.
    [[nodiscard]] Common::BoolResultStr MoveSection( Track& track, size_t index, int32_t deltaTicks );

    /// Sets [start, end] (inclusive: start == end is one tick). Keys stay where they are — UE's SetRange;
    /// a key outside the new range still shapes the curve entering it (Section.hpp).
    [[nodiscard]] Common::BoolResultStr SetSectionRange( Track& track, size_t index, FrameNumber start,
                                                         FrameNumber end );

    /// Moves the section to @p row (UE: SetRowIndex). Rows fold in ascending order, so a HIGHER row is what
    /// wins an overlap — this is the author's handle on which section wins (Section.hpp's fold).
    [[nodiscard]] Common::BoolResultStr SetSectionRow( Track& track, size_t index, int32_t row );

    [[nodiscard]] Common::BoolResultStr RemoveSection( Track& track, size_t index );

    /**
     * @brief Keys the section's weight at @p tick: an existing key on that tick keeps its shape (Interp,
     * tangents) and takes the value; a new key is Linear (a fade is a ramp).
     *
     * NOT CLAMPED to [0, 1]: `WeightAt` does not clamp (Section.hpp) and UE's section weight channel is an
     * unclamped float, so a clamp here would be a second rule — and would make a file-authored 1.5 differ
     * from an editor-authored one. Refused: a value that is not finite, and any key on a Camera Cut (a cut
     * is full weight by definition, `Validate`).
     */
    [[nodiscard]] Common::BoolResultStr SetSectionWeightKey( Track& track, size_t index, FrameNumber tick,
                                                             float value );

    /// Removes weight key @p keyIndex. The last key gone leaves an EMPTY channel, which is full weight.
    [[nodiscard]] Common::BoolResultStr RemoveSectionWeightKey( Track& track, size_t index, size_t keyIndex );

    /// Every weight key gone: the section is back at full weight, not at silence.
    [[nodiscard]] Common::BoolResultStr ClearSectionWeight( Track& track, size_t index );
} // namespace Desert::Animation::Timeline
