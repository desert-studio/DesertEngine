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
 * `UIAnimTrack`/`UIAnimKey` are deleted (`UIEasing` stays: UITween and UIScreenStack still author it, and
 * `PresetOf` is its one table into `EasingPreset`). The Sequencer's UI mode and its skeletal mode become
 * one editor over one type (the two key models were PRESCAN's duplicate (2)). Scene v41 lifts the v40
 * block (`UIAnimationV40`, the lift's input and nothing else's) in the migrator, once.
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

#include <glm/glm.hpp>

#include <cstdint>
#include <string_view>
#include <vector>

namespace Desert::ECS
{
    enum class UIEasing;
} // namespace Desert::ECS

namespace Desert::Animation::Timeline
{
    inline constexpr std::string_view kLevelSequenceExtension = ".dseq";
    inline constexpr std::string_view kLevelSequenceKind      = "LevelSequence";

    // The two lifts — `LiftClip` (generation 3 → sequence, ClipGeneration3.hpp) and `LiftUIAnimation` (scene
    // v40 UIAnim → sequence, UILift.hpp) — live in Tools/SceneMigrator: the engine reads neither old form.

    /// `UIEasing`'s one table into `EasingPreset`, value for value (UITween, UIScreenStack and the UI lift).
    [[nodiscard]] EasingPreset PresetOf( ECS::UIEasing easing );
} // namespace Desert::Animation::Timeline
