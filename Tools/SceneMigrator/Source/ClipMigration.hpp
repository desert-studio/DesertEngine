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
#include <Engine/Animation/SkeletonReference.hpp>

#include <optional>
#include <span>
#include <string_view>
#include <string>

namespace Desert::Migration
{
    struct ClipMigrationOutcome
    {
        std::string Text; ///< the canonical text of the current ANIM generation (TMLN body + Skeleton GUID)
        std::size_t BoneTracks = 0;
        std::size_t Curves     = 0;
        std::size_t Notifies   = 0;
        std::size_t Sections   = 0; ///< generation-3 ClipSections, now cut into the tracks' Sections
        std::size_t TicksProved = 0;
    };

    /// On every tick of [0, Duration]: `EvaluatePose` == `SampleTrack` and each curve == `Evaluate`, bit for
    /// bit (the one stated divergence — a Constant rotation key's own tick — asserted, not skipped).
    [[nodiscard]] Common::BoolResultStr VerifyLift( const ClipGen3::AnimationClip&    clip,
                                                    const Animation::Timeline::Sequence& lift );

    /// ANIM 4/5 -> 6: generation 3 (per-bone Channels) lifted to the TMLN body, proved by VerifyLift. The skeleton
    /// is the one ANIM 5 names by GUID; ANIM 4's bone hash `SkeletonSignature` is resolved against @p skeletons
    /// (Animation::MigrateSkeletonReference: exactly one .skeleton with that signature, else its refusal).
    [[nodiscard]] Common::ResultStr<ClipMigrationOutcome>
    MigrateClipGeneration3( std::string_view path, const std::string& text,
                            std::span<const Animation::SkeletonCandidate> skeletons );

} // namespace Desert::Migration
