#include <Water/Public/WaterBodyGather.hpp>

#include <Engine/ECS/TransformComponent.hpp>
#include <Water/Public/WaterBodyComponent.hpp>
#include <Water/Public/WaterWaveSources.hpp>

#include <utility>

namespace Desert::ECS
{
    namespace
    {
        void RaiseWaterBodyRevision( entt::registry& registry, entt::entity )
        {
            if ( auto* revision = registry.try_ctx<WaterBodyRevision>() )
                ++revision->Value;
        }

        // A body without a transform is no water: gaining or losing one is a change of membership.
        void RaiseForPlacement( entt::registry& registry, entt::entity entity )
        {
            if ( registry.try_get<WaterBodyComponent>( entity ) != nullptr )
                RaiseWaterBodyRevision( registry, entity );
        }
    } // namespace

    uint64_t WaterBodyRevisionOf( entt::registry& registry )
    {
        auto* found = registry.try_ctx<WaterBodyRevision>();
        if ( found == nullptr )
        {
            found = &registry.set<WaterBodyRevision>();
            registry.on_construct<WaterBodyComponent>().connect<&RaiseWaterBodyRevision>();
            registry.on_update<WaterBodyComponent>().connect<&RaiseWaterBodyRevision>();
            registry.on_destroy<WaterBodyComponent>().connect<&RaiseWaterBodyRevision>();
            registry.on_construct<TransformComponent>().connect<&RaiseForPlacement>();
            registry.on_destroy<TransformComponent>().connect<&RaiseForPlacement>();
        }
        return found->Value;
    }

    std::span<const Water::WaterBodyState> WaterBodyGather::Gather( entt::registry&         registry,
                                                                    const WaterWaveSources& sources )
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

        const Water::WaterWaveTable& table = *sources.Table();
        m_Bodies.clear();
        for ( const entt::entity entity : m_Members )
        {
            if ( sources.IsRefused( entity ) )
                continue; // a second ocean is no water
            // A body without a TransformComponent is no water until it has one.
            const auto* transform = registry.try_get<TransformComponent>( entity );
            if ( transform == nullptr )
                continue;
            const WaterBodyData& data = registry.get<WaterBodyComponent>( entity ).Data;

            Water::WaterBodyState body;
            body.Id = static_cast<uint32_t>( entity );
            // The entity's position is the still water level (Y) and the WaveOrigin (X, Z), as UE's ocean
            // actor location is.
            body.Location = transform->GetTranslation();
            body.Extents  = data.OceanExtents;
            body.Type     = data.Type;
            body.Density  = data.Density;
            body.Drag     = data.Drag;
            if ( data.Type == Water::WaterBodyType::River )
            {
                // A river is its ribbon: its footprint's centre at its mean level, its bounds the extents.
                const Water::RiverBakeInput input{ .LocalPoints = RiverPointsOf( data ),
                                                   .Origin      = body.Location,
                                                   .Width       = data.RiverWidth,
                                                   .FlowSpeed   = data.RiverFlowSpeed,
                                                   .FlowPeriod  = data.RiverFlowPeriod };
                RiverBake&                  bake = m_Rivers[entity];
                const uint64_t              key  = Water::RiverBakeKey( input );
                if ( bake.Flow == nullptr || bake.Key != key )
                    bake = RiverBake{ key, Water::BakeRiver( input ) };
                const Water::RiverFlowField& flow = *bake.Flow;
                if ( flow.Size.x == 0u )
                    continue; // fewer than two points or no width: no water yet
                body.Location = glm::vec3( flow.Min.x + 0.5f * flow.Extent.x, flow.SurfaceY,
                                           flow.Min.y + 0.5f * flow.Extent.y );
                body.Extents  = flow.Extent;
                body.Flow     = bake.Flow;
            }

            // No lane is a flat body (UE HasWaves() false), not an error.
            if ( const int32_t index = sources.LaneOf( entity ); index >= 0 )
                Water::SetWaveLane( body, table.Lanes[static_cast<size_t>( index )], *table.Surface );
            m_Bodies.push_back( std::move( body ) );
        }
        return m_Bodies;
    }

    void WaterBodyGather::Reset()
    {
        m_Rivers.clear();
        m_Members.clear();
        m_Bodies.clear();
        m_BuiltFor      = nullptr;
        m_BuiltRevision = 0;
    }
} // namespace Desert::ECS
