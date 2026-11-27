#include <Engine/Core/WorldSystems.hpp>

#include <Engine/Core/ModuleHooks.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/System/AnimationECSSystem.hpp>
#include <Engine/ECS/System/AttachmentSystem.hpp>
#include <Engine/ECS/System/AudioECSSystem.hpp>
#include <Engine/ECS/System/ComponentEventSystem.hpp>
#include <Engine/ECS/System/DecalECSSystem.hpp>
#include <Engine/ECS/System/GameModeSystem.hpp>
#include <Engine/ECS/System/HeightFogECSSystem.hpp>
#include <Engine/ECS/System/LevelSequenceSystem.hpp>
#include <Engine/ECS/System/LocomotionSystem.hpp>
#include <Engine/ECS/System/MeshECSSystem.hpp>
#include <Engine/ECS/System/PhysicsECSSystem.hpp>
#include <Engine/ECS/System/PointLightSystem.hpp>
#include <Engine/ECS/System/RectLightSystem.hpp>
#include <Engine/ECS/System/SkyboxECSSystem.hpp>
#include <Engine/ECS/System/SpotLightSystem.hpp>
#include <Engine/ECS/System/TextECSSystem.hpp>
#include <Engine/ECS/System/TimeOfDayECSSystem.hpp>
#include <Engine/ECS/System/VolumetricCloudECSSystem.hpp>

#include <algorithm>
#include <format>
#include <iterator>
#include <string>
#include <tuple>
#include <vector>

namespace Desert::Core
{
    void RegisterEngineWorldSystems()
    {
        static constexpr WorldSystemRegistration kRows[] = {
             { "Mesh", WorldStage::Geometry, 10, kDrawnWorlds, false, false,
               []( Scene& scene, const WorldServices& /*services*/ ) { scene.AddSystem<ECS::MeshECSSystem>(); } },
             { "Text", WorldStage::Geometry, 20, kDrawnWorlds, false, false,
               []( Scene& scene, const WorldServices& /*services*/ ) { scene.AddSystem<ECS::TextECSSystem>(); } },
             { "TimeOfDay", WorldStage::Environment, 10, kDrawnWorlds, false, false,
               []( Scene& scene, const WorldServices& /*services*/ )
               { scene.AddSystem<ECS::TimeOfDayECSSystem>(); } },
             { "Skybox", WorldStage::Environment, 20, kDrawnWorlds, false, false,
               []( Scene& scene, const WorldServices& /*services*/ )
               { scene.AddSystem<ECS::SkyboxECSSystem>(); } },
             { "HeightFog", WorldStage::Environment, 30, kDrawnWorlds, false, false,
               []( Scene& scene, const WorldServices& /*services*/ )
               { scene.AddSystem<ECS::HeightFogECSSystem>(); } },
             { "Decal", WorldStage::Environment, 40, kDrawnWorlds, false, false,
               []( Scene& scene, const WorldServices& /*services*/ )
               { scene.AddSystem<ECS::DecalECSSystem>( scene.GetDecalImpacts() ); } },
             { "VolumetricCloud", WorldStage::Environment, 50, kDrawnWorlds, false, false,
               []( Scene& scene, const WorldServices& /*services*/ )
               { scene.AddSystem<ECS::VolumetricCloudECSSystem>(); } },
             { "PointLight", WorldStage::Lights, 10, kDrawnWorlds, false, false,
               []( Scene& scene, const WorldServices& /*services*/ )
               { scene.AddSystem<ECS::PointLightECSSystem>(); } },
             { "SpotLight", WorldStage::Lights, 20, kDrawnWorlds, false, false,
               []( Scene& scene, const WorldServices& /*services*/ )
               { scene.AddSystem<ECS::SpotLightECSSystem>(); } },
             { "RectLight", WorldStage::Lights, 30, kDrawnWorlds, false, false,
               []( Scene& scene, const WorldServices& /*services*/ )
               { scene.AddSystem<ECS::RectLightECSSystem>(); } },
             { "Animation", WorldStage::Pose, 10, kAnimatedWorlds, true, true,
               []( Scene& scene, const WorldServices& services )
               { scene.AddSystem<ECS::AnimationECSSystem>( services.Animations, services.Assets ); } },
             { "Attachment", WorldStage::Pose, 20, kAnimatedWorlds, false, false,
               []( Scene& scene, const WorldServices& /*services*/ )
               { scene.AddSystem<ECS::AttachmentSystem>( &scene ); } },
             { "GameMode", WorldStage::Think, 20, kPlayedWorlds, true, false,
               []( Scene& scene, const WorldServices& services )
               { scene.AddSystem<ECS::GameModeSystem>( &scene, services.Assets ); } },
             { "Physics", WorldStage::Simulate, 10, kPlayedWorlds, false, false,
               []( Scene& scene, const WorldServices& /*services*/ )
               { scene.AddSystem<ECS::PhysicsECSSystem>( &scene ); } },
             { "ComponentEvent", WorldStage::Simulate, 20, kPlayedWorlds, false, false,
               []( Scene& scene, const WorldServices& /*services*/ )
               { scene.AddSystem<ECS::ComponentEventSystem>(); } },
             { "Locomotion", WorldStage::Simulate, 30, kPlayedWorlds, false, false,
               []( Scene& scene, const WorldServices& /*services*/ )
               { scene.AddSystem<ECS::LocomotionSystem>( &scene ); } },
             { "Audio", WorldStage::Present, 10, kPlayedWorlds, false, false,
               []( Scene& scene, const WorldServices& /*services*/ )
               { scene.AddSystem<ECS::AudioECSSystem>( &scene ); } },
             { "LevelSequence", WorldStage::Sequence, 10, kPlayedWorlds, true, false,
               []( Scene& scene, const WorldServices& services )
               { scene.AddSystem<ECS::LevelSequenceSystem>( &scene, services.Assets ); } },
        };
        auto& systems = ModuleHooks().WorldSystems;
        systems.insert( systems.end(), std::begin( kRows ), std::end( kRows ) );
    }

    Common::BoolResultStr CreateWorldSystems( Scene& scene, const WorldServices& services )
    {
        const auto refuse = [&scene]( const std::string& why )
        { return Common::MakeError<bool>( std::format( "world '{}': {}", scene.GetSceneName(), why ) ); };

        if ( scene.HasSystems() )
            return refuse( "its systems are already made" );

        std::vector<const WorldSystemRegistration*> rows;
        for ( const WorldSystemRegistration& row : ModuleHooks().WorldSystems )
        {
            if ( row.Add == nullptr )
                return refuse( std::format( "system '{}' is registered without Add", row.Name ) );
            if ( ( row.Kinds & MaskOf( scene.Kind() ) ) == 0 )
                continue;
            if ( row.NeedsAssets && services.Assets == nullptr )
                return refuse( std::format( "system '{}' needs an asset manager", row.Name ) );
            if ( row.NeedsAnimations && services.Animations == nullptr )
                return refuse( std::format( "system '{}' needs an animation library", row.Name ) );
            rows.push_back( &row );
        }
        const auto key = []( const WorldSystemRegistration* row )
        { return std::tuple( static_cast<int>( row->Stage ), row->Order ); };
        std::ranges::sort( rows, {}, key );
        const auto twin = std::ranges::adjacent_find( rows, {}, key );
        if ( twin != rows.end() )
            return refuse( std::format( "systems '{}' and '{}' are registered at one place ({}.{})",
                                        ( *twin )->Name, ( *std::next( twin ) )->Name,
                                        static_cast<int>( ( *twin )->Stage ), ( *twin )->Order ) );

        for ( const WorldSystemRegistration* row : rows )
            row->Add( scene, services );
        return Common::MakeSuccess( true );
    }
} // namespace Desert::Core
