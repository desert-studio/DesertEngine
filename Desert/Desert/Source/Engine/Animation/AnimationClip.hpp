#pragma once

#include <Engine/Animation/TimeModel.hpp>
#include <Engine/Animation/Timeline/Sequence.hpp>

#include <Common/Content/AssetEnvelope.hpp>

#include <string>

namespace Desert::Animation
{
    /**
     * @brief A skeletal animation clip: its identity and ONE `Timeline::Sequence` (Host = AnimationClip).
     *
     * UE's shape: a UAnimSequence keeps its data in one place (IAnimationDataModel) and everything else
     * reads it from there. Here that place is `Sequence` — Bone bindings with Transform tracks, a Sequence
     * binding with the named Float curves and one Event track (the notifies), each track cut into Sections
     * (Timeline/Hosts.hpp). The clip restates none of it: its length, tick rate and display rate are the
     * sequence's (`Sequence.End`, `TickRate`, `DisplayRate`); the per-bone `Tracks`, `Curves`, `Notifies`
     * and `Sections` of generation 3 are gone, lifted once in the files by Tools/SceneMigrator.
     */
    class AnimationClip
    {
    public:
        std::string AnimationName;

        // THE CLIP'S SKELETON, BY GUID (SKEL-TREE; Engine/Animation/SkeletonReference.hpp) — the one home of the
        // value: AnimationAsset::GetSkeleton reads it, SaveClipToFile writes it. Null = the clip names no
        // skeleton and plays on no mesh (ClipPlaysOnMesh refuses it by name).
        Common::Content::AssetGuid Skeleton;

        Timeline::Sequence Sequence = MakeClipSequence();

        /// Length of the clip in ticks on `Sequence.TickRate` — the sequence's playback range, [Start, End].
        [[nodiscard]] FrameNumber DurationTicks() const
        {
            return FrameNumber{ Sequence.End.Value - Sequence.Start.Value };
        }

        /// The clip's length in seconds, for the callers whose question really is about seconds — a
        /// crossfade duration, a UI readout, the normalized fraction the AnimGraph gates exit time on.
        /// Derived rather than stored: a second copy of the length is a second answer to it.
        [[nodiscard]] double DurationSeconds() const
        {
            return FrameTimeToSeconds( FrameTime{ DurationTicks(), 0.0F }, Sequence.TickRate );
        }

        /// An empty sequence that already states it belongs to a clip (Validate refuses a clip host's
        /// sequence carrying anything a `.anim` may not).
        [[nodiscard]] static Timeline::Sequence MakeClipSequence()
        {
            Timeline::Sequence sequence;
            sequence.Host = Timeline::SequenceHost::AnimationClip;
            return sequence;
        }
    };
} // namespace Desert::Animation
