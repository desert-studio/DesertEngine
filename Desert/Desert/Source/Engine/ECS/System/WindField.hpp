#pragma once

// THE ONE WIND QUERY (UE FScene::GetWindParameters): the wind at a world point, from every WindSource entity in
// the registry. Consumers - foliage sway (MeshECSSystem), the cloud layer's drift (VolumetricCloudECSSystem),
// cloth (ClothStepContext::WindVelocity) and groom (GroomStepContext::WindVelocity) - call this and keep no wind
// fields of their own; the math is ECS::WindAtFromSources (ECS/WindSourceComponent.hpp).

#include <Engine/ECS/Entity.hpp>
#include <Engine/ECS/WindSourceComponent.hpp>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <vector>

namespace Desert::ECS
{
    // Non-const registry: Entity::GetWorldTransform walks the hierarchy through it, as every system's does.
    [[nodiscard]] inline WindAtPoint WindAt( entt::registry& registry, const glm::vec3& position )
    {
        std::vector<WindSourceSample> sources;
        for ( const entt::entity entity : registry.view<WindSourceComponent>() )
        {
            const glm::mat4 pose = Entity( entity, registry ).GetWorldTransform();
            sources.push_back( { registry.get<WindSourceComponent>( entity ).Data, glm::vec3( pose[3] ) } );
        }
        return WindAtFromSources( sources, position );
    }
} // namespace Desert::ECS
