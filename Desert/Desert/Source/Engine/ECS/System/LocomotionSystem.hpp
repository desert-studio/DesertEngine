#pragma once

#include <Engine/ECS/System/System.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/System/CharacterMovement.hpp>

namespace Desert::ECS
{
    // Publishes each character's MOVEMENT STATE (planar speed, falling, crouched — produced by PhysicsECSSystem)
    // into its skinned child's AnimGraph parameters, as UE's AnimBP reads Speed / IsFalling / IsCrouching from
    // the CharacterMovementComponent: the GRAPH picks and blends the clips (a blend space by Speed, jump states
    // on IsFalling, crouch states on IsCrouched). This is the one path from movement to animation — the old
    // clip-name switch (idle / walk / run / jump by speed thresholds) is gone. Play-only; runs AFTER
    // PhysicsECSSystem and before AnimationECSSystem drains the parameter queue.
    // DOES NOT HONOUR VisibilityComponent, AND MUST NOT: hiding a character must not change the state its
    // graph is in when it is shown again. Verdict and mutation gate: Desert/Tests/Engine/VisibilityHonoured.
    class LocomotionSystem final : public System
    {
    public:
        explicit LocomotionSystem( Core::Scene* scene ) : m_Scene( scene )
        {
        }

        void Update( entt::registry& registry, Graphic::Render::RenderCommandBuffer&,
                     const Common::Timestep& ) override
        {
            if ( m_Scene == nullptr || !m_Scene->TicksGameplay() ) // Play, or a paused frame skip
                return;

            auto view = registry.view<CharacterControllerComponent>();
            for ( auto entity : view )
            {
                const auto& cc = view.get<CharacterControllerComponent>( entity );
                Drive( registry, entity, cc );
            }
        }

    private:
        static void Drive( entt::registry& registry, entt::entity character,
                           const CharacterControllerComponent& cc )
        {
            if ( !registry.has<RelationshipComponent>( character ) )
                return;
            for ( entt::entity child : registry.get<RelationshipComponent>( character ).Children )
            {
                if ( !registry.has<SkinnedMeshComponent>( child ) || !registry.has<AnimationComponent>( child ) )
                    continue;
                CharacterMovement::PublishAnimGraphParameters( cc, registry.get<AnimationComponent>( child ) );
                return;
            }
        }

    private:
        Core::Scene* m_Scene = nullptr;
    };
} // namespace Desert::ECS
