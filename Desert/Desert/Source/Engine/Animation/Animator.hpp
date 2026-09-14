#pragma once

#include <glm/glm.hpp>

#include "Skeleton.hpp"
#include "AnimationClip.hpp"
#include "Pose.hpp"

#include <Common/Core/Timestep.hpp>

namespace Desert::Animation
{
    class Animator
    {
    public:
        explicit Animator( const Skeleton& skeleton );

        void Play( const AnimationClip& clip, bool loop = true );
        void CrossFade( const AnimationClip& clip, float duration, bool loop = true );
        void Stop();

        void Update( const Common::Timestep& ts );

        [[nodiscard]] const Skeleton& GetSkeleton() const
        {
            return m_Skeleton;
        }

        [[nodiscard]] const Pose& GetPose() const;

        // The model-space (mesh-local) transform of a bone in the CURRENT pose. The pose stores the skinning
        // matrix (global * offset), so the bone's actual placement is recovered by undoing the inverse-bind:
        // global = skinMatrix * inverse(offset). Multiply by the entity's world matrix for a world socket.
        // Returns identity if the index is out of range. (Bone index from Skeleton::FindBoneIndex.)
        [[nodiscard]] glm::mat4 GetBoneModelMatrix( uint32_t boneIndex ) const
        {
            const auto& bones = m_Skeleton.GetBones();
            if ( boneIndex >= bones.size() || boneIndex >= m_CurrentPose.BoneMatrices.size() )
                return glm::mat4( 1.0f );
            return m_CurrentPose.BoneMatrices[boneIndex] * glm::inverse( bones[boneIndex].OffsetMatrix );
        }

        [[nodiscard]] bool IsPlaying() const
        {
            return m_Current.Clip != nullptr;
        }

        [[nodiscard]] float GetCurrentTime() const
        {
            return m_Current.Time;
        }

        [[nodiscard]] float GetDuration() const
        {
            return m_Current.Clip ? m_Current.Clip->Duration : 0.0f;
        }

        [[nodiscard]] bool IsFinished() const;

        void SetTime( float time );
        void SetPlaybackSpeed( float speed )
        {
            m_PlaybackSpeed = speed;
        }

        void SetLoop( bool loop );

        [[nodiscard]] const AnimationClip* GetCurrentClip() const;

        // --- Pose authoring: a separate, EDITABLE animated-pose buffer ----------------------------------
        // The rig's per-bone LocalBindTransform is the shared REST pose; posing a bone to author a clip must
        // NOT mutate it. This buffer holds a per-bone LOCAL (parent-relative) transform, independent of bind,
        // that the editor gizmo writes to and the Sequencer keys from. Init = bind. Normal playback
        // (Update/SetTime) is UNAFFECTED by it — only ApplyLocalPose() renders it into the skinning matrices.
        void                    SetBoneLocalPose( uint32_t boneIndex, const glm::mat4& localTransform );
        [[nodiscard]] glm::mat4 GetBoneLocalPose( uint32_t boneIndex ) const; // identity if out of range
        // Loads `clip`'s sampled LOCAL transforms at `time` into the pose buffer (bind for untracked bones),
        // so the user can edit an existing keyed pose and re-key from it.
        void SampleClipIntoLocalPose( const AnimationClip& clip, float time );
        // Rebuilds GetPose() from the pose buffer, ignoring any playing clip — call after editing the buffer
        // to show the posed skeleton in the viewport.
        void ApplyLocalPose();

        // Returns (and clears) the names of the current clip's notifies crossed during the last Update — for
        // the ECS to dispatch to scripts. Call once per frame after Update. Scrubbing via SetTime does NOT
        // fire notifies (only forward playback does).
        std::vector<std::string> ConsumeNotifies()
        {
            std::vector<std::string> out;
            out.swap( m_FiredNotifies );
            return out;
        }

        // --- Animation layers (override / additive, with optional per-bone masks) ---
        // A layer plays a clip ON TOP of the base clip, restricted to its masked bones (empty mask = all).
        //   Override (default): masked bones are blended base -> layer by Weight (e.g. an upper-body reload
        //     over a full-body run — mask the spine/arms, weight 1).
        //   Additive: the layer's delta from the rig's BIND pose is added (scaled by Weight) on top of the
        //     base — for aim offsets / lean / breathing.
        // Layers are skipped during a base crossfade (that brief frame plays the blend only). AddLayer returns
        // the new layer index; the setters no-op on an out-of-range index.
        int  AddLayer( const AnimationClip& clip, float weight = 1.0f, bool additive = false, bool loop = true );
        void SetLayerClip( int index, const AnimationClip& clip );
        void SetLayerWeight( int index, float weight );
        void SetLayerAdditive( int index, bool additive );
        // Restrict the layer to the named bones (and, by default, their descendants — a masked shoulder also
        // masks the whole arm, which is what "upper body" means). Unknown names are ignored.
        void                 SetLayerMaskByNames( int index, const std::vector<std::string>& boneNames,
                                                  bool includeChildren = true );
        void                 ClearLayerMask( int index ); // layer affects all bones
        void                 RemoveLayer( int index );
        void                 ClearLayers();
        [[nodiscard]] size_t GetLayerCount() const
        {
            return m_Layers.size();
        }
        [[nodiscard]] float GetLayerWeight( int index ) const
        {
            return ( index >= 0 && index < static_cast<int>( m_Layers.size() ) ) ? m_Layers[index].Weight : 0.0f;
        }
        [[nodiscard]] bool GetLayerAdditive( int index ) const
        {
            return ( index >= 0 && index < static_cast<int>( m_Layers.size() ) ) && m_Layers[index].Additive;
        }
        [[nodiscard]] const AnimationClip* GetLayerClip( int index ) const
        {
            return ( index >= 0 && index < static_cast<int>( m_Layers.size() ) ) ? m_Layers[index].Playback.Clip
                                                                                 : nullptr;
        }

    private:
        struct ClipPlayback
        {
            const AnimationClip* Clip = nullptr;
            float                Time = 0.0f;
            bool                 Loop = true;

            bool IsValid() const
            {
                return Clip != nullptr;
            }
        };

        struct AnimationLayer
        {
            ClipPlayback         Playback;         // clip + time + loop for this layer
            float                Weight   = 1.0f;  // 0 = off, 1 = full
            bool                 Additive = false; // additive delta vs bind, else override blend
            std::vector<uint8_t> BoneMask;         // per skeleton bone (1 = affected); empty = all bones
        };

    private:
        void UpdatePlayback( ClipPlayback& playback, float deltaTime );
        void CalculatePose( const ClipPlayback& playback );
        void CalculateBlendedPose();

        // How far the crossfade has run, 0..1. Derived rather than stored: the alpha and the clock cannot
        // disagree if there is only one of them.
        [[nodiscard]] float BlendAlpha() const
        {
            return m_IsBlending ? glm::clamp( m_BlendTime / m_BlendDuration, 0.0f, 1.0f ) : 0.0f;
        }

        // The base pose's LOCAL transform for one bone: the current clip, or current->next blended by
        // BlendAlpha() while crossfading. The one place that answers "what is the base pose right now",
        // shared by the crossfade render and by layer composition.
        [[nodiscard]] glm::mat4 BlendedBaseLocal( uint32_t boneIndex ) const;

        // local (parent-relative) transforms -> m_CurrentPose skinning matrices.
        void ResolveSkinningInto( const std::vector<glm::mat4>& local );

        // Local (parent-relative) transform of bone `boneIndex` driven by `clip` at `time`, or the bind-pose
        // local when the clip has no track for it. The building block for layer composition.
        glm::mat4 SampleLocalTransform( const AnimationClip* clip, uint32_t boneIndex, float time ) const;

        // Recomputes m_CurrentPose from the base clip combined with every active layer, entirely in local
        // bone space, then rebuilds the skinning matrices. Only called when layers exist (base path untouched).
        void ApplyLayers();

        void CalculateBoneTransform( const ClipPlayback& playback, uint32_t boneIndex,
                                     const glm::mat4& parentTransform );

        // Fills m_CurrentPose with the skeleton's REST/BIND pose (chainGlobal * OffsetMatrix per bone). This is
        // the correct idle pose — an all-identity pose would skin the RAW authored vertices and collapse the
        // mesh. Computed at construction so GetPose() is valid before any clip plays.
        void ComputeBindPose();

        // Sizes m_LocalPose to the skeleton and fills it with the bind pose. Called at construction and
        // whenever the buffer is found out of step with the rig.
        void InitLocalPose();

        // Returns the clip track that drives skeleton bone `boneIndex`, matched by bone NAME (not by the clip's
        // own bone index). This lets a clip authored against a differently-ordered or skinless export of the
        // same rig still drive the correct bones. Built lazily per clip, and rebuilt whenever the clip's own
        // track storage has been replaced under it — see TrackBinding.
        const BoneTrack* ResolveTrack( const AnimationClip* clip, uint32_t boneIndex ) const;

    private:
        /**
         * @brief The bone -> track lookup for ONE clip, together with the two facts about that clip's
         *        storage the lookup is only valid against.
         *
         * THE CLIP'S ADDRESS IS NOT A SUFFICIENT KEY, and believing it was is a use-after-free that
         * segfaulted the moment a file-backed clip first played in this engine. `AnimationAsset` owns its
         * `AnimationClip` BY VALUE, so the clip keeps its address for the asset's whole life while
         * `Unload()` frees the `Tracks` vector (`clear()` + `shrink_to_fit()`) and a later `Load()`
         * allocates a new one. Asset eviction does exactly that to a clip an entity is still playing —
         * deliberately, because `AnimationLibrary::Resolve` reloads on every lookup and the design accepts
         * eviction on that basis — and the cached `BoneTrack*` then pointed into freed storage. The crash
         * was inside `lower_bound` over a keyframe vector that no longer existed.
         *
         * The relation asserted here is between the cache and the container it points into: the binding is
         * usable only while the clip's tracks still live where they lived when it was built.
         */
        struct TrackBinding
        {
            const BoneTrack*              TracksData = nullptr; ///< clip->Tracks.data() at build time
            size_t                        TrackCount = 0;       ///< clip->Tracks.size() at build time
            std::vector<const BoneTrack*> ByBone;               ///< skeleton bone index -> its track, or null
        };

        const Skeleton& m_Skeleton;

        // clip -> its binding. See ResolveTrack and TrackBinding.
        mutable std::unordered_map<const AnimationClip*, TrackBinding> m_TrackBinding;

        ClipPlayback m_Current;
        ClipPlayback m_Next;

        bool  m_IsBlending    = false;
        float m_BlendTime     = 0.0f;
        float m_BlendDuration = 0.0f;

        float m_PlaybackSpeed = 1.0f;

        // Notify names crossed during the last Update of the current clip, drained by ConsumeNotifies().
        std::vector<std::string> m_FiredNotifies;

        // Active animation layers, applied on top of the base clip each frame (see ApplyLayers).
        std::vector<AnimationLayer> m_Layers;

        // Editable per-bone LOCAL transforms for pose authoring (init = bind). Independent of the rig's bind
        // pose and of clip playback; only ApplyLocalPose() renders it. See the pose-authoring API above.
        std::vector<glm::mat4> m_LocalPose;

        Pose m_CurrentPose;
    };
} // namespace Desert::Animation
