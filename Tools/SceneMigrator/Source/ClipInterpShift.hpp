#pragma once

// THE KEY-MODE STEP OF ANIM v5 -> v6 (ANIM-I8b-6): a key's `KeyInterp` moves from shaping the segment
// ARRIVING at it to shaping the segment LEAVING it — UE's rule (FRichCurve / FMovieSceneFloatChannel, FBX),
// which the engine's sampler (Timeline/Channel.cpp) now follows. Every value key takes the mode of the key
// after it; the last key keeps its own (it shapes no segment until a key is appended after it).
//
// `LiftClip` (generation 3) shifts its output with the same function, so both ways into v6 write one rule;
// `VerifyInterpShift` proves a shift is the identity on every tick, bit for bit, against a sampler frozen
// here at the v5 rule — the engine no longer carries one.

#include <Engine/Animation/Timeline/Sequence.hpp>

#include <Common/Content/TextAssetHeader.hpp>
#include <Common/Core/ResultStr.hpp>
#include <Common/Json/Json.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::Migration
{
    /// One channel component: key i takes key i+1's mode.
    void ShiftInterpToLeavingKey( std::vector<Animation::ScalarKey>& keys );

    /// Every value channel of every section (Float, Vector, Rotation, Transform, Bool) and every section's
    /// Weight. Event channels carry no modes. Returns the number of key lists shifted.
    std::size_t ShiftInterpToLeavingKey( Animation::Timeline::Sequence& sequence );

    /// @p arriving (the v5 rule) and @p leaving (the v6 rule, the engine's) sample equal, bit for bit, on
    /// every integer tick of every channel's keyed range; outside it both hold an end key. The two must have
    /// the same tracks, sections and key ticks. Returns the number of (component, tick) samples compared.
    [[nodiscard]] Common::ResultStr<std::size_t> VerifyInterpShift( const Animation::Timeline::Sequence& arriving,
                                                                    const Animation::Timeline::Sequence& leaving );

    // THE TMLN STEP v1 -> v2 (ANIM-FMT): the same shift for a timeline block in any host (.anim, a scene's
    // UIAnim, .dseq). v1's layout is v2's — only a key mode's meaning moved — so the block is read by the
    // engine's one reader with its number raised; the engine itself refuses v1 by name.

    /// A timeline block's header, and every other member carried untouched (LevelSequenceAsset.cpp's envelope).
    struct TimelineEnvelope
    {
        Common::Content::TextAssetHeaderSerialized Header;
        Common::Json::CarriedKeys                  Body;
    };

    /// The TMLN number the block's own header states.
    [[nodiscard]] Common::ResultStr<uint32_t> StatedTimelineVersion( std::string_view block );

    /// A TMLN v1 block, read as it is: its modes still shape the segment ARRIVING at a key. Refuses any other
    /// number.
    [[nodiscard]] Common::ResultStr<Animation::Timeline::Sequence> ReadTimelineV1( std::string_view block );

    struct TimelineShift
    {
        Animation::Timeline::Sequence Shifted; ///< under v2's rule, for the one writer (WriteSequence)
        std::size_t                   KeyLists      = 0;
        std::size_t                   SamplesProved = 0;
    };

    /// ReadTimelineV1, ShiftInterpToLeavingKey, then VerifyInterpShift: refused unless bit for bit.
    [[nodiscard]] Common::ResultStr<TimelineShift> ShiftTimelineV1( std::string_view block );

    /// A .dseq around the one writer's block, keeping @p source's Kind and GUID — LevelSequenceAsset::Write's
    /// stamping, which this tool cannot call (the asset class brings the ContentRegistry).
    [[nodiscard]] Common::ResultStr<std::string> WriteLevelSequence( const Animation::Timeline::Sequence& sequence,
                                                                     const TimelineEnvelope&              source );
} // namespace Desert::Migration
