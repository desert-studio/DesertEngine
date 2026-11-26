#include <Water/Public/WaterBodyGather.hpp>

#include <Engine/ECS/TransformComponent.hpp>
#include <Water/Public/WaterBodyComponent.hpp>
#include <Water/Public/WaterRiverBakes.hpp>
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
        // The world's river bakes (refreshed before any system): a river with none is no water.
        const auto* const      bakesLink = registry.try_ctx<std::shared_ptr<WaterRiverBakes>>();
        const WaterRiverBakes* bakes     = bakesLink != nullptr ? bakesLink->get() : nullptr;
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
            // The body whose waves it carries: its own, or, for a river that ends in an ocean, that ocean's.
            entt::entity waveOwner = entity;
            if ( data.Type == Water::WaterBodyType::River )
            {
                // A river is its ribbon: its footprint's centre at its mean level, its bounds the extents.
                const WaterRiverBakes::River* const bake = bakes != nullptr ? bakes->Find( entity ) : nullptr;
                if ( bake == nullptr || bake->Flow->Size.x == 0u )
                    continue; // hidden, refused, fewer than two points or no width: no water
                const Water::RiverFlowField& flow = *bake->Flow;
                body.Location = glm::vec3( flow.Min.x + 0.5f * flow.Extent.x, flow.SurfaceY,
                                           flow.Min.y + 0.5f * flow.Extent.y );
                body.Extents  = flow.Extent;
                body.Flow     = bake->Flow;
                if ( flow.Mouth )
                    waveOwner = static_cast<entt::entity>( flow.Mouth->Into );
            }

            // No lane is a flat body (UE HasWaves() false), not an error.
            if ( const int32_t index = sources.LaneOf( waveOwner ); index >= 0 )
                Water::SetWaveLane( body, table.Lanes[static_cast<size_t>( index )], *table.Surface );
            m_Bodies.push_back( std::move( body ) );
        }

        // Every fall's plunge belongs to the body it lands in: of those covering its foot, the one of the highest
        // WaterBodyPriority (the river where it still flows there, else the ocean it pours into), as the surface
        // drawn there is. A plunge no body covers lands in no water.
        for ( const Water::WaterBodyState& river : m_Bodies )
        {
            if ( river.Type != Water::WaterBodyType::River )
                continue;
            for ( const Water::RiverFall& fall : river.Flow->Falls )
            {
                Water::WaterBodyState* into = nullptr;
                for ( Water::WaterBodyState& candidate : m_Bodies )
                    if ( Water::WaterBodyCovers( candidate, fall.Plunge.Foot ) &&
                         ( into == nullptr ||
                           Water::WaterBodyPriority( candidate.Type ) > Water::WaterBodyPriority( into->Type ) ) )
                        into = &candidate;
                if ( into != nullptr )
                    into->Plunges.push_back( fall.Plunge );
            }
        }
        return m_Bodies;
    }

    void WaterBodyGather::Reset()
    {
        m_Members.clear();
        m_Bodies.clear();
        m_BuiltFor      = nullptr;
        m_BuiltRevision = 0;
    }
} // namespace Desert::ECS
