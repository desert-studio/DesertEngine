#pragma once

#include <glm/glm.hpp>

#include "Skeleton.hpp"
#include "AnimationClip.hpp"
#include "Pose.hpp"

#include <Common/Core/Timestep.hpp>

#include <unordered_map>

namespace Desert::Animation
{
    /**
     * @brief One step of the pose pipeline, in the order it runs.
     *
     * `Animator::Update` used to be three hardcoded branches — sample the base clip (or blend two), then
     * "if layers exist AND we are not crossfading" fold layers in, or, if the editor asked, ignore both.
     * No step could be added, reordered or inserted between, which is the exact reason a Control Rig
     * (report 05 §652: "the rig is a pose -> pose operator appended after the animation source") or an IK
     * solver could not be attached without rewriting the function.
     *
     * Membership is DATA, not a branch: `AddLayer` puts `Layers` into the list and removing the last layer
     * takes it out, so the list says what will run rather than the code deciding again every frame. The
     * resolve-to-component-space and multiply-by-offset tail is NOT a stage — it is the pipeline's output
     * and there is nothing after it to reorder against.
     *
     * The list is of enumerators rather than of polymorphic objects deliberately. All the shape buys today
     * is ordering and membership, both of which an enum gives; when the first stage with state of its own
     * arrives (a rig, a solver) this becomes a list of objects, and that is a change inside one file
     * BECAUSE the list already exists.
     */
    enum class PoseStage : uint8_t
    {
        Source, ///< the base clip, or the crossfade of the outgoing and incoming clips
        Layers, ///< override / additive layers folded over the base, per masked bone
    };

    [[nodiscard]] const char* ToString( PoseStage stage );

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

        /// The pipeline's OUTPUT: `component_i * OffsetMatrix_i` per bone, which is what the GPU skins with.
        [[nodiscard]] const SkinningMatrices& GetPose() const;

        /// The pose the pipeline produced, in the space it was computed in. This is the currency: what a
        /// blend blended, what a layer folded, and what a solver would modify.
        [[nodiscard]] const LocalPose& GetLocalPose() const
        {
            return m_EvaluatedPose;
        }

        /// The stages that will run, in order. Read-only — the list is maintained by the layer API, and a
        /// caller reordering it would be deciding something the Animator has to be able to guarantee.
        [[nodiscard]] const std::vector<PoseStage>& GetStages() const
        {
            return m_Stages;
        }

        /**
         * @brief The model-space (mesh-local) transform of a bone in the CURRENT pose.
         *
         * A LOOKUP NOW, AND IT USED TO BE A MATRIX INVERSE. The pose stored only skinning matrices, so the
         * bone's actual placement had to be recovered by undoing the inverse-bind — `skin *
         * inverse(OffsetMatrix)` — once per call, per socket, per frame. The component-space pose is a
         * thing the pipeline now has, so the question is answered instead of reconstructed.
         * Multiply by the entity's world matrix for a world socket. Identity if the index is out of range.
         */
        [[nodiscard]] glm::mat4 GetBoneModelMatrix( uint32_t boneIndex ) const;

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

        // --- Pose authoring: a separate, EDITABLE pose ----------------------------------------------------
        // The rig's per-bone LocalBindTransform is the shared REST pose; posing a bone to author a clip must
        // NOT mutate it. This is a second LocalPose, independent of bind and of playback, that the editor
        // gizmo writes to and the Sequencer keys from. Init = bind. Normal playback (Update/SetTime) is
        // UNAFFECTED by it — only ApplyLocalPose() renders it into the skinning matrices.
        void                    SetBoneLocalPose( uint32_t boneIndex, const glm::mat4& localTransform );
        [[nodiscard]] glm::mat4 GetBoneLocalPose( uint32_t boneIndex ) const; // identity if out of range
        // Loads `clip`'s sampled LOCAL transforms at `time` into the authoring pose (bind for untracked
        // bones), so the user can edit an existing keyed pose and re-key from it.
        void SampleClipIntoLocalPose( const AnimationClip& clip, float time );
        // Rebuilds GetPose() from the authoring pose, ignoring any playing clip — call after editing it to
        // show the posed skeleton in the viewport.
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
        // Layers run THROUGH a crossfade: the base they fold over is the blend itself. AddLayer returns the
        // new layer index; the setters no-op on an out-of-range index.
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
        /// Whether bone `boneIndex` is inside layer `index`'s mask. An empty mask means every bone.
        [[nodiscard]] bool IsBoneInLayerMask( int index, uint32_t boneIndex ) const;

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

        /// Runs every stage in m_Stages over m_EvaluatedPose, then resolves it into m_Skinning.
        void EvaluatePipeline();

        /// PoseStage::Source — the base clip, or the crossfade of current and next.
        void EvaluateSource( LocalPose& pose ) const;

        /// PoseStage::Layers — folds each active layer over `pose`, in LOCAL space, per masked bone.
        void EvaluateLayers( LocalPose& pose ) const;

        /// m_EvaluatedPose -> m_Skinning (and invalidates the component-space cache behind it).
        void PublishPose();

        /// Adds/removes PoseStage::Layers so the list matches whether there is anything to layer.
        void SyncLayerStage();

        // How far the crossfade has run, 0..1. Derived rather than stored: the alpha and the clock cannot
        // disagree if there is only one of them.
        [[nodiscard]] float BlendAlpha() const
        {
            return m_IsBlending ? glm::clamp( m_BlendTime / m_BlendDuration, 0.0f, 1.0f ) : 0.0f;
        }

        /// Local (parent-relative) transform of `boneIndex` driven by `clip` at `time`, or the bind-pose
        /// local when the clip has no track for it. Straight from the clip's TRS keys — the matrix this
        /// used to build, only to be decomposed again by the next step, is gone.
        [[nodiscard]] BoneTransform SampleLocalTransform( const AnimationClip* clip, uint32_t boneIndex,
                                                          float time ) const;

        /// The base pose's local transform for one bone: the current clip, or current -> next blended by
        /// BlendAlpha(). The ONE answer to "what is the base pose right now", shared by the source stage
        /// and by layer composition, so the two cannot have different opinions.
        [[nodiscard]] BoneTransform BlendedBaseLocal( uint32_t boneIndex ) const;

        /// Returns the clip track that drives skeleton bone `boneIndex`, matched by bone NAME (not by the
        /// clip's own bone index). This lets a clip authored against a differently-ordered or skinless
        /// export of the same rig still drive the correct bones. Built lazily per clip, and rebuilt whenever
        /// the clip's own track storage has been replaced under it — see TrackBinding.
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
            static constexpr uint32_t NO_TRACK = UINT32_MAX;

            const BoneTrack*      TracksData = nullptr; ///< clip->Tracks.data() at build time
            size_t                TrackCount = 0;       ///< clip->Tracks.size() at build time
            uint32_t              Revision   = 0;       ///< clip->TrackRevision at build time
            std::vector<uint32_t> ByBone;               ///< skeleton bone index -> track INDEX, or NO_TRACK
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

        // Active animation layers, folded over the base pose by PoseStage::Layers.
        std::vector<AnimationLayer> m_Layers;

        std::vector<PoseStage> m_Stages;

        // The rig at rest, decomposed ONCE at construction. Every untracked bone in every clip and every
        // additive layer's reference reads it; it used to be decomposed from its matrix on every access.
        LocalPose m_BindPose;

        // Editable pose for authoring (init = bind). Independent of the rig's bind pose and of clip
        // playback; only ApplyLocalPose() renders it. See the pose-authoring API above.
        LocalPose m_AuthoringPose;

        // What the pipeline produced this frame, in local space.
        LocalPose m_EvaluatedPose;

        // m_EvaluatedPose resolved through the parent chain. Declared AFTER m_EvaluatedPose: it holds a
        // reference to it, and a member initialised before its referent is a dangling one.
        ComponentPose m_Component;

        SkinningMatrices m_Skinning;
    };
} // namespace Desert::Animation
