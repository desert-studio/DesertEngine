#include <Water/Public/WaterFallEffects.hpp>

#include <Engine/Core/Scene.hpp>
#include <Engine/Core/SceneSettings.hpp>
#include <Engine/ECS/Entity.hpp>
#include <Engine/ECS/EntityVisibility.hpp>
#include <Engine/ECS/TransformComponent.hpp>
#include <Engine/ECS/TransientComponent.hpp>
#include <Engine/ECS/UUIDComponent.hpp>
#include <VFX/Public/VFXComponent.hpp>
#include <Water/Public/WaterBodyComponent.hpp>
#include <Water/Public/WaterWaveSources.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <utility>
#include <vector>

namespace Desert::Water
{
    namespace
    {
        constexpr std::array<const char*, 3> kEffectNames = { "WaterFallFoam", "WaterFallSpray", "WaterFallMist" };

        struct Wanted
        {
            entt::entity        River = entt::null;
            uint32_t            Fall  = 0u;
            uint32_t            Kind  = 0u;
            Assets::AssetHandle System;
            glm::vec3           Foot{ 0.0f };
            float               Yaw       = 0.0f;
            float               Width     = 0.0f;
            float               Intensity = 0.0f;
        };

        bool IsAlive( entt::registry& registry, entt::entity entity, const Common::UUID& id )
        {
            if ( !registry.valid( entity ) )
                return false;
            const auto* uuid = registry.try_get<ECS::UUIDComponent>( entity );
            return uuid != nullptr && uuid->UUID == id;
        }
    } // namespace

    void WaterFallEffects::Tick( Core::Scene& scene, entt::registry& registry, double )
    {
        ++m_Tick;
        const float gravity = scene.GetSettings().Gravity;

        const auto* const sources = registry.try_ctx<std::shared_ptr<ECS::WaterWaveSources>>();

        // Collected before any entity is made: making one writes the pools the rivers are read from.
        std::vector<Wanted>                     wanted;
        std::unordered_map<entt::entity, River> rivers;
        for ( const entt::entity entity : registry.view<ECS::WaterBodyComponent, ECS::TransformComponent>() )
        {
            const ECS::WaterBodyData& data = registry.get<ECS::WaterBodyComponent>( entity ).Data;
            if ( data.Type != WaterBodyType::River || ECS::IsHidden( registry, entity ) ||
                 ( sources != nullptr && ( *sources )->IsRefused( entity ) ) ) // a second ocean is no water
                continue;
            const std::array<Assets::AssetHandle, 3> systems = { data.FallFoam, data.FallSpray, data.FallMist };
            if ( std::all_of( systems.begin(), systems.end(),
                              []( const Assets::AssetHandle& s ) { return s == Assets::AssetHandle::Null(); } ) )
                continue;

            const RiverBakeInput input{ .LocalPoints = ECS::RiverPointsOf( data ),
                                        .Origin = registry.get<ECS::TransformComponent>( entity ).GetTranslation(),
                                        .Width  = data.RiverWidth,
                                        .FlowSpeed  = data.RiverFlowSpeed,
                                        .FlowPeriod = data.RiverFlowPeriod };
            const uint64_t       key   = RiverBakeKey( input );
            const auto           found = m_Rivers.find( entity );
            const River          river = found != m_Rivers.end() && found->second.Key == key
                                              ? found->second
                                              : River{ key, BakeRiver( input ) };
            rivers.emplace( entity, river );

            for ( uint32_t i = 0u; i < static_cast<uint32_t>( river.Flow->Falls.size() ); ++i )
            {
                const RiverFall& fall      = river.Flow->Falls[i];
                const float      speed     = RiverFallSpeed( fall.FlowSpeed, fall.Height, gravity );
                const float      intensity = std::clamp( speed / kFallFullIntensitySpeed, 0.0f, 1.0f );
                // The yaw turning +X onto Across (a turn by yaw takes X to ( cos, 0, -sin )).
                const float yaw = std::atan2( -fall.Across.z, fall.Across.x );
                for ( uint32_t kind = 0u; kind < static_cast<uint32_t>( systems.size() ); ++kind )
                {
                    if ( systems[kind] == Assets::AssetHandle::Null() )
                        continue;
                    wanted.push_back( Wanted{ .River     = entity,
                                              .Fall      = i,
                                              .Kind      = kind,
                                              .System    = systems[kind],
                                              .Foot      = fall.Foot,
                                              .Yaw       = yaw,
                                              .Width     = fall.Width,
                                              .Intensity = intensity } );
                }
            }
        }
        m_Rivers = std::move( rivers );

        for ( const Wanted& want : wanted )
        {
            Effect& effect  = m_Effects[EffectKey{ want.River, want.Fall, want.Kind }];
            effect.SeenTick = m_Tick;
            if ( !IsAlive( registry, effect.Entity, effect.Id ) )
            {
                effect.Id     = Common::UUID::Generate();
                effect.Entity = scene.CreateEntityWithUUID( effect.Id, kEffectNames[want.Kind] ).GetHandle();
                registry.emplace<ECS::TransientComponent>( effect.Entity );
                registry.emplace<ECS::VFXComponent>( effect.Entity );
            }

            ECS::LocalTransform local;
            local.Translation = want.Foot;
            local.Rotation    = glm::vec3( 0.0f, want.Yaw, 0.0f );
            ECS::SetLocalTransform( registry, effect.Entity, local );

            auto& vfx       = registry.get<ECS::VFXComponent>( effect.Entity );
            vfx.Data.System = want.System;
            vfx.Data.SetVariable( "Extent", glm::vec4( want.Width, 0.0f, kRiverFallSheetThicknessCm, 0.0f ) );
            vfx.SpawnScale = want.Intensity;
        }

        for ( auto it = m_Effects.begin(); it != m_Effects.end(); )
        {
            if ( it->second.SeenTick == m_Tick )
            {
                ++it;
                continue;
            }
            if ( IsAlive( registry, it->second.Entity, it->second.Id ) )
                scene.DestroyEntityAtFence( ECS::Entity( it->second.Entity, registry ) );
            it = m_Effects.erase( it );
        }
    }

    // Play begins or ends with the entities standing: they are the same falls'.
    void WaterFallEffects::Reset()
    {
    }

    // The scene's entities are gone already (Scene::Clear); the next tick makes the effects again.
    void WaterFallEffects::Clear()
    {
        m_Rivers.clear();
        m_Effects.clear();
    }
} // namespace Desert::Water
