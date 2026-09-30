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
} // namespace Desert::Animation::Timeline
