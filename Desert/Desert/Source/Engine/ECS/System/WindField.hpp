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

    // THE GUSTS the scene's air carries (ECS::WindGustAir): every WindSource entity visited once, the same
    // per-source term as WindGustFromSources. MeshECSSystem advances the fronts with it
    // (Graphic::AdvanceWindGust).
    [[nodiscard]] inline WindGustAir WindGust( entt::registry& registry )
    {
        WindGustSums sums;
        for ( const entt::entity entity : registry.view<WindSourceComponent>() )
            sums.Add( registry.get<WindSourceComponent>( entity ).Data );
        return sums.Air();
    }

    // Whether any point (local) source blows: a swaying type then sways even in still directional air, by the local
    // sources the camera block carries (Graphic::SelectLocalWind, Common/LocalWind.glslh).
    [[nodiscard]] inline bool AnyLocalWind( entt::registry& registry )
    {
        for ( const entt::entity entity : registry.view<WindSourceComponent>() )
        {
            const WindSourceData& data = registry.get<WindSourceComponent>( entity ).Data;
            if ( data.PointWind && data.Speed > 0.0f && data.Radius > 0.0f )
                return true;
        }
        return false;
    }
} // namespace Desert::ECS
