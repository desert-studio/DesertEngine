#include <Engine/Core/SceneRenderCollectors.hpp>

#include <Engine/Core/ModuleHooks.hpp>
#include <Engine/Core/Scene.hpp>

#include <Engine/ECS/System/DecalECSSystem.hpp>
#include <Engine/ECS/System/HeightFogECSSystem.hpp>
#include <Engine/ECS/System/LandscapeECSSystem.hpp>
#include <Engine/ECS/System/MeshECSSystem.hpp>
#include <Engine/ECS/System/PointLightSystem.hpp>
#include <Engine/ECS/System/RectLightSystem.hpp>
#include <Engine/ECS/System/SkyboxECSSystem.hpp>
#include <Engine/ECS/System/SpotLightSystem.hpp>
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
        // A pure render-data collector: one command per visible decal box (DECALS-DBUFFER).
        // ... and the world's impact decals (NG-DECALS-MESH), the same pass's rows without entities.
        scene.AddSystem<ECS::DecalECSSystem>( scene.GetDecalImpacts() );
        scene.AddSystem<ECS::VolumetricCloudECSSystem>();
        // The tiles' GPU copies are the scene's ledger rows (Scene::GetLedgerInstance).
        scene.AddSystem<ECS::LandscapeECSSystem>( scene.GetLedgerInstance() );
        // The feature modules' collectors (Water: one command per water zone, WATER-W6), after the landscape's.
        for ( const RenderCollectorAdder add : ModuleHooks().RenderCollectors )
            add( scene );
        scene.AddSystem<ECS::PointLightECSSystem>();
        scene.AddSystem<ECS::SpotLightECSSystem>();
        scene.AddSystem<ECS::RectLightECSSystem>();
    }
} // namespace Desert::Core
