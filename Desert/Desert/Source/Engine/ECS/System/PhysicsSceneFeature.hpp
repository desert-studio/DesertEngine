#pragma once

// A FEATURE MODULE'S PART OF THE PHYSICS SCENE (UE: a plugin's physics proxies join the Chaos scene through the
// solver's callbacks, never through code of the engine that names the plugin). PhysicsECSSystem builds one
// instance of every registered feature per Play, over its PhysicsWorld, and calls it at the fixed points of its
// frame; a feature's bodies live in that world and are released by the feature, which is destroyed before the
// world. Destruction (Destruction/Private/DestructionModule.cpp) and Water (Water/Private/WaterModule.cpp: the
// water subsystem buoyancy and scripts query); landscape and foliage are next.

#include <entt/entt.hpp>

#include <memory>
#include <vector>

namespace Desert::Physics
{
    class PhysicsWorld;
}

namespace Desert::ECS
{
    class IPhysicsSceneFeature
    {
    public:
        virtual ~IPhysicsSceneFeature() = default;

        /// Before the frame's steps: the events of the last frame's steps stop being readable.
        virtual void BeginFrame() = 0;
        /// The registry made current (listeners attached, the feature's entities synced into the world).
        virtual void Sync( entt::registry& registry ) = 0;
        /// Before the frame's fixed steps, with the scene's game clock at their start (seconds): a feature's own
        /// clock is set to it (Water: the wave time the renderer draws at). A feature with no clock does nothing.
        virtual void BeginSteps( double gameTimeSeconds )
        {
            (void)gameTimeSeconds;
        }
        /// Before each fixed substep (the world's pre-step callback), with its fixed @p dt: a feature's own clock.
        /// A feature with no clock of its own (Destruction) does nothing here.
        virtual void PreStep( float dt )
        {
            (void)dt;
        }
        /// After the steps, before the contact events are published: the world's state written back.
        virtual void AfterStep( entt::registry& registry ) = 0;
        /// After the contact events: the feature's own news published into the registry.
        virtual void PublishEvents( entt::registry& registry ) = 0;
    };

    using PhysicsSceneFeatureFactory = std::unique_ptr<IPhysicsSceneFeature> ( * )( Physics::PhysicsWorld& world );

    /// The registered features, in registration order (module startup, Engine/Core/Modules.cpp).
    inline std::vector<PhysicsSceneFeatureFactory>& PhysicsSceneFeatures()
    {
        static std::vector<PhysicsSceneFeatureFactory> factories;
        return factories;
    }

    inline void RegisterPhysicsSceneFeature( PhysicsSceneFeatureFactory factory )
    {
        PhysicsSceneFeatures().push_back( factory );
    }
} // namespace Desert::ECS
