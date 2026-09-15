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
#include <Engine/Geometry/SkinnedMesh.hpp>

#include <Common/Core/Logger.hpp>

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <unordered_set>

namespace Desert::ECS
{
    class AnimationECSSystem : public System
    {
    public:
        explicit AnimationECSSystem( Animation::AnimationLibrary* animationLibrary )
             : m_AnimationLibrary( animationLibrary )
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

                // Before either playback path, because a control is a stage of the same pipeline and the
                // pipeline runs inside Animator::Update below.
                SyncSkeletalControls( registry, entity, *anim.Animator, skeleton );

                // AnimGraph path: the state machine PICKS the clip; the Animator just plays it. Falls back to
                // the CurrentClip path below when no graph is attached.
                if ( anim.Graph && !anim.Graph->States.empty() )
                {
                    if ( !anim.GraphEvaluator )
                    {
                        anim.GraphEvaluator     = std::make_shared<Animation::Graph::Evaluator>( *anim.Graph );
                        anim.BuiltGraphRevision = anim.GraphRevision;
                    }
                    else if ( anim.BuiltGraphRevision != anim.GraphRevision )
                    {
                        // Re-sync after an editor edit WITHOUT resetting the active state / live parameters.
                        anim.GraphEvaluator->SyncGraph( *anim.Graph );
                        anim.BuiltGraphRevision = anim.GraphRevision;
                    }

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
                            const auto found = m_AnimationLibrary->FindForSkeleton( skeleton, res.Current->Clip );
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
                                ReportUnplayableState( skeleton, res.Current->Name, res.Current->Clip,
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
                    const auto found = m_AnimationLibrary->FindForSkeleton( skeleton, anim.CurrentClip );
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
                        ReportUnplayableState( skeleton, "AnimationComponent.CurrentClip", anim.CurrentClip,
                                               found.GetError() );
                    }
                }

                else
                {
                    const auto animations = m_AnimationLibrary->GetForSkeleton( skeleton );

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
                        ReportUnplayableState( skeleton, "AnimationComponent (no clip named)", "<any>",
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
            const std::string key = std::to_string( skeleton.GetSignature() ) + '|' + stateName + '|' + clipName;
            if ( !m_ReportedUnplayable.insert( key ).second )
                return;

            LOG_ERROR( "[Animation] state '{}' asks for clip '{}' and nothing will play: {} The rig has {} "
                       "bone(s), signature {}.",
                       stateName, clipName, reason, skeleton.GetBones().size(), skeleton.GetSignature() );
        }

    private:
        Animation::AnimationLibrary*          m_AnimationLibrary;
        std::chrono::steady_clock::time_point m_LastTime;
        bool                                  m_HasLast = false;

        // Mutable because reporting is a property of the log, not of the world being simulated; Update is
        // non-const anyway, but the reporter is called from a const context in the state-machine branch.
        mutable std::unordered_set<std::string> m_ReportedUnplayable;
    };
} // namespace Desert::ECS