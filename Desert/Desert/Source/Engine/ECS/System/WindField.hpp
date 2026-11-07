#pragma once

// THE ONE WIND QUERY (UE FScene::GetWindParameters): the wind at a world point, from every WindSource entity in
// the registry. Consumers - foliage sway (MeshECSSystem), the cloud layer's drift (VolumetricCloudECSSystem),
// cloth (ClothStepContext::WindVelocity) and groom (GroomStepContext::WindVelocity) - call this and keep no wind
// fields of their own; the math is ECS::WindAtFromSources (ECS/WindSourceComponent.hpp).

#include <Engine/ECS/Entity.hpp>
#include <Engine/ECS/WindSourceComponent.hpp>

#include <entt/entt.hpp>
#include <glm/glm.hpp>


namespace Desert::ECS
{
    // Sums each source as it is visited (the same per-source term as WindAtFromSources): no list is built, and
    // each source's position is a load of its cached world transform (no parent walk).
    [[nodiscard]] inline WindAtPoint WindAt( entt::registry& registry, const glm::vec3& position )
    {
        WindAtPoint wind;
        for ( const entt::entity entity : registry.view<WindSourceComponent>() )
        {
            const glm::mat4        pose   = Entity( entity, registry ).GetWorldTransform();
            const WindSourceSample source = { registry.get<WindSourceComponent>( entity ).Data,
                                              glm::vec3( pose[3] ) };
            wind.Velocity += WindVelocityFrom( source, position );
        }
        return wind;
    }
} // namespace Desert::ECS
