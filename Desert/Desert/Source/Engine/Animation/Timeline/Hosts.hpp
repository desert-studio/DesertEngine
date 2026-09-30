#pragma once

/**
 * THE THREE HOSTS, AND HOW EACH EXISTING FORMAT BECOMES ONE.
 *
 * A host is an asset that OWNS a `Sequence` and restricts it (Sequence.hpp, `SequenceHost`). The two
 * existing formats migrate once, in the files, by the scene/asset migrator — no dual reader survives
 * (the project keeps no legacy paths). The functions below ARE the migration; their contracts are the
 * acceptance tests.
 *
 * ── AnimationClip (.anim) ─────────────────────────────────────────────────────────────────────────────
 *
 * END STATE: `AnimationClip` keeps its identity fields (name, SkeletonSignature) and holds ONE
 * `Timeline::Sequence Sequence` (Host = AnimationClip). `Tracks`, `Curves`, `Notifies` and `Sections`
 * are deleted; `DurationTicks`/`TickRate`/`DisplayRate` are the sequence's. `.anim` generation 4 stores
 * the `TMLN` block (as ANIM v5); generation 3 (ANIM v4) is lifted by `LiftClip` in Tools/SceneMigrator
 * and never read again.
 *
 *   BoneTrack "Hand_L"          → Binding{Bone, "Hand_L"} + Track{Transform, ""}
 *   AnimationCurve "Footstep"   → Track{Sequence binding, "Footstep", Float}
 *   Notifies                    → ONE Track{Sequence binding, "", Event}; notify.Track → EventKey.Row
 *   ClipSection s               → for each track s speaks: a Section with s's range/blend/weight and a
 *                                 COPY of the track's keys (they shared one list; now each owns its own)
 *   no ClipSection              → one Absolute full-range section per track, empty weight
 *
 * ── UI animation (UIAnimData on a widget) ─────────────────────────────────────────────────────────────
 *
 * END STATE: `UIAnimData` holds a `Timeline::Sequence` (Host = UIAnimation) + a `LoopMode` + autoplay;
 * `UIAnimTrack`/`UIAnimKey`/`UIEasing` are deleted. The Sequencer's UI mode and its skeletal mode become
 * one editor over one type (the two key models were PRESCAN's duplicate (2)).
 *
 *   UIAnimTrack{Offset|Size}    → Track{Widget binding, "Offset"|"Size", Vector} (Z unkeyed)
 *   UIAnimTrack{Opacity}        → Track{..., "Opacity", Float}
 *   UIAnimTrack{Color}          → Track{..., "Color", Vector}
 *   UIAnimKey.Time (float s)    → tick on `tickRate`, ROUNDED — reported in `UILiftReport` (TimeModel's
 *                                 rule: rounding is fine, unreported rounding is not)
 *   UIAnimKey.Easing            → `ApplyEasingPreset` on the segment ending at that key
 *
 * ── LevelSequence (.dseq) — new ──────────────────────────────────────────────────────────────────────
 *
 * A text-header asset (Kind "LevelSequence") whose body is the `TMLN` block. Placed in a scene by a
 * `LevelSequenceComponent { Assets::AssetHandle Sequence; LoopMode; bool AutoPlay; binding overrides }`; its
 * Entity bindings' locators are entity UUIDs of THAT scene.
 */

#include <Engine/Animation/TimeModel.hpp>
#include <Engine/Animation/Timeline/Channel.hpp>
#include <Engine/Animation/Timeline/Sequence.hpp>

#include <Common/Core/ResultStr.hpp>

#include <cstdint>
#include <string_view>

namespace Desert::ECS
{
    struct UIAnimData;
    enum class UIEasing;
} // namespace Desert::ECS

namespace Desert::Animation::Timeline
{
    inline constexpr std::string_view kLevelSequenceExtension = ".dseq";
    inline constexpr std::string_view kLevelSequenceKind      = "LevelSequence";

    // `LiftClip` (generation 3 → sequence) lives in Tools/SceneMigrator (ClipGeneration3.hpp): the engine
    // reads no generation 3, so the lift is the migrator's alone (ANIM-I8a).

    struct UILiftReport
    {
        uint32_t RoundedKeys        = 0; ///< keys whose float time was not on the tick grid
        float    MaxRoundingSeconds = 0.0F;
        float    MaxEasingDeviation = 0.0F; ///< from Elastic/Bounce bakes (Channel.hpp, EasingResult)
    };

    struct UILiftResult
    {
        Sequence     Lifted;
        UILiftReport Report;
    };

    /**
     * @brief A widget's UIAnimData → its sequence. @p widgetLocator is the owning element's entity UUID.
     * Refuses a track with unsorted keys and an unknown property by name.
     */
    [[nodiscard]] Common::ResultStr<UILiftResult> LiftUIAnimation( const ECS::UIAnimData& legacy,
                                                                   std::string_view       widgetLocator,
                                                                   FrameRate tickRate, FrameRate displayRate );

    /// The migration's one table: `UIEasing` → `EasingPreset`, value for value.
    [[nodiscard]] EasingPreset PresetOf( ECS::UIEasing easing );
} // namespace Desert::Animation::Timeline
