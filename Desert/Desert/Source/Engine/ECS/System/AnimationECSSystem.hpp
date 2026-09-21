#pragma once

#include "System.hpp"

#include <Engine/ECS/Components.hpp>
// SELF-CONTAINED, AND IT WAS NOT. `Animation::AnimationLibrary` is used through a pointer and CALLED, and
// `Mesh`/`SkinnedMesh` are cast between — all three arrived transitively, so this header only compiled
// because of who happened to include it first. The analyser compiles a header on its own and said so the
// moment this file was edited at all: "no type named 'AnimationLibrary'", and two static_casts between
// classes "not related by inheritance" because only the forward declarations were visible.
#include <Engine/Animation/AnimationLibrary.hpp>
#include <Engine/Animation/Graph/AnimGraph.hpp>
#include <Engine/Animation/Skeleton.hpp>
#include <Engine/Animation/TwoBoneIKControl.hpp>
#include <Engine/Animation/Retarget/RetargetSource.hpp>
#include <Engine/Animation/Rig/ControlRigStage.hpp>
#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/AnimGraphAsset.hpp>
#include <Engine/Assets/ControlRigAsset.hpp>
#include <Engine/Assets/RetargetAsset.hpp>
#include <Engine/Assets/Serialization/ControlRig.hpp>
#include <Engine/Assets/Serialization/Retarget.hpp>
#include <Engine/Geometry/SkinnedMesh.hpp>

#include <Common/Core/Logger.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <string>
#include <unordered_map>

namespace Desert::ECS
{
    class AnimationECSSystem : public System
    {
    public:
        /**
         * @param assetManager where a `ControlRigComponent`'s handle is resolved to a parsed `.derig`. May
         *        be null: a host with no asset manager simply has no rigs, and the refusal says so once
         *        rather than crashing on the first entity that names one.
         */
        AnimationECSSystem( Animation::AnimationLibrary* animationLibrary, Assets::AssetManager* assetManager )
             : m_AnimationLibrary( animationLibrary ), m_AssetManager( assetManager )
        {
        }

        void Update( entt::registry& registry, Graphic::Render::RenderCommandBuffer& /*renderCommandBuffer*/,
                     const Common::Timestep& ts ) override
        {
            // Editor PREVIEW: the gameplay timestep is 0 in Edit mode (gameplay frozen), but animation should
            // still preview when "Playing" is on. So advance by a real wall-clock delta when the gameplay ts
            // is ~0; use the gameplay ts in Play mode. Clamped to avoid huge jumps after a stall.
            const auto  now    = std::chrono::steady_clock::now();
            float       realDt = m_HasLast ? std::chrono::duration<float>( now - m_LastTime ).count() : 0.0f;
            m_LastTime         = now;
            m_HasLast          = true;
            realDt             = std::min( realDt, 0.1f );
            const float effectiveSeconds = ts.GetSeconds() > 1e-6f ? ts.GetSeconds() : realDt;
            const Common::Timestep animTs( effectiveSeconds );
            auto view = registry.view<ECS::SkinnedMeshComponent, ECS::AnimationComponent>();

            for ( auto entity : view )
            {
                auto& skinnedMesh = view.get<ECS::SkinnedMeshComponent>( entity );
                auto& anim        = view.get<ECS::AnimationComponent>( entity );

                // Editor-built runtime rig (Convert to Skinned) has no MeshHandle — prefer it (mirrors the
                // render/pick paths) so a converted mesh can still animate.
                Desert::Mesh* meshBase =
                     skinnedMesh.RuntimeMesh
                          ? static_cast<Desert::Mesh*>( skinnedMesh.RuntimeMesh.get() )
                          : Runtime::ResourceRegistry::GetMeshService()->Get( skinnedMesh.MeshHandle );

                if ( !meshBase || !meshBase->IsSkinned() )
                {
                    anim.Animator.reset();
                    continue;
                }

                auto skinnedMeshPtr = static_cast<Desert::SkinnedMesh*>( meshBase );

                if ( !anim.Animator )
                {
                    anim.Animator = std::make_unique<Animation::Animator>( skinnedMeshPtr->GetSkeleton() );
                }

                const Animation::Skeleton& skeleton = skinnedMeshPtr->GetSkeleton();

                // FIRST OF THE THREE SYNCS, because it is first in the pipeline it feeds: a retarget
                // changes which rig the SOURCE stage samples, and everything below reads the pose that
                // stage produces. It is also the one that can change the pose's whole provenance, so a
                // frame in which it was not yet applied would be a frame of the un-retargeted character.
                SyncRetarget( registry, entity, *anim.Animator, skeleton );

                // Before either playback path, because a control is a stage of the same pipeline and the
                // pipeline runs inside Animator::Update below.
                SyncSkeletalControls( registry, entity, *anim.Animator, skeleton );

                // AFTER the controls and before playback, for the same reason: the rig is the LAST stage of
                // the same pipeline (Animator::SyncStages), and the pipeline runs inside Animator::Update
                // below. Attaching after the update would put the rig one frame behind the pose it operates on.
                SyncControlRig( registry, entity, anim, *anim.Animator, skeleton );

                // THE RIG A CLIP IS LOOKED UP AGAINST, which a retarget changes and which every clip
                // lookup below has to use. `ClipDrivesRig` binds on the clip's bone NAMES, so asking the
                // TARGET rig about a foreign clip refuses exactly the clips a retarget exists to play —
                // the middle-link defect, introduced by the change that makes retargeting reachable.
                // SyncRetarget above has already attached or detached, so this is settled for the frame.
                const Animation::Skeleton& clipRig = anim.Animator->GetRetarget() != nullptr
                                                          ? anim.Animator->GetRetarget()->GetSourceSkeleton()
                                                          : skeleton;

                // BEFORE the graph path, because it is what puts a graph there: the entity names a
                // `.danimgraph` and this is where that handle becomes the object below.
                const uint32_t graphRevision = SyncAnimGraph( anim );

                // AnimGraph path: the state machine PICKS the clip; the Animator just plays it. Falls back to
                // the CurrentClip path below when no graph is attached.
                if ( anim.Graph && !anim.Graph->States.empty() )
                {
                    if ( !anim.GraphEvaluator )
                    {
                        anim.GraphEvaluator     = std::make_shared<Animation::Graph::Evaluator>( *anim.Graph );
                        anim.BuiltGraphRevision = graphRevision;
                    }
                    else if ( anim.BuiltGraphRevision != graphRevision )
                    {
                        // Re-sync after an editor edit WITHOUT resetting the active state / live parameters.
                        anim.GraphEvaluator->SyncGraph( *anim.Graph );
                        anim.BuiltGraphRevision = graphRevision;
                    }

                    ReportGraphStructure( *anim.GraphEvaluator );

                    // THE ORDER, DECIDED. Script writes are applied HERE — after the evaluator exists and
                    // BEFORE Update() below reads the conditions — so a queued parameter always acts on the
                    // very next graph tick and never on the one after it.
                    //
                    // That next tick is the NEXT FRAME, and this is the decision rather than the accident:
                    // ScriptSystem is registered AFTER AnimationECSSystem (EditorLayer::BuildSceneSystems,
                    // RuntimeLayer), so a value set in OnUpdate on frame N is drained at the top of frame
                    // N+1. ONE frame of latency, and the alternative was moving a system: the registration
                    // order that produces it is load-bearing twice over — AttachmentSystem must run right
                    // after animation to follow a freshly-posed bone THIS frame, and ScriptSystem must run
                    // before physics so move intent executes THIS frame. Putting animation after scripts
                    // would buy same-frame parameters and hand the notify dispatch the one-frame lag
                    // instead, because notifies travel the other way (Animator -> PendingNotifies ->
                    // ScriptSystem). One frame on a state change is 16 ms and invisible; one frame on a
                    // footstep is an audible desync. Asserted by Tests/Engine/AnimGraphScript so it stays a
                    // decision and not a habit.
                    DrainGraphParams( anim );

                    if ( anim.Playing )
                    {
                        // Clip fraction [0,1] drives exit-time transitions.
                        float       norm = 0.0f;
                        const float dur  = anim.Animator->GetDuration();
                        if ( dur > 1e-4f )
                            norm = anim.Animator->GetCurrentTime() / dur;

                        const auto res = anim.GraphEvaluator->Update( norm );
                        if ( res.Current )
                        {
                            const auto found = m_AnimationLibrary->FindForSkeleton( clipRig, res.Current->Clip );
                            if ( found )
                            {
                                const auto& clip = found.GetValue()->GetClip();
                                const auto* cur  = anim.Animator->GetCurrentClip();
                                if ( !cur || cur->AnimationName != clip.AnimationName )
                                {
                                    if ( res.Changed && res.Blend > 0.0f )
                                        anim.Animator->CrossFade( clip, res.Blend, res.Current->Loop );
                                    else
                                        anim.Animator->Play( clip, res.Current->Loop );
                                }
                            }
                            else
                            {
                                ReportUnplayableState( clipRig, res.Current->Name, res.Current->Clip,
                                                       found.GetError() );
                            }
                            anim.Animator->SetPlaybackSpeed( anim.PlaybackSpeed * res.Current->Speed );
                        }

                        anim.Animator->Update( animTs );
                        anim.PendingNotifies = anim.Animator->ConsumeNotifies();
                    }
                    continue;
                }

                if ( !anim.CurrentClip.empty() )
                {
                    // SAME RULE AS THE PICKER that wrote this name into the component. It used to be an
                    // exact-signature scan here against a tolerant one in the Details panel, so a clip an
                    // artist had just chosen could fail to play with nothing said.
                    const auto found = m_AnimationLibrary->FindForSkeleton( clipRig, anim.CurrentClip );
                    if ( found )
                    {
                        const auto& clip    = found.GetValue()->GetClip();
                        const auto* current = anim.Animator->GetCurrentClip();

                        // Cross-fade on change (smooth idle<->walk<->run) — LocomotionSystem used to do this
                        // itself; now clip selection is data-driven there, so the blend lives here.
                        if ( !current )
                            anim.Animator->Play( clip, anim.Loop );
                        else if ( current->AnimationName != clip.AnimationName )
                            anim.Animator->CrossFade( clip, 0.15f, anim.Loop );
                    }
                    else
                    {
                        ReportUnplayableState( clipRig, "AnimationComponent.CurrentClip", anim.CurrentClip,
                                               found.GetError() );
                    }
                }

                else
                {
                    const auto animations = m_AnimationLibrary->GetForSkeleton( clipRig );

                    if ( !animations.empty() )
                    {
                        const auto& clip = animations.front()->GetClip();

                        anim.CurrentClip = clip.AnimationName;
                        anim.Animator->Play( clip, anim.Loop );
                    }
                    else
                    {
                        // THE T-POSE'S OWN VOICE. This branch is what an entity does when it names no clip
                        // and the library offers none for its rig, and until now it did it in complete
                        // silence — the character stood in its bind pose, every frame, with not one line
                        // anywhere in the process to distinguish "this project has no clips for this rig"
                        // from "the library was never filled", which is exactly the state a packaged game
                        // shipped in. Deduped by the same reporter as the named-clip failures above, so
                        // it costs one line per rig rather than sixty a second.
                        ReportUnplayableState( clipRig, "AnimationComponent (no clip named)", "<any>",
                                               "the library offers no clip for this rig at all." );
                    }
                }

                if ( anim.Playing )
                {
                    anim.Animator->SetLoop( anim.Loop );
                    anim.Animator->SetPlaybackSpeed( anim.PlaybackSpeed );

                    anim.Animator->Update( animTs );

                    // Notify markers crossed this frame -> queued for ScriptSystem to dispatch (assigned, so
                    // a paused/cleared frame leaves it empty and nothing re-fires).
                    anim.PendingNotifies = anim.Animator->ConsumeNotifies();

                    if ( !anim.Loop && anim.Animator->IsFinished() )
                    {
                        anim.Playing = false;
                    }
                }
            }
        }

    private:
        /**
         * @brief Applies the parameter writes a script queued, then clears the queue.
         *
         * The name and the type were validated where the script called (AnimationBindings), so a refusal
         * here means the graph changed under a queued write — a real event with a different cause, and it
         * is reported with the same once-per-distinct-message rule the clip failures next door use.
         */
        void DrainGraphParams( ECS::AnimationComponent& anim )
        {
            if ( anim.PendingGraphParams.empty() )
            {
                return;
            }

            for ( const auto& pending : anim.PendingGraphParams )
            {
                // THE DECLARATION IS READ HERE, not carried in the queue: one statement about what a
                // parameter is, made by the graph, read by whoever is about to write into it.
                const auto& parameters = anim.GraphEvaluator->Graph().Parameters;
                const auto  declared   = std::find_if( parameters.begin(), parameters.end(),
                                                       [&pending]( const Animation::Graph::Parameter& p )
                                                       { return p.Name == pending.Name; } );

                Common::BoolResultStr applied =
                     declared == parameters.end()
                          ? anim.GraphEvaluator->SetFloat( pending.Name, pending.Value ) // refuses, by name
                          : Common::MakeSuccess( true );

                if ( declared != parameters.end() )
                {
                    switch ( static_cast<Animation::Graph::ParamType>( declared->Type ) )
                    {
                        case Animation::Graph::ParamType::Bool:
                            applied = anim.GraphEvaluator->SetBool( pending.Name, pending.Value != 0.0f );
                            break;
                        case Animation::Graph::ParamType::Int:
                            applied = anim.GraphEvaluator->SetInt(
                                 pending.Name, static_cast<int>( std::lround( pending.Value ) ) );
                            break;
                        case Animation::Graph::ParamType::Float:
                            applied = anim.GraphEvaluator->SetFloat( pending.Name, pending.Value );
                            break;
                    }
                }

                if ( !applied.IsSuccess() )
                {
                    ReportOnce( fmt::format( "param:{}:{}", anim.GraphEvaluator->Graph().Name, pending.Name ),
                                applied.GetError() );
                }
            }

            // CLEARED WHETHER OR NOT EACH ONE LANDED. A queue that kept a refused write would re-report it
            // every frame and, worse, would apply a stale value the moment the graph grew the parameter.
            anim.PendingGraphParams.clear();
        }

        /**
         * @brief Says so, once, when a graph's conditions name parameters it does not declare.
         *
         * The read side cannot refuse (Evaluator::GetFloat is called per condition per frame), so the
         * report lives here — at the one place per frame that holds the evaluator and a logger.
         */
        void ReportGraphStructure( const Animation::Graph::Evaluator& evaluator )
        {
            const std::string& error = evaluator.GetStructureError();
            if ( !error.empty() )
            {
                ReportOnce( fmt::format( "structure:{}", evaluator.Graph().Name ), error );
            }
        }

        /// One line per distinct (key, message), because this runs at 60 Hz over every animated entity.
        void ReportOnce( const std::string& key, const std::string& message ) const
        {
            const auto it = m_Reported.find( key );
            if ( it != m_Reported.end() && it->second == message )
            {
                return;
            }
            m_Reported[key] = message;
            LOG_ERROR( "[Animation] {}", message );
        }

        /**
         * @brief AUTHORED DATA IN, LIVE SOLVER OUT — once per frame, per entity.
         *
         * THE COMPONENT IS NOT THE CONTROL, and keeping them apart is the point. The component is four
         * values that undo rewrites, duplicate copies and the prefab path rebuilds; the control is a live
         * object holding bone indices resolved against THIS entity's rig. Putting the control in the
         * component would have handed a duplicated entity its source's resolved indices, which are correct
         * exactly until the two entities have different meshes.
         *
         * The reverse direction matters as much: an entity that LOSES the component must lose the control,
         * or the last authored goal would go on being solved forever with nothing in the editor showing it.
         */
        static void SyncSkeletalControls( entt::registry& registry, entt::entity entity,
                                          Animation::Animator& animator, const Animation::Skeleton& skeleton )
        {
            if ( !registry.has<ECS::TwoBoneIKComponent>( entity ) )
            {
                if ( animator.GetControlCount() > 0 )
                {
                    animator.ClearControls();
                }
                return;
            }

            const auto& ikData = registry.get<ECS::TwoBoneIKComponent>( entity ).Data;

            if ( animator.GetControlCount() == 0 )
            {
                // Named BEFORE it is added, so AddControl resolves the chain the artist authored rather
                // than resolving an empty name and reporting a failure that is one frame stale.
                auto created = std::make_unique<Animation::TwoBoneIKControl>();
                created->SetEndBone( ikData.EndBone );
                animator.AddControl( std::move( created ) );
            }

            auto* twoBonePtr = dynamic_cast<Animation::TwoBoneIKControl*>( animator.GetControl( 0 ) );
            if ( twoBonePtr == nullptr )
            {
                return;
            }

            auto& twoBone = *twoBonePtr;
            if ( twoBone.GetEndBoneName() != ikData.EndBone )
            {
                // RE-RESOLVED ON CHANGE, NOT ON READ (report 03 §1.4). A name the rig does not have leaves
                // the control refusing every solve and saying so once — which is what the artist needs to
                // see — so the result is not re-reported here.
                twoBone.SetEndBone( ikData.EndBone );
                static_cast<void>( twoBone.Resolve( skeleton ) );
            }

            twoBone.SetGoal( ikData.Goal );
            twoBone.SetPoleTarget( ikData.PoleTarget );
            twoBone.SetAlpha( ikData.Alpha );
        }

        /**
         * @brief Hand the entity the graph OBJECT its handle names, and report which revision that is.
         *
         * THE OBJECT IS THE ASSET'S OWN AND IS NOT COPIED. Every entity naming one `.danimgraph` ends up
         * with the same `shared_ptr`, so an edit in the Anim Graph window is the graph all of them
         * evaluate on the next frame. The EVALUATORS stay per entity — each holds its own copy of the
         * graph and its own live parameter values — which is what lets two characters share a graph and
         * still stand in different states.
         *
         * THE RETURNED REVISION IS THE ASSET'S. It used to be `AnimationComponent::GraphRevision`, bumped
         * by whoever edited — which could only ever be the one component in front of the editor, so a
         * shared graph would have re-synced ONE of its entities and left the rest evaluating the previous
         * shape with nothing to show that they were stale. One counter on the thing that changes.
         *
         * AN ENTITY WITH NO HANDLE KEEPS WHATEVER GRAPH IT HAS, and that is deliberate rather than an
         * omission: a graph built in C++ or by a test (`anim.Graph = make_shared<AnimGraph>()`) has no
         * file and no handle, and clearing it here would make "no asset" mean "no graph" — which would
         * break every in-memory user of the state machine to enforce a rule about files. Revision 0 is
         * what such a graph reports, and 0 never re-syncs, which is correct: nothing can have edited it.
         */
        NO_DISCARD uint32_t SyncAnimGraph( ECS::AnimationComponent& anim )
        {
            const Assets::AssetHandle wanted = anim.GraphAsset;
            if ( static_cast<uint64_t>( wanted ) == 0 )
            {
                return 0;
            }

            if ( m_AssetManager == nullptr )
            {
                ReportOnce( "graph-no-manager",
                            "an entity names an anim graph, but this host has no asset manager to resolve "
                            "it through; the entity plays its single clip instead" );
                return 0;
            }

            auto asset = m_AssetManager->FindByHandle<Assets::AnimGraphAsset>( Common::UUID( wanted ) );
            if ( !asset || !asset->IsReadyForUse() )
            {
                // SAID, NOT SWALLOWED, for SyncControlRig's reason: the silent version is a character
                // playing one clip while its scene file plainly names a state machine, which reads as a
                // graph system that does not work.
                ReportOnce( fmt::format( "graph-missing:{}", static_cast<uint64_t>( wanted ) ),
                            fmt::format( "anim graph handle {} is not loaded; the entity naming it plays "
                                         "its single clip instead",
                                         static_cast<uint64_t>( wanted ) ) );
                return 0;
            }

            // Re-pointed rather than compared field by field: the asset replaces its graph object on a
            // hot reload (AnimGraphAsset::Load), so the pointer is the identity of "which graph" and the
            // revision is the identity of "which version of it".
            if ( anim.Graph != asset->GetGraph() )
            {
                anim.Graph = asset->GetGraph();
                // The evaluator was built from the OLD object; the revision compare below rebuilds it.
                anim.BuiltGraphSource = static_cast<uint64_t>( wanted );
            }
            return asset->GetRevision();
        }

        /**
         * @brief THE HANDLE BECOMES A PIPELINE STAGE — and this is the function tier T5 did not have.
         *
         * T5 built a control hierarchy, a manipulator, keying and a stage, every one of them proven by a
         * suite, and no scene could have a rig: `Animator::AttachRig` takes an object somebody has to
         * construct in C++, and nobody constructed one. Everything above this line is that somebody.
         *
         * IT IS THE SAME SHAPE AS `SyncSkeletalControls`, deliberately: authored data in, live solver out,
         * once per frame per entity, with the reverse direction — an entity that LOSES the component loses
         * the stage — given equal weight. A rig left attached after its component went would go on posing
         * the character forever with nothing in the editor showing why.
         *
         * WHAT IT DOES NOT DO IS REBUILD. See `AnimationComponent::BuiltRigSource`: the stage holds the
         * animator's live control poses, so rebuilding it every frame would make the manipulator
         * undraggable, and rebuilding it never would leave a hot-reloaded or re-pointed rig silently
         * running the old one.
         */
        void SyncControlRig( entt::registry& registry, entt::entity entity, ECS::AnimationComponent& anim,
                             Animation::Animator& animator, const Animation::Skeleton& skeleton ) const
        {
            const auto forget = [&anim, &animator]()
            {
                if ( animator.GetRig() != nullptr )
                {
                    animator.DetachRig();
                }
                anim.BuiltRigSource    = 0;
                anim.BuiltRigRevision  = 0;
                anim.BuiltRigSignature = 0;
            };

            if ( !registry.has<ECS::ControlRigComponent>( entity ) )
            {
                forget();
                return;
            }

            const Assets::AssetHandle wanted = registry.get<ECS::ControlRigComponent>( entity ).Data.Rig;
            if ( static_cast<uint64_t>( wanted ) == 0 )
            {
                // An empty slot is the off switch, and it is the ONLY one — there is no second "enabled"
                // flag that could disagree with it (see ControlRigData).
                forget();
                return;
            }

            if ( m_AssetManager == nullptr )
            {
                forget();
                ReportOnce( "rig-no-manager",
                            "an entity names a control rig, but this host has no asset manager to resolve "
                            "it through; the entity is posed by its clip alone" );
                return;
            }

            auto asset = m_AssetManager->FindByHandle<Assets::ControlRigAsset>( Common::UUID( wanted ) );
            if ( !asset || !asset->IsReadyForUse() )
            {
                // SAID, NOT SWALLOWED. The silent version of this is a character that poses from its clip
                // while the scene file plainly names a rig, which is indistinguishable from a rig system
                // that does not work.
                forget();
                ReportOnce( fmt::format( "rig-missing:{}", static_cast<uint64_t>( wanted ) ),
                            fmt::format( "control rig handle {} is not loaded; the entity naming it is "
                                         "posed by its clip alone",
                                         static_cast<uint64_t>( wanted ) ) );
                return;
            }

            const bool current = animator.GetRig() != nullptr &&
                                 anim.BuiltRigSource == static_cast<uint64_t>( wanted ) &&
                                 anim.BuiltRigRevision == asset->GetRevision() &&
                                 anim.BuiltRigSignature == skeleton.GetSignature();
            if ( current )
            {
                return;
            }

            auto stage = std::make_unique<Animation::ControlRigStage>();
            if ( auto built = Assets::Serialization::BuildControlRig( asset->GetData(), skeleton, *stage );
                 !built )
            {
                forget();
                ReportOnce( fmt::format( "rig-build:{}", static_cast<uint64_t>( wanted ) ), built.GetError() );
                return;
            }

            if ( auto attached = animator.AttachRig( std::move( stage ) ); !attached )
            {
                forget();
                ReportOnce( fmt::format( "rig-attach:{}", static_cast<uint64_t>( wanted ) ), attached.GetError() );
                return;
            }

            anim.BuiltRigSource    = static_cast<uint64_t>( wanted );
            anim.BuiltRigRevision  = asset->GetRevision();
            anim.BuiltRigSignature = skeleton.GetSignature();
        }

        /**
         * @brief THE HANDLE BECOMES A SOURCE RIG — and this is the function tier T6.2 did not have.
         *
         * T6.2 built `Retarget/`, measured it against `JPH::SkeletonMapper` and beat it on every row, and
         * no scene could use it: `Retargeter::Initialize` takes a `RetargetSetup` somebody has to fill in
         * in C++, and nobody did. Everything above this line is that somebody. It is the same shape as
         * `SyncControlRig` below and deliberately so — authored data in, live object out, once per frame
         * per entity, with the reverse direction given equal weight.
         *
         * ── THE REBUILD DECISION HAS FOUR FACTS, NOT THREE ───────────────────────────────────────────
         *
         * `SyncControlRig` compares the handle, the asset's revision and this entity's skeleton signature.
         * A retarget has a SECOND SIDE, and its signature is the fact nothing else in the chain would
         * notice moving: re-export the source character and the `.retarget` file, the handle, the revision
         * and the target rig are all unchanged while the bone order under the source rig is not. The
         * answer is asked of the built object itself (`RetargetSource::IsBuiltFrom`) rather than kept in
         * three fields on the component, so there is no `forget()` that can miss one.
         *
         * ── AND WHY THE SOURCE RIG IS RE-RESOLVED RATHER THAN ASSUMED ────────────────────────────────
         *
         * The source `.skeleton` is reachable only through this retarget's own dependency, so an eviction
         * sweep can release it while the retarget itself stays loaded — the state `SkeletonAsset`'s
         * signature comment describes from the mesh's side, where it cost a scene 410 log lines and no
         * character. Asking the asset to resolve again is what makes that recoverable instead of terminal.
         */
        void SyncRetarget( entt::registry& registry, entt::entity entity, Animation::Animator& animator,
                           const Animation::Skeleton& skeleton ) const
        {
            const auto forget = [&animator]()
            {
                if ( animator.GetRetarget() != nullptr )
                {
                    animator.DetachRetarget();
                }
            };

            if ( !registry.has<ECS::RetargetComponent>( entity ) )
            {
                forget();
                return;
            }

            const Assets::AssetHandle wanted = registry.get<ECS::RetargetComponent>( entity ).Data.Retarget;
            if ( static_cast<uint64_t>( wanted ) == 0 )
            {
                // An empty slot is the off switch, and it is the ONLY one — there is no second "enabled"
                // flag that could disagree with it (see RetargetData).
                forget();
                return;
            }

            if ( m_AssetManager == nullptr )
            {
                forget();
                ReportOnce( "retarget-no-manager",
                            "an entity names a retarget, but this host has no asset manager to resolve it "
                            "through; the entity plays its clip on its own rig" );
                return;
            }

            auto asset = m_AssetManager->FindByHandle<Assets::RetargetAsset>( Common::UUID( wanted ) );
            if ( !asset || !asset->IsReadyForUse() )
            {
                // SAID, NOT SWALLOWED, for SyncControlRig's reason: the silent version is a character
                // played on its own rig while the scene file plainly names a retarget, which is
                // indistinguishable from a retargeting system that does not work.
                forget();
                ReportOnce( fmt::format( "retarget-missing:{}", static_cast<uint64_t>( wanted ) ),
                            fmt::format( "retarget handle {} is not loaded; the entity naming it plays its "
                                         "clip on its own rig",
                                         static_cast<uint64_t>( wanted ) ) );
                return;
            }

            const Animation::Skeleton* source = asset->GetSourceSkeleton();
            if ( source == nullptr )
            {
                // Re-resolve rather than give up: see the note above on eviction releasing a rig that is
                // reachable only through this dependency. `ResolveDependencies` is written to be re-run.
                asset->ResolveDependencies( *m_AssetManager );
                source = asset->GetSourceSkeleton();
            }
            if ( source == nullptr )
            {
                forget();
                ReportOnce( fmt::format( "retarget-rig:{}", static_cast<uint64_t>( wanted ) ),
                            fmt::format( "retarget '{}' names source rig '{}' and no loaded skeleton "
                                         "answers it; the entity plays its clip on its own rig",
                                         asset->GetDisplayName(), asset->GetData().SourceSkeleton ) );
                return;
            }

            if ( const auto* current = animator.GetRetarget();
                 current != nullptr && current->IsBuiltFrom( static_cast<uint64_t>( wanted ),
                                                             asset->GetRevision(), source->GetSignature(),
                                                             skeleton.GetSignature() ) )
            {
                return;
            }

            auto setup = Assets::Serialization::BuildRetargetSetup( asset->GetData() );
            if ( !setup )
            {
                forget();
                ReportOnce( fmt::format( "retarget-setup:{}", static_cast<uint64_t>( wanted ) ),
                            setup.GetError() );
                return;
            }

            auto built = Animation::Retarget::RetargetSource::Create(
                 *source, skeleton, setup.ExtractValue(), static_cast<uint64_t>( wanted ),
                 asset->GetRevision() );
            if ( !built )
            {
                forget();
                ReportOnce( fmt::format( "retarget-build:{}", static_cast<uint64_t>( wanted ) ),
                            fmt::format( "retarget '{}': {}", asset->GetDisplayName(), built.GetError() ) );
                return;
            }

            if ( auto attached = animator.AttachRetarget( built.ExtractValue() ); !attached )
            {
                forget();
                ReportOnce( fmt::format( "retarget-attach:{}", static_cast<uint64_t>( wanted ) ),
                            attached.GetError() );
            }
        }

        /**
         * @brief SAYS SO WHEN A STATE CANNOT PLAY. A state whose clip does not resolve used to be a `nullptr`
         *        that the caller stepped over: the character stood still, no log line, nothing for an artist
         *        to search for. That silence is the half of the defect a name-matching fix alone would leave.
         *
         * ONCE PER DISTINCT COMPLAINT, not once per frame — this runs at 60 Hz over every animated entity,
         * and an error repeated 60 times a second is a log nobody reads, which is the same silence wearing a
         * different hat. The key is (rig, state, clip), so a second rig with the same broken state still
         * reports, and a state that starts resolving and breaks again reports again only if the reason
         * changes rigs.
         */
        void ReportUnplayableState( const Animation::Skeleton& skeleton, const std::string& stateName,
                                    const std::string& clipName, const std::string& reason ) const
        {
            ReportOnce( std::to_string( skeleton.GetSignature() ) + '|' + stateName + '|' + clipName,
                        fmt::format( "state '{}' asks for clip '{}' and nothing will play: {} The rig has {} "
                                     "bone(s), signature {}.",
                                     stateName, clipName, reason, skeleton.GetBones().size(),
                                     skeleton.GetSignature() ) );
        }

    private:
        Animation::AnimationLibrary*          m_AnimationLibrary;
        // Non-owning: the manager belongs to the host, which outlives its scene. MAY BE NULL — a host
        // that builds no asset manager simply has no rigs, and SyncControlRig says so once.
        Assets::AssetManager*                 m_AssetManager = nullptr;
        std::chrono::steady_clock::time_point m_LastTime;
        bool                                  m_HasLast = false;

        // ONE dedupe store for every complaint this system makes, and it remembers the MESSAGE rather than
        // just the key. The set it replaces could only say "already complained about this state", so a
        // state whose clip failed for one reason and then for another stayed silent about the second.
        // Mutable because reporting is a property of the log, not of the world being simulated.
        mutable std::unordered_map<std::string, std::string> m_Reported;
    };
} // namespace Desert::ECS