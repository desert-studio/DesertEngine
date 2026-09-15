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

namespace Desert::Animation
{
    const char* ToString( PoseStage stage )
    {
        switch ( stage )
        {
            case PoseStage::Source:
                return "Source";
            case PoseStage::Layers:
                return "Layers";
            case PoseStage::Controls:
                return "Controls";
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

    void Animator::SampleClipIntoLocalPose( const AnimationClip& clip, FrameTime time )
    {
        const size_t n = m_Skeleton.GetBones().size();
        if ( m_AuthoringPose.Size() != n )
            m_AuthoringPose = m_BindPose;
        for ( uint32_t i = 0; i < n; ++i )
            m_AuthoringPose[i] = SampleLocalTransform( &clip, i, time );
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

    void Animator::Play( const AnimationClip& clip, bool loop )
    {
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

        for ( auto& layer : m_Layers )
            if ( layer.Playback.IsValid() )
                UpdatePlayback( layer.Playback, deltaTime );

        EvaluatePipeline();

        // Retire the blend AFTER evaluating, so the frame that reaches alpha 1 renders the target clip
        // rather than a pose built from a blend that has already been thrown away.
        if ( m_IsBlending && m_Next.IsValid() && BlendAlpha() >= 1.0F )
        {
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
                case PoseStage::Layers:
                    EvaluateLayers( m_EvaluatedPose );
                    break;
                case PoseStage::Controls:
                    EvaluateControls( m_EvaluatedPose );
                    break;
            }
        }

        PublishPose();
    }

    void Animator::EvaluateSource( LocalPose& pose ) const
    {
        for ( uint32_t i = 0; i < pose.Size(); ++i )
            pose[i] = BlendedBaseLocal( i );
    }

    void Animator::EvaluateLayers( LocalPose& pose ) const
    {
        const auto&    bones = m_Skeleton.GetBones();
        const uint32_t n     = static_cast<uint32_t>( bones.size() );

        for ( const auto& layer : m_Layers )
        {
            if ( !layer.Playback.IsValid() || layer.Weight <= 0.0F )
                continue;
            const float w = glm::clamp( layer.Weight, 0.0F, 1.0F );

            for ( uint32_t i = 0; i < n; ++i )
            {
                if ( !layer.BoneMask.empty() && ( i >= layer.BoneMask.size() || layer.BoneMask[i] == 0 ) )
                    continue;

                const BoneTransform layerLocal =
                     SampleLocalTransform( layer.Playback.Clip, i, layer.Playback.Time );
                const BoneTransform& base = pose[i];

                if ( layer.Additive )
                {
                    // Additive: apply the layer's delta from the BIND pose, scaled by weight, on top of base.
                    const BoneTransform& bind = m_BindPose[i];
                    BoneTransform        out;
                    out.Translation    = base.Translation + w * ( layerLocal.Translation - bind.Translation );
                    const glm::quat dR = layerLocal.Rotation * glm::inverse( bind.Rotation );
                    out.Rotation       = glm::slerp( glm::quat( 1.0F, 0.0F, 0.0F, 0.0F ), dR, w ) * base.Rotation;
                    const glm::vec3 dS = layerLocal.Scale / glm::max( bind.Scale, glm::vec3( 1e-6F ) );
                    out.Scale          = base.Scale * glm::mix( glm::vec3( 1.0F ), dS, w );
                    pose[i]            = out;
                }
                else
                {
                    // Override: blend base -> layer by weight.
                    pose[i] = Blend( base, layerLocal, w );
                }
            }
        }
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
        if ( !m_Layers.empty() )
        {
            m_Stages.push_back( PoseStage::Layers );
        }
        if ( !m_Controls.empty() )
        {
            m_Stages.push_back( PoseStage::Controls );
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

        const FrameNumber duration = playback.Clip->DurationTicks;
        const FrameTime   previous = playback.Time;

        // THE WHOLE TICKS GO INTO AN INTEGER AND ONLY THE FRACTION OF ONE STAYS IN A FLOAT. What this
        // replaces was `playback.Time += deltaTime * tps` on a float, wrapped with `fmod`: the error grew
        // with the value already there, and every loop left a residue behind. Measured in
        // Tests/Engine/AnimationTimeModel: an hour of 1/60 s steps drifts less than one tick here and
        // hundreds of times further on the float.
        playback.Time = AdvanceFrameTime( playback.Time, deltaTime, playback.Clip->TickRate );

        const bool looped = playback.Loop && duration.Value > 0 && playback.Time.Frame.Value >= duration.Value;

        if ( playback.Loop )
        {
            playback.Time = WrapFrameTime( playback.Time, duration );
        }
        else if ( playback.Time.Frame.Value >= duration.Value )
        {
            playback.Time = FrameTime{ duration, 0.0F };
        }

        // Fire the CURRENT clip's notifies whose TICK was crossed this frame (forward playback only). The
        // covered interval is (previous, now]; on a loop wrap it is (previous, duration) then [0, now].
        // One frame is assumed not to skip a whole loop, which holds for real playback.
        if ( &playback == &m_Current && deltaTime > 0.0F && !playback.Clip->Notifies.empty() )
        {
            const double before = previous.AsTicks();
            const double after  = playback.Time.AsTicks();
            for ( const auto& notify : playback.Clip->Notifies )
            {
                const auto at   = static_cast<double>( notify.Tick.Value );
                const bool fire = looped ? ( at > before || at <= after ) : ( at > before && at <= after );
                if ( fire )
                {
                    m_FiredNotifies.push_back( notify.Name );
                }
            }
        }
    }

    // ============================================================
    // Sampling
    // ============================================================

    BoneTransform Animator::SampleLocalTransform( const AnimationClip* clip, uint32_t boneIndex,
                                                  FrameTime time ) const
    {
        if ( const BoneTrack* track = ResolveTrack( clip, boneIndex ) )
            if ( track->HasKeys() )
            {
                return track->Sample( time, clip->TickRate );
            }
        return m_BindPose[boneIndex];
    }

    BoneTransform Animator::BlendedBaseLocal( uint32_t boneIndex ) const
    {
        const BoneTransform a = SampleLocalTransform( m_Current.Clip, boneIndex, m_Current.Time );
        if ( !m_IsBlending || !m_Next.IsValid() )
        {
            return a;
        }

        const float alpha = BlendAlpha();

        // Exactly the endpoints at the endpoints. `Blend` slerps, and slerp at alpha 0 is not bit-identical
        // to its input for every quaternion; short-circuiting is what makes "a crossfade at 0 is the source
        // clip, bit for bit" a property that can be asserted rather than approximated.
        if ( alpha <= 0.0F )
        {
            return a;
        }

        const BoneTransform b = SampleLocalTransform( m_Next.Clip, boneIndex, m_Next.Time );
        if ( alpha >= 1.0F )
        {
            return b;
        }

        return Blend( a, b, alpha );
    }

    const BoneTrack* Animator::ResolveTrack( const AnimationClip* clip, uint32_t boneIndex ) const
    {
        if ( !clip )
        {
            return nullptr;
        }

        auto& binding = m_TrackBinding[clip];

        // REBUILT WHEN THE CLIP'S TRACK STORAGE HAS MOVED, not only when the cache is empty. An unload +
        // reload of the clip's asset leaves the AnimationClip at the same address holding a freshly
        // allocated Tracks vector, so a cache keyed on the address alone hands back pointers into memory
        // that has been returned to the allocator. See Animator::TrackBinding for the crash this caused.
        if ( binding.ByBone.empty() || binding.TracksData != clip->Tracks.data() ||
             binding.TrackCount != clip->Tracks.size() || binding.Revision != clip->TrackRevision )
        {
            const auto& bones = m_Skeleton.GetBones();
            binding.ByBone.assign( bones.size(), TrackBinding::NO_TRACK );
            binding.TracksData = clip->Tracks.data();
            binding.TrackCount = clip->Tracks.size();
            binding.Revision   = clip->TrackRevision;

            // The skeleton's own name -> index map, rather than a second one built here per clip. One
            // answer to "which bone is called this", and it is the one `FindBoneIndex` gives.
            for ( uint32_t t = 0; t < clip->Tracks.size(); ++t )
            {
                const auto& track = clip->Tracks[t];
                if ( track.BoneName.empty() )
                {
                    continue;
                }
                if ( const auto bone = m_Skeleton.FindBoneIndex( track.BoneName ) )
                {
                    binding.ByBone[*bone] = t;
                }
            }
        }

        if ( boneIndex >= binding.ByBone.size() )
        {
            return nullptr;
        }
        const uint32_t track = binding.ByBone[boneIndex];
        return track == TrackBinding::NO_TRACK ? nullptr : &clip->Tracks[track];
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

        return m_Current.Time.Frame.Value >= m_Current.Clip->DurationTicks.Value;
    }

    void Animator::SetTick( FrameTime time )
    {
        if ( !m_Current.IsValid() )
        {
            return;
        }

        const FrameNumber duration = m_Current.Clip->DurationTicks;
        m_Current.Time             = time;
        if ( m_Current.Time.Frame.Value < 0 )
        {
            m_Current.Time = FrameTime{};
        }
        else if ( duration.Value > 0 && m_Current.Time.Frame.Value > duration.Value )
        {
            m_Current.Time = FrameTime{ duration, 0.0F };
        }
        EvaluatePipeline();
    }

    void Animator::SetTime( float time )
    {
        if ( !m_Current.IsValid() )
        {
            return;
        }

        // Through the tick, and onto the NEAREST one: a scrub arrives as a real number that has been
        // through a pixel position and a division, so flooring it would put a user who dragged onto a key
        // one tick before it. See TimeModel.hpp.
        const FrameTime   asTicks = SecondsToFrameTime( static_cast<double>( time ), m_Current.Clip->TickRate );
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

    int Animator::AddLayer( const AnimationClip& clip, float weight, bool additive, bool loop )
    {
        AnimationLayer layer;
        layer.Playback = { &clip, FrameTime{}, loop };
        layer.Weight   = weight;
        layer.Additive = additive;
        m_Layers.push_back( std::move( layer ) );
        SyncStages();
        return static_cast<int>( m_Layers.size() ) - 1;
    }

    void Animator::SetLayerClip( int index, const AnimationClip& clip )
    {
        if ( index < 0 || index >= static_cast<int>( m_Layers.size() ) )
        {
            return;
        }
        if ( m_Layers[index].Playback.Clip != &clip )
            m_Layers[index].Playback = { &clip, FrameTime{}, m_Layers[index].Playback.Loop };
    }

    void Animator::SetLayerWeight( int index, float weight )
    {
        if ( index >= 0 && index < static_cast<int>( m_Layers.size() ) )
            m_Layers[index].Weight = weight;
    }

    void Animator::SetLayerAdditive( int index, bool additive )
    {
        if ( index >= 0 && index < static_cast<int>( m_Layers.size() ) )
            m_Layers[index].Additive = additive;
    }

    void Animator::SetLayerMaskByNames( int index, const std::vector<std::string>& boneNames,
                                        bool includeChildren )
    {
        if ( index < 0 || index >= static_cast<int>( m_Layers.size() ) )
        {
            return;
        }

        const auto&          bones = m_Skeleton.GetBones();
        std::vector<uint8_t> mask( bones.size(), 0 );

        // A hash lookup per NAME. This was a nested loop over (names x bones) comparing std::strings, which
        // is the linear scan T1.2 is about, spelled out again by hand instead of calling FindBoneIndex.
        for ( const auto& name : boneNames )
            if ( const auto bone = m_Skeleton.FindBoneIndex( name ) )
                mask[*bone] = 1;

        if ( includeChildren )
        {
            // A bone is affected if it OR any ancestor was named — masking a shoulder masks the whole arm.
            // Parent-before-child order makes this a single flat pass: by the time a bone is reached its
            // parent's answer is final. The memoised recursion this replaces had the same stack-overflow
            // hazard on a parent cycle as the seven chain walks did.
            for ( const uint32_t i : m_Skeleton.GetResolveOrder() )
            {
                const uint32_t parent = m_Skeleton.ResolveParent( i );
                if ( !mask[i] && parent != Skeleton::NO_PARENT )
                    mask[i] = mask[parent];
            }
        }

        m_Layers[index].BoneMask = std::move( mask );
    }

    void Animator::ClearLayerMask( int index )
    {
        if ( index >= 0 && index < static_cast<int>( m_Layers.size() ) )
            m_Layers[index].BoneMask.clear();
    }

    bool Animator::IsBoneInLayerMask( int index, uint32_t boneIndex ) const
    {
        if ( index < 0 || index >= static_cast<int>( m_Layers.size() ) )
        {
            return false;
        }
        const auto& mask = m_Layers[index].BoneMask;
        if ( mask.empty() )
        {
            return boneIndex < m_Skeleton.GetBones().size();
        }
        return boneIndex < mask.size() && mask[boneIndex] != 0;
    }

    void Animator::RemoveLayer( int index )
    {
        if ( index >= 0 && index < static_cast<int>( m_Layers.size() ) )
        {
            m_Layers.erase( m_Layers.begin() + index );
            SyncStages();
        }
    }

    void Animator::ClearLayers()
    {
        m_Layers.clear();
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

} // namespace Desert::Animation
