#include "DestructionVFXEvents.hpp"

#include <Engine/ECS/DestructibleComponent.hpp>
#include <Engine/VFX/VFXDataChannel.hpp>

#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace Desert::ECS
{
    namespace
    {
        using Destruction::DestructionEvent;
        using Destruction::DestructionEventKind;

        bool Publishes( const DestructionEventSource& source, const DestructionEventKind kind )
        {
            switch ( kind )
            {
                case DestructionEventKind::Break:
                    return source.NotifyBreaks;
                case DestructionEventKind::Collision:
                    return source.NotifyCollisions;
                case DestructionEventKind::Removed:
                    return source.NotifyRemovals;
            }
            return false;
        }

        std::string_view ChannelOf( const DestructionEventKind kind )
        {
            switch ( kind )
            {
                case DestructionEventKind::Break:
                    return kDestructionBreakChannel;
                case DestructionEventKind::Collision:
                    return kDestructionCollisionChannel;
                case DestructionEventKind::Removed:
                    return kDestructionRemovedChannel;
            }
            return kDestructionBreakChannel;
        }

        // The payload of one event into entry @p i of @p w; the field names are the channel assets' own.
        Common::BoolResultStr WriteEntry( VFX::VFXDataChannelWriter& w, const std::size_t i,
                                          const DestructionEvent& e, const int32_t entity )
        {
            std::vector<Common::BoolResultStr> writes;
            writes.push_back( w.WritePosition( i, "Position", e.Position ) );
            writes.push_back( w.WriteDirection( i, "Velocity", e.Velocity ) );
            writes.push_back( w.WriteFloat( i, "MassKg", e.MassKg ) );
            writes.push_back( w.WriteInt( i, "SourceEntity", entity ) );
            switch ( e.Kind )
            {
                case DestructionEventKind::Break:
                    writes.push_back( w.WriteFloat( i, "Speed", glm::length( e.Velocity ) ) );
                    writes.push_back( w.WriteInt( i, "PieceCount", static_cast<int32_t>( e.PieceCount ) ) );
                    break;
                case DestructionEventKind::Collision:
                    writes.push_back( w.WriteDirection( i, "Normal", e.Normal ) );
                    writes.push_back( w.WriteFloat( i, "Impulse", e.Impulse ) );
                    break;
                case DestructionEventKind::Removed:
                    writes.push_back( w.WriteInt( i, "PieceCount", static_cast<int32_t>( e.PieceCount ) ) );
                    break;
            }
            for ( const Common::BoolResultStr& written : writes )
                if ( !written.IsSuccess() )
                    return Common::MakeFormattedError<bool>( "destruction channel '{}': {}", ChannelOf( e.Kind ),
                                                             written.GetError() );
            return Common::MakeSuccess( true );
        }
    } // namespace

    DestructionEventSources CollectDestructionEventSources( entt::registry& registry )
    {
        DestructionEventSources sources;
        auto                    view = registry.view<DestructibleComponent>();
        for ( const entt::entity entity : view )
        {
            const DestructibleComponent& component = view.get<DestructibleComponent>( entity );
            if ( component.RuntimeObject == Destruction::kInvalidDestructible )
                continue;
            DestructionEventSource source;
            source.Entity                    = static_cast<int32_t>( entt::to_integral( entity ) &
                                                                     entt::entt_traits<entt::entity>::entity_mask );
            source.NotifyBreaks              = component.Data.NotifyBreaks;
            source.NotifyCollisions          = component.Data.NotifyCollisions;
            source.NotifyRemovals            = component.Data.NotifyRemovals;
            sources[component.RuntimeObject] = source;
        }
        return sources;
    }

    Common::BoolResultStr UseDestructionChannels( const DestructionEventSources& sources,
                                                  VFX::VFXDataChannels& channels, Assets::AssetManager& assets )
    {
        bool wanted[3] = { false, false, false };
        for ( const auto& [object, source] : sources )
        {
            wanted[0] = wanted[0] || source.NotifyBreaks;
            wanted[1] = wanted[1] || source.NotifyCollisions;
            wanted[2] = wanted[2] || source.NotifyRemovals;
        }
        const std::string_view names[3] = { kDestructionBreakChannel, kDestructionCollisionChannel,
                                            kDestructionRemovedChannel };
        for ( int k = 0; k < 3; ++k )
        {
            if ( !wanted[k] || channels.Find( names[k] ) != nullptr )
                continue;
            if ( auto used = channels.Use( names[k], assets ); !used.IsSuccess() )
                return used;
        }
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr WriteDestructionEventsToVFX( const std::span<const DestructionEvent> events,
                                                       const DestructionEventSources&          sources,
                                                       VFX::VFXDataChannels&                   channels,
                                                       DestructionVFXReport&                   report )
    {
        report = DestructionVFXReport{};
        for ( const DestructionEventKind kind :
              { DestructionEventKind::Break, DestructionEventKind::Collision, DestructionEventKind::Removed } )
        {
            std::vector<std::pair<const DestructionEvent*, int32_t>> published;
            for ( const DestructionEvent& event : events )
            {
                if ( event.Kind != kind )
                    continue;
                const auto source = sources.find( event.Object );
                if ( source == sources.end() || !Publishes( source->second, kind ) )
                    continue;
                published.emplace_back( &event, source->second.Entity );
            }
            if ( published.empty() )
                continue;

            auto writer = channels.Write( ChannelOf( kind ), published.size() );
            if ( !writer.IsSuccess() )
                return Common::MakeFormattedError<bool>( "destruction events: {}", writer.GetError() );
            VFX::VFXDataChannelWriter& w = writer.GetValue();
            for ( std::size_t i = 0; i < published.size(); ++i )
                if ( auto written = WriteEntry( w, i, *published[i].first, published[i].second );
                     !written.IsSuccess() )
                    return written;

            const auto count = static_cast<uint32_t>( published.size() );
            switch ( kind )
            {
                case DestructionEventKind::Break:
                    report.Breaks = count;
                    break;
                case DestructionEventKind::Collision:
                    report.Collisions = count;
                    break;
                case DestructionEventKind::Removed:
                    report.Removals = count;
                    break;
            }
        }
        return Common::MakeSuccess( true );
    }
} // namespace Desert::ECS
