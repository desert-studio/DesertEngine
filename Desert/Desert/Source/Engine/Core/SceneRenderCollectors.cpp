#include <Engine/Core/SceneRenderCollectors.hpp>

#include <Engine/Core/Scene.hpp>

#include <Engine/ECS/System/HeightFogECSSystem.hpp>
#include <Engine/ECS/System/LandscapeECSSystem.hpp>
#include <Engine/ECS/System/MeshECSSystem.hpp>
#include <Engine/ECS/System/PointLightSystem.hpp>
#include <Engine/ECS/System/SkyboxECSSystem.hpp>
#include <Engine/ECS/System/SpotLightSystem.hpp>
#include <Engine/ECS/System/TerrainECSSystem.hpp>
#include <Engine/ECS/System/TextECSSystem.hpp>
#include <Engine/ECS/System/TimeOfDayECSSystem.hpp>
#include <Engine/ECS/System/VolumetricCloudECSSystem.hpp>

namespace Desert::Core
{
    void AddSceneRenderCollectors( Scene& scene )
    {
        scene.AddSystem<ECS::MeshECSSystem>();
        scene.AddSystem<ECS::TextECSSystem>();
        // BEFORE the collectors: it writes the atmosphere sun's transform, which the sky collector, the
        // light collector and the shadow path all read this same frame.
        scene.AddSystem<ECS::TimeOfDayECSSystem>();
        scene.AddSystem<ECS::SkyboxECSSystem>();
        // A pure render-data collector: reads the fog component (and its entity's transform Y, the fog
        // floor) and emits one command.
        scene.AddSystem<ECS::HeightFogECSSystem>();
        scene.AddSystem<ECS::VolumetricCloudECSSystem>();
        scene.AddSystem<ECS::TerrainECSSystem>();
        scene.AddSystem<ECS::LandscapeECSSystem>();
        scene.AddSystem<ECS::PointLightECSSystem>();
        scene.AddSystem<ECS::SpotLightECSSystem>();
    }
} // namespace Desert::Core
