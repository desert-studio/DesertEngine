#pragma once

#include <Engine/ECS/System/System.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/System/SystemRules.hpp>

namespace Desert::ECS
{
    // Maps a character's MOVEMENT STATE (planar speed + on-ground, produced by PhysicsECSSystem) to a locomotion
    // CLIP NAME on its skinned child. Deliberately SEPARATE from physics (mechanism vs behaviour), and — by
    // design — the system holds NO clip knowledge: the state->clip-name mapping + speed thresholds come from a
    // LocomotionComponent (data), and the clip itself is resolved by name in AnimationECSSystem from the
    // AnimationLibrary. So neither a clip instance nor a clip name is hard-coded here. Play-only; runs AFTER
    // PhysicsECSSystem.
    // DOES NOT HONOUR VisibilityComponent, AND MUST NOT: it picks a clip NAME from a character's speed, and
    // hiding a character must not change which animation it is playing when it is shown again.
    // Verdict and mutation gate: Desert/Tests/Engine/VisibilityHonoured.
    class LocomotionSystem final : public System
    {
    public:
        explicit LocomotionSystem( Core::Scene* scene ) : m_Scene( scene )
        {
        }

        void Update( entt::registry& registry, Graphic::Render::RenderCommandBuffer&,
                     const Common::Timestep& ) override
        {
            if ( !m_Scene || m_Scene->GetState() != Core::Scene::SceneState::Play )
                return;

            auto view = registry.view<CharacterControllerComponent>();
            for ( auto entity : view )
            {
                const auto& cc = view.get<CharacterControllerComponent>( entity );
                Drive( registry, entity, cc.CurrentSpeed, cc.OnGround );
            }
        }

    private:
        // Picks the locomotion clip NAME for the current speed from the entity's LocomotionComponent (or the
        // struct defaults when absent) and writes it to the skinned child's AnimationComponent.CurrentClip.
        // AnimationECSSystem resolves + cross-fades to that clip; an unknown name simply plays nothing.
        static void Drive( entt::registry& registry, entt::entity character, float speed, bool onGround )
        {
            if ( !registry.has<RelationshipComponent>( character ) )
                return;

            // Defaults live in the struct (data), so the system code contains no clip names.
            static const LocomotionComponent kDefault{};
            const LocomotionComponent&       loco = registry.has<LocomotionComponent>( character )
                                                         ? registry.get<LocomotionComponent>( character )
                                                         : kDefault;

            for ( entt::entity child : registry.get<RelationshipComponent>( character ).Children )
            {
                if ( !registry.has<SkinnedMeshComponent>( child ) || !registry.has<AnimationComponent>( child ) )
                    continue;

                auto& anim = registry.get<AnimationComponent>( child );
                if ( anim.Graph ) // a state machine already owns clip selection
                    return;

                // The rule itself lives in SystemRules.hpp so it can be tested without a Scene (and so the
                // ordering — airborne beats any ground speed — is stated in one place).
                const std::string& name = Rules::LocomotionClipFor( loco, speed, onGround );

                if ( anim.CurrentClip != name )
                {
                    anim.CurrentClip = name; // AnimationECSSystem cross-fades on change
                    anim.Playing     = true;
                    anim.Loop        = true;
                }
                return;
            }
        }

    private:
        Core::Scene* m_Scene = nullptr;
    };
} // namespace Desert::ECS
