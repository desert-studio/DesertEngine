#pragma once

/**
 * THE EVALUATOR: A TIME STEP → VALUES → THE HOST APPLIES THEM.
 *
 * Two halves, deliberately separate (UE: evaluation template → IMovieScenePlayer):
 *
 *   1. `Evaluate` is PURE: sequence + step → an `EvaluatedFrame` (values per track, events crossed, the
 *      active camera, the animation sections sampling a `.anim`). It resolves nothing and writes nothing,
 *      so the Sequencer can evaluate for a preview, a thumbnail or a test without a world.
 *   2. `Apply` hands that frame to the HOST through `ITimelineHost`, the one seam that knows what a locator
 *      means. Bindings are resolved ONCE per `Sequence::Revision` and cached; an unresolved binding is not
 *      skipped silently — it is reported in `ApplyReport::Unresolved` by label, and the Sequencer paints
 *      the track red (UE's missing-binding state).
 *
 * THE BONE FAST PATH. A character plays hundreds of bone tracks a frame, and a virtual call per bone per
 * frame is the cost this seam must not add. `BindBones` resolves the Bone bindings against a skeleton ONCE
 * (by locator = bone name, the rule Animator::ResolveTrack has) and `EvaluatePose` samples straight into a
 * `LocalPose` — the AnimationClip host's whole playback path, and the Animator's `SampleTrack` replacement.
 */

#include <Engine/Animation/Pose.hpp>
#include <Engine/Animation/TimeModel.hpp>
#include <Engine/Animation/Timeline/Binding.hpp>
#include <Engine/Animation/Timeline/Channel.hpp>
#include <Engine/Animation/Timeline/Player.hpp>
#include <Engine/Animation/Timeline/Sequence.hpp>

#include <Common/Content/AssetEnvelope.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace Desert::Animation
{
    class Skeleton;
}

namespace Desert::Animation::Timeline
{
    using EvaluatedValue = std::variant<float, glm::vec3, glm::quat, BoneTransform, bool>;

    /// One track's value at the step's end. `TrackIndex` indexes `Sequence::Tracks`.
    struct EvaluatedTrack
    {
        uint32_t       TrackIndex = 0;
        EvaluatedValue Value;
    };

    struct FiredEvent
    {
        uint32_t     TrackIndex = 0;
        CrossedEvent Event;
    };

    /// A LevelSequence Animation section active at the step's end: which clip, where in it, how much.
    struct AnimationSample
    {
        uint32_t                   TrackIndex = 0;
        Common::Content::AssetGuid Clip;
        FrameTime                  ClipTime;
        SectionBlendType           Blend  = SectionBlendType::Absolute;
        float                      Weight = 1.0F;
        /// The section's Loop: the HOST wraps `ClipTime` by the clip's length, because the host holds the
        /// clip (UE MapTimeToAnimation reads the length from the asset).
        bool Loop = false;
    };

    /**
     * @brief One leg of the step inside one Subsequence section (UE: the sub-sequence's evaluation range,
     * FMovieSceneSubSequenceData's outer-to-inner transform applied to the parent's range).
     *
     * `From`/`To` are PARENT ticks, already clipped to the section: forward to (Start - 0.5, End], so a key at
     * the section's first tick is crossed once; backward to [Start, End + 0.5). A wrapped step gives two
     * legs. `Moved` = false is a jump (From == To): the child is posed, crosses nothing. `ValuesAt` = the
     * section covers the step's end and this is the leg that ends there: the child's values, animations and
     * camera are applied; on every other leg only its events fire. The host maps both ends with
     * `MapSubsequenceTime`.
     */
    struct SubsequenceSample
    {
        uint32_t      TrackIndex   = 0;
        uint32_t      SectionIndex = 0;
        FrameTime     From;
        FrameTime     To;
        PlayDirection Direction = PlayDirection::Forward;
        bool          Moved     = true;
        bool          ValuesAt  = false;
    };

    struct EvaluatedFrame
    {
        std::vector<EvaluatedTrack>    Values;
        std::vector<FiredEvent>        Events;
        std::vector<AnimationSample>   Animations;
        std::vector<SubsequenceSample> Subsequences;
        /// The Camera Cut in force, or nullopt when the sequence has none (the viewport keeps its camera).
        std::optional<BindingGuid> ActiveCamera;
    };

    /// Opaque to the core: whatever the host needs to find the object again (an entt id, a bone index).
    struct ResolvedBinding
    {
        uint64_t Handle = 0;
    };

    /**
     * @brief The seam. Implemented by each host: the Animator (bones), the UI system (widgets), the
     * LevelSequence ECS system (entities). The core calls nothing else.
     */
    class ITimelineHost
    {
    public:
        virtual ~ITimelineHost() = default;

        /// Find the live object @p binding names. nullopt = not present in this world (reported, not fatal).
        [[nodiscard]] virtual std::optional<ResolvedBinding> Resolve( const Binding& binding ) = 0;

        /// Write one track's value into the object. `property` is the track's `Property`, verbatim.
        virtual void Apply( const ResolvedBinding& target, std::string_view property,
                            const EvaluatedValue& value ) = 0;

        /// One crossed event key. @p target is the event track's binding resolved (nullopt on the Sequence
        /// binding, or an unresolved actor): the object a key's `Action` acts on.
        virtual void Fire( const FiredEvent& event, const std::optional<ResolvedBinding>& target ) = 0;
        virtual void SetCamera( const std::optional<ResolvedBinding>& camera )                     = 0;
        virtual void PlayAnimation( const ResolvedBinding& target, const AnimationSample& sample ) = 0;
    };

    struct ApplyReport
    {
        /// Labels of bindings the host could not resolve; their tracks were not applied.
        std::vector<std::string> Unresolved;
    };

    /// What `Evaluator::Apply` hands the host. A SUBSEQUENCE's frame is applied `Nested` (its camera only when
    /// it has a cut in force: no cut leaves the parent's) or, on a leg that does not end the step, `EventsOnly`.
    enum class ApplyScope : uint8_t
    {
        Whole,
        Nested,
        EventsOnly,
    };

    class Evaluator
    {
    public:
        explicit Evaluator( const Sequence& sequence );

        /// Pure. Muted tracks are absent from `out.Values`. `out` is cleared first and reused (no allocation
        /// once warm).
        void Evaluate( const TimeStep& step, EvaluatedFrame& out ) const;

        /// Resolve (cached per `Sequence::Revision`) and apply. Events fire in (tick, track) order.
        ApplyReport Apply( const EvaluatedFrame& frame, ITimelineHost& host,
                           ApplyScope scope = ApplyScope::Whole );

        [[nodiscard]] const Sequence& GetSequence() const
        {
            return *m_Sequence;
        }

    private:
        const Sequence*                             m_Sequence         = nullptr;
        uint32_t                                    m_ResolvedRevision = UINT32_MAX;
        std::vector<std::optional<ResolvedBinding>> m_Resolved; // index-aligned with Sequence::Bindings
    };

    /**
     * @brief The events @p step crosses on every unmuted Event track, in firing order — appended to @p out.
     * `Evaluate`'s own event half, public for a host that plays events without a whole frame (the
     * Animator's notifies: its bone tracks go through `EvaluatePose`, and evaluating them twice is the
     * cost the fast path exists to avoid). A jump (From == To, no wrap, no turn) crosses nothing.
     */
    void CollectFired( const Sequence& sequence, const TimeStep& step, std::vector<FiredEvent>& out );

    /**
     * @brief Parent time @p at inside Subsequence @p section → the subsequence's own time (UE: the section's
     * outer-to-inner transform): `StartOffset + (at - Start) * TimeScale`, converted from the parent's tick rate
     * to the subsequence's. Pure; computed in doubles, so a fraction of a tick survives the scale.
     */
    [[nodiscard]] FrameTime MapSubsequenceTime( const Section& section, const SubsequenceSectionContent& content,
                                                FrameRate parentRate, FrameRate subRate, FrameTime at );

    /// One track's folded value at @p at — `Evaluate`'s per-track half (a clip curve read by name). false =
    /// muted, or no section covers @p at.
    [[nodiscard]] bool EvaluateTrack( const Track& track, FrameTime at, FrameRate tickRate, EvaluatedValue& out );

    /// Bone binding i → skeleton bone index, or `Skeleton::NO_PARENT`-style UINT32_MAX when the skeleton
    /// has no bone of that name (the clip animates a bone this rig lacks — legal, reported by count).
    struct BoneBindingTable
    {
        std::vector<uint32_t> BoneOfTrack; // index-aligned with Sequence::Tracks; UINT32_MAX = not a bone track
        uint32_t              Missing  = 0;
        uint32_t              Revision = 0;
    };

    [[nodiscard]] BoneBindingTable BindBones( const Sequence& sequence, const Skeleton& skeleton );

    /**
     * @brief Sample every bone Transform track into @p pose (sized to the skeleton; bones without a track
     * keep what @p pose already holds — the caller seeds it with the bind pose).
     *
     * Refuses (by name) a table built for another `Sequence::Revision`. Bit-identical to
     * `BoneTrack::Sample` for a lifted clip — the migration's acceptance test.
     */
    [[nodiscard]] Common::BoolResultStr EvaluatePose( const Sequence& sequence, const BoneBindingTable& table,
                                                      FrameTime at, LocalPose& pose );
} // namespace Desert::Animation::Timeline
