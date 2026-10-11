#include <Engine/ECS/System/WaterBodyGather.hpp>

#include <Engine/ECS/Components.hpp>

#include <Common/Core/Logger.hpp>

namespace Desert::ECS
{
    std::vector<Water::WaterBodyState> WaterBodyGather::Gather( entt::registry&    registry,
                                                                const WavesLookup& lookup )
    {
        std::vector<Water::WaterBodyState> bodies;
        const auto                         view = registry.view<WaterBodyComponent, TransformComponent>();
        for ( const entt::entity entity : view )
        {
            if ( m_Refused.contains( entity ) )
                continue;
            const WaterBodyData& data = view.get<WaterBodyComponent>( entity ).Data;

            Water::WaterBodyState body;
            // The entity's position is the still water level (Y) and the WaveOrigin (X, Z), as UE's ocean
            // actor location is.
            body.Location            = view.get<TransformComponent>( entity ).Translation;
            body.Extents             = data.OceanExtents;
            body.TargetWaveMaskDepth = data.TargetWaveMaskDepth;

            // No wave set is a flat ocean (UE HasWaves() false), not an error.
            if ( data.WaterWaves != Assets::AssetHandle::Null() )
            {
                auto waves = lookup( data.WaterWaves );
                if ( !waves )
                {
                    m_Refused.insert( entity );
                    LOG_ERROR( "[Water] entity {} is no water body: {}", static_cast<uint32_t>( entity ),
                               waves.GetError() );
                    continue;
                }
                // Pending: the set is still being read; this step goes without the body, a later one has it.
                if ( !waves.GetValue() )
                    continue;
                body.Waves = waves.GetValue();
            }
            bodies.push_back( std::move( body ) );
        }
        return bodies;
    }

    void WaterBodyGather::Reset()
    {
        m_Refused.clear();
    }
} // namespace Desert::ECS
