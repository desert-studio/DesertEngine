#pragma once

#include <glm/glm.hpp>

#include "Skeleton.hpp"
#include "AnimationClip.hpp"
#include "BoneControl.hpp"
#include "Pose.hpp"
#include "Graph/PoseGraphInstance.hpp"
#include "Rig/ControlRigStage.hpp"
#include "Retarget/RetargetSource.hpp"

#include <Common/Core/Timestep.hpp>

#include <memory>
#include <optional>
#include <string_view>
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
     * Membership is DATA, not a branch: `SetPoseGraph` puts `Graph` into the list and
     * `ClearPoseGraph` takes it out, so the list says what will run rather than the code deciding again every
     * frame. The resolve-to-component-space and multiply-by-offset tail is NOT a stage — it is the pipeline's
     * output and there is nothing after it to reorder against.
     *
     * The list is of enumerators rather than of polymorphic objects deliberately. All the shape buys is
     * ordering and membership, both of which an enum gives. `Rig` (T5.4) was the predicted arrival of "the
     * first stage with state of its own", and it did NOT force the list to become polymorphic: the state
     * went into the stage's own object (`ControlRigStage`) and the enumerator still only says when it runs.
     * The list of objects becomes worth its vtable when two stages of the SAME kind must be ordered against
     * each other, which nothing yet asks for.
     */
    enum class PoseStage : uint8_t
    {
        Source,       ///< the base clip, or the crossfade of the outgoing and incoming clips
        Graph,        ///< the AnimGraph's pose graph, node by node; the Source stage's pose is its base source's
        Controls,     ///< skeletal controls (IK and friends): sparse component-space overrides, blended locally
        Rig,          ///< a control rig: the animator's controls, resolved and written onto the bones they drive
    };

    [[nodiscard]] const char* ToString( PoseStage stage );

    class Animator
    {
    public:
        // ПУБЛИЧНО, И ЭТО НЕ ПОСЛАБЛЕНИЕ. Панель анимационного графа показывает переход как
        // «откуда → куда NN%», и проценту неоткуда взяться, кроме как отсюда: длительность перехода
        // знает граф, а его ПРОГРЕСС — только тот, кто ведёт часы. Читатель есть, и он один.
        //
        // Насколько прошёл кроссфейд, 0..1. Выводится, а не хранится: альфа и часы не могут разойтись,
        // если они одни.
        [[nodiscard]] float BlendAlpha() const
        {
            return m_IsBlending ? glm::clamp( m_BlendTime / m_BlendDuration, 0.0F, 1.0F ) : 0.0F;
        }
        explicit Animator( const Skeleton& skeleton );

        void Play( const AnimationClip& clip, bool loop = true );

        /// THE RVALUE OVERLOAD IS DELETED BECAUSE THIS CLASS KEEPS THE ADDRESS. `Playhead::Clip` is a
        /// NON-OWNING `const AnimationClip*`, which is right — clips live in an `AnimationAsset` that
        /// outlives every animator playing them — but `const&` cannot say so, and a caller reasonably
        /// reads it as "used for the duration of the call".
        ///
        /// It bound happily to a temporary, and the result was a dangling pointer that no test could
        /// see: the animator kept working, reading a stack frame that had been reused. ASan found it
        /// (stack-use-after-scope in `SetTick`, reading `m_Current.Clip->DurationTicks`) and nothing
        /// else would have. Deleting this turns that into a COMPILE error at the call site, which is
        /// the only place that knows how long the clip is going to live.
        void Play( AnimationClip&& clip, bool loop = true ) = delete;
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

        /// The stages that will run, in order. Read-only — the list is maintained by the stage APIs, and a
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

        /// Where the playhead is, ON THE CLIP'S TICK GRID. This is the position; the seconds below are a
        /// readout derived from it.
        [[nodiscard]] FrameTime GetCurrentTick() const
        {
            return m_Current.Time;
        }

        /// The playhead in seconds, for the callers whose question is genuinely about seconds — a UI
        /// readout, the exit-time fraction. Derived, so it cannot disagree with the tick.
        [[nodiscard]] float GetCurrentTime() const
        {
            return m_Current.Clip != nullptr
                        ? static_cast<float>( FrameTimeToSeconds( m_Current.Time, m_Current.Clip->TickRate ) )
                        : 0.0F;
        }

        [[nodiscard]] FrameNumber GetDurationTicks() const
        {
            return m_Current.Clip != nullptr ? m_Current.Clip->DurationTicks : FrameNumber{};
        }

        [[nodiscard]] float GetDuration() const
        {
            return m_Current.Clip != nullptr ? static_cast<float>( m_Current.Clip->DurationSeconds() ) : 0.0F;
        }

        [[nodiscard]] bool IsFinished() const;

        /// Move the playhead to a tick. THE PRIMITIVE — `SetTime` below converts and calls this.
        void SetTick( FrameTime time );

        /// Move the playhead to a number of seconds. Kept because scrubbing arrives as seconds from a UI,
        /// and it lands on the nearest tick rather than flooring: a user who drags onto a key means that
        /// key (see TimeModel.hpp's NearestTick).
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
        // THE BUFFER ITSELF, for the keyer. `GetBoneLocalPose` composes a matrix out of a `BoneTransform`
        // the keyer would immediately decompose again — a round trip that cannot be exact for a
        // non-uniformly scaled rotated bone (`BoneTransform::FromMatrix` folds shear into the rotation),
        // so keying through it would record a pose slightly different from the one on screen.
        [[nodiscard]] const LocalPose& GetAuthoringPose() const
        {
            return m_AuthoringPose;
        }
        /**
         * @brief Replace the WHOLE authoring buffer by value. THE PUT-BACK SIDE OF THE NOTE ABOVE.
         *
         * Undo needs it for the same reason keying needed `GetAuthoringPose`: restoring a pose one bone
         * at a time would go through `SetBoneLocalPose`, whose argument is a MATRIX, and the
         * TRS -> matrix -> TRS round trip is not the identity. An undo built on it would put back a pose
         * slightly different from the one the animator started the drag with -- every drag, and
         * compounding across a stack of them -- which is a round trip that LOOKS like the original
         * rather than being it.
         *
         * Refuses a pose of the wrong length, naming both counts, rather than resizing to fit: index-for-
         * index with `Skeleton::GetBones()` is the one thing `LocalPose` promises (Pose.hpp), so a buffer
         * of another length is another rig's pose and silently truncating it would scramble this one.
         */
        [[nodiscard]] Common::BoolResultStr SetAuthoringPose( const LocalPose& pose );
        // Loads `clip`'s sampled LOCAL transforms at `time` into the authoring pose (bind for untracked
        // bones), so the user can edit an existing keyed pose and re-key from it.
        void SampleClipIntoLocalPose( const AnimationClip& clip, FrameTime time );
        // Rebuilds GetPose() from the authoring pose, ignoring any playing clip — call after editing it to
        // show the posed skeleton in the viewport.
        void ApplyLocalPose();

        // Returns (and clears) the current clip's notify events since the last call — instant notifies
        // crossed by forward playback (Fire), and notify states entered / left (Begin / End) by playback,
        // a loop, a scrub (SetTick / SetTime) or a clip change. For the ECS to dispatch to scripts; call
        // once per frame after Update.
        std::vector<NotifyEvent> ConsumeNotifyEvents()
        {
            std::vector<NotifyEvent> out;
            out.swap( m_NotifyEvents );
            return out;
        }

        /// The current clip's notify states active at the playhead (UE: the active AnimNotifyStates).
        [[nodiscard]] const std::vector<AnimationNotify>& GetActiveNotifyStates() const
        {
            return m_ActiveStates;
        }

        /**
         * @brief The value of anim curve @p name at the playhead (UE: UAnimInstance::GetCurveValue).
         *
         * During a crossfade the two clips' values are blended by the fade's alpha, a clip without the
         * curve contributing 0 — UE's curve blend. Empty when neither clip carries a keyed curve of that
         * name: "no such curve" is a different answer from 0, and a script must be able to tell.
         */
        [[nodiscard]] std::optional<float> GetCurveValue( std::string_view name ) const;

        // --- The pose graph (the AnimGraph's nodes, UE FAnimNode_Base::Evaluate) ------------------------
        // The graph is evaluated node by node by `Graph::PoseGraphInstance`; the Animator keeps what the nodes
        // cannot: the CLOCK of every source node (a state machine's running clip, a sequence player's clip)
        // and the sampling (and retarget) of its clip, and the graph's parameter values. The BASE source
        // (`Graph::BaseSourceNode`) is the Source stage — Play/CrossFade, with its crossfade, notifies, curves
        // and root motion; every other source node has its own clock here. AnimationECSSystem drives it: which
        // clip each source plays this tick (from Graph::Evaluator) and the parameters.
        //
        // `SetPoseGraph` builds the per-bone tables ONCE and refuses an unplannable graph or a filter bone
        // the skeleton lacks, by name.
        [[nodiscard]] Common::BoolResultStr SetPoseGraph( const Graph::AnimGraph& graph );
        /// Source node `node`'s clip (its clock restarts when the clip changes). Ignored for the base source
        /// (the Source stage plays it), for a node that is no source and with no graph set.
        void SetPoseGraphSource( size_t node, const AnimationClip& clip, bool loop = true );
        /// Deleted for `Play`'s reason: the source keeps the clip's address.
        void SetPoseGraphSource( size_t node, AnimationClip&& clip, bool loop = true ) = delete;
        /// The live value of a declared graph parameter (a pin bound to it reads it). Ignored for an undeclared
        /// name: Graph::Evaluator's setters have refused it by name already.
        void SetPoseGraphParameter( std::string_view name, float value );
        void ClearPoseGraph();
        /// The graph the stage runs, or nullptr when none is set.
        [[nodiscard]] const Graph::AnimGraph* GetPoseGraph() const
        {
            return m_PoseGraph ? &m_PoseGraph->Instance.Graph() : nullptr;
        }

        // --- Linked anim layers (UE LinkAnimClassLayers / UnlinkAnimClassLayers) -------------------------
        // `implementation`'s layer graphs now answer the pose graph's LinkedAnimLayer nodes of the interfaces
        // it implements — a weapon swaps a layer without an edit to the character's graph. Refuses with no
        // pose graph set (the host the interfaces are checked against) and every LinkedLayerTable::Link
        // refusal, by name. Links outlive a re-set pose graph (they resolve by name at evaluation); each
        // linked layer's sequence players have their own clocks here, fed by SetLinkedLayerSource.
        [[nodiscard]] Common::BoolResultStr LinkLayers( const Graph::AnimGraph& implementation );
        void                                UnlinkLayers( const Graph::AnimGraph& implementation );
        [[nodiscard]] const Graph::LinkedLayerTable& GetLinkedLayers() const
        {
            return m_LinkedLayers;
        }
        /// Source node `node` of the layer graph in `slot` of GetLinkedLayers() plays `clip`.
        void SetLinkedLayerSource( size_t slot, size_t node, const AnimationClip& clip, bool loop = true );
        void SetLinkedLayerSource( size_t slot, size_t node, AnimationClip&& clip, bool loop = true ) = delete;

        // --- Skeletal controls (IK and friends) ---------------------------------------------------------
        // A control is a pose -> pose operator that writes a SPARSE set of bones and leaves the rest of the
        // pose untouched; see BoneControl.hpp for the contract and why the base class owns the blend. They
        // run after the layers, in list order, because a control's job is to correct the pose that the
        // animation produced — running one before the clip that overwrites its bones would be writing into
        // a buffer that is about to be filled again.
        //
        // The stage joins and leaves the pipeline with the list (SyncStages), exactly as Graph does.
        int                  AddControl( std::unique_ptr<BoneControl> control );
        void                 RemoveControl( int index );
        void                 ClearControls();
        [[nodiscard]] size_t GetControlCount() const
        {
            return m_Controls.size();
        }
        /// Non-owning, for configuring a control after it has been added. Null for an out-of-range index.
        [[nodiscard]] BoneControl* GetControl( int index );

        // --- The control rig (T5.4) ---------------------------------------------------------------------
        //
        // THE LAST STAGE, AND REPORT 05 §658 IS WHY. Sequencer does not put a Control Rig inside the
        // character's AnimGraph; it wraps the whole graph in a layer instance whose input pose IS that
        // graph's output, so the rig runs "after the AnimBP produces its pose and before the pose reaches
        // the mesh, as a post-process stack". `Source` + `Graph` + `Controls` are our AnimBP — the clips,
        // the layering and the skeletal-control stack UE also puts inside it — so the rig goes after all
        // three, and an animator's hand-authored control beats the procedural IK on a bone they share.
        // That is the intended reading, not an accident of push_back order: see SyncStages.
        //
        // AT MOST ONE. UE's layer instance can stack several rig nodes, and every reason to stack them
        // (additive rig layers, ControlRig.cpp:571-714) is on report 01's "defer it" list. A second rig is
        // an ordering question nobody has asked yet.
        [[nodiscard]] Common::BoolResultStr AttachRig( std::unique_ptr<ControlRigStage> rig );
        void                                DetachRig();
        /// Non-owning, for the tool that drives the operator's inputs (report 05 §658). Null when none.
        [[nodiscard]] ControlRigStage*       GetRig();
        [[nodiscard]] const ControlRigStage* GetRig() const;

        // --- The retarget (T6.2) ------------------------------------------------------------------------
        //
        // NOT A STAGE, AND THAT IS THE DECISION. Every other optional step of this pipeline is an
        // enumerator in `m_Stages` because it has an ORDER to be decided against the others. A retarget has
        // none: it does not add a step, it changes WHICH RIG the source step samples and which rig the
        // layered blend samples its layers' clips on. An enumerator that could only ever sit at exactly one place
        // is a second spelling of `m_Retarget != nullptr`, and two spellings of one fact is how they come to
        // disagree.
        //
        // WHAT IT DOES TO THE PIPELINE, in full:
        //   Source   the base clip (and the crossfade's second clip) is sampled on the SOURCE rig, then
        //            retargeted onto this one. Both clips, because both come from the same library through
        //            the same component and are therefore on the same rig by construction.
        //   Graph    each other source node's clip is sampled and retargeted the same way before it is blended.
        //            Not retargeting them would blend source-rig local transforms straight onto target bones —
        //            exactly the proportion defect this tier exists to remove — and there is no third
        //            possibility, because one component names one source rig for the whole entity.
        //   Controls, Rig   untouched. Both operate on the target rig's own pose and always did.
        //
        // AT MOST ONE, for `AttachRig`'s reason: a second source rig is an ordering question nobody has
        // asked, and the component authors one handle.
        [[nodiscard]] Common::BoolResultStr AttachRetarget( std::unique_ptr<Retarget::RetargetSource> retarget );
        void                                DetachRetarget();
        [[nodiscard]] Retarget::RetargetSource*       GetRetarget();
        [[nodiscard]] const Retarget::RetargetSource* GetRetarget() const;

    private:
        struct ClipPlayback
        {
            const AnimationClip* Clip = nullptr;
            /// ON THE CLIP'S OWN TICK GRID, carrying the sub-tick. The float seconds this replaces were
            /// advanced by `Time += dt * tps` and wrapped with `fmod`, so both the addition and the wrap
            /// lost a little every frame and the loss grew with the number already in the accumulator.
            FrameTime            Time;
            bool                 Loop = true;

            bool IsValid() const
            {
                return Clip != nullptr;
            }
        };

        /// The pose graph, its source clocks (one per node; the base source's and the blend nodes' unused), the
        /// parameter values (parallel to the graph's Parameters) and the per-frame scratch.
        struct PoseGraphState
        {
            Graph::PoseGraphInstance  Instance;
            std::vector<ClipPlayback> Sources;
            std::vector<float>        Parameters;
            int                       BaseSource = -1;
            Graph::GraphPose          Out;
        };

    private:
        // DEFINED BELOW, next to the `TrackBinding` it names. Declared here because the sampling functions
        // take it by reference and a reference needs no complete type — which is what lets the definition
        // stay beside the cache it is a view of, rather than being dragged up here away from it.
        struct RigSampling;

        void UpdatePlayback( ClipPlayback& playback, float deltaTime );

        /// Runs every stage in m_Stages over m_EvaluatedPose, then resolves it into m_Skinning.
        void EvaluatePipeline();

        /// PoseStage::Source — the base clip, or the crossfade of current and next. NOT const: under a
        /// retarget this samples into the source scratch and runs the retargeter, both of which are state.
        void EvaluateSource( LocalPose& pose );

        /// PoseStage::Graph — evaluates the pose graph; its base source reads `pose` (the Source stage's), every
        /// other source samples its own clip. NOT const for EvaluateSource's reason.
        void EvaluateGraph( LocalPose& pose );
        /// Sizes m_LinkedSources to m_LinkedLayers after a link or unlink.
        void RebuildLinkedClocks();

        /// PoseStage::Controls — runs each control over `pose`. NOT const: a control reads the pose in
        /// component space, which is a cache fill on m_Component, and writes back through the blend.
        void EvaluateControls( LocalPose& pose );

        /// PoseStage::Rig — the rig reads the pose the stages above produced and writes its driven bones
        /// back into it. Only reached when a rig is attached, which is the stage's membership rule.
        void EvaluateRig( LocalPose& pose );

        /// m_EvaluatedPose -> m_Skinning (and invalidates the component-space cache behind it).
        void PublishPose();

        /// Rebuilds m_Stages so its MEMBERSHIP matches the lists that feed the optional stages, in the
        /// pipeline's fixed order. See the definition for why this is a rebuild and not an insert.
        void SyncStages();

        /// Local (parent-relative) transform of `boneIndex` driven by `clip` at `time`, or the bind-pose
        /// local when the clip has no track for it. Straight from the clip's TRS keys — the matrix this
        /// used to build, only to be decomposed again by the next step, is gone.
        /// STATIC SINCE A25, for `ResolveTrack`'s reason: the rest pose an untracked bone falls back to
        /// is `rig.Rest` now and not `m_BindPose`, so nothing here reads a member. `BlendedBaseLocal`
        /// below is NOT static and must not become so — it reads the playheads, which are the Animator's.
        [[nodiscard]] static BoneTransform SampleLocalTransform( const RigSampling& rig, const AnimationClip* clip,
                                                                 uint32_t boneIndex, FrameTime time );

        /// The base pose's local transform for one bone: the current clip, or current -> next blended by
        /// BlendAlpha(). The ONE answer to "what is the base pose right now", shared by the source stage
        /// and by layer composition, so the two cannot have different opinions.
        [[nodiscard]] BoneTransform BlendedBaseLocal( const RigSampling& rig, uint32_t boneIndex ) const;

        /// Returns the clip track that drives skeleton bone `boneIndex`, matched by bone NAME (not by the
        /// clip's own bone index). This lets a clip authored against a differently-ordered or skinless
        /// export of the same rig still drive the correct bones. Built lazily per clip, and rebuilt whenever
        /// the clip's own track storage has been replaced under it — see TrackBinding.
        /// STATIC SINCE A25, AND THAT IS THE SHAPE OF THE CHANGE RATHER THAN AN ANNOTATION. It used to
        /// read `m_Skeleton` and `m_TrackBinding` directly; both now arrive in `rig`, so the function
        /// touches no member and the analyser said so. Leaving it non-static would have hidden the one
        /// property that makes two rigs safe to sample in the same frame: this lookup depends on nothing
        /// but its arguments.
        static const BoneTrack* ResolveTrack( const RigSampling& rig, const AnimationClip* clip,
                                              uint32_t boneIndex );

        /// The rig the pipeline's own stages read: this Animator's skeleton, its bind pose and its clip
        /// binding cache.
        [[nodiscard]] RigSampling TargetSampling() const;

        /// The rig the CLIPS are on when a retarget is attached. Must not be called without one.
        [[nodiscard]] RigSampling SourceSampling() const;

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

        /**
         * @brief WHICH RIG A CLIP IS BEING SAMPLED AGAINST, and the two things that answer needs.
         *
         * A retarget makes the Animator sample clips on a rig that is NOT its own, and the three functions
         * that do the sampling — track resolution, the untracked-bone fallback, and the crossfade blend —
         * each needed the rig to be a parameter rather than `m_Skeleton`. Passing this one struct instead
         * of a mode flag is what keeps `BlendedBaseLocal` the ONE answer to "what is the base pose right
         * now" (see its comment) while letting it be asked about two rigs: a second copy of the blend for
         * the source rig is exactly how the source stage and layer composition would come to have
         * different opinions about a crossfade.
         */
        struct RigSampling
        {
            const Skeleton&  Rig;
            const LocalPose& Rest; ///< what a bone with no track in this clip reads
            std::unordered_map<const AnimationClip*, TrackBinding>& Binding;
        };

        const Skeleton& m_Skeleton;

        // clip -> its binding. See ResolveTrack and TrackBinding.
        mutable std::unordered_map<const AnimationClip*, TrackBinding> m_TrackBinding;

        // The same cache, built against the SOURCE rig. A SECOND MAP AND NOT A SECOND ENTRY IN THE FIRST:
        // the key is the clip, and one clip is legally sampled on both rigs in the same frame (the base on
        // the source, an editor's authoring sample on the target), so one map would hand back a binding
        // built for the wrong rig's bone order. Cleared by Attach/DetachRetarget, because the rig it was
        // built against is exactly what those two change.
        mutable std::unordered_map<const AnimationClip*, TrackBinding> m_SourceTrackBinding;

        ClipPlayback m_Current;
        ClipPlayback m_Next;

        bool  m_IsBlending    = false;
        float m_BlendTime     = 0.0F;
        float m_BlendDuration = 0.0F;

        float m_PlaybackSpeed = 1.0F;

        // Notify events of the current clip not yet drained by ConsumeNotifyEvents().
        std::vector<NotifyEvent> m_NotifyEvents;
        // The current clip's notify states active at its playhead — see StepNotifyStates.
        std::vector<AnimationNotify> m_ActiveStates;

        /// Ends every active state: the clip they belong to stops being the current one.
        void RetireNotifyStates();

        // The pose graph, run by PoseStage::Graph; absent = the stage is not in the list.
        std::optional<PoseGraphState> m_PoseGraph;
        Graph::LinkedLayerTable       m_LinkedLayers;
        /// Per slot of m_LinkedLayers, one clock per node of its layer graph.
        std::vector<std::vector<ClipPlayback>> m_LinkedSources;

        // Skeletal controls, run by PoseStage::Controls. `unique_ptr` because a control is polymorphic and
        // holds its own resolved bone indices; the Animator is its one owner and outlives it by definition.
        std::vector<std::unique_ptr<BoneControl>> m_Controls;

        // The control rig, run by PoseStage::Rig. `unique_ptr` and not a value member because a rig is the
        // optional thing an entity has or has not, and "has not" must not cost every Animator a
        // ControlHierarchy's four vectors; null IS the answer to "is there a rig", with no second flag to
        // disagree with it.
        std::unique_ptr<ControlRigStage> m_Rig;

        // The source rig and its retargeter, when this Animator's clips are authored on another rig. Null
        // IS the answer to "is there a retarget", with no second flag to disagree with it — and null costs
        // nothing, which a `Skeleton` value member would not.
        std::unique_ptr<Retarget::RetargetSource> m_Retarget;

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
