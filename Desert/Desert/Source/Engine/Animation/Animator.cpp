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
#include <format>

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
        void ActiveStatesAt( const Timeline::Sequence& sequence, const double at, std::vector<ActiveNotifyState>& out )
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
                    const auto* events  = channel != nullptr ? std::get_if<Timeline::EventChannel>( channel ) : nullptr;
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
            return std::any_of( states.begin(), states.end(), [&key]( const ActiveNotifyState& state )
                                { return state.Name == key.Name && state.Tick == key.Tick && state.Duration == key.Duration; } );
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
         : m_Skeleton( skeleton ), m_Component( skeleton, m_EvaluatedPose )
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
        SampleClipPose( TargetSampling(), &clip, time, m_AuthoringPose );
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

    // ============================================================
    // Play / CrossFade
    // ============================================================

    void Animator::RetireNotifyStates()
    {
        for ( const auto& state : m_ActiveStates )
        {
            m_NotifyEvents.push_back( NotifyEvent{ state.Name, NotifyEventKind::End } );
        }
        m_ActiveStates.clear();
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
                     Timeline::EvaluateTrack( track, OnSequence( *playback.Clip, playback.Time ), sequence.TickRate,
                                              value ) )
                {
                    return std::get<float>( value );
                }
            }
            return std::nullopt;
        };

        const std::optional<float> current = valueOf( m_Current, name );
        if ( !m_IsBlending )
        {
            return current;
        }
        const std::optional<float> next = valueOf( m_Next, name );
        if ( !current && !next )
        {
            return std::nullopt;
        }
        return glm::mix( current.value_or( 0.0F ), next.value_or( 0.0F ), BlendAlpha() );
    }

    void Animator::Play( const AnimationClip& clip, bool loop )
    {
        RetireNotifyStates();
        m_Current    = { &clip, FrameTime{}, loop };
        m_Next       = {};
        m_IsBlending = false;
    }

    void Animator::CrossFade( const AnimationClip& clip, float duration, bool loop )
    {
        if ( m_Current.Clip == &clip )
        {
            return;
        }

        m_Next = { &clip, FrameTime{}, loop };

        m_IsBlending    = true;
        m_BlendTime     = 0.0F;
        // A crossfade of zero would divide by zero in BlendAlpha; the floor is small enough that the
        // first Update already reaches alpha 1, so a zero-length blend behaves as an instant cut.
        constexpr float MIN_BLEND_SECONDS = 0.0001F;
        m_BlendDuration                   = glm::max( duration, MIN_BLEND_SECONDS );
    }

    void Animator::Stop()
    {
        RetireNotifyStates();
        m_Current    = {};
        m_Next       = {};
        m_IsBlending = false;
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
        // unresolved track falls back to it, bone by bone) and the controls correct it from there.
        if ( !m_Current.IsValid() && m_Controls.empty() )
        {
            return;
        }

        UpdatePlayback( m_Current, deltaTime );

        if ( m_IsBlending && m_Next.IsValid() )
        {
            UpdatePlayback( m_Next, deltaTime );
            m_BlendTime += deltaTime;
        }

        if ( m_PoseGraph )
            for ( auto& source : m_PoseGraph->Sources )
                if ( source.IsValid() )
                    UpdatePlayback( source, deltaTime );
        for ( auto& layer : m_LinkedSources )
            for ( auto& source : layer )
                if ( source.IsValid() )
                    UpdatePlayback( source, deltaTime );

        EvaluatePipeline();
        // After the evaluation, which is what weighed every source this tick.
        StepGraphNotifies( true );

        // Retire the blend AFTER evaluating, so the frame that reaches alpha 1 renders the target clip
        // rather than a pose built from a blend that has already been thrown away.
        if ( m_IsBlending && m_Next.IsValid() && BlendAlpha() >= 1.0F )
        {
            // The outgoing clip's states end here; the incoming clip's begin on its next step, by the
            // difference of the (now empty) active set against its playhead.
            RetireNotifyStates();
            m_Current    = m_Next;
            m_Next       = {};
            m_IsBlending = false;
        }
    }

    void Animator::EvaluatePipeline()
    {
        if ( m_EvaluatedPose.Size() != m_Skeleton.GetBones().size() )
        {
            m_EvaluatedPose = m_BindPose;
            m_Component.Reset( m_EvaluatedPose.Size() );
        }

        for ( const PoseStage stage : m_Stages )
        {
            switch ( stage )
            {
                case PoseStage::Source:
                    EvaluateSource( m_EvaluatedPose );
                    break;
                case PoseStage::Graph:
                    EvaluateGraph( m_EvaluatedPose );
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
            BlendedBasePose( TargetSampling(), pose );
            return;
        }

        // THE CLIPS ARE ON ANOTHER RIG. Sample the whole base pose there — through the same
        // `BlendedBasePose`, so a crossfade is blended once and identically on either rig — and let the
        // retargeter say what it is on this one.
        const RigSampling rig        = SourceSampling();
        LocalPose&        sourcePose = m_Retarget->SourceScratch();
        BlendedBasePose( rig, sourcePose );

        // A REFUSAL LEAVES `pose` AS THE LAST FRAME LEFT IT, and `Run` has already said why, once per
        // distinct message. Falling back to the bind pose instead would look like a character snapping to
        // rest for one frame — which reads as a physics glitch rather than as the configuration error it
        // is — and re-running the stage cannot help, because every refusal `Retarget` makes is structural.
        static_cast<void>( m_Retarget->Run( m_Skeleton, sourcePose, pose ) );
    }

    void Animator::EvaluateGraph( LocalPose& pose )
    {
        PoseGraphState&    state = *m_PoseGraph;
        const size_t       n     = m_Skeleton.GetBones().size();
        const Graph::AnimGraph& graph = state.Instance.Graph();

        Graph::PoseGraphSources sources;
        const auto              sampleClock = [&]( const ClipPlayback& playback, Graph::GraphPose& out )
        {
            if ( !playback.IsValid() )
            {
                out.Pose = m_BindPose; // a source with no clip yet (still loading) stands in the bind pose
                return;
            }
            // ITS CURVES TRAVEL WITH ITS POSE (UE FPoseContext::Curve): the clip's Float tracks, named by
            // Property, at the playhead — what the nodes blend.
            const Timeline::Sequence& sequence = playback.Clip->Sequence;
            for ( const Timeline::Track& track : sequence.Tracks )
            {
                Timeline::EvaluatedValue value;
                if ( track.Kind == Timeline::TrackKind::Float &&
                     Timeline::EvaluateTrack( track, OnSequence( *playback.Clip, playback.Time ), sequence.TickRate,
                                              value ) )
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
                // THE BIND POSE, NOT THE SOURCE RIG'S TRANSFORMS: `Run` has reported the reason, and source-rig
                // local transforms written onto target bones are the proportion defect a retarget removes.
                if ( !m_Retarget->Run( m_Skeleton, sourcePose, out.Pose ) )
                    out.Pose = m_BindPose;
                return;
            }
            SampleClipPose( TargetSampling(), playback.Clip, playback.Time, out.Pose );
        };
        sources.Sample = [&]( size_t node, Graph::GraphPose& out )
        {
            // THE BASE SOURCE IS THE SOURCE STAGE: its crossfade, notifies and root motion are the Animator's.
            if ( static_cast<int>( node ) == state.BaseSource )
            {
                out.Pose = pose;
                for ( const ClipPlayback* player : { &m_Current, &m_Next } )
                    if ( player->IsValid() && ( player == &m_Current || m_IsBlending ) )
                        for ( const Timeline::Track& track : player->Clip->Sequence.Tracks )
                            if ( track.Kind == Timeline::TrackKind::Float )
                                if ( const auto value = BaseCurveValue( track.Property ) )
                                    AddCurve( out, track.Property, *value );
                return;
            }
            sampleClock( state.Sources[node], out );
        };
        // A linked layer's sequence players run on their own clocks, sampled exactly as the host's are.
        sources.SampleLinked = [&]( size_t slot, size_t node, Graph::GraphPose& out )
        { sampleClock( m_LinkedSources[slot][node], out ); };
        sources.Linked    = &m_LinkedLayers;
        sources.Parameter = [&]( const std::string& name )
        {
            for ( size_t p = 0; p < graph.Parameters.size(); ++p )
                if ( graph.Parameters[p].Name == name )
                    return state.Parameters[p];
            return 0.0F; // PlanPoseGraph refused a pin bound to an undeclared parameter
        };
        // THE REST POSE THE ADDITIVE WAS AUTHORED AGAINST, ON THIS RIG: under a retarget the additive clip is
        // on the source rig, so its difference is taken from the source rest retargeted the same way.
        sources.AdditiveReference = m_Retarget ? &m_Retarget->GetRetargetedRest() : &m_BindPose;

        state.Instance.Evaluate( sources, m_Skeleton, state.Out );
        if ( state.Out.Pose.Size() == n )
            pose = state.Out.Pose;
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
        m_Component.WriteSkinningMatrices( m_Skinning.Matrices );
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

    void Animator::UpdatePlayback( ClipPlayback& playback, float deltaTime )
    {
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

        // The CURRENT clip's notifies: instant ones crossed this frame fire (forward playback only), and
        // states Begin / End as the playhead enters / leaves them. The covered interval is (previous, now];
        // on a loop wrap it is (previous, duration) then [0, now]. One frame is assumed not to skip a whole
        // loop, which holds for real playback.
        playback.StepFrom     = previous;
        playback.StepWrapped  = looped;
        playback.StepBackward = deltaTime < 0.0F;
        if ( &playback == &m_Current )
        {
            StepNotifies( previous, looped, true, deltaTime < 0.0F );
        }
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
        if ( auto sampled = Timeline::EvaluatePose( clip->Sequence, table, OnSequence( *clip, time ), out );
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
        SampleClipPose( rig, m_Current.Clip, m_Current.Time, out );
        if ( !m_IsBlending || !m_Next.IsValid() )
        {
            return;
        }

        const float alpha = BlendAlpha();

        // Exactly the endpoints at the endpoints. `Blend` slerps, and slerp at alpha 0 is not bit-identical
        // to its input for every quaternion; short-circuiting is what makes "a crossfade at 0 is the source
        // clip, bit for bit" a property that can be asserted rather than approximated.
        if ( alpha <= 0.0F )
        {
            return;
        }

        SampleClipPose( rig, m_Next.Clip, m_Next.Time, m_BlendScratch );
        if ( alpha >= 1.0F )
        {
            out = m_BlendScratch;
            return;
        }
        for ( uint32_t b = 0; b < out.Size(); ++b )
        {
            out[b] = Blend( out[b], m_BlendScratch[b], alpha );
        }
    }

    Animator::RigSampling Animator::TargetSampling() const
    {
        return RigSampling{ m_Skeleton, m_BindPose, m_TrackBinding };
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
             binding.Table.Revision != sequence.Revision || binding.Table.BoneOfTrack.size() != sequence.Tracks.size() )
        {
            binding.Table      = Timeline::BindBones( sequence, rig.Rig );
            binding.TracksData = sequence.Tracks.data();
            binding.TrackCount = sequence.Tracks.size();
        }
        return binding.Table;
    }

    void Animator::ScrubTo( ClipPlayback& playback, const FrameTime time )
    {
        if ( !playback.IsValid() )
        {
            return;
        }
        const FrameNumber duration = playback.Clip->DurationTicks();
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
    }

    void Animator::StepNotifies( const FrameTime previous, const bool wrapped, const bool played, const bool backward )
    {
        StepNotifiesOf( m_Current, previous, wrapped, played, backward, true, m_ActiveStates, -1, -1 );
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
            const bool ended = std::any_of( m_Crossed.begin(), m_Crossed.end(), [&key]( const Timeline::FiredEvent& other )
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
            if ( static_cast<int>( node ) == state.BaseSource ) // the Source stage's: StepNotifies
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
                                layer.Weight( node ) > Graph::kNotifyTriggerWeight, m_LinkedActiveStates[slot][node],
                                static_cast<int>( node ), static_cast<int>( slot ) );
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
        const FrameRate rate    = m_Current.IsValid() ? m_Current.Clip->Sequence.TickRate : PROJECT_TICK_RATE;
        const double    seconds = FrameTimeToSeconds( time, rate );
        const auto      scrub   = [seconds]( ClipPlayback& playback )
        {
            if ( playback.IsValid() )
            {
                ScrubTo( playback, SecondsToFrameTime( seconds, playback.Clip->Sequence.TickRate ) );
            }
        };

        if ( m_Current.IsValid() )
        {
            const FrameTime before = m_Current.Time;
            ScrubTo( m_Current, time );
            StepNotifies( before, false, false, false );
        }
        scrub( m_Next );
        if ( m_PoseGraph )
            for ( auto& source : m_PoseGraph->Sources )
                scrub( source );
        for ( auto& layer : m_LinkedSources )
            for ( auto& source : layer )
                scrub( source );
        EvaluatePipeline();
        StepGraphNotifies( false );
    }

    void Animator::SetTime( float time )
    {
        // Through the tick, and onto the NEAREST one: a scrub arrives as a real number that has been
        // through a pixel position and a division, so flooring it would put a user who dragged onto a key
        // one tick before it. See TimeModel.hpp.
        const FrameRate   rate    = m_Current.IsValid() ? m_Current.Clip->Sequence.TickRate : PROJECT_TICK_RATE;
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

        if ( m_IsBlending && m_Next.IsValid() )
        {
            m_Next.Loop = loop;
        }
    }

    const AnimationClip* Animator::GetCurrentClip() const
    {
        // While cross-fading, report the TARGET clip: callers (e.g. AnimationECSSystem's name re-sync) treat
        // this as "the clip that should be playing", and seeing the old clip would make them Play() it and
        // snap-cancel the blend.
        return ( m_IsBlending && m_Next.IsValid() ) ? m_Next.Clip : m_Current.Clip;
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
        state.Parameters.resize( bound.Parameters.size() );
        for ( size_t p = 0; p < bound.Parameters.size(); ++p )
            state.Parameters[p] = bound.Parameters[p].Default;
        if ( const Graph::PoseNode* base = Graph::BaseSourceNode( bound ) )
            state.BaseSource = static_cast<int>( base - bound.Nodes.data() );
        // A graph re-set with the same nodes keeps each source's clock and the parameters: the graph is
        // re-sent after every edit, and a source that restarted on each edit would stutter while authored.
        if ( m_PoseGraph && m_PoseGraph->Sources.size() == state.Sources.size() )
        {
            state.Sources      = std::move( m_PoseGraph->Sources );
            state.ActiveStates = std::move( m_PoseGraph->ActiveStates );
        }
        else if ( m_PoseGraph )
            for ( size_t node = 0; node < m_PoseGraph->ActiveStates.size(); ++node )
                RetireStates( m_PoseGraph->ActiveStates[node], static_cast<int>( node ), -1 );
        if ( m_PoseGraph && m_PoseGraph->Parameters.size() == state.Parameters.size() )
            state.Parameters = std::move( m_PoseGraph->Parameters );
        m_PoseGraph = std::move( state );
        SyncStages();
        return Common::MakeSuccess( true );
    }

    void Animator::SetPoseGraphSource( size_t node, const AnimationClip& clip, bool loop )
    {
        if ( !m_PoseGraph || node >= m_PoseGraph->Sources.size() || static_cast<int>( node ) == m_PoseGraph->BaseSource ||
             !Graph::IsSourceKind( static_cast<Graph::PoseNodeKind>( m_PoseGraph->Instance.Graph().Nodes[node].Kind ) ) )
            return;
        ClipPlayback& playback = m_PoseGraph->Sources[node];
        if ( playback.Clip != &clip )
            playback = { &clip, FrameTime{}, loop };
        playback.Loop = loop;
    }

    void Animator::SetPoseGraphParameter( std::string_view name, float value )
    {
        if ( !m_PoseGraph )
            return;
        const auto& parameters = m_PoseGraph->Instance.Graph().Parameters;
        for ( size_t p = 0; p < parameters.size(); ++p )
            if ( parameters[p].Name == name )
                m_PoseGraph->Parameters[p] = value;
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
        return duration > 1e-4 ? static_cast<float>( FrameTimeToSeconds( playback.Time, playback.Clip->Sequence.TickRate ) /
                                                     duration )
                               : 0.0F;
    }

    void Animator::RebuildLinkedClocks()
    {
        // The slots moved; every layer's clocks start fresh with its link (the ECS re-feeds the clips each tick),
        // and the states their clips held end under the slots they were reported from.
        for ( size_t slot = 0; slot < m_LinkedActiveStates.size(); ++slot )
            for ( size_t node = 0; node < m_LinkedActiveStates[slot].size(); ++node )
                RetireStates( m_LinkedActiveStates[slot][node], static_cast<int>( node ), static_cast<int>( slot ) );
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
            playback = { &clip, FrameTime{}, loop };
        playback.Loop = loop;
    }

    void Animator::ClearPoseGraph()
    {
        if ( m_PoseGraph )
            for ( size_t node = 0; node < m_PoseGraph->ActiveStates.size(); ++node )
                RetireStates( m_PoseGraph->ActiveStates[node], static_cast<int>( node ), -1 );
        m_PoseGraph.reset();
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
