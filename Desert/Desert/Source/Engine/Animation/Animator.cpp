#include "Animator.hpp"

#include <Common/Core/Logger.hpp>
#include <Common/Core/Timestep.hpp>

#include <Engine/Animation/AnimationClip.hpp>
#include <Engine/Animation/Pose.hpp>
#include <Engine/Animation/Skeleton.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <span>

namespace Desert::Animation
{
    namespace
    {
        /// A playhead (ticks from the clip's Start) on the clip's Sequence.
        [[nodiscard]] FrameTime OnSequence( const AnimationClip& clip, const FrameTime time )
        {
            return FrameTime{ FrameNumber{ clip.Sequence.Start.Value + time.Frame.Value }, time.Subframe };
        }

        /// Adds `name` to `out`'s curves with `value` (a name already there keeps its first value).
        void AddCurve( Graph::GraphPose& out, const std::string& name, const float value )
        {
            if ( std::find( out.CurveNames.begin(), out.CurveNames.end(), name ) != out.CurveNames.end() )
                return;
            out.CurveNames.push_back( name );
            out.CurveValues.push_back( value );
        }

        /// The notify states (Event keys with a duration) spanning @p at, in track / key order.
        void ActiveStatesAt( const Timeline::Sequence& sequence, const double at,
                             std::vector<ActiveNotifyState>& out )
        {
            out.clear();
            for ( const Timeline::Track& track : sequence.Tracks )
            {
                if ( track.Muted || track.Kind != Timeline::TrackKind::Event )
                {
                    continue;
                }
                for ( const Timeline::Section& section : track.Sections )
                {
                    const auto* channel = std::get_if<Timeline::Channel>( &section.Content );
                    const auto* events =
                         channel != nullptr ? std::get_if<Timeline::EventChannel>( channel ) : nullptr;
                    if ( events == nullptr )
                    {
                        continue;
                    }
                    for ( const Timeline::EventKey& key : events->Keys )
                    {
                        const auto begin = static_cast<double>( key.Tick.Value );
                        if ( key.Duration.Value > 0 && section.Covers( key.Tick ) && at >= begin &&
                             at < begin + static_cast<double>( key.Duration.Value ) )
                        {
                            out.push_back( ActiveNotifyState{ key.Name, key.Tick, key.Duration } );
                        }
                    }
                }
            }
        }

        [[nodiscard]] bool Holds( const std::vector<ActiveNotifyState>& states, const Timeline::EventKey& key )
        {
            return std::any_of(
                 states.begin(), states.end(), [&key]( const ActiveNotifyState& state )
                 { return state.Name == key.Name && state.Tick == key.Tick && state.Duration == key.Duration; } );
        }

        /// The debug trace of one axis's smoothing: a line when the clamped input changes (with the filtered
        /// value it starts from), and a line when the filtered value arrives at it (with the seconds it took) --
        /// the evidence that a smoothed axis travels over its SmoothingTime instead of jumping.
        void TraceBlendAxis( const Graph::BlendSpaceAxis& axis, Animator::BlendSpaceRun& run, const size_t a,
                             const float input, const bool first, const float deltaTime,
                             const std::string_view label )
        {
            constexpr std::array<char, 2> kAxisName{ 'X', 'Y' };
            const float                   filtered = run.Input[a];
            if ( first )
            {
                run.TracedInput[a]    = input;
                run.SinceChange[a]    = 0.0F;
                run.ArrivalPending[a] = false;
                return;
            }
            if ( input != run.TracedInput[a] )
            {
                LOG_DEBUG(
                     "[Animator] blend space '{}' axis {}: input {:.3f} -> {:.3f}, filtered {:.3f} (smoothing "
                     "{:.2f} s)",
                     label, kAxisName[a], run.TracedInput[a], input, filtered, axis.SmoothingTime );
                run.TracedInput[a]    = input;
                run.SinceChange[a]    = 0.0F;
                run.ArrivalPending[a] = true;
            }
            else
                run.SinceChange[a] += std::max( deltaTime, 0.0F );
            if ( run.ArrivalPending[a] && std::abs( filtered - input ) <= 1e-3F * ( axis.Max - axis.Min ) )
            {
                LOG_DEBUG( "[Animator] blend space '{}' axis {}: filtered {:.3f} reached the input {:.3f} after "
                           "{:.3f} s",
                           label, kAxisName[a], filtered, input, run.SinceChange[a] );
                run.ArrivalPending[a] = false;
            }
        }

        /// UE FAnimNode_BlendSpacePlayer::UpdateAssetPlayer for one blend space, 1D or 2D, at the raw axis values
        /// `raw` (one per axis): each axis clamped or wrapped and filtered (UE FBlendFilter), the weights toward
        /// the filtered point's target (the first update takes the target), then the shared phase advanced over
        /// the weighted samples' lengths at `playRate` x the scale axis's multiplier. The one step a
        /// blend-space node and a blend-space state both take.
        void AdvanceBlendRun( const Graph::BlendSpaceSetup& space, Animator::BlendSpaceRun& run,
                              const std::span<const float> raw, const float playRate, const float deltaTime,
                              const std::string_view label )
        {
            const size_t         axes = std::min( space.Axes.size(), run.Filters.size() );
            std::array<float, 2> clamped{ 0.0F, 0.0F };
            for ( size_t a = 0; a < axes; ++a )
            {
                const float value = a < raw.size() ? raw[a] : 0.0F;
                clamped[a]        = Graph::ClampBlendSpaceAxis( space.Axes[a], value );
                const bool first  = !run.Filters[a].Initialised;
                run.Input[a]      = Graph::FilterBlendSpaceAxis( space.Axes[a], run.Filters[a], value, deltaTime );
                TraceBlendAxis( space.Axes[a], run, a, clamped[a], first, deltaTime, label );
            }
            std::vector<float>& target = run.TargetScratch; // reused across ticks: assign keeps its capacity
            Graph::BlendSpaceTargetWeights( space, run.Triangulation,
                                            std::span<const float>( run.Input.data(), axes ), target );
            if ( !run.Started || run.Weights.size() != target.size() )
            {
                run.Weights = target;
                run.Started = true;
            }
            else
                Graph::InterpolateBlendWeights( run.Weights, target, space.WeightSpeed, space.WeightEaseInOut,
                                                deltaTime );
            // UE AxisToScaleAnimation: while the scale axis's filtered value lags its input, the samples play
            // faster or slower by input / filtered, so the feet keep up with the speed the input already asks
            // for. OUR CHOICE, NOT PROVEN against UE's TickAssetPlayer (D9): the multiplier is input / filtered
            // when both lie on one side of 0 and the filtered value is away from it, else 1.
            float rate = playRate;
            if ( space.ScaleAxis >= 0 && static_cast<size_t>( space.ScaleAxis ) < axes )
            {
                const auto  scaleAxis = static_cast<size_t>( space.ScaleAxis );
                const float filtered  = run.Input[scaleAxis];
                const float input     = clamped[scaleAxis];
                if ( std::abs( filtered ) > 1e-3F && filtered * input > 0.0F )
                    rate *= input / filtered;
            }
            // An unresolved sample has no length: it neither stretches nor shrinks the shared cycle.
            std::vector<float>& durations = run.DurationScratch; // reused across ticks
            durations.assign( space.Samples.size(), 0.0F );
            for ( size_t i = 0; i < run.Clips.size() && i < durations.size(); ++i )
                if ( run.Clips[i] != nullptr )
                    durations[i] = static_cast<float>( run.Clips[i]->DurationSeconds() );
            run.PreviousPhase = run.Phase;
            run.Backward      = rate * deltaTime < 0.0F;
            run.Phase         = Graph::AdvanceSyncedPhase( run.Phase, run.Weights, durations, space.Samples, rate,
                                                           space.Loop, deltaTime );
        }

        /// Whether two blend-space states play the same space on the same parameters (a request for the space
        /// already on top keeps its run).
        [[nodiscard]] bool SameBlendSpace( const Graph::StateBlendSpace& a, const Graph::StateBlendSpace& b )
        {
            return a.AxisParameters == b.AxisParameters && a.Space == b.Space;
        }
    } // namespace

    const char* ToString( PoseStage stage )
    {
        switch ( stage )
        {
            case PoseStage::Source:
                return "Source";
            case PoseStage::Graph:
                return "Graph";
            case PoseStage::Controls:
                return "Controls";
            case PoseStage::Rig:
                return "Rig";
        }
        return "?";
    }

    Animator::Animator( const Skeleton& skeleton )
         : m_Skeleton( skeleton ), m_Component( skeleton, m_EvaluatedPose ),
           m_DisplayComponent( skeleton, m_DisplayPose )
    {
        // THE BIND POSE IS DECOMPOSED ONCE, HERE. Every untracked bone of every clip and every additive
        // layer's reference reads it; it used to be recovered from `LocalBindTransform` on each access, and
        // in the additive path that was a full Decompose per bone per layer per frame.
        auto bind = LocalPose::FromBindPose( skeleton );
        if ( bind.IsSuccess() )
        {
            m_BindPose = std::move( bind.GetValue() );
        }
        else
        {
            // Not a silent fallback: the rig is named, the reason is named, and the pose it renders is the
            // identity rest rather than a quietly straightened mirror. Refusing to construct the Animator
            // was the alternative and it is worse — an entity would lose its animation with the same
            // silence this engine spent a whole task removing from the clip lookup.
            LOG_ERROR( "[Animator] rig (signature {}) has a bind pose that cannot be decomposed: {} Bones "
                       "fall back to an identity rest transform.",
                       skeleton.GetSignature(), bind.GetError() );
            m_BindPose.Resize( skeleton.GetBones().size() );
        }

        if ( !skeleton.GetStructureError().empty() )
        {
            LOG_ERROR( "[Animator] rig (signature {}) has a malformed parent structure: {}",
                       skeleton.GetSignature(), skeleton.GetStructureError() );
        }

        m_AuthoringPose = m_BindPose;
        m_EvaluatedPose = m_BindPose;

        // The pipeline. `Source` is unconditional — something has to produce a pose — and the optional
        // stages join and leave with the lists that feed them (SyncStages).
        SyncStages();

        m_Component.Reset( m_EvaluatedPose.Size() );
        PublishPose(); // a valid rest pose before any clip plays; an identity pose would collapse the mesh
    }

    // ============================================================
    // Pose authoring
    // ============================================================

    void Animator::SetBoneLocalPose( uint32_t boneIndex, const glm::mat4& localTransform )
    {
        if ( boneIndex >= m_AuthoringPose.Size() )
        {
            return;
        }

        auto decomposed = BoneTransform::FromMatrix( localTransform );
        if ( !decomposed.IsSuccess() )
        {
            LOG_ERROR( "[Animator] refusing to pose bone {} ('{}'): {}", boneIndex,
                       m_Skeleton.GetBones()[boneIndex].Name, decomposed.GetError() );
            return;
        }
        m_AuthoringPose[boneIndex] = decomposed.GetValue();
    }

    glm::mat4 Animator::GetBoneLocalPose( uint32_t boneIndex ) const
    {
        return boneIndex < m_AuthoringPose.Size() ? m_AuthoringPose[boneIndex].ToMatrix() : glm::mat4( 1.0F );
    }

    Common::BoolResultStr Animator::SetAuthoringPose( const LocalPose& pose )
    {
        const size_t bones = m_Skeleton.GetBones().size();
        if ( pose.Size() != bones )
        {
            return Common::MakeFormattedError<bool>(
                 "refusing to install a pose of {} bone(s) on a rig of {}: a LocalPose is index-for-index "
                 "with the skeleton, so a buffer of another length belongs to another rig",
                 pose.Size(), bones );
        }
        m_AuthoringPose = pose;
        return Common::MakeSuccess( true );
    }

    void Animator::SampleClipIntoLocalPose( const AnimationClip& clip, FrameTime time )
    {
        const size_t n = m_Skeleton.GetBones().size();
        if ( m_AuthoringPose.Size() != n )
            m_AuthoringPose = m_BindPose;
        // The authoring pose is the whole rig, whatever LOD the entity draws.
        SampleClipPose( RigSampling{ m_Skeleton, m_BindPose, m_TrackBinding }, &clip, time, m_AuthoringPose );
    }

    Common::BoolResultStr Animator::RebindRestPose()
    {
        auto bind = LocalPose::FromBindPose( m_Skeleton );
        if ( !bind.IsSuccess() )
            return Common::MakeFormattedError<bool>( "the rest pose of rig {} does not decompose: {}",
                                                     m_Skeleton.GetSignature(), bind.GetError() );
        m_BindPose      = bind.GetValue();
        m_AuthoringPose = m_BindPose;
        ApplyLocalPose();
        return Common::MakeSuccess( true );
    }

    void Animator::ApplyLocalPose()
    {
        const size_t n = m_Skeleton.GetBones().size();
        if ( m_AuthoringPose.Size() != n )
            m_AuthoringPose = m_BindPose;

        // The authoring pose is a DIFFERENT SOURCE, not a pipeline stage. It is engaged only with playback
        // stopped (the Sequencer clears `Playing` before it keys), so a stage would carry a membership bit
        // no frame could ever distinguish — the honest shape is "render this pose instead", which is what
        // this is.
        //
        // IT WRITES THROUGH THE EVALUATED POSE rather than resolving the authoring buffer on the side. The
        // Animator has to have ONE answer to "what is being rendered right now": `GetPose()` and
        // `GetBoneModelMatrix()` are two views of it, and a socket reading a bone while the artist poses it
        // must see the posed bone. A private resolve here would have made the two views disagree for
        // exactly as long as posing lasts — the middle-link shape, introduced by the very change meant to
        // remove it.
        m_EvaluatedPose = m_AuthoringPose;
        PublishPose();
    }

    Common::BoolResultStr Animator::ApplyPhysicsPose( const std::vector<BoneOverride>& componentOverrides )
    {
        auto applied = ApplyBoneOverrides( m_Skeleton, m_EvaluatedPose, m_Component, componentOverrides, 1.0f,
                                           m_PhysicsScratch );
        if ( !applied.IsSuccess() )
            return applied;
        PublishPose();
        return Common::MakeSuccess( true );
    }

    // ============================================================
    // Play / CrossFade
    // ============================================================

    void Animator::RetireNotifyStates()
    {
        RetireStates( m_ActiveStates, -1, -1 );
        for ( IncomingFade& fade : m_Fades )
            RetireStates( fade.ActiveStates, -1, -1 );
    }

    std::vector<float> Animator::BaseLayerWeights() const
    {
        std::vector<float> alphas;
        alphas.reserve( m_Fades.size() );
        for ( const IncomingFade& fade : m_Fades )
            alphas.push_back( fade.Playback.IsValid() ? fade.Alpha() : 0.0F );
        std::vector<float> weights;
        FadeStackWeights( alphas, weights );
        return weights;
    }

    std::optional<float> Animator::GetCurveValue( const std::string_view name ) const
    {
        if ( !m_PoseGraph )
            return BaseCurveValue( name );
        const Graph::GraphPose& out = m_PoseGraph->Out;
        for ( size_t c = 0; c < out.CurveNames.size() && c < out.CurveValues.size(); ++c )
            if ( out.CurveNames[c] == name )
                return out.CurveValues[c];
        return std::nullopt;
    }

    std::optional<float> Animator::BaseCurveValue( const std::string_view name ) const
    {
        const auto valueOf = []( const ClipPlayback& playback, std::string_view curveName ) -> std::optional<float>
        {
            if ( !playback.IsValid() )
            {
                return std::nullopt;
            }
            // The clip's curves are its Float tracks, named by Property (the lift's mapping); the fold is the
            // Evaluator's own, so a curve reads here what the Sequencer shows.
            const Timeline::Sequence& sequence = playback.Clip->Sequence;
            for ( const Timeline::Track& track : sequence.Tracks )
            {
                Timeline::EvaluatedValue value;
                if ( track.Kind == Timeline::TrackKind::Float && track.Property == curveName &&
                     Timeline::EvaluateTrack( track, OnSequence( *playback.Clip, playback.Time ),
                                              sequence.TickRate, value ) )
                {
                    return std::get<float>( value );
                }
            }
            return std::nullopt;
        };

        // The same nesting as the pose (BlendedBasePose): each fade mixes over everything below it. A clip
        // without the curve reads 0 inside a blend, and no clip of the stack having it is "no curve".
        std::optional<float> value = valueOf( m_Current, name );
        for ( const IncomingFade& fade : m_Fades )
        {
            const std::optional<float> incoming = valueOf( fade.Playback, name );
            if ( !value && !incoming )
                continue;
            value = glm::mix( value.value_or( 0.0F ), incoming.value_or( 0.0F ), fade.Alpha() );
        }
        return value;
    }

    void Animator::Play( const AnimationClip& clip, bool loop )
    {
        RetireNotifyStates();
        m_Current = { .Clip = &clip, .Time = FrameTime{}, .Loop = loop, .StepFrom = FrameTime{} };
        m_Fades.clear();
    }

    void Animator::CrossFade( const AnimationClip& clip, float duration, bool loop, AlphaBlendOption curve,
                              float elapsed )
    {
        // A blend space on top whose heaviest sample is `clip` is NOT that clip playing: the clip fades in over
        // it.
        if ( !TopPlayback().IsBlendSpace() && GetCurrentClip() == &clip )
        {
            return;
        }

        // СТЕК, А НЕ ЗАМЕНА: клип, ещё входящий в смешивание, НЕ обрывается — он продолжает играть под
        // новым и угасает вместе со всем, что под ним (UE ActiveTransitionArray). Раньше новый кроссфейд
        // выбрасывал входящий клип, и поза прыгала на кадре прерывания.
        IncomingFade fade;
        fade.Playback = { .Clip = &clip, .Time = FrameTime{}, .Loop = loop, .StepFrom = FrameTime{} };
        fade.Curve    = curve;
        // A crossfade of zero would divide by zero in Alpha; the floor is small enough that the first Update
        // already reaches alpha 1, so a zero-length blend behaves as an instant cut.
        constexpr float MIN_BLEND_SECONDS = 0.0001F;
        fade.Duration                     = glm::max( duration, MIN_BLEND_SECONDS );
        fade.Time                         = glm::max( elapsed, 0.0F );
        m_Fades.push_back( std::move( fade ) );
    }

    Animator::ClipPlayback Animator::BlendPlaybackOf( const Graph::StateBlendSpace&         space,
                                                      std::span<const AnimationClip* const> clips )
    {
        ClipPlayback playback;
        playback.BlendSpace          = space;
        playback.Loop                = space.Space.Loop;
        playback.Blend.Triangulation = Graph::TriangulateBlendSpace( space.Space );
        playback.Blend.Clips.assign( space.Space.Samples.size(), nullptr );
        for ( size_t i = 0; i < clips.size() && i < playback.Blend.Clips.size(); ++i )
            playback.Blend.Clips[i] = clips[i];
        // Valid from entry with a clip on its clock (until UpdateBlendPlayback picks the heaviest sample): every
        // reader of a valid player - a scrub, IsFinished, the curves - reads `Clip`.
        playback.Clip = ClockClip( playback );
        return playback;
    }

    void Animator::PlayBlendSpace( const Graph::StateBlendSpace&         space,
                                   std::span<const AnimationClip* const> clips )
    {
        if ( m_Fades.empty() && m_Current.IsBlendSpace() && SameBlendSpace( m_Current.BlendSpace, space ) )
        {
            // The row already playing: its run goes on; only the sample clips are taken.
            for ( size_t i = 0; i < clips.size() && i < m_Current.Blend.Clips.size(); ++i )
                m_Current.Blend.Clips[i] = clips[i];
            return;
        }
        RetireNotifyStates();
        m_Current = BlendPlaybackOf( space, clips );
        m_Fades.clear();
    }

    void Animator::CrossFadeBlendSpace( const Graph::StateBlendSpace&         space,
                                        std::span<const AnimationClip* const> clips, const float duration,
                                        const AlphaBlendOption curve, const float elapsed )
    {
        ClipPlayback& top = m_Fades.empty() ? m_Current : m_Fades.back().Playback;
        if ( top.IsBlendSpace() && SameBlendSpace( top.BlendSpace, space ) )
        {
            for ( size_t i = 0; i < clips.size() && i < top.Blend.Clips.size(); ++i )
                top.Blend.Clips[i] = clips[i];
            return;
        }
        // Stacked over everything below it, as CrossFade stacks a clip (UE ActiveTransitionArray).
        IncomingFade fade;
        fade.Playback                     = BlendPlaybackOf( space, clips );
        fade.Curve                        = curve;
        constexpr float MIN_BLEND_SECONDS = 0.0001F;
        fade.Duration                     = glm::max( duration, MIN_BLEND_SECONDS );
        fade.Time                         = glm::max( elapsed, 0.0F );
        m_Fades.push_back( std::move( fade ) );
    }

    const Graph::StateBlendSpace* Animator::GetCurrentBlendSpace() const
    {
        const ClipPlayback& top = TopPlayback();
        return top.IsBlendSpace() ? &top.BlendSpace : nullptr;
    }

    const Animator::BlendSpaceRun* Animator::GetStateBlendSpace() const
    {
        const ClipPlayback& top = TopPlayback();
        return top.IsBlendSpace() ? &top.Blend : nullptr;
    }

    void Animator::Stop()
    {
        RetireNotifyStates();
        m_Current = {};
        m_Fades.clear();
    }

    // ============================================================
    // Update — the pipeline
    // ============================================================

    void Animator::Update( const Common::Timestep& step )
    {
        const float deltaTime = step.GetSeconds() * m_PlaybackSpeed;

        // "NO CLIP" IS NOT THE SAME QUESTION AS "NOTHING TO DO", and it used to be. The early return here
        // tested only the clip, which was correct while every stage read one — but a skeletal control drives
        // the pose from a GOAL, not from a track, and a rig standing in its bind pose with an IK goal moving
        // over it is an ordinary thing to want. With no clip the source stage produces the bind pose (an
        // unresolved track falls back to it, bone by bone) and the controls correct it from there. A pose
        // graph with a blend space plays clips of its own with no base clip at all (its output may be only
        // the blend space), so it is something to do too.
        const bool blendSpaces =
             m_PoseGraph && std::any_of( m_PoseGraph->BlendSpaces.begin(), m_PoseGraph->BlendSpaces.end(),
                                         []( const BlendSpaceRun& run ) { return !run.Clips.empty(); } );
        if ( !m_Current.IsValid() && m_Controls.empty() && !blendSpaces )
        {
            return;
        }

        UpdatePlayback( m_Current, deltaTime );

        for ( IncomingFade& fade : m_Fades )
        {
            if ( fade.Playback.IsValid() )
                UpdatePlayback( fade.Playback, deltaTime );
            fade.Time += deltaTime;
        }

        if ( m_PoseGraph )
        {
            // A blend-space node's Sources entry is not a clock of its own: UpdateBlendSpaces writes its run's
            // heaviest sample there.
            const Graph::AnimGraph& graph = m_PoseGraph->Instance.Graph();
            for ( size_t node = 0; node < m_PoseGraph->Sources.size(); ++node )
                if ( m_PoseGraph->Sources[node].IsValid() && !graph.Nodes[node].BlendSpace )
                    UpdatePlayback( m_PoseGraph->Sources[node], deltaTime );
            UpdateBlendSpaces( *m_PoseGraph, deltaTime );
        }
        for ( auto& layer : m_LinkedSources )
            for ( auto& source : layer )
                if ( source.IsValid() )
                    UpdatePlayback( source, deltaTime );

        // THE ONE EVALUATION WHOSE CLOCK STEPS ARE PLAYED TIME: only this one extracts root motion.
        m_ExtractingStep = true;
        EvaluatePipeline();
        m_ExtractingStep = false;
        if ( m_RootMotionMode == RootMotionMode::RootMotionFromEverything )
            m_RootMotion = AccumulateRootMotion( m_RootMotion, m_StepRootMotion );
        // After the evaluation, which is what weighed every source this tick — the base players included.
        StepBaseNotifies( true );
        StepGraphNotifies( true );

        // Retire AFTER evaluating, so the frame that reaches alpha 1 renders the target clip rather than a
        // pose built from a blend that has already been thrown away. The NEWEST finished fade covers
        // everything below it at weight 1: it becomes the current clip and every layer under it retires.
        for ( size_t i = m_Fades.size(); i-- > 0; )
        {
            if ( m_Fades[i].Alpha() < 1.0F )
                continue;
            // The layers below are at weight 0 now (their states ended in StepBaseNotifies); the finished
            // fade's states, reported while it faded in, are the current clip's from here.
            RetireStates( m_ActiveStates, -1, -1 );
            for ( size_t below = 0; below < i; ++below )
                RetireStates( m_Fades[below].ActiveStates, -1, -1 );
            m_ActiveStates.swap( m_Fades[i].ActiveStates );
            m_Current = m_Fades[i].Playback;
            m_Fades.erase( m_Fades.begin(), m_Fades.begin() + static_cast<std::ptrdiff_t>( i ) + 1 );
            break;
        }
    }

    void Animator::EvaluatePipeline()
    {
        if ( m_EvaluatedPose.Size() != m_Skeleton.GetBones().size() )
        {
            m_EvaluatedPose = m_BindPose;
            m_Component.Reset( m_EvaluatedPose.Size() );
        }
        m_StepRootMotion = NoRootMotion();

        for ( const PoseStage stage : m_Stages )
        {
            switch ( stage )
            {
                case PoseStage::Source:
                    EvaluateSource( m_EvaluatedPose );
                    break;
                case PoseStage::Graph:
                    // The stage is listed exactly when the graph is attached (SyncStages); the state is bound
                    // here, after that check, rather than dereferenced unchecked inside.
                    if ( m_PoseGraph )
                        EvaluateGraph( *m_PoseGraph, m_EvaluatedPose );
                    break;
                case PoseStage::Controls:
                    EvaluateControls( m_EvaluatedPose );
                    break;
                case PoseStage::Rig:
                    EvaluateRig( m_EvaluatedPose );
                    break;
            }
        }

        PublishPose();
    }

    void Animator::EvaluateSource( LocalPose& pose )
    {
        if ( !m_Retarget )
        {
            const RigSampling rig = TargetSampling();
            BlendedBasePose( rig, pose );
            m_StepRootMotion = BlendedBaseRootMotion( rig );
            return;
        }

        // THE CLIPS ARE ON ANOTHER RIG. Sample the whole base pose there — through the same
        // `BlendedBasePose`, so a crossfade is blended once and identically on either rig — and let the
        // retargeter say what it is on this one.
        const RigSampling rig        = SourceSampling();
        LocalPose&        sourcePose = m_Retarget->SourceScratch();
        BlendedBasePose( rig, sourcePose );
        // ON THE SOURCE RIG, where the clip's root track lives; RootMotionBetween scales it to this one.
        m_StepRootMotion = BlendedBaseRootMotion( rig );

        // A REFUSAL LEAVES `pose` AS THE LAST FRAME LEFT IT, and `Run` has already said why, once per
        // distinct message. Falling back to the bind pose instead would look like a character snapping to
        // rest for one frame — which reads as a physics glitch rather than as the configuration error it
        // is — and re-running the stage cannot help, because every refusal `Retarget` makes is structural.
        static_cast<void>( m_Retarget->Run( m_Skeleton, sourcePose, pose ) );
    }

    void Animator::EvaluateGraph( PoseGraphState& state, LocalPose& pose )
    {
        const size_t            n     = m_Skeleton.GetBones().size();
        const Graph::AnimGraph& graph = state.Instance.Graph();

        Graph::PoseGraphSources sources;
        const auto              sampleClock = [&]( const ClipPlayback& playback, Graph::GraphPose& out )
        {
            if ( !playback.IsValid() )
            {
                out.Pose = m_BindPose; // a source with no clip yet (still loading) stands in the bind pose
                LockRoot( TargetSampling(), out.Pose );
                return;
            }
            // ITS CURVES TRAVEL WITH ITS POSE (UE FPoseContext::Curve): the clip's Float tracks, named by
            // Property, at the playhead — what the nodes blend.
            const Timeline::Sequence& sequence = playback.Clip->Sequence;
            for ( const Timeline::Track& track : sequence.Tracks )
            {
                Timeline::EvaluatedValue value;
                if ( track.Kind == Timeline::TrackKind::Float &&
                     Timeline::EvaluateTrack( track, OnSequence( *playback.Clip, playback.Time ),
                                              sequence.TickRate, value ) )
                    AddCurve( out, track.Property, std::get<float>( value ) );
            }
            // UNDER A RETARGET THE SOURCE IS RETARGETED WHOLE, BEFORE ANY NODE READS IT: the retarget equation
            // reads a bone's chain to place it, so retargeting only the bones a blend keeps would place them
            // against a source pose that had never been resolved.
            if ( m_Retarget )
            {
                const RigSampling rig        = SourceSampling();
                LocalPose&        sourcePose = m_Retarget->SourceScratch();
                SampleClipPose( rig, playback.Clip, playback.Time, sourcePose );
                LockRoot( rig, sourcePose );
                out.RootMotion = ClockRootMotion( rig, playback );
                // THE BIND POSE, NOT THE SOURCE RIG'S TRANSFORMS: `Run` has reported the reason, and source-rig
                // local transforms written onto target bones are the proportion defect a retarget removes.
                if ( !m_Retarget->Run( m_Skeleton, sourcePose, out.Pose ) )
                    out.Pose = m_BindPose;
                return;
            }
            const RigSampling rig = TargetSampling();
            SampleClipPose( rig, playback.Clip, playback.Time, out.Pose );
            LockRoot( rig, out.Pose );
            out.RootMotion = ClockRootMotion( rig, playback );
        };
        sources.Sample = [&]( size_t node, Graph::GraphPose& out )
        {
            // THE BASE SOURCE IS THE SOURCE STAGE: its crossfade, notifies and root motion are the Animator's.
            if ( static_cast<int>( node ) == state.BaseSource )
            {
                out.Pose               = pose;
                out.RootMotion         = m_StepRootMotion;
                const auto addCurvesOf = [&]( const ClipPlayback& player )
                {
                    if ( player.IsValid() )
                        for ( const Timeline::Track& track : player.Clip->Sequence.Tracks )
                            if ( track.Kind == Timeline::TrackKind::Float )
                                if ( const auto value = BaseCurveValue( track.Property ) )
                                    AddCurve( out, track.Property, *value );
                };
                addCurvesOf( m_Current );
                for ( const IncomingFade& fade : m_Fades )
                    addCurvesOf( fade.Playback );
                return;
            }
            if ( graph.Nodes[node].BlendSpace )
            {
                SampleBlendSpace( state, state.BlendSpaces[node], graph.Nodes[node].BlendSpace->Loop, sampleClock,
                                  out );
                return;
            }
            sampleClock( state.Sources[node], out );
        };
        // A linked layer's sequence players run on their own clocks, sampled exactly as the host's are.
        sources.SampleLinked = [&]( size_t slot, size_t node, Graph::GraphPose& out )
        { sampleClock( m_LinkedSources[slot][node], out ); };
        sources.Linked    = &m_LinkedLayers;
        sources.Parameter = [this]( const std::string& name ) { return GraphParameter( name ); };
        // THE REST POSE THE ADDITIVE WAS AUTHORED AGAINST, ON THIS RIG: under a retarget the additive clip is
        // on the source rig, so its difference is taken from the source rest retargeted the same way.
        sources.AdditiveReference = m_Retarget ? &m_Retarget->GetRetargetedRest() : &m_BindPose;

        state.Instance.Evaluate( sources, m_Skeleton, state.Out );
        if ( state.Out.Pose.Size() == n )
        {
            pose = state.Out.Pose;
            // THE GRAPH'S ROOT MOTION IS THE FRAME'S: every source carried its own and the nodes weighed them
            // as they weighed the poses (a layered blend by the root bone's weight, an additive not at all).
            m_StepRootMotion = state.Out.RootMotion;
        }
    }

    float Animator::GraphParameter( const std::string_view name ) const
    {
        for ( const GraphParameterValue& live : m_GraphParameters )
            if ( live.Name == name )
                return live.Value;
        if ( m_PoseGraph )
            for ( const Graph::Parameter& declared : m_PoseGraph->Instance.Graph().Parameters )
                if ( declared.Name == name )
                    return declared.Default;
        return 0.0F; // PlanPoseGraph refused a pin bound to an undeclared parameter
    }

    namespace
    {
        /// The heaviest resolved sample of `run` (its index), or Clips.size() when none is resolved.
        [[nodiscard]] size_t HeaviestSample( const Animator::BlendSpaceRun& run )
        {
            size_t heaviest = run.Clips.size();
            for ( size_t i = 0; i < run.Clips.size() && i < run.Weights.size(); ++i )
                if ( run.Clips[i] != nullptr &&
                     ( heaviest == run.Clips.size() || run.Weights[i] > run.Weights[heaviest] ) )
                    heaviest = i;
            return heaviest;
        }

        /// A clock at `phase` of `clip`'s own length.
        [[nodiscard]] FrameTime PhaseTime( const AnimationClip& clip, const float phase )
        {
            return SecondsToFrameTime( static_cast<double>( phase ) * clip.DurationSeconds(),
                                       clip.Sequence.TickRate );
        }

        /// `playback`'s clock moved to the heaviest resolved sample of `run` at its shared phase, with the run's
        /// last step: what an exit time reads and whose notifies are heard (UE HighestWeightedAnimation). No
        /// clip when no sample is resolved.
        void FollowHeaviestSample( const Animator::BlendSpaceRun& run, const bool loop, const AnimationClip*& clip,
                                   FrameTime& time, FrameTime& stepFrom, bool& wrapped, bool& backward )
        {
            const size_t heaviest = HeaviestSample( run );
            if ( heaviest == run.Clips.size() )
            {
                clip = nullptr;
                return;
            }
            clip     = run.Clips[heaviest];
            time     = PhaseTime( *clip, run.Phase );
            stepFrom = PhaseTime( *clip, run.PreviousPhase );
            wrapped  = loop && ( run.Backward ? run.Phase > run.PreviousPhase : run.Phase < run.PreviousPhase );
            backward = run.Backward;
        }
    } // namespace

    void Animator::UpdateBlendSpaces( PoseGraphState& state, const float deltaTime ) const
    {
        const Graph::AnimGraph&                          graph     = state.Instance.Graph();
        const std::function<float( const std::string& )> parameter = [this]( const std::string& name )
        { return GraphParameter( name ); };
        for ( size_t n = 0; n < state.BlendSpaces.size(); ++n )
        {
            const Graph::PoseNode& node = graph.Nodes[n];
            if ( !node.BlendSpace )
                continue;
            const Graph::BlendSpaceSetup& space = *node.BlendSpace;
            BlendSpaceRun&                run   = state.BlendSpaces[n];
            const std::array<float, 2>    raw{
                 Graph::PoseGraphInstance::PinValue( node, Graph::kBlendSpaceXPin, 0.0F, parameter ),
                 Graph::PoseGraphInstance::PinValue( node, Graph::kBlendSpaceYPin, 0.0F, parameter ) };
            const float playRate =
                 Graph::PoseGraphInstance::PinValue( node, Graph::kBlendSpacePlayRatePin, 1.0F, parameter );
            AdvanceBlendRun( space, run, std::span<const float>( raw.data(), space.Axes.size() ), playRate,
                             deltaTime, node.Name );
            ClipPlayback& source = state.Sources[n];
            source.Loop          = space.Loop;
            FollowHeaviestSample( run, space.Loop, source.Clip, source.Time, source.StepFrom, source.StepWrapped,
                                  source.StepBackward );
        }
    }

    template <typename Visit>
    bool Animator::ForEachBlendSample( const BlendSpaceRun& run, const bool loop, const Visit& visit )
    {
        const bool wrapped =
             loop && ( run.Backward ? run.Phase > run.PreviousPhase : run.Phase < run.PreviousPhase );
        float total = 0.0F;
        for ( size_t i = 0; i < run.Clips.size() && i < run.Weights.size(); ++i )
        {
            const AnimationClip* clip   = run.Clips[i];
            const float          weight = run.Weights[i];
            if ( clip == nullptr || weight <= Graph::kBlendSampleWeight )
                continue;
            // SYNCED: every sample at the one normalized phase of its OWN length (UE's blend space sync), its step
            // the run's, so its root motion is its own stride over that step.
            ClipPlayback playback;
            playback.Clip         = clip;
            playback.Loop         = loop;
            playback.Time         = PhaseTime( *clip, run.Phase );
            playback.StepFrom     = PhaseTime( *clip, run.PreviousPhase );
            playback.StepWrapped  = wrapped;
            playback.StepBackward = run.Backward;
            // This sample's share of everything weighed so far: blending the running result toward it by
            // w / (total + w) leaves every sample at its weight over the sum of the weights.
            const float alpha = weight / ( total + weight );
            total += weight;
            visit( playback, alpha );
        }
        return total > 0.0F;
    }

    template <typename SampleClock>
    void Animator::SampleBlendSpace( PoseGraphState& state, const BlendSpaceRun& run, const bool loop,
                                     const SampleClock& sampleClock, Graph::GraphPose& out )
    {
        const bool sampled = ForEachBlendSample(
             run, loop,
             [&]( const ClipPlayback& playback, const float alpha )
             {
                 if ( alpha >= 1.0F )
                 {
                     sampleClock( playback, out ); // the first sample: the running result is its pose
                     return;
                 }
                 Graph::GraphPose& sample = state.BlendScratch;
                 sample.CurveNames.clear();
                 sample.CurveValues.clear();
                 sample.RootMotion = BoneTransform{};
                 sampleClock( playback, sample );
                 for ( uint32_t b = 0; b < out.Pose.Size() && b < sample.Pose.Size(); ++b )
                     out.Pose[b] = Blend( out.Pose[b], sample.Pose[b], alpha );
                 // Each sample's own stride over the step, weighed as its pose is (UE: every sample's root motion
                 // x its weight).
                 out.RootMotion = Blend( out.RootMotion, sample.RootMotion, alpha );
                 for ( float& value : out.CurveValues )
                     value *= 1.0F - alpha;
                 for ( size_t c = 0; c < sample.CurveNames.size(); ++c )
                 {
                     const auto at =
                          std::find( out.CurveNames.begin(), out.CurveNames.end(), sample.CurveNames[c] );
                     if ( at == out.CurveNames.end() )
                     {
                         out.CurveNames.push_back( sample.CurveNames[c] );
                         out.CurveValues.push_back( sample.CurveValues[c] * alpha );
                     }
                     else
                         out.CurveValues[static_cast<size_t>( at - out.CurveNames.begin() )] +=
                              sample.CurveValues[c] * alpha;
                 }
             } );
        if ( !sampled )
            out.Pose = m_BindPose; // no sample resolved yet (still loading): the bind pose, as a source's
    }

    void Animator::EvaluateControls( LocalPose& pose )
    {
        // The component view is over `pose`, and the stages before this one have just rewritten every bone
        // of it. Its cached matrices are last frame's; dropping the flags here is what makes the first
        // `Get` inside a solve read THIS frame's pose. (ApplyBoneOverrides re-invalidates after each write.)
        m_Component.Invalidate();

        for ( auto& control : m_Controls )
        {
            if ( !control )
            {
                continue;
            }

            // DISCARDED DELIBERATELY. The control has already reported its own refusal — once per distinct
            // message, so a standing condition does not become a log per frame — and a refused control
            // leaves the pose exactly as the previous stage produced it. The next control still runs: one
            // solver failing to resolve its bone name is not a reason to throw away another's work.
            static_cast<void>( control->Evaluate( m_Skeleton, pose, m_Component ) );
        }
    }

    void Animator::EvaluateRig( LocalPose& pose )
    {
        // DISCARDED for the reason `EvaluateControls` discards: the rig has already reported its own
        // refusal, once per distinct message, and a refused rig leaves the pose exactly as the stage before
        // it produced it. There is nothing this function could do with the string that the rig has not
        // already done, and `GetRig()->GetLastError()` is where an editor reads it.
        static_cast<void>( m_Rig->Evaluate( m_Skeleton, pose, m_Component ) );
    }

    void Animator::PublishPose()
    {
        m_Component.Invalidate();
        // Update's evaluation while interpolating skipped frames: the shown pose is published by
        // PublishInterpolated, every frame. Every other publish (a scrub, a physics pose) writes as before.
        if ( m_InterpolateSkipped && m_ExtractingStep )
            return;
        m_Component.WriteSkinningMatrices( m_Skinning.Matrices, m_RequiredOrder );
        ++m_SkinningRevision;
    }

    bool Animator::SetRequiredBones( const std::span<const uint32_t> skinnedBones )
    {
        if ( std::equal( skinnedBones.begin(), skinnedBones.end(), m_RequiredSource.begin(), m_RequiredSource.end() ) )
            return false;
        m_RequiredSource.assign( skinnedBones.begin(), skinnedBones.end() );
        const std::size_t boneCount = m_Skeleton.GetBones().size();
        const std::vector<uint8_t> previous = m_RequiredMask;
        m_RequiredMask.assign( boneCount, 0u );
        // CLOSED OVER THE PARENTS (UE FBoneContainer: a required bone's ancestors are required): a bone's matrix
        // is its parent's times its own local.
        for ( uint32_t bone : skinnedBones )
            while ( bone < boneCount && m_RequiredMask[bone] == 0u )
            {
                m_RequiredMask[bone] = 1u;
                bone                 = m_Skeleton.ResolveParent( bone );
            }
        m_RequiredOrder.clear();
        for ( const uint32_t bone : m_Skeleton.GetResolveOrder() )
            if ( bone < boneCount && m_RequiredMask[bone] != 0u )
                m_RequiredOrder.push_back( bone );
        // Every bone (or none named): no restriction at all - the full pipeline, bit for bit.
        if ( m_RequiredOrder.size() == boneCount || m_RequiredOrder.empty() )
        {
            m_RequiredMask.clear();
            m_RequiredOrder.clear();
        }
        // Grew: a bone now drawn was not evaluated (or was evaluated under a narrower set).
        if ( m_RequiredMask.empty() )
            return !previous.empty();
        if ( previous.empty() )
            return false;
        for ( std::size_t bone = 0; bone < boneCount && bone < previous.size(); ++bone )
            if ( m_RequiredMask[bone] != 0u && previous[bone] == 0u )
                return true;
        return false;
    }

    void Animator::SetSkippedFrameInterpolation( const bool interpolate )
    {
        if ( interpolate == m_InterpolateSkipped )
            return;
        m_InterpolateSkipped = interpolate;
        if ( interpolate )
        {
            m_DisplayPose = m_EvaluatedPose;
            m_DisplayComponent.Reset( m_DisplayPose.Size() );
        }
    }

    void Animator::PublishInterpolated( const float alpha )
    {
        if ( !m_InterpolateSkipped )
            return;
        if ( m_DisplayPose.Size() != m_EvaluatedPose.Size() )
        {
            m_DisplayPose = m_EvaluatedPose;
            m_DisplayComponent.Reset( m_DisplayPose.Size() );
        }
        const auto step = [this, alpha]( const std::size_t bone )
        { m_DisplayPose[bone] = alpha >= 1.0F ? m_EvaluatedPose[bone] : Blend( m_DisplayPose[bone], m_EvaluatedPose[bone], alpha ); };
        if ( m_RequiredOrder.empty() )
            for ( std::size_t bone = 0; bone < m_DisplayPose.Size(); ++bone )
                step( bone );
        else
            for ( const uint32_t bone : m_RequiredOrder )
                step( bone );
        m_DisplayComponent.Invalidate();
        m_DisplayComponent.WriteSkinningMatrices( m_Skinning.Matrices, m_RequiredOrder );
        ++m_SkinningRevision;
    }

    void Animator::SyncStages()
    {
        // REBUILT IN CANONICAL ORDER, NOT APPENDED TO. The previous shape — "if wanted and not present,
        // push_back" — was correct while exactly one optional stage existed, and silently wrong the moment a
        // second one did: adding a control before a layer produced [Source, Controls, Layers], which runs
        // the IK and then overwrites its bones with the layer's clip. Order is a property of the PIPELINE,
        // so it is written once, here, and membership stays data.
        m_Stages.clear();
        m_Stages.push_back( PoseStage::Source ); // unconditional: something has to produce a pose
        if ( m_PoseGraph )
        {
            m_Stages.push_back( PoseStage::Graph );
        }
        if ( !m_Controls.empty() )
        {
            m_Stages.push_back( PoseStage::Controls );
        }
        if ( m_Rig )
        {
            // LAST, and report 05 §658 is the argument — see the AttachRig comment in the header. The
            // membership test is the pointer and nothing else: `AttachRig` refuses a rig that drives no
            // bones, so a rig that is here is a rig that changes the pose.
            m_Stages.push_back( PoseStage::Rig );
        }
    }

    // ============================================================
    // Playback Update
    // ============================================================

    void Animator::UpdateBlendPlayback( ClipPlayback& playback, const float deltaTime )
    {
        // A state has no pins: its axes read the parameters it names, and it plays at rate 1 (the state's own
        // Speed is the machine's, as for a clip state).
        const std::vector<std::string>& names = playback.BlendSpace.AxisParameters;
        std::array<float, 2>            raw{ 0.0F, 0.0F };
        for ( size_t a = 0; a < names.size() && a < raw.size(); ++a )
            raw[a] = GraphParameter( names[a] );
        const size_t axes = std::min( playback.BlendSpace.Space.Axes.size(), raw.size() );
        AdvanceBlendRun( playback.BlendSpace.Space, playback.Blend, std::span<const float>( raw.data(), axes ),
                         1.0F, deltaTime, "a state machine state" );
        // The heaviest resolved sample is the clock the rest of the Animator reads (exit time, notifies).
        FollowHeaviestSample( playback.Blend, playback.Loop, playback.Clip, playback.Time, playback.StepFrom,
                              playback.StepWrapped, playback.StepBackward );
    }

    void Animator::SamplePlayback( const RigSampling& rig, const ClipPlayback& playback, LocalPose& out )
    {
        if ( !playback.IsBlendSpace() )
        {
            SampleClipPose( rig, playback.Clip, playback.Time, out );
            return;
        }
        // Every weighted sample at the shared phase of its own length, each blended toward by its share of the
        // running weight — SampleBlendSpace's walk, on this rig.
        out = rig.Rest;
        static_cast<void>(
             ForEachBlendSample( playback.Blend, playback.Loop,
                                 [&]( const ClipPlayback& sample, const float alpha )
                                 {
                                     if ( alpha >= 1.0F )
                                     {
                                         SampleClipPose( rig, sample.Clip, sample.Time, out );
                                         return;
                                     }
                                     SampleClipPose( rig, sample.Clip, sample.Time, m_SampleScratch );
                                     for ( uint32_t b = 0; b < out.Size() && b < m_SampleScratch.Size(); ++b )
                                         out[b] = Blend( out[b], m_SampleScratch[b], alpha );
                                 } ) );
    }

    void Animator::UpdatePlayback( ClipPlayback& playback, float deltaTime )
    {
        if ( playback.IsBlendSpace() )
        {
            UpdateBlendPlayback( playback, deltaTime );
            return;
        }
        if ( !playback.IsValid() )
        {
            return;
        }

        const FrameNumber duration = playback.Clip->DurationTicks();
        const FrameTime   previous = playback.Time;

        // THE WHOLE TICKS GO INTO AN INTEGER AND ONLY THE FRACTION OF ONE STAYS IN A FLOAT. What this
        // replaces was `playback.Time += deltaTime * tps` on a float, wrapped with `fmod`: the error grew
        // with the value already there, and every loop left a residue behind. Measured in
        // Tests/Engine/AnimationTimeModel: an hour of 1/60 s steps drifts less than one tick here and
        // hundreds of times further on the float.
        playback.Time = AdvanceFrameTime( playback.Time, deltaTime, playback.Clip->Sequence.TickRate );

        const bool looped = playback.Loop && duration.Value > 0 &&
                            ( playback.Time.Frame.Value >= duration.Value || playback.Time.Frame.Value < 0 );

        if ( playback.Loop )
        {
            playback.Time = WrapFrameTime( playback.Time, duration );
        }
        else if ( playback.Time.Frame.Value >= duration.Value )
        {
            playback.Time = FrameTime{ duration, 0.0F };
        }

        // The step, for this clock's notifies AFTER the evaluation has weighed it (StepBaseNotifies,
        // StepGraphNotifies). The covered interval is (previous, now]; on a loop wrap it is
        // (previous, duration) then [0, now]. One frame is assumed not to skip a whole loop, which holds for
        // real playback.
        playback.StepFrom     = previous;
        playback.StepWrapped  = looped;
        playback.StepBackward = deltaTime < 0.0F;
    }

    // ============================================================
    // Sampling
    // ============================================================

    void Animator::SampleClipPose( const RigSampling& rig, const AnimationClip* clip, const FrameTime time,
                                   LocalPose& out )
    {
        // THE REST OF THE RIG BEING SAMPLED, NOT OF THIS ANIMATOR, under every untracked bone. Under a
        // retarget that is the source rig's retarget pose (`SourceInitial`), the ONE value that makes an
        // untracked bone a no-op: the equation is `sourceCurrent * sourceInitial^-1`.
        out = rig.Rest;
        if ( clip == nullptr )
        {
            return;
        }
        const Timeline::BoneBindingTable& table = BindingFor( rig, *clip );
        // Only the drawn LOD's bones on this Animator's own rig (TargetSampling); a source rig is sampled whole.
        if ( auto sampled = Timeline::EvaluatePose( clip->Sequence, table, OnSequence( *clip, time ), out, rig.Required );
             !sampled.IsSuccess() )
        {
            // BindingFor rebuilt the table for this very revision, so this is a broken invariant, said loudly.
            LOG_ERROR( "[Animator] clip '{}' did not sample: {} The rig holds its rest pose.", clip->AnimationName,
                       sampled.GetError() );
            out = rig.Rest;
        }
    }

    void Animator::BlendedBasePose( const RigSampling& rig, LocalPose& out )
    {
        // Nested, as the fades were stacked: each one blends from the whole pose below it to its own clip
        // (UE: each active transition blends the previous result into its target).
        SamplePlayback( rig, m_Current, out );
        for ( const IncomingFade& fade : m_Fades )
        {
            if ( !fade.Playback.IsValid() )
                continue;
            const float alpha = fade.Alpha();

            // Exactly the endpoints at the endpoints. `Blend` slerps, and slerp at alpha 0 is not
            // bit-identical to its input for every quaternion; short-circuiting is what makes "a crossfade at
            // 0 is the source clip, bit for bit" a property that can be asserted rather than approximated.
            if ( alpha <= 0.0F )
                continue;

            SamplePlayback( rig, fade.Playback, m_BlendScratch );
            if ( alpha >= 1.0F )
            {
                out = m_BlendScratch;
                continue;
            }
            for ( uint32_t b = 0; b < out.Size(); ++b )
                out[b] = Blend( out[b], m_BlendScratch[b], alpha );
        }
        // Every clip's root sat at its own place; the pose shows it at the reference (all locked alike, so
        // locking once after the blend is locking each before it).
        LockRoot( rig, out );
    }

    uint32_t Animator::RootBoneOf( const Skeleton& rig )
    {
        const auto count = static_cast<uint32_t>( rig.GetBones().size() );
        for ( uint32_t b = 0; b < count; ++b )
            if ( rig.ResolveParent( b ) == Skeleton::NO_PARENT )
                return b;
        return Skeleton::NO_PARENT;
    }

    void Animator::LockRoot( const RigSampling& rig, LocalPose& pose ) const
    {
        if ( !LocksRoot( m_RootMotionMode ) )
            return;
        const uint32_t root = RootBoneOf( rig.Rig );
        if ( root < pose.Size() && root < rig.Rest.Size() )
            pose[root] = rig.Rest[root]; // UE ERootMotionRootLock::RefPose
    }

    BoneTransform Animator::ClockRootMotion( const RigSampling& rig, const ClipPlayback& playback )
    {
        if ( !m_ExtractingStep || m_RootMotionMode != RootMotionMode::RootMotionFromEverything ||
             !playback.IsValid() )
            return NoRootMotion();
        const uint32_t root = RootBoneOf( rig.Rig );
        if ( root >= rig.Rest.Size() )
            return NoRootMotion();

        const AnimationClip&              clip   = *playback.Clip;
        const Timeline::BoneBindingTable& table  = BindingFor( rig, clip );
        const BoneTransform&              lock   = rig.Rest[root];
        const auto                        rootAt = [&]( const FrameTime time )
        {
            BoneTransform at = lock; // a clip with no root track holds the root still
            if ( auto sampled = Timeline::EvaluateBone( clip.Sequence, table, OnSequence( clip, time ), root, at );
                 !sampled.IsSuccess() )
            {
                // BindingFor rebuilt the table for this very revision: a broken invariant, said loudly.
                LOG_ERROR( "[Animator] root motion of clip '{}' did not sample: {}", clip.AnimationName,
                           sampled.GetError() );
                return lock;
            }
            return at;
        };
        // A retargeted clip walks on the source rig: its strides are scaled to this one by pelvis height
        // (UE IK Retargeter's root motion follows the pelvis scale the same way).
        const float scale = &rig.Rig == &m_Skeleton ? 1.0F : m_Retarget->GetRetargeter().GetPelvisHeightScale();
        const auto  piece = [&]( const FrameTime from, const FrameTime to )
        { return RootMotionBetween( rootAt( from ), rootAt( to ), lock, scale ); };

        if ( !playback.StepWrapped )
            return piece( playback.StepFrom, playback.Time );
        // A WRAPPED LOOP IS TWO PIECES (UE ExtractRootMotion's loop split): to the end and from the start
        // going forward, to the start and from the end going backward.
        const FrameTime start{};
        const FrameTime end{ clip.DurationTicks(), 0.0F };
        if ( playback.StepBackward )
            return AccumulateRootMotion( piece( playback.StepFrom, start ), piece( end, playback.Time ) );
        return AccumulateRootMotion( piece( playback.StepFrom, end ), piece( start, playback.Time ) );
    }

    BoneTransform Animator::PlaybackRootMotion( const RigSampling& rig, const ClipPlayback& playback )
    {
        if ( !playback.IsBlendSpace() )
            return ClockRootMotion( rig, playback );
        // Every weighted sample's own stride over the run's step, blended by weight as the pose is.
        BoneTransform motion = NoRootMotion();
        static_cast<void>( ForEachBlendSample( playback.Blend, playback.Loop,
                                               [&]( const ClipPlayback& sample, const float alpha )
                                               {
                                                   const BoneTransform step = ClockRootMotion( rig, sample );
                                                   motion = alpha >= 1.0F ? step : Blend( motion, step, alpha );
                                               } ) );
        return motion;
    }

    BoneTransform Animator::BlendedBaseRootMotion( const RigSampling& rig )
    {
        // UE FRootMotionMovementParams::AccumulateWithBlend, nested as BlendedBasePose nests the poses: each
        // fade mixes its clip's movement over everything below it by its alpha.
        BoneTransform motion = PlaybackRootMotion( rig, m_Current );
        for ( const IncomingFade& fade : m_Fades )
        {
            const float alpha = fade.Alpha();
            if ( !fade.Playback.IsValid() || alpha <= 0.0F )
                continue;
            const BoneTransform incoming = PlaybackRootMotion( rig, fade.Playback );
            motion                       = alpha >= 1.0F ? incoming : Blend( motion, incoming, alpha );
        }
        return motion;
    }

    Animator::RigSampling Animator::TargetSampling() const
    {
        return RigSampling{ m_Skeleton, m_BindPose, m_TrackBinding, m_RequiredMask };
    }

    Animator::RigSampling Animator::SourceSampling() const
    {
        // The source rig's REST is its retarget pose applied to its bind pose — `SourceInitial`, which is
        // what the retarget equation measures a delta against. See SampleClipPose.
        return RigSampling{ m_Retarget->GetSourceSkeleton(), m_Retarget->GetRetargeter().GetSourceInitialPose(),
                            m_SourceTrackBinding };
    }

    const Timeline::BoneBindingTable& Animator::BindingFor( const RigSampling& rig, const AnimationClip& clip )
    {
        const Timeline::Sequence& sequence = clip.Sequence;
        ClipBinding&              binding  = rig.Binding[&clip];

        // REBUILT WHEN THE CLIP'S SEQUENCE HAS BEEN REPLACED, not only when the cache is empty — an unload +
        // reload leaves the clip at the same address holding a new track list. See ClipBinding.
        if ( binding.TracksData != sequence.Tracks.data() || binding.TrackCount != sequence.Tracks.size() ||
             binding.Table.Revision != sequence.Revision ||
             binding.Table.BoneOfTrack.size() != sequence.Tracks.size() )
        {
            binding.Table      = Timeline::BindBones( sequence, rig.Rig );
            binding.TracksData = sequence.Tracks.data();
            binding.TrackCount = sequence.Tracks.size();
        }
        return binding.Table;
    }

    const AnimationClip* Animator::ClockClip( const ClipPlayback& playback )
    {
        if ( playback.Clip != nullptr || !playback.IsBlendSpace() )
            return playback.Clip;
        for ( const AnimationClip* sample : playback.Blend.Clips )
            if ( sample != nullptr )
                return sample;
        return nullptr;
    }

    void Animator::ScrubTo( ClipPlayback& playback, const FrameTime time )
    {
        // A Blend Space is valid with no `Clip` until its first UpdateBlendPlayback (IsValid reads its samples),
        // so the clock is the one ClockClip names, never `Clip` unchecked.
        const AnimationClip* clock = ClockClip( playback );
        if ( clock == nullptr )
        {
            return;
        }
        const FrameNumber duration = clock->DurationTicks();
        if ( time.Frame.Value < 0 )
        {
            playback.Time = FrameTime{};
        }
        else if ( duration.Value > 0 && time.Frame.Value >= duration.Value )
        {
            playback.Time = playback.Loop ? WrapFrameTime( time, duration ) : FrameTime{ duration, 0.0F };
        }
        else
        {
            playback.Time = time;
        }
        // A Blend Space's clock is its shared normalized phase (UpdateBlendPlayback derives Time from it), so the
        // scrub moves the phase too - else its next update put Time back where it was.
        if ( playback.IsBlendSpace() )
        {
            playback.Clip       = clock;
            const double length = clock->DurationSeconds();
            playback.Blend.Phase =
                 length > 0.0 ? static_cast<float>( FrameTimeToSeconds( playback.Time, clock->Sequence.TickRate ) /
                                                    length )
                              : 0.0F;
        }
    }

    void Animator::StepBaseNotifies( const bool played )
    {
        // UE: the base is a sequence player like any other — its notifies enter the queue with its weight, and
        // an outgoing state of a crossfade is heard while (1 - alpha) x weight is above the trigger weight.
        float weight = 1.0F;
        if ( m_PoseGraph )
            weight = m_PoseGraph->BaseSource >= 0
                          ? m_PoseGraph->Instance.Weight( static_cast<size_t>( m_PoseGraph->BaseSource ) )
                          : 0.0F; // a graph that does not play the Source stage
        // Every layer of the stack by its own weight: an interrupted fade is still heard while it fades out.
        const std::vector<float> layers = BaseLayerWeights();
        StepNotifiesOf( m_Current, m_Current.StepFrom, m_Current.StepWrapped, played, m_Current.StepBackward,
                        layers[0] * weight > Graph::kNotifyTriggerWeight, m_ActiveStates, -1, -1 );
        for ( size_t i = 0; i < m_Fades.size(); ++i )
        {
            IncomingFade& fade = m_Fades[i];
            if ( fade.Playback.IsValid() || !fade.ActiveStates.empty() )
                StepNotifiesOf( fade.Playback, fade.Playback.StepFrom, fade.Playback.StepWrapped, played,
                                fade.Playback.StepBackward, layers[i + 1] * weight > Graph::kNotifyTriggerWeight,
                                fade.ActiveStates, -1, -1 );
        }
    }

    void Animator::StepNotifiesOf( const ClipPlayback& playback, const FrameTime previous, const bool wrapped,
                                   const bool played, const bool backward, const bool relevant,
                                   std::vector<ActiveNotifyState>& active, const int node, const int slot )
    {
        const auto event = [&]( const std::string& name, const NotifyEventKind kind )
        { m_NotifyEvents.push_back( NotifyEvent{ name, kind, node, slot } ); };

        m_Crossed.clear();
        m_StatesScratch.clear();
        if ( relevant && playback.IsValid() )
        {
            const AnimationClip&      clip     = *playback.Clip;
            const Timeline::Sequence& sequence = clip.Sequence;
            if ( played )
            {
                Timeline::TimeStep step{ OnSequence( clip, previous ), OnSequence( clip, playback.Time ),
                                         backward ? Timeline::PlayDirection::Backward
                                                  : Timeline::PlayDirection::Forward };
                step.Wrapped = wrapped;
                Timeline::CollectFired( sequence, step, m_Crossed );
            }
            ActiveStatesAt( sequence, OnSequence( clip, playback.Time ).AsTicks(), m_StatesScratch );
        }

        // Ends first: states active before the step and not after it.
        for ( const ActiveNotifyState& state : active )
        {
            if ( std::find( m_StatesScratch.begin(), m_StatesScratch.end(), state ) == m_StatesScratch.end() )
            {
                event( state.Name, NotifyEventKind::End );
            }
        }
        // A state begun AND ended inside the step (shorter than the frame) is active at neither end, and
        // still reports both — the crossing says so.
        for ( const Timeline::FiredEvent& fired : m_Crossed )
        {
            const Timeline::EventKey& key = *fired.Event.Key;
            if ( fired.Event.Edge != Timeline::EventEdge::Begin || Holds( active, key ) ||
                 Holds( m_StatesScratch, key ) )
            {
                continue;
            }
            const bool ended = std::any_of(
                 m_Crossed.begin(), m_Crossed.end(), [&key]( const Timeline::FiredEvent& other )
                 { return other.Event.Key == &key && other.Event.Edge == Timeline::EventEdge::End; } );
            if ( ended )
            {
                event( key.Name, NotifyEventKind::Begin );
                event( key.Name, NotifyEventKind::End );
            }
        }
        // Begins: states active after the step and not before it.
        for ( const ActiveNotifyState& state : m_StatesScratch )
        {
            if ( std::find( active.begin(), active.end(), state ) == active.end() )
            {
                event( state.Name, NotifyEventKind::Begin );
            }
        }
        // Instant notifies the step crossed, in firing order (either direction, as UE plays them).
        for ( const Timeline::FiredEvent& fired : m_Crossed )
        {
            if ( fired.Event.Edge == Timeline::EventEdge::Instant )
            {
                event( fired.Event.Key->Name, NotifyEventKind::Fire );
            }
        }
        active.swap( m_StatesScratch );
    }

    void Animator::StepGraphNotifies( const bool played )
    {
        // UE: every sequence player reports its notifies to the notify queue with its update weight, and the
        // queue keeps those above NotifyTriggerWeight — so a source blended out is silent and a layer at full
        // weight is heard, whatever node it sits under.
        if ( !m_PoseGraph )
            return;
        PoseGraphState& state = *m_PoseGraph;
        state.ActiveStates.resize( state.Sources.size() );
        for ( size_t node = 0; node < state.Sources.size(); ++node )
        {
            if ( static_cast<int>( node ) == state.BaseSource ) // the Source stage's: StepBaseNotifies
                continue;
            const ClipPlayback& source = state.Sources[node];
            if ( !source.IsValid() && state.ActiveStates[node].empty() )
                continue;
            StepNotifiesOf( source, source.StepFrom, source.StepWrapped, played, source.StepBackward,
                            state.Instance.Weight( node ) > Graph::kNotifyTriggerWeight, state.ActiveStates[node],
                            static_cast<int>( node ), -1 );
        }
        m_LinkedActiveStates.resize( m_LinkedSources.size() );
        for ( size_t slot = 0; slot < m_LinkedSources.size(); ++slot )
        {
            const Graph::PoseGraphInstance& layer = m_LinkedLayers.At( slot ).Instance;
            m_LinkedActiveStates[slot].resize( m_LinkedSources[slot].size() );
            for ( size_t node = 0; node < m_LinkedSources[slot].size(); ++node )
            {
                const ClipPlayback& source = m_LinkedSources[slot][node];
                if ( !source.IsValid() && m_LinkedActiveStates[slot][node].empty() )
                    continue;
                StepNotifiesOf( source, source.StepFrom, source.StepWrapped, played, source.StepBackward,
                                layer.Weight( node ) > Graph::kNotifyTriggerWeight,
                                m_LinkedActiveStates[slot][node], static_cast<int>( node ),
                                static_cast<int>( slot ) );
            }
        }
    }

    void Animator::RetireStates( std::vector<ActiveNotifyState>& active, const int node, const int slot )
    {
        for ( const ActiveNotifyState& state : active )
            m_NotifyEvents.push_back( NotifyEvent{ state.Name, NotifyEventKind::End, node, slot } );
        active.clear();
    }

    // ============================================================
    // Utilities
    // ============================================================

    const SkinningMatrices& Animator::GetPose() const
    {
        return m_Skinning;
    }

    glm::mat4 Animator::GetBoneModelMatrix( uint32_t boneIndex ) const
    {
        // const_cast because the conversion is a CACHE FILL, not a change of state: the pose this view is
        // over is fixed between Updates, so Get() is idempotent and observationally const. The alternative
        // is `mutable` on both of ComponentPose's vectors, which would spread the exception further.
        return const_cast<ComponentPose&>( m_Component ).Get( boneIndex );
    }

    bool Animator::IsFinished() const
    {
        if ( !m_Current.IsValid() )
        {
            return true;
        }

        if ( m_Current.Loop )
        {
            return false;
        }

        return m_Current.Time.Frame.Value >= m_Current.Clip->DurationTicks().Value;
    }

    void Animator::SetTick( FrameTime time )
    {
        // THE SCRUB MOVES EVERY PLAYER (UE: SetPosition reaches each sequence player): the base clip, the
        // incoming one of a crossfade, and every source clock of the pose graph and its linked layers. The
        // tick is on the current clip's grid (the project's, without one); every other player gets the same
        // moment in seconds on its own grid.
        const AnimationClip* currentClock = ClockClip( m_Current );
        const FrameRate      rate = currentClock != nullptr ? currentClock->Sequence.TickRate : PROJECT_TICK_RATE;
        const double         seconds = FrameTimeToSeconds( time, rate );
        const auto           scrub   = [seconds]( ClipPlayback& playback )
        {
            if ( const AnimationClip* clock = ClockClip( playback ) )
            {
                ScrubTo( playback, SecondsToFrameTime( seconds, clock->Sequence.TickRate ) );
            }
        };

        ScrubTo( m_Current, time );
        for ( IncomingFade& fade : m_Fades )
            scrub( fade.Playback );
        if ( m_PoseGraph )
            for ( auto& source : m_PoseGraph->Sources )
                scrub( source );
        for ( auto& layer : m_LinkedSources )
            for ( auto& source : layer )
                scrub( source );
        EvaluatePipeline();
        StepBaseNotifies( false );
        StepGraphNotifies( false );
    }

    void Animator::SetTime( float time )
    {
        // Through the tick, and onto the NEAREST one: a scrub arrives as a real number that has been
        // through a pixel position and a division, so flooring it would put a user who dragged onto a key
        // one tick before it. See TimeModel.hpp.
        const AnimationClip* clock   = ClockClip( m_Current );
        const FrameRate      rate    = clock != nullptr ? clock->Sequence.TickRate : PROJECT_TICK_RATE;
        const FrameTime   asTicks = SecondsToFrameTime( static_cast<double>( time ), rate );
        const FrameNumber nearest = NearestTick( asTicks );
        SetTick( FrameTime{ nearest, 0.0F } );
    }

    void Animator::SetLoop( bool loop )
    {
        if ( m_Current.IsValid() )
        {
            m_Current.Loop = loop;
        }

        for ( IncomingFade& fade : m_Fades )
            if ( fade.Playback.IsValid() )
                fade.Playback.Loop = loop;
    }

    const AnimationClip* Animator::GetCurrentClip() const
    {
        // While cross-fading, report the TARGET clip: callers (e.g. AnimationECSSystem's name re-sync) treat
        // this as "the clip that should be playing", and seeing the old clip would make them Play() it and
        // snap-cancel the blend.
        return !m_Fades.empty() && m_Fades.back().Playback.IsValid() ? m_Fades.back().Playback.Clip
                                                                     : m_Current.Clip;
    }

    // ============================================================
    // Layers
    // ============================================================

    Common::BoolResultStr Animator::SetPoseGraph( const Graph::AnimGraph& graph )
    {
        PoseGraphState state;
        if ( const auto bound = state.Instance.Bind( graph, m_Skeleton ); !bound )
        {
            ClearPoseGraph();
            return bound;
        }
        const Graph::AnimGraph& bound = state.Instance.Graph();
        state.Sources.resize( bound.Nodes.size() );
        state.BlendSpaces.resize( bound.Nodes.size() );
        if ( const Graph::PoseNode* base = Graph::BaseSourceNode( bound ) )
            state.BaseSource = static_cast<int>( base - bound.Nodes.data() );
        // A graph re-set with the same nodes keeps each source's clock: the graph is re-sent after every edit, and
        // a source that restarted on each edit would stutter while authored. (The parameters are the Animator's,
        // m_GraphParameters, and outlive any pose graph.)
        if ( m_PoseGraph && m_PoseGraph->Sources.size() == state.Sources.size() )
        {
            state.Sources      = std::move( m_PoseGraph->Sources );
            state.BlendSpaces  = std::move( m_PoseGraph->BlendSpaces );
            state.ActiveStates = std::move( m_PoseGraph->ActiveStates );
        }
        else if ( m_PoseGraph )
            for ( size_t node = 0; node < m_PoseGraph->ActiveStates.size(); ++node )
                RetireStates( m_PoseGraph->ActiveStates[node], static_cast<int>( node ), -1 );
        // A run is parallel to its node's samples: a node that is no blend space has none, and a row edited
        // to another length restarts its weights (a kept phase is still the cycle's position).
        for ( size_t node = 0; node < bound.Nodes.size(); ++node )
        {
            BlendSpaceRun& run = state.BlendSpaces[node];
            if ( !bound.Nodes[node].BlendSpace )
            {
                run = BlendSpaceRun{};
                continue;
            }
            // The triangulation is the setup's, re-derived on every set: an edit that moved a sample re-shapes it.
            run.Triangulation    = Graph::TriangulateBlendSpace( *bound.Nodes[node].BlendSpace );
            const size_t samples = bound.Nodes[node].BlendSpace->Samples.size();
            if ( run.Clips.size() != samples )
            {
                run.Clips.assign( samples, nullptr );
                run.Weights.clear();
                run.Started = false;
            }
        }
        m_PoseGraph = std::move( state );
        // The layers the graph implements itself answer its interfaces until something is linked (UE's
        // default linked layers); every earlier link was made against the previous graph and is undone.
        if ( auto defaults = m_LinkedLayers.SetDefaults( m_PoseGraph->Instance.Graph(), m_Skeleton ); !defaults )
        {
            ClearPoseGraph();
            return defaults;
        }
        RebuildLinkedClocks();
        SyncStages();
        return Common::MakeSuccess( true );
    }

    void Animator::SetPoseGraphSource( size_t node, const AnimationClip& clip, bool loop )
    {
        if ( !m_PoseGraph || node >= m_PoseGraph->Sources.size() ||
             static_cast<int>( node ) == m_PoseGraph->BaseSource ||
             !Graph::IsSourceKind(
                  static_cast<Graph::PoseNodeKind>( m_PoseGraph->Instance.Graph().Nodes[node].Kind ) ) )
            return;
        ClipPlayback& playback = m_PoseGraph->Sources[node];
        if ( playback.Clip != &clip )
            playback = { .Clip = &clip, .Time = FrameTime{}, .Loop = loop, .StepFrom = FrameTime{} };
        playback.Loop = loop;
    }

    void Animator::SetPoseGraphBlendSample( size_t node, size_t sample, const AnimationClip& clip )
    {
        if ( !m_PoseGraph || node >= m_PoseGraph->BlendSpaces.size() )
            return;
        BlendSpaceRun& run = m_PoseGraph->BlendSpaces[node];
        if ( sample < run.Clips.size() )
            run.Clips[sample] = &clip;
    }

    void Animator::SetGraphParameter( std::string_view name, float value )
    {
        for ( GraphParameterValue& live : m_GraphParameters )
            if ( live.Name == name )
            {
                live.Value = value;
                return;
            }
        m_GraphParameters.push_back( { .Name = std::string( name ), .Value = value } );
    }

    Common::BoolResultStr Animator::LinkLayers( uint64_t implementationId, const Graph::AnimGraph& implementation )
    {
        if ( !m_PoseGraph )
            return Common::MakeError<bool>(
                 std::format( "cannot link the layers of '{}': the character has no pose graph to call them",
                              implementation.Name ) );
        if ( auto linked = m_LinkedLayers.Link( m_PoseGraph->Instance.Graph(), implementationId, implementation,
                                                m_Skeleton );
             !linked )
            return linked;
        RebuildLinkedClocks();
        return Common::MakeSuccess( true );
    }

    void Animator::UnlinkLayers( uint64_t implementationId )
    {
        m_LinkedLayers.Unlink( implementationId );
        RebuildLinkedClocks();
    }

    void Animator::ClearLinkedLayers()
    {
        m_LinkedLayers.Clear();
        RebuildLinkedClocks();
    }

    Graph::Evaluator* Animator::GetLinkedLayerMachines( size_t slot )
    {
        if ( slot >= m_LinkedLayers.Layers().size() )
            return nullptr;
        auto& machines = m_LinkedLayers.At( slot ).Machines;
        return machines ? &*machines : nullptr;
    }

    float Animator::GetLinkedLayerSourceFraction( size_t slot, size_t node ) const
    {
        if ( slot >= m_LinkedSources.size() || node >= m_LinkedSources[slot].size() )
            return 0.0F;
        const ClipPlayback& playback = m_LinkedSources[slot][node];
        if ( playback.Clip == nullptr )
            return 0.0F;
        const double duration = playback.Clip->DurationSeconds();
        return duration > 1e-4
                    ? static_cast<float>( FrameTimeToSeconds( playback.Time, playback.Clip->Sequence.TickRate ) /
                                          duration )
                    : 0.0F;
    }

    void Animator::RebuildLinkedClocks()
    {
        // The slots moved; every layer's clocks start fresh with its link (the ECS re-feeds the clips each tick),
        // and the states their clips held end under the slots they were reported from.
        for ( size_t slot = 0; slot < m_LinkedActiveStates.size(); ++slot )
            for ( size_t node = 0; node < m_LinkedActiveStates[slot].size(); ++node )
                RetireStates( m_LinkedActiveStates[slot][node], static_cast<int>( node ),
                              static_cast<int>( slot ) );
        m_LinkedActiveStates.assign( m_LinkedLayers.Layers().size(), {} );
        m_LinkedSources.assign( m_LinkedLayers.Layers().size(), {} );
        for ( size_t slot = 0; slot < m_LinkedSources.size(); ++slot )
            m_LinkedSources[slot].resize( m_LinkedLayers.Layers()[slot].Instance.Graph().Nodes.size() );
    }

    void Animator::SetLinkedLayerSource( size_t slot, size_t node, const AnimationClip& clip, bool loop )
    {
        if ( slot >= m_LinkedSources.size() || node >= m_LinkedSources[slot].size() )
            return;
        ClipPlayback& playback = m_LinkedSources[slot][node];
        if ( playback.Clip != &clip )
            playback = { .Clip = &clip, .Time = FrameTime{}, .Loop = loop, .StepFrom = FrameTime{} };
        playback.Loop = loop;
    }

    void Animator::ClearPoseGraph()
    {
        if ( m_PoseGraph )
            for ( size_t node = 0; node < m_PoseGraph->ActiveStates.size(); ++node )
                RetireStates( m_PoseGraph->ActiveStates[node], static_cast<int>( node ), -1 );
        m_PoseGraph.reset();
        // No host, no defaults and nothing its interfaces could be linked against.
        (void)m_LinkedLayers.SetDefaults( Graph::AnimGraph{}, m_Skeleton );
        RebuildLinkedClocks();
        SyncStages();
    }

    // ============================================================
    // Skeletal controls
    // ============================================================

    int Animator::AddControl( std::unique_ptr<BoneControl> control )
    {
        if ( !control )
        {
            LOG_ERROR( "[Animator] refusing to add a null skeletal control to rig (signature {}).",
                       m_Skeleton.GetSignature() );
            return -1;
        }

        // RESOLVED ON THE WAY IN, ONCE. Report 03 §1.4: bone references are cached when the rig changes and
        // never looked up inside a solve. The Animator's rig is fixed for its lifetime, so "the rig changed"
        // is "the control joined a rig", and this is that moment. A failure is reported and the control is
        // still added — it will refuse every solve and say why, which an artist can see and fix, whereas a
        // silently dropped control is a feature that stopped existing.
        if ( const auto resolved = control->Resolve( m_Skeleton ); !resolved.IsSuccess() )
        {
            LOG_ERROR( "[Animator] skeletal control '{}' on rig (signature {}): {}",
                       ToString( control->GetKind() ), m_Skeleton.GetSignature(), resolved.GetError() );
        }

        m_Controls.push_back( std::move( control ) );
        SyncStages();
        return static_cast<int>( m_Controls.size() ) - 1;
    }

    void Animator::RemoveControl( int index )
    {
        if ( index >= 0 && index < static_cast<int>( m_Controls.size() ) )
        {
            m_Controls.erase( m_Controls.begin() + index );
            SyncStages();
        }
    }

    void Animator::ClearControls()
    {
        m_Controls.clear();
        SyncStages();
    }

    BoneControl* Animator::GetControl( int index )
    {
        return ( index >= 0 && index < static_cast<int>( m_Controls.size() ) ) ? m_Controls[index].get() : nullptr;
    }

    // ============================================================
    // The control rig
    // ============================================================

    Common::BoolResultStr Animator::AttachRig( std::unique_ptr<ControlRigStage> rig )
    {
        if ( !rig )
        {
            return Common::MakeFormattedError<bool>(
                 "refusing to attach a null control rig to rig (signature {}).", m_Skeleton.GetSignature() );
        }

        if ( rig->GetDrives().empty() )
        {
            // REFUSED, NOT ACCEPTED-AND-IGNORED, and this is the load-bearing half of T5.4's proof. A rig
            // with no drives would join `m_Stages` as a stage that cannot change the pose — and a stage
            // that silently does nothing passes every assertion a stage that works passes. The suite's
            // positive control ("the pose came out changed, by this much") only means something because
            // this state cannot be reached.
            return Common::MakeFormattedError<bool>(
                 "refusing to attach a control rig that drives no bones to rig (signature {}): it would be "
                 "a pipeline stage that cannot change the pose. Call ControlRigStage::SetDrives first.",
                 m_Skeleton.GetSignature() );
        }

        m_Rig = std::move( rig );
        SyncStages();
        return Common::MakeSuccess( true );
    }

    void Animator::DetachRig()
    {
        m_Rig.reset();
        SyncStages();
    }

    ControlRigStage* Animator::GetRig()
    {
        return m_Rig.get();
    }

    const ControlRigStage* Animator::GetRig() const
    {
        return m_Rig.get();
    }

    // ============================================================
    // The retarget
    // ============================================================

    Common::BoolResultStr Animator::AttachRetarget( std::unique_ptr<Retarget::RetargetSource> retarget )
    {
        if ( !retarget )
        {
            return Common::MakeFormattedError<bool>( "refusing to attach a null retarget to rig (signature {}).",
                                                     m_Skeleton.GetSignature() );
        }

        // REFUSED, NOT ACCEPTED-AND-IGNORED, and it is the same load-bearing refusal `AttachRig` makes one
        // function up. `RetargetSource::Create` has already refused a setup that could only ever emit the
        // rest pose; this one refuses the other way a retarget can be a no-op that passes every assertion a
        // working one passes — being built for a rig that is not the one it would run on. The suite's
        // positive control ("the skinning matrices came out different, by this much") only means something
        // because these two states cannot be reached.
        if ( retarget->GetRetargeter().GetTargetInitialPose().Size() != m_Skeleton.GetBones().size() )
        {
            return Common::MakeFormattedError<bool>(
                 "refusing to attach a retarget built for a {}-bone target to this {}-bone rig (signature "
                 "{}).",
                 retarget->GetRetargeter().GetTargetInitialPose().Size(), m_Skeleton.GetBones().size(),
                 m_Skeleton.GetSignature() );
        }

        m_Retarget = std::move( retarget );

        // THE BINDING CACHE IS BUILT AGAINST A RIG, and this call is exactly the moment that rig changes.
        // Keeping it would hand the next frame a bone -> track map built for the other skeleton's bone
        // order: every clip would play, on the wrong bones, with nothing refused.
        m_SourceTrackBinding.clear();
        return Common::MakeSuccess( true );
    }

    void Animator::DetachRetarget()
    {
        m_Retarget.reset();
        m_SourceTrackBinding.clear();
    }

    Retarget::RetargetSource* Animator::GetRetarget()
    {
        return m_Retarget.get();
    }

    const Retarget::RetargetSource* Animator::GetRetarget() const
    {
        return m_Retarget.get();
    }

} // namespace Desert::Animation
