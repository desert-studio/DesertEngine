#include <Engine/ECS/System/WaterBodyGather.hpp>

#include <Engine/ECS/Components.hpp>

#include <Common/Core/Logger.hpp>

namespace Desert::ECS
{
    namespace
    {
        void RaiseWaterBodyRevision( entt::registry& registry, entt::entity )
        {
            if ( auto* revision = registry.ctx().find<WaterBodyRevision>() )
                ++revision->Value;
        }

        // The registry's membership key; the first ask connects the signals that raise it.
        uint64_t WaterBodyRevisionOf( entt::registry& registry )
        {
            auto* found = registry.ctx().find<WaterBodyRevision>();
            if ( found == nullptr )
            {
                found = &registry.ctx().emplace<WaterBodyRevision>();
                registry.on_construct<WaterBodyComponent>().connect<&RaiseWaterBodyRevision>();
                registry.on_update<WaterBodyComponent>().connect<&RaiseWaterBodyRevision>();
                registry.on_destroy<WaterBodyComponent>().connect<&RaiseWaterBodyRevision>();
            }
            return found->Value;
        }
    } // namespace

    std::span<const Water::WaterBodyState> WaterBodyGather::Gather( entt::registry&    registry,
                                                                    const WavesLookup& lookup )
    {
        const uint64_t revision = WaterBodyRevisionOf( registry );
        if ( m_BuiltFor != &registry || m_BuiltRevision != revision )
        {
            m_Members.clear();
            for ( const entt::entity entity : registry.view<WaterBodyComponent>() )
                m_Members.push_back( entity );
            m_BuiltFor      = &registry;
            m_BuiltRevision = revision;
        }

        m_Bodies.clear();
        for ( const entt::entity entity : m_Members )
        {
            if ( m_Refused.contains( entity ) )
                continue;
            // A body whose TransformComponent went (no signal of this list's) is no water until it has one.
            const auto* transform = registry.try_get<TransformComponent>( entity );
            if ( transform == nullptr )
                continue;
            const WaterBodyData& data = registry.get<WaterBodyComponent>( entity ).Data;

            Water::WaterBodyState body;
            // The entity's position is the still water level (Y) and the WaveOrigin (X, Z), as UE's ocean
            // actor location is.
            body.Location            = transform->GetTranslation();
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
            m_Bodies.push_back( std::move( body ) );
        }
        return m_Bodies;
    }

    void WaterBodyGather::Reset()
    {
        m_Refused.clear();
        m_Members.clear();
        m_Bodies.clear();
        m_BuiltFor      = nullptr;
        m_BuiltRevision = 0;
    }
} // namespace Desert::ECS
