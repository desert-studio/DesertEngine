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

#include <Common/Core/ResultStr.hpp>

#include <cstddef>
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
} // namespace Desert::Migration
