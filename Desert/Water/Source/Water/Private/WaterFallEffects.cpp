#include <Water/Public/WaterFallEffects.hpp>

#include <Engine/Core/Camera.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/AudioSourceComponent.hpp>
#include <Engine/ECS/Entity.hpp>
#include <Engine/ECS/EntityVisibility.hpp>
#include <Engine/ECS/TransformComponent.hpp>
#include <Engine/ECS/TransientComponent.hpp>
#include <Engine/ECS/UUIDComponent.hpp>
#include <VFX/Public/VFXComponent.hpp>
#include <Water/Public/WaterBodyComponent.hpp>
#include <Water/Public/WaterRiverBakes.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

namespace Desert::Water
{
    namespace
    {
        // The kinds, in WaterBodyData's order (FallSpray, FallMist, FallSound): the last one is heard, not seen.
        constexpr std::array<const char*, 3> kEffectNames = { "WaterFallSpray", "WaterFallMist",
                                                              "WaterFallSound" };
        constexpr uint32_t                   kSoundKind   = 2u;

        struct Wanted
        {
            entt::entity        River = entt::null;
            uint32_t            Fall  = 0u;
            uint32_t            Kind  = 0u;
            Assets::AssetHandle Asset;
            glm::vec3           Position{ 0.0f };
            float               Yaw       = 0.0f;
            glm::vec4           Extent{ 0.0f };
            float               Intensity = 0.0f;
        };

        bool IsAlive( entt::registry& registry, entt::entity entity, const Common::UUID& id )
        {
            if ( !registry.valid( entity ) )
                return false;
            const auto* uuid = registry.try_get<ECS::UUIDComponent>( entity );
            return uuid != nullptr && uuid->UUID == id;
        }

        // The point of the fall's sheet (its centre line swept ±Width/2 along Across) nearest @p listener.
        glm::vec3 NearestSheetPoint( const RiverFall& fall, const glm::vec3& listener )
        {
            const float half         = 0.5f * fall.Width;
            glm::vec3   best         = fall.Plunge.Foot;
            float       bestDistance = std::numeric_limits<float>::max();
            for ( size_t k = 0; k + 1 < fall.Samples.size(); ++k )
            {
                // The segment's centre line point nearest the listener once its offset across is taken out.
                const glm::vec3 a       = fall.Samples[k].Position;
                const glm::vec3 b       = fall.Samples[k + 1].Position;
                const glm::vec3 seen    = listener - fall.Across * glm::dot( listener - a, fall.Across );
                const glm::vec3 ab      = b - a;
                const float     length2 = glm::dot( ab, ab );
                const float     t =
                     length2 > 0.0f ? std::clamp( glm::dot( seen - a, ab ) / length2, 0.0f, 1.0f ) : 0.0f;
                const glm::vec3 centre   = a + t * ab;
                const float     across   = std::clamp( glm::dot( listener - centre, fall.Across ), -half, half );
                const glm::vec3 point    = centre + across * fall.Across;
                const float     distance = glm::distance( point, listener );
                if ( distance < bestDistance )
                {
                    bestDistance = distance;
                    best         = point;
                }
            }
            return best;
        }
    } // namespace

    void WaterFallEffects::Tick( Core::Scene& scene, entt::registry& registry, double )
    {
        ++m_Tick;

        // The world's river bakes: a river with no bake (hidden, refused, a world without gravity) plays nothing.
        const auto* const bakesLink = registry.try_ctx<std::shared_ptr<ECS::WaterRiverBakes>>();
        if ( bakesLink == nullptr )
            return;
        const ECS::WaterRiverBakes& bakes = **bakesLink;

        // The listener the sound follows (AudioECSSystem's: the active camera).
        std::optional<glm::vec3> listener;
        if ( const auto camera = scene.GetActiveCamera() )
            listener = camera->GetPosition();

        // Collected before any entity is made: making one writes the pools the rivers are read from.
        std::vector<Wanted> wanted;
        for ( const entt::entity entity : registry.view<ECS::WaterBodyComponent, ECS::TransformComponent>() )
        {
            const ECS::WaterBodyData& data = registry.get<ECS::WaterBodyComponent>( entity ).Data;
            if ( data.Type != WaterBodyType::River )
                continue;
            const std::array<Assets::AssetHandle, 3> assets = { data.FallSpray, data.FallMist, data.FallSound };
            if ( std::all_of( assets.begin(), assets.end(),
                              []( const Assets::AssetHandle& a ) { return a == Assets::AssetHandle::Null(); } ) )
                continue;
            const ECS::WaterRiverBakes::River* const river = bakes.Find( entity );
            if ( river == nullptr )
                continue;

            for ( uint32_t i = 0u; i < static_cast<uint32_t>( river->Flow->Falls.size() ); ++i )
            {
                const RiverFall&  fall   = river->Flow->Falls[i];
                const FallPlunge& plunge = fall.Plunge;
                const float intensity    = std::clamp( plunge.LandingSpeed / kFallFullIntensitySpeed, 0.0f, 1.0f );
                // The yaw turning +X onto the landing line (a turn by yaw takes X to ( cos, 0, -sin )).
                const float yaw = std::atan2( -plunge.Across.z, plunge.Across.x );
                for ( uint32_t kind = 0u; kind < static_cast<uint32_t>( assets.size() ); ++kind )
                {
                    if ( assets[kind] == Assets::AssetHandle::Null() )
                        continue;
                    wanted.push_back( Wanted{ .River     = entity,
                                              .Fall      = i,
                                              .Kind      = kind,
                                              .Asset     = assets[kind],
                                              .Position  = kind == kSoundKind && listener
                                                                ? NearestSheetPoint( fall, *listener )
                                                                : plunge.Foot,
                                              .Yaw       = yaw,
                                              .Extent    = glm::vec4( fall.Width, 0.0f, plunge.Radius, 0.0f ),
                                              .Intensity = intensity } );
                }
            }
        }

        for ( const Wanted& want : wanted )
        {
            Effect& effect  = m_Effects[EffectKey{ want.River, want.Fall, want.Kind }];
            effect.SeenTick = m_Tick;
            if ( !IsAlive( registry, effect.Entity, effect.Id ) )
            {
                effect.Id     = Common::UUID::Generate();
                effect.Entity = scene.CreateEntityWithUUID( effect.Id, kEffectNames[want.Kind] ).GetHandle();
                registry.emplace<ECS::TransientComponent>( effect.Entity );
                if ( want.Kind == kSoundKind )
                    registry.emplace<ECS::AudioSourceComponent>( effect.Entity );
                else
                    registry.emplace<ECS::VFXComponent>( effect.Entity );
            }

            ECS::LocalTransform local;
            local.Translation = want.Position;
            local.Rotation    = glm::vec3( 0.0f, want.Yaw, 0.0f );
            ECS::SetLocalTransform( registry, effect.Entity, local );

            if ( want.Kind == kSoundKind )
            {
                // Played by AudioECSSystem in Play, as any source: a loop, placed, as loud as the water lands
                // fast.
                ECS::AudioSourceData& sound = registry.get<ECS::AudioSourceComponent>( effect.Entity ).Data;
                sound.Sound                 = want.Asset;
                sound.Volume                = want.Intensity;
                sound.Loop                  = true;
                sound.AutoPlay              = true;
                sound.Spatial               = true;
                continue;
            }
            auto& vfx       = registry.get<ECS::VFXComponent>( effect.Entity );
            vfx.Data.System = want.Asset;
            vfx.Data.SetVariable( "Extent", want.Extent );
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
        m_Effects.clear();
    }
} // namespace Desert::Water
