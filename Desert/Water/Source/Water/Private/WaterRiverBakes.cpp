#include <Water/Public/WaterRiverBakes.hpp>

#include <Water/Public/WaterBodyComponent.hpp>
#include <Water/Public/WaterWaveSources.hpp>

#include <Common/Core/Logger.hpp>
#include <Engine/Core/SceneSettings.hpp>
#include <Engine/ECS/EntityVisibility.hpp>
#include <Engine/ECS/TransformComponent.hpp>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <format>
#include <string>

namespace Desert::ECS
{
    void WaterRiverBakes::Refresh( entt::registry& registry, const WaterWaveSources& sources )
    {
        // THE GRAVITY: the scene's, the only one (a fall's flight, its speeds and its plunge follow from it).
        const auto* const link = registry.try_ctx<Core::SceneSettingsLink>();
        std::string       refusal;
        if ( link == nullptr || link->Settings == nullptr )
            refusal = "the world has no scene settings (Core::SceneSettingsLink), so no gravity to fly a fall by";
        else if ( !( link->Settings->Gravity > 0.0f ) )
            refusal = "the scene's Gravity is 0, so a fall's water never lands";
        if ( !refusal.empty() )
        {
            if ( refusal != m_Refusal )
                LOG_ERROR( "[Water] no river is baked: {}", refusal );
            m_Refusal = std::move( refusal );
            if ( !m_Rivers.empty() )
            {
                m_Rivers.clear();
                ++m_Revision;
            }
            return;
        }
        m_Refusal.clear();
        const float gravity = link->Settings->Gravity;

        const auto view   = registry.view<WaterBodyComponent, TransformComponent>();
        const auto absent = [&registry, &sources]( const entt::entity entity )
        { return IsHidden( registry, entity ) || sources.IsRefused( entity ); };

        // THE RECEIVERS: the world's oceans a river may end in (part of every river's key).
        m_Receivers.clear();
        for ( const entt::entity entity : view )
        {
            const WaterBodyData& data = view.get<WaterBodyComponent>( entity ).Data;
            if ( data.Type != Water::WaterBodyType::Ocean || absent( entity ) )
                continue;
            const glm::vec3& location = view.get<TransformComponent>( entity ).GetTranslation();
            const glm::vec2  centre( location.x, location.z );
            const glm::vec2  half = 0.5f * data.OceanExtents;
            m_Receivers.push_back( Water::RiverReceiver{ .Id    = static_cast<uint32_t>( entity ),
                                                         .Min   = centre - half,
                                                         .Max   = centre + half,
                                                         .Level = location.y } );
        }

        bool changed = false;
        // The rivers that are gone (destroyed, hidden, refused, no longer a river) are forgotten.
        for ( auto it = m_Rivers.begin(); it != m_Rivers.end(); )
        {
            const entt::entity entity = it->first;
            const bool         river  = registry.valid( entity ) && view.contains( entity ) && !absent( entity ) &&
                               view.get<WaterBodyComponent>( entity ).Data.Type == Water::WaterBodyType::River;
            if ( river )
            {
                ++it;
                continue;
            }
            it      = m_Rivers.erase( it );
            changed = true;
        }

        for ( const entt::entity entity : view )
        {
            const WaterBodyData& data = view.get<WaterBodyComponent>( entity ).Data;
            if ( data.Type != Water::WaterBodyType::River || absent( entity ) )
                continue;
            const Water::RiverBakeInput input{ .LocalPoints = RiverPointsOf( data ),
                                               .Origin = view.get<TransformComponent>( entity ).GetTranslation(),
                                               .Width  = data.RiverWidth,
                                               .FlowSpeed  = data.RiverFlowSpeed,
                                               .FlowPeriod = data.RiverFlowPeriod,
                                               .Gravity    = gravity,
                                               .Receivers  = m_Receivers };
            const uint64_t              key   = Water::RiverBakeKey( input );
            River&                      river = m_Rivers[entity];
            if ( river.Flow != nullptr && river.Key == key )
                continue;
            river.Key  = key;
            river.Flow = Water::BakeRiver( input );
            changed    = true;

            // One line per re-bake: what the falls and the mouth came out as (the acceptance reads these numbers).
            std::string what;
            for ( size_t k = 0; k < river.Flow->Falls.size(); ++k )
            {
                const Water::RiverFall& fall = river.Flow->Falls[k];
                what +=
                     std::format( "; fall {} lip ({:.0f}, {:.0f}, {:.0f}) foot ({:.0f}, {:.0f}, {:.0f}) landing "
                                  "speed {:.0f} cm/s plunge radius {:.0f} cm",
                                  k, fall.Lip.x, fall.Lip.y, fall.Lip.z, fall.Foot.x, fall.Foot.y, fall.Foot.z,
                                  fall.Plunge.LandingSpeed, fall.Plunge.Radius );
            }
            if ( river.Flow->Mouth )
                what += std::format( "; mouth into {} at ({:.0f}, {:.0f}) radius {:.0f} cm",
                                     river.Flow->Mouth->Into, river.Flow->Mouth->Point.x,
                                     river.Flow->Mouth->Point.y, river.Flow->Mouth->Radius );
            else
                what += "; no mouth";
            LOG_INFO( "[Water] river {}: baked {}x{} texels{}", static_cast<uint32_t>( entity ),
                      river.Flow->Size.x, river.Flow->Size.y, what );
        }
        if ( changed )
            ++m_Revision;
    }

    const WaterRiverBakes::River* WaterRiverBakes::Find( const entt::entity river ) const noexcept
    {
        const auto it = m_Rivers.find( river );
        return it != m_Rivers.end() ? &it->second : nullptr;
    }

    void WaterRiverBakes::Reset() noexcept
    {
        m_Rivers.clear();
        m_Receivers.clear();
        m_Refusal.clear();
        ++m_Revision;
    }
} // namespace Desert::ECS
