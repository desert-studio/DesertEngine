#pragma once

#include <entt/entt.hpp>

#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/UUID.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

#include <CoreReflection/ReflectionMacros.hpp>

#include <Engine/Animation/AnimationUpdateRate.hpp>
#include <Engine/Animation/RootMotionMode.hpp>
#include <Engine/Assets/Common.hpp>

namespace Desert
{
    namespace Animation
    {
        class Animator;     // Animator.hpp: the component owns one, it does not need its definition
        struct NotifyEvent; // Animator.hpp
    } // namespace Animation
    namespace Animation
    {
        class ClipBindingCache;
        namespace Graph
        {
            struct AnimGraph;
            class Evaluator;
        } // namespace Graph
    } // namespace Animation
} // namespace Desert

namespace Desert::ECS
{
    struct AnimationComponent
    {
        // active Animator (runtime instance)
        std::unique_ptr<Animation::Animator> Animator;

        // current name (debug / editor)
        std::string CurrentClip;

        bool Playing = true;
        bool Loop    = true;

        float PlaybackSpeed = 1.0f;

        /**
         * @brief UE's `bUpdateAnimationInEditor`: whether this component advances in the EDITOR world (Edit
         *        mode). Off by default, as in UE — an edited level holds still while it is being laid out;
         *        Play always advances. AUTHORED (scene + prefab block); read by AnimationECSSystem through
         *        Animation::AnimationAdvanceSeconds. Scrubbing Time in Details poses the character either way.
         */
        bool UpdateAnimationInEditor = false;

        /**
         * @brief UE ERootMotionMode (Animation/RootMotion.hpp). AUTHORED (scene + prefab block, by name; absent
         *        = NoExtraction, UE's default). With RootMotionFromEverything the Animator locks the root bone
         *        and AnimationECSSystem hands its movement, in world space, to the CharacterControllerComponent
         *        of the entity's parent — consumed by CharacterMovement::Step as the planar velocity, UE
         *        PhysWalking's root-motion velocity — or, with no controller, moves this entity's
         *        TransformComponent itself. Play (and UpdateAnimationInEditor) only: a scrub moves nothing.
         */
        Animation::RootMotionMode RootMotion = Animation::RootMotionMode::NoExtraction;

        // Notify events of the Animator THIS frame (instant markers crossed, notify states begun / ended).
        // Filled by AnimationECSSystem, drained + dispatched to the entity's scripts (OnAnimationNotify /
        // OnAnimationNotifyBegin / OnAnimationNotifyEnd) by ScriptSystem. Transient.
        std::vector<Animation::NotifyEvent> PendingNotifies;

        /**
         * @brief The `.danimgraph` this entity plays — a data-driven state machine that PICKS the clip
         *        from live parameters. AUTHORED, and the only half of the graph a scene file states.
         *
         * IT USED TO BE A JSON STRING INSIDE THE ENTITY. `AnimationComponentSer::GraphJson` carried
         * `Animation::Graph::Serialize(*Graph)` verbatim, so the graph had no identity of its own: two
         * characters could not share one walk graph, and copying it copied a blob that then drifted.
         *
         * AND THE COMMENT THAT STOOD HERE WAS FALSE BY THE TIME IT MATTERED. It said the schema question
         * could wait because "today the repository has zero such scenes and the window is open" — the
         * repository had 10 scenes carrying an `Animation` block and 6 non-empty `GraphJson` blobs across
         * three of them, all three load-bearing for a test suite. The window had closed; the step that
         * closed it is `kSceneVersionAnimGraphAsset` (21) and it converted those three.
         */
        Assets::AssetHandle GraphAsset;

        /**
         * @brief The graph object itself. TRANSIENT, and SHARED with every other entity naming the same
         *        file: it is `AnimGraphAsset`'s own `shared_ptr`, handed over by AnimationECSSystem.
         *
         * NOT A COPY, deliberately. A copy per entity would mean an edit in the Anim Graph window reached
         * exactly one of the characters using that graph, which is the defect the asset was made to end.
         * The EVALUATORS are per entity — each holds its own copy of the graph and its own live parameter
         * values — so two characters share a graph and still stand in different states.
         */
        std::shared_ptr<Animation::Graph::AnimGraph> Graph;
        std::shared_ptr<Animation::Graph::Evaluator> GraphEvaluator; // transient runtime state
        // TRANSIENT: this entity's clips resolved against its rig (AnimationLibrary.hpp), asked again only
        // when the library's revision or the rig moves. Shared, not unique, for the incomplete type here.
        std::shared_ptr<Animation::ClipBindingCache> ClipBindings;

        /**
         * @brief Parameter writes a script has asked for and the graph has not consumed yet. TRANSIENT.
         *
         * WHY AN INTENT AND NOT A DIRECT WRITE INTO THE EVALUATOR. The evaluator does not exist until
         * AnimationECSSystem has seen this entity WITH a loaded skinned mesh — so a script setting a
         * parameter in `OnStart`, or on any frame before an async mesh load finishes, would be writing into
         * a null. Swallowing that is the silent no-op this whole task is about; refusing it would make the
         * feature depend on asset timing the script author cannot see. The queue makes the write survive
         * until there is something to apply it to.
         *
         * It is also the shape this tree already uses for script intent —
         * `CharacterControllerComponent::MoveInput` / `JumpRequested` are written by Lua and executed by a
         * system that runs later — so the ordering argument is made once, in one place, for both.
         *
         * THE NAME AND THE TYPE ARE VALIDATED AT THE WRITE, not here: the refusal has to name the script
         * that made the mistake, and by the time this queue is drained that caller is gone.
         *
         * IT CARRIES NO TYPE, and that is deliberate rather than economical. A type field here would be a
         * SECOND statement about what a parameter is, and the graph already makes the first one
         * (`Parameter::Type`, chosen by an artist in the panel). The drain reads the declaration it is
         * about to write into, so the two cannot drift; a copy travelling in the queue could.
         */
        struct PendingGraphParam
        {
            std::string Name;
            float       Value = 0.0f; // bool as 0/1, int as a whole number — read through the declaration
        };
        std::vector<PendingGraphParam> PendingGraphParams;

        /**
         * @brief The `.danimgraph`s whose implemented layers answer this entity's LinkedAnimLayer nodes, in
         *        link order (UE: the Default Linked Layers of the AnimBP plus what LinkAnimClassLayers /
         *        UnlinkAnimClassLayers did since). AUTHORED — the scene states it — and the one list both the
         *        Details default and a script's `linkAnimLayers` write, so "what is linked" has one home.
         *
         * Applied by AnimationECSSystem whenever the entity's pose graph is set or the list / a listed
         * graph changes: every link is undone and the list is linked again in order, so a later entry
         * replaces an earlier one's interfaces exactly as a later LinkAnimClassLayers does. A GUID and not
         * a name, because a link is identified by its graph (UE: its class) and two files may share a name.
         */
        std::vector<Assets::AssetHandle> LinkedLayerGraphs;

        /// What the Animator's links were last built from: per entry of LinkedLayerGraphs its GUID, graph
        /// object and asset revision. TRANSIENT, plain numbers (not references; see BuiltGraphSource).
        struct AppliedLayerLink
        {
            uint64_t                           Guid                                        = 0;
            const Animation::Graph::AnimGraph* Graph                                       = nullptr;
            uint32_t                           Revision                                    = 0;
            bool                               operator==( const AppliedLayerLink& ) const = default;
        };
        std::vector<AppliedLayerLink> AppliedLayerLinks;

        /**
         * @brief What this entity's evaluator was built FROM. TRANSIENT, and the same shape as
         *        BuiltRigSource/BuiltRigRevision below.
         *
         * THERE IS NO `GraphRevision` ON THE COMPONENT ANY MORE, and its removal is the point. It was
         * bumped by whoever edited the graph — which could only ever be the component in front of the
         * editor, so a graph shared by two characters would have re-synced ONE of them and left the other
         * evaluating the previous shape with no sign that anything was stale. The counter belongs to the
         * thing that changes: `AnimGraphAsset::GetRevision()`, one number for every entity that names it.
         */
        // A PLAIN uint64 AND NOT AN AssetHandle, exactly like BuiltRigSource below, and the census is what
        // insists on it: an `AssetHandle` on a component is a REFERENCE the eviction root walk must mark
        // (Desert/Tests/Engine/AssetRoots names any it cannot find). This field is not a reference — it is
        // a fingerprint of the last build, and marking it would keep a graph alive after the slot that
        // named it was cleared. The authored reference is `GraphAsset` above, and that one IS a root.
        uint64_t BuiltGraphSource   = 0; // the handle the evaluator was built from
        uint32_t BuiltGraphRevision = 0; // the asset revision it was built at

        /**
         * @brief What the Animator's CURRENT control-rig stage was built from. TRANSIENT, and the same
         *        shape as BuiltGraphRevision above.
         *
         * A `ControlRigStage` holds bone INDICES resolved against one skeleton, out of a file whose every
         * reference is a NAME. Rebuilding it per frame would throw away the animator's live control poses
         * sixty times a second — the manipulator would be undraggable — and resolving nothing would leave a
         * hot-reloaded rig, a re-pointed slot or a swapped mesh silently running the old rig.
         *
         * So the three things a build depends on are remembered, and a rebuild happens when any of them
         * moves: the handle (the author picked another rig), the asset's revision (the file was edited on
         * disk) and the skeleton's signature (this entity's mesh changed under it).
         *
         * It lives HERE and not beside the handle in ControlRigComponent because it describes the live
         * Animator, which is this component's own property: `ControlRigComponent` is authored data that
         * undo rewrites, duplicate copies and the prefab path rebuilds, and a copied stamp would tell a
         * duplicated entity that a stage it does not have is up to date.
         */
        // A PLAIN INTEGER AND NOT AN `AssetHandle`, and the type is the statement: this is an IDENTITY
        // STAMP, not a reference. Nothing dereferences it, and nothing must keep the rig resident on its
        // account — the component's own `ControlRigData::Rig` is the reference, and SceneAssetRoots marks
        // that one. Spelt as a handle it was indistinguishable from a second, unmarked reference, and
        // Tests/Engine/AssetRoots said so by name on the first sweep.
        uint64_t BuiltRigSource    = 0;
        uint32_t BuiltRigRevision  = 0;
        uint64_t BuiltRigSignature = 0;

        /**
         * @brief The signature of the skeleton `Animator` was constructed on. TRANSIENT, same shape as the
         *        stamps above.
         *
         * A reimport re-reads the rig AT THE SAME ADDRESS (SkeletonAsset::LoadFromFile), so the Animator's
         * `const Skeleton&` stays valid while its bind pose, pose buffers and clip bindings were all sized
         * from the OLD bone list. The address cannot tell a reimported rig from the one it was built on; the
         * signature can. AnimationECSSystem rebuilds the Animator when this differs (UE: the anim instance is
         * re-initialised when the skeleton changes).
         */
        uint64_t BuiltSkeletonSignature = 0;

        /**
         * @brief SKELETAL LOD AND UPDATE RATE OPTIMIZATION, TRANSIENT (UE USkinnedMeshComponent's PredictedLODLevel
         *        and FAnimUpdateRateParameters). The renderer's view of this instance comes back through
         *        LodFeedback (co-owned by the frame's skinned record); AnimationECSSystem evaluates only the bones
         *        of the level drawn and ticks the Animator at the rate its screen size sets.
         */
        std::shared_ptr<Animation::SkinnedLodFeedback> LodFeedback;
        Animation::AnimUpdateRate                      UpdateRate;
        uint64_t                                       ReportedUpdateRate = UINT64_MAX; // last [SkinnedLOD] anim line

        // Out of line (AnimationComponent.cpp): the owned Animator is only forward-declared here, so its
        // destruction and moves are compiled where Animator.hpp is complete.
        AnimationComponent();
        explicit AnimationComponent( std::unique_ptr<Animation::Animator>&& animator );
        AnimationComponent( AnimationComponent&& ) noexcept;
        AnimationComponent& operator=( AnimationComponent&& ) noexcept;
        ~AnimationComponent();
    };
} // namespace Desert::ECS
