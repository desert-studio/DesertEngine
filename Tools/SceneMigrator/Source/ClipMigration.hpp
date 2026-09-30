#pragma once

// THE `.anim` STEP ANIM v4 -> v5 (ANIM-I8a): generation 3 (per-bone Channels) -> the TMLN body.
//
// Read by the migrator's OWN generation-3 reader (ClipGeneration3.hpp), lifted by `LiftClip`, PROVEN by
// `VerifyLift` (bit for bit on every tick), written by the engine's one writer (BuildAssetDataFromClip +
// WriteAnimationJson) keeping the file's GUID and import record. A clip that does not prove is refused and
// its file left as it was.

#include "ClipGeneration3.hpp"

#include <Common/Core/ResultStr.hpp>

#include <cstddef>
#include <string>

namespace Desert::Migration
{
    struct ClipMigrationOutcome
    {
        std::string Text; ///< the canonical ANIM v5 text
        std::size_t BoneTracks = 0;
        std::size_t Curves     = 0;
        std::size_t Notifies   = 0;
        std::size_t Sections   = 0; ///< generation-3 ClipSections, now cut into the tracks' Sections
        std::size_t TicksProved = 0;
    };

    /// On every tick of [0, Duration]: `EvaluatePose` == `SampleTrack` and each curve == `Evaluate`, bit for
    /// bit (the one stated divergence — a Constant key's own tick on a curve — asserted, not skipped).
    [[nodiscard]] Common::BoolResultStr VerifyLift( const ClipGen3::AnimationClip&    clip,
                                                    const Animation::Timeline::Sequence& lift );

    [[nodiscard]] Common::ResultStr<ClipMigrationOutcome> MigrateClipGeneration3( const std::string& text );
} // namespace Desert::Migration
