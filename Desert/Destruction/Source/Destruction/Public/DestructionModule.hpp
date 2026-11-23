#pragma once

// THE DESTRUCTION MODULE'S STARTUP AND ITS PART OF THE PHYSICS SCENE (UE: GeometryCollectionEngine's module startup
// and its physics proxies in the Chaos scene). Engine names none of this: StartupModule (called by Engine's module
// list, Engine/Core/Modules.cpp) registers the physics feature, the fracture pieces' stand-in draw, the
// FractureService, the `.dfrac` asset roots and the module's component pools through Engine's hooks.

#include <Destruction/Public/DestructibleLifetime.hpp>
#include <Destruction/Public/DestructionWorld.hpp>
#include <Engine/ECS/System/PhysicsSceneFeature.hpp>

#include <cstdint>
#include <memory>

namespace Desert::ECS
{
    class PhysicsECSSystem;
}

namespace Desert::Destruction
{
    /// The scene's destructibles while Play runs: their DestructionWorld in the scene's PhysicsWorld and the
    /// entities' objects in it (DestructibleLifetime), stepped with the physics scene.
    class DestructionPhysicsFeature final : public ECS::IPhysicsSceneFeature
    {
    public:
        explicit DestructionPhysicsFeature( Physics::PhysicsWorld& world );

        void BeginFrame() override;
        void Sync( entt::registry& registry ) override;
        void AfterStep( entt::registry& registry ) override;
        void PublishEvents( entt::registry& registry ) override;

        [[nodiscard]] DestructionWorld& GetWorld()
        {
            return *m_World;
        }

    private:
        // Declared before m_Lifetime so it is destroyed after it: the entities' objects live in the world.
        std::unique_ptr<DestructionWorld>          m_World;
        std::unique_ptr<ECS::DestructibleLifetime> m_Lifetime;
        std::uint64_t                              m_Translated = 0; // DestructionEventQueue::Publication
    };

    /// The scene's destruction world while Play runs, null in Edit (or with no physics system): what a field entity
    /// fires into (ECS::FireDestructionField), from the Sequencer or from gameplay.
    [[nodiscard]] DestructionWorld* FindDestructionWorld( const ECS::PhysicsECSSystem* physics );

    /// Registers the module with the engine (Engine/Core/Modules.cpp calls it once).
    void StartupModule();
} // namespace Desert::Destruction
