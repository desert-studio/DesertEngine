#pragma once

// AI-STATETREE CONTRACT (header only, owner review pending) - docs/contracts/AI-STATETREE.md.
//
// THE AI FRAME (UE UAIPerceptionSystem::Tick, then UStateTreeComponent::TickComponent).
//
// ONE system, two steps in a fixed order each Play frame:
//   1. PERCEPTION. Noise and damage events queued since the last frame are delivered (range, affiliation).
//      Sight: for each perceiver, candidate sources inside SightRadius (LoseSightRadius for one already seen)
//      and inside the cone; then line-of-sight traces through Scene::Raycast from the eye to the target point,
//      at most `MaxSightTracesPerFrame` across ALL perceivers (UE MaxTracesPerTick, round-robin so no NPC
//      starves). The traces run on the JobSystem (Scene::Raycast is const); results are applied on the main
//      thread. Ages advance; a lost stimulus past MaxAge is forgotten.
//      Every change becomes a tree event on that NPC: "Perception.Sight.Gained" / ".Lost" / ".Forgotten",
//      "Perception.Hearing.<Tag>", "Perception.Damage.<Tag>", Subject = the source.
//   2. DECISIONS. For each StateTreeComponent: (re)start when needed (Play began, asset revision changed,
//      hot reload), then Tick when the execution ShouldTick (scheduled tick).
// Movement (NAV-FOLLOW) runs after this system and executes the move requests tasks made; this system never
// moves an entity itself.
//
// Play only, and only where the world is authoritative: when the server line (ENG-SERVER) lands, a client
// world does not register this system (UE: AI exists on the server only).

#include <Engine/AI/AIPerceptionComponent.hpp>
#include <Engine/AI/Perception/PerceptionTypes.hpp>
#include <Engine/AI/StateTree/StateTreeLuau.hpp>
#include <Engine/AI/StateTreeComponent.hpp>
#include <Engine/ECS/System/System.hpp>

#include <cstdint>
#include <vector>

namespace Desert::Core
{
    class Scene;
}

namespace Desert::Assets
{
    class AssetManager;
}

namespace Desert::ECS
{
    struct AISystemSettings
    {
        /// UE AIPerceptionSystem MaxTracesPerTick (project setting). Owner sets the NPC target (open question 2).
        std::uint32_t MaxSightTracesPerFrame = 64;
    };

    class AISystem final : public System
    {
    public:
        AISystem( Core::Scene* scene, Assets::AssetManager* assets, AI::StateTree::IStateTreeScriptHost* scripts,
                  AISystemSettings settings = {} );
        ~AISystem() override;

        void Update( entt::registry& registry, Graphic::Render::RenderCommandBuffer& renderCommandBuffer,
                     const Common::Timestep& ts ) override;

        /// UE UAISense_Hearing::ReportNoiseEvent. Gameplay (C++ or Luau: ai.reportNoise(location, loudness,
        /// instigator, tag)) calls it; delivered on the next Update.
        void ReportNoiseEvent( const AI::Perception::NoiseEvent& noise );

        /// UE UAISense_Damage::ReportDamageEvent.
        void ReportDamageEvent( const AI::Perception::DamageEvent& damage );

        /// UE UStateTreeComponent::SendStateTreeEvent: queue an event on one NPC's tree (ai.sendEvent).
        /// Refused when the entity has no running tree.
        Common::BoolResultStr SendEvent( entt::entity npc, AI::StateTree::StateTreeEvent event );

        /// Start / stop one NPC's tree by hand (StartOnPlay = false; ai.startTree / ai.stopTree).
        Common::BoolResultStr StartTree( entt::entity npc );
        void                  StopTree( entt::entity npc );

        /// Stops every tree (Play ended, level unloaded): ExitState / TreeStop run, Luau slots released.
        void StopAll( entt::registry& registry );

    private:
        void UpdatePerception( entt::registry& registry, float deltaSeconds );
        void UpdateDecisions( entt::registry& registry, float deltaSeconds );

        Core::Scene*                             m_Scene   = nullptr;
        Assets::AssetManager*                    m_Assets  = nullptr;
        AI::StateTree::IStateTreeScriptHost*     m_Scripts = nullptr;
        AISystemSettings                         m_Settings;
        std::vector<AI::Perception::NoiseEvent>  m_Noises;
        std::vector<AI::Perception::DamageEvent> m_Damages;
        std::uint32_t                            m_SightRoundRobin = 0;
    };
} // namespace Desert::ECS
