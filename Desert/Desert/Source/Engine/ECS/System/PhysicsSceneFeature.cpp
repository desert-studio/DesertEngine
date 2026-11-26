#include <Engine/ECS/System/PhysicsSceneFeature.hpp>

namespace Desert::ECS
{
    std::vector<PhysicsSceneFeatureFactory>& PhysicsSceneFeatures()
    {
        static std::vector<PhysicsSceneFeatureFactory> factories;
        return factories;
    }
}
