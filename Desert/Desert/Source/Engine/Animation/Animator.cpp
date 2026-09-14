#include "Animator.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

namespace Desert::Animation
{
    struct TRS
    {
        glm::vec3 Translation;
        glm::quat Rotation;
        glm::vec3 Scale;
    };

    TRS Decompose( const glm::mat4& m )
    {
        TRS result;

        result.Translation = glm::vec3( m[3] );

        result.Scale.x = glm::length( glm::vec3( m[0] ) );
        result.Scale.y = glm::length( glm::vec3( m[1] ) );
        result.Scale.z = glm::length( glm::vec3( m[2] ) );

        glm::mat3 rotationMatrix;
        rotationMatrix[0] = glm::vec3( m[0] ) / result.Scale.x;
        rotationMatrix[1] = glm::vec3( m[1] ) / result.Scale.y;
        rotationMatrix[2] = glm::vec3( m[2] ) / result.Scale.z;

        result.Rotation = glm::quat_cast( rotationMatrix );

        return result;
    }

    glm::mat4 Compose( const TRS& trs )
    {
        glm::mat4 T = glm::translate( glm::mat4( 1.0f ), trs.Translation );
        glm::mat4 R = glm::toMat4( trs.Rotation );
        glm::mat4 S = glm::scale( glm::mat4( 1.0f ), trs.Scale );

        return T * R * S;
    }

    Animator::Animator( const Skeleton& skeleton ) : m_Skeleton( skeleton )
    {
        InitLocalPose();   // editable pose buffer starts at bind
        ComputeBindPose(); // valid rest pose before any clip plays (identity would collapse the mesh)
    }

    void Animator::InitLocalPose()
    {
        const auto& bones = m_Skeleton.GetBones();
        m_LocalPose.resize( bones.size() );
        for ( size_t i = 0; i < bones.size(); ++i )
            m_LocalPose[i] = bones[i].LocalBindTransform;
    }

    void Animator::SetBoneLocalPose( uint32_t boneIndex, const glm::mat4& localTransform )
    {
        if ( boneIndex < m_LocalPose.size() )
            m_LocalPose[boneIndex] = localTransform;
    }

    glm::mat4 Animator::GetBoneLocalPose( uint32_t boneIndex ) const
    {
        return boneIndex < m_LocalPose.size() ? m_LocalPose[boneIndex] : glm::mat4( 1.0f );
    }

    void Animator::SampleClipIntoLocalPose( const AnimationClip& clip, float time )
    {
        const auto& bones = m_Skeleton.GetBones();
        if ( m_LocalPose.size() != bones.size() )
            InitLocalPose();
        for ( uint32_t i = 0; i < bones.size(); ++i )
            m_LocalPose[i] = SampleLocalTransform( &clip, i, time );
    }

    void Animator::ApplyLocalPose()
    {
        const auto& bones = m_Skeleton.GetBones();
        if ( m_LocalPose.size() != bones.size() )
            InitLocalPose();

        m_CurrentPose.BoneMatrices.assign( bones.size(), glm::mat4( 1.0f ) );

        std::vector<glm::mat4>             global( bones.size(), glm::mat4( 1.0f ) );
        std::vector<bool>                  done( bones.size(), false );
        std::function<glm::mat4( size_t )> resolve = [&]( size_t i ) -> glm::mat4
        {
            if ( done[i] )
                return global[i];
            glm::mat4 m = m_LocalPose[i];
            if ( bones[i].ParentBoneID.has_value() && bones[i].ParentBoneID.value() < bones.size() )
                m = resolve( bones[i].ParentBoneID.value() ) * m_LocalPose[i];
            global[i] = m;
            done[i]   = true;
            return m;
        };
        for ( size_t i = 0; i < bones.size(); ++i )
            m_CurrentPose.BoneMatrices[i] = resolve( i ) * bones[i].OffsetMatrix;
    }

    void Animator::ComputeBindPose()
    {
        const auto& bones = m_Skeleton.GetBones();
        m_CurrentPose.BoneMatrices.assign( bones.size(), glm::mat4( 1.0f ) );

        std::vector<glm::mat4>             global( bones.size(), glm::mat4( 1.0f ) );
        std::vector<bool>                  done( bones.size(), false );
        std::function<glm::mat4( size_t )> resolve = [&]( size_t i ) -> glm::mat4
        {
            if ( done[i] )
                return global[i];
            glm::mat4 m = bones[i].LocalBindTransform;
            if ( bones[i].ParentBoneID.has_value() && bones[i].ParentBoneID.value() < bones.size() )
                m = resolve( bones[i].ParentBoneID.value() ) * bones[i].LocalBindTransform;
            global[i] = m;
            done[i]   = true;
            return m;
        };
        for ( size_t i = 0; i < bones.size(); ++i )
            m_CurrentPose.BoneMatrices[i] = resolve( i ) * bones[i].OffsetMatrix;
    }

    // ============================================================
    // Play / CrossFade
    // ============================================================

    void Animator::Play( const AnimationClip& clip, bool loop )
    {
        m_Current    = { &clip, 0.0f, loop };
        m_Next       = {};
        m_IsBlending = false;
    }

    void Animator::CrossFade( const AnimationClip& clip, float duration, bool loop )
    {
        if ( m_Current.Clip == &clip )
            return;

        m_Next = { &clip, 0.0f, loop };

        m_IsBlending    = true;
        m_BlendTime     = 0.0f;
        m_BlendDuration = glm::max( duration, 0.0001f );
    }

    void Animator::Stop()
    {
        m_Current    = {};
        m_Next       = {};
        m_IsBlending = false;
    }

    // ============================================================
    // Update
    // ============================================================

    void Animator::Update( const Common::Timestep& ts )
    {
        float deltaTime = ts.GetSeconds() * m_PlaybackSpeed;

        if ( !m_Current.IsValid() )
            return;

        UpdatePlayback( m_Current, deltaTime );

        if ( m_IsBlending && m_Next.IsValid() )
        {
            UpdatePlayback( m_Next, deltaTime );

            m_BlendTime += deltaTime;

            CalculateBlendedPose();

            if ( BlendAlpha() >= 1.0f )
            {
                m_Current    = m_Next;
                m_Next       = {};
                m_IsBlending = false;
            }
        }
        else
        {
            CalculatePose( m_Current );
        }

        // Overlay the animation layers on the base pose, CROSSFADE INCLUDED. This used to read
        // `if ( !m_IsBlending ) ApplyLayers();` under a comment calling it "that brief frame" — it is not a
        // frame, it is the whole blend: a 0.5 s crossfade dropped an upper-body override for 0.5 s, and the
        // comment was the only thing saying otherwise. ApplyLayers now builds its base from the SAME blend
        // the no-layer path renders, so the layer stack survives the transition.
        if ( !m_Layers.empty() )
        {
            for ( auto& layer : m_Layers )
                if ( layer.Playback.IsValid() )
                    UpdatePlayback( layer.Playback, deltaTime );

            ApplyLayers();
        }
    }

    // ============================================================
    // Playback Update
    // ============================================================

    void Animator::UpdatePlayback( ClipPlayback& playback, float dt )
    {
        if ( !playback.IsValid() )
            return;

        float tps = playback.Clip->TicksPerSecond > 0.0f ? playback.Clip->TicksPerSecond : 25.0f;

        const float prev     = playback.Time;
        const float duration = playback.Clip->Duration;

        playback.Time += dt * tps;

        const bool looped = playback.Loop && duration > 0.0f && playback.Time >= duration;

        if ( playback.Loop )
            playback.Time = duration > 0.0f ? fmod( playback.Time, duration ) : 0.0f;
        else
            playback.Time = glm::min( playback.Time, duration );

        // Fire the CURRENT clip's notifies whose time was crossed this frame (forward playback only). The
        // covered interval is (prev, newTime]; on a loop wrap it is (prev, duration) ∪ [0, newTime]. One
        // frame is assumed not to skip a whole loop (dt*tps < duration), which holds for real playback.
        if ( &playback == &m_Current && dt > 0.0f && !playback.Clip->Notifies.empty() )
        {
            const float newTime = playback.Time;
            for ( const auto& n : playback.Clip->Notifies )
            {
                const bool fire =
                     looped ? ( n.Time > prev || n.Time <= newTime ) : ( n.Time > prev && n.Time <= newTime );
                if ( fire )
                    m_FiredNotifies.push_back( n.Name );
            }
        }
    }

    // ============================================================
    // Pose Calculation
    // ============================================================

    void Animator::CalculatePose( const ClipPlayback& playback )
    {
        const auto& bones = m_Skeleton.GetBones();

        for ( uint32_t i = 0; i < bones.size(); ++i )
        {
            if ( bones[i].IsRoot() )
            {
                CalculateBoneTransform( playback, i, glm::mat4( 1.0f ) );
            }
        }
    }

    glm::mat4 Animator::BlendedBaseLocal( uint32_t boneIndex ) const
    {
        const glm::mat4 a = SampleLocalTransform( m_Current.Clip, boneIndex, m_Current.Time );
        if ( !m_IsBlending || !m_Next.IsValid() )
            return a;

        const glm::mat4 b     = SampleLocalTransform( m_Next.Clip, boneIndex, m_Next.Time );
        const float     alpha = BlendAlpha();

        // Exactly the endpoints at the endpoints. Decompose/Compose is not an identity on a matrix with
        // shear, so returning the sampled matrix itself — rather than a round-tripped copy of it — is what
        // makes "alpha 0 is bit-for-bit the source clip" a property and not an approximation.
        if ( alpha <= 0.0f )
            return a;
        if ( alpha >= 1.0f )
            return b;

        const TRS ta = Decompose( a );
        const TRS tb = Decompose( b );

        TRS blended;
        blended.Translation = glm::mix( ta.Translation, tb.Translation, alpha );
        blended.Rotation    = glm::slerp( ta.Rotation, tb.Rotation, alpha );
        blended.Scale       = glm::mix( ta.Scale, tb.Scale, alpha );
        return Compose( blended );
    }

    void Animator::CalculateBlendedPose()
    {
        // IT BLENDS LOCAL TRANSFORMS, AND IT USED TO BLEND SKINNING MATRICES. The old body sampled both
        // clips into m_CurrentPose — whose entries are `chainGlobal * OffsetMatrix` — copied the array
        // twice per root, and then slerped the rotation OF THAT PRODUCT. The inverse bind matrix is baked
        // into it, so that quantity is not the bone's rotation and its slerp is not the bone's rotation
        // blended; the error is invisible on a rig whose OffsetMatrix is near identity (our one probe is
        // exactly that: one bone, both matrices identity) and shows up on the first imported character.
        // It also looped over ROOTS and redid the whole array inside each iteration, so a two-root rig paid
        // for the blend twice and pushed the first root's bones through a second Decompose/Compose round
        // trip for nothing.
        const auto&  bones = m_Skeleton.GetBones();
        const size_t n     = bones.size();

        std::vector<glm::mat4> local( n );
        for ( size_t i = 0; i < n; ++i )
            local[i] = BlendedBaseLocal( static_cast<uint32_t>( i ) );

        ResolveSkinningInto( local );
    }

    void Animator::ResolveSkinningInto( const std::vector<glm::mat4>& local )
    {
        const auto&  bones = m_Skeleton.GetBones();
        const size_t n     = bones.size();

        std::vector<glm::mat4>             global( n, glm::mat4( 1.0f ) );
        std::vector<bool>                  done( n, false );
        std::function<glm::mat4( size_t )> resolve = [&]( size_t i ) -> glm::mat4
        {
            if ( done[i] )
                return global[i];
            glm::mat4 g = local[i];
            if ( bones[i].ParentBoneID.has_value() && bones[i].ParentBoneID.value() < n )
                g = resolve( bones[i].ParentBoneID.value() ) * local[i];
            global[i] = g;
            done[i]   = true;
            return g;
        };

        if ( m_CurrentPose.BoneMatrices.size() != n )
            m_CurrentPose.BoneMatrices.assign( n, glm::mat4( 1.0f ) );
        for ( size_t i = 0; i < n; ++i )
            m_CurrentPose.BoneMatrices[i] = resolve( i ) * bones[i].OffsetMatrix;
    }

    const BoneTrack* Animator::ResolveTrack( const AnimationClip* clip, uint32_t boneIndex ) const
    {
        if ( !clip )
            return nullptr;

        auto& binding = m_TrackBinding[clip];

        // REBUILT WHEN THE CLIP'S TRACK STORAGE HAS MOVED, not only when the cache is empty. An unload +
        // reload of the clip's asset leaves the AnimationClip at the same address holding a freshly
        // allocated Tracks vector, so a cache keyed on the address alone hands back pointers into memory
        // that has been returned to the allocator. See Animator::TrackBinding for the crash this caused.
        if ( binding.ByBone.empty() || binding.TracksData != clip->Tracks.data() ||
             binding.TrackCount != clip->Tracks.size() )
        {
            const auto& bones = m_Skeleton.GetBones();
            binding.ByBone.assign( bones.size(), nullptr );
            binding.TracksData = clip->Tracks.data();
            binding.TrackCount = clip->Tracks.size();

            std::unordered_map<std::string, const BoneTrack*> byName;
            for ( const auto& track : clip->Tracks )
                if ( !track.BoneName.empty() )
                    byName[track.BoneName] = &track;

            for ( size_t i = 0; i < bones.size(); ++i )
            {
                auto it = byName.find( bones[i].Name );
                if ( it != byName.end() )
                    binding.ByBone[i] = it->second;
            }
        }

        return ( boneIndex < binding.ByBone.size() ) ? binding.ByBone[boneIndex] : nullptr;
    }

    void Animator::CalculateBoneTransform( const ClipPlayback& playback, uint32_t boneIndex,
                                           const glm::mat4& parentTransform )
    {
        const auto& bones = m_Skeleton.GetBones();
        const auto& bone  = bones[boneIndex];

        glm::mat4 localTransform = bone.LocalBindTransform;

        if ( const BoneTrack* track = ResolveTrack( playback.Clip, boneIndex ) )
        {
            if ( !track->PositionKeys.empty() || !track->RotationKeys.empty() || !track->ScaleKeys.empty() )
            {
                localTransform = track->GetTransform( playback.Time );
            }
        }

        glm::mat4 globalTransform = parentTransform * localTransform;

        m_CurrentPose.BoneMatrices[boneIndex] = globalTransform * bone.OffsetMatrix;

        for ( uint32_t i = 0; i < bones.size(); ++i )
        {
            if ( bones[i].ParentBoneID == boneIndex )
            {
                CalculateBoneTransform( playback, i, globalTransform );
            }
        }
    }

    // ============================================================
    // Utilities
    // ============================================================

    const Pose& Animator::GetPose() const
    {
        return m_CurrentPose;
    }

    bool Animator::IsFinished() const
    {
        if ( !m_Current.IsValid() )
            return true;

        if ( m_Current.Loop )
            return false;

        return m_Current.Time >= m_Current.Clip->Duration;
    }

    void Animator::SetTime( float time )
    {
        if ( !m_Current.IsValid() )
            return;

        m_Current.Time = glm::clamp( time, 0.0f, m_Current.Clip->Duration );
        CalculatePose( m_Current );
    }

    void Animator::SetLoop( bool loop )
    {
        if ( m_Current.IsValid() )
            m_Current.Loop = loop;

        if ( m_IsBlending && m_Next.IsValid() )
            m_Next.Loop = loop;
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

    glm::mat4 Animator::SampleLocalTransform( const AnimationClip* clip, uint32_t boneIndex, float time ) const
    {
        const auto& bones = m_Skeleton.GetBones();
        glm::mat4   local = bones[boneIndex].LocalBindTransform;
        if ( const BoneTrack* track = ResolveTrack( clip, boneIndex ) )
            if ( !track->PositionKeys.empty() || !track->RotationKeys.empty() || !track->ScaleKeys.empty() )
                local = track->GetTransform( time );
        return local;
    }

    void Animator::ApplyLayers()
    {
        const auto&  bones = m_Skeleton.GetBones();
        const size_t n     = bones.size();

        // 1) Base local transforms: the current clip, or the CROSSFADE of current and next — the same
        //    quantity CalculateBlendedPose renders, so layering does not have a second opinion about what
        //    the base pose is.
        std::vector<glm::mat4> local( n );
        for ( size_t i = 0; i < n; ++i )
            local[i] = BlendedBaseLocal( static_cast<uint32_t>( i ) );

        // 2) Fold each active layer over its masked bones, in local space.
        for ( const auto& layer : m_Layers )
        {
            if ( !layer.Playback.IsValid() || layer.Weight <= 0.0f )
                continue;
            const float w = glm::clamp( layer.Weight, 0.0f, 1.0f );

            for ( size_t i = 0; i < n; ++i )
            {
                if ( !layer.BoneMask.empty() && ( i >= layer.BoneMask.size() || layer.BoneMask[i] == 0 ) )
                    continue;

                const glm::mat4 layerLocal =
                     SampleLocalTransform( layer.Playback.Clip, static_cast<uint32_t>( i ), layer.Playback.Time );
                const TRS baseT = Decompose( local[i] );
                const TRS layT  = Decompose( layerLocal );

                TRS outT;
                if ( layer.Additive )
                {
                    // Additive: apply the layer's delta from the BIND pose, scaled by weight, on top of base.
                    const TRS bindT    = Decompose( bones[i].LocalBindTransform );
                    outT.Translation   = baseT.Translation + w * ( layT.Translation - bindT.Translation );
                    const glm::quat dR = layT.Rotation * glm::inverse( bindT.Rotation );
                    outT.Rotation      = glm::slerp( glm::quat( 1.0f, 0.0f, 0.0f, 0.0f ), dR, w ) * baseT.Rotation;
                    const glm::vec3 dS = layT.Scale / glm::max( bindT.Scale, glm::vec3( 1e-6f ) );
                    outT.Scale         = baseT.Scale * glm::mix( glm::vec3( 1.0f ), dS, w );
                }
                else
                {
                    // Override: blend base -> layer by weight.
                    outT.Translation = glm::mix( baseT.Translation, layT.Translation, w );
                    outT.Rotation    = glm::slerp( baseT.Rotation, layT.Rotation, w );
                    outT.Scale       = glm::mix( baseT.Scale, layT.Scale, w );
                }
                local[i] = Compose( outT );
            }
        }

        // 3) Rebuild the global chain + skinning matrices from the combined local transforms.
        ResolveSkinningInto( local );
    }

    int Animator::AddLayer( const AnimationClip& clip, float weight, bool additive, bool loop )
    {
        AnimationLayer layer;
        layer.Playback = { &clip, 0.0f, loop };
        layer.Weight   = weight;
        layer.Additive = additive;
        m_Layers.push_back( std::move( layer ) );
        return static_cast<int>( m_Layers.size() ) - 1;
    }

    void Animator::SetLayerClip( int index, const AnimationClip& clip )
    {
        if ( index < 0 || index >= static_cast<int>( m_Layers.size() ) )
            return;
        if ( m_Layers[index].Playback.Clip != &clip )
            m_Layers[index].Playback = { &clip, 0.0f, m_Layers[index].Playback.Loop };
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
            return;

        const auto&          bones = m_Skeleton.GetBones();
        std::vector<uint8_t> mask( bones.size(), 0 );
        for ( const auto& name : boneNames )
            for ( size_t i = 0; i < bones.size(); ++i )
                if ( bones[i].Name == name )
                {
                    mask[i] = 1;
                    break;
                }

        if ( includeChildren )
        {
            // A bone is affected if it OR any ancestor was named — masking a shoulder masks the whole arm.
            std::vector<bool>                done( bones.size(), false );
            std::function<uint8_t( size_t )> resolve = [&]( size_t i ) -> uint8_t
            {
                if ( done[i] )
                    return mask[i];
                uint8_t m = mask[i];
                if ( !m && bones[i].ParentBoneID.has_value() && bones[i].ParentBoneID.value() < bones.size() )
                    m = resolve( bones[i].ParentBoneID.value() );
                mask[i] = m;
                done[i] = true;
                return m;
            };
            for ( size_t i = 0; i < bones.size(); ++i )
                resolve( i );
        }

        m_Layers[index].BoneMask = std::move( mask );
    }

    void Animator::ClearLayerMask( int index )
    {
        if ( index >= 0 && index < static_cast<int>( m_Layers.size() ) )
            m_Layers[index].BoneMask.clear();
    }

    void Animator::RemoveLayer( int index )
    {
        if ( index >= 0 && index < static_cast<int>( m_Layers.size() ) )
            m_Layers.erase( m_Layers.begin() + index );
    }

    void Animator::ClearLayers()
    {
        m_Layers.clear();
    }

} // namespace Desert::Animation
