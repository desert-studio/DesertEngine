#include <Engine/ECS/ComponentEvents.hpp>

#include <Engine/Reflection/ReflectionRegistry.hpp>

#include <algorithm>
#include <format>
#include <string>
#include <utility>

namespace Desert::ECS
{
    Common::ResultStr<ComponentEvents::Subscription> ComponentEvents::Subscribe( entt::entity     self,
                                                                                 std::string_view type,
                                                                                 std::string_view event,
                                                                                 Listener         listener )
    {
        const Reflection::TypeInfo* info = Reflection::ReflectionRegistry::Get().Find( std::string( type ) );
        if ( info == nullptr )
            return Common::MakeError<Subscription>(
                 std::format( "no reflected type '{}' declares events", type ) );
        const Reflection::EventInfo* found = info->FindEvent( event );
        if ( found == nullptr )
            return Common::MakeError<Subscription>( std::format( "'{}' has no event '{}'", type, event ) );
        return Subscribe( self, *found, std::move( listener ) );
    }

    Common::ResultStr<ComponentEvents::Subscription>
    ComponentEvents::Subscribe( entt::entity self, const Reflection::EventInfo& event, Listener listener )
    {
        if ( !listener )
            return Common::MakeError<Subscription>(
                 std::format( "{}.{}: an empty listener listens to nothing", event.Owner, event.Name ) );
        if ( self == entt::null )
            return Common::MakeError<Subscription>(
                 std::format( "{}.{}: the null entity announces nothing", event.Owner, event.Name ) );
        const Subscription id = m_Next++;
        m_Bound[Key{ self, &event }].push_back( Bound{ id, std::move( listener ) } );
        return Common::MakeSuccess( id );
    }

    bool ComponentEvents::Unsubscribe( Subscription subscription )
    {
        for ( auto it = m_Bound.begin(); it != m_Bound.end(); ++it )
        {
            auto&      listeners = it->second;
            const auto found     = std::find_if( listeners.begin(), listeners.end(),
                                                 [&]( const Bound& bound ) { return bound.Id == subscription; } );
            if ( found == listeners.end() )
                continue;
            listeners.erase( found );
            if ( listeners.empty() )
                m_Bound.erase( it );
            return true;
        }
        return false;
    }

    bool ComponentEvents::IsBound( entt::entity self, const Reflection::EventInfo& event ) const
    {
        return m_Bound.contains( Key{ self, &event } );
    }

    Common::ResultStr<std::size_t> ComponentEvents::Broadcast( entt::entity                       self,
                                                               const Reflection::EventInfo&       event,
                                                               std::span<const Reflection::Value> payload ) const
    {
        const auto found = m_Bound.find( Key{ self, &event } );
        if ( found == m_Bound.end() )
            return Common::MakeSuccess( std::size_t{ 0 } );
        if ( payload.size() != event.Params.size() )
            return Common::MakeError<std::size_t>( std::format( "{}.{}: {} values for {} parameters", event.Owner,
                                                                event.Name, payload.size(),
                                                                event.Params.size() ) );
        for ( std::size_t i = 0; i < payload.size(); ++i )
            if ( payload[i].Type() != event.Params[i].Type )
                return Common::MakeError<std::size_t>(
                     std::format( "{}.{}: '{}' is a {}, the payload carries a {}", event.Owner, event.Name,
                                  event.Params[i].Name, Reflection::FieldTypeName( event.Params[i].Type ),
                                  Reflection::FieldTypeName( payload[i].Type() ) ) );

        // A copy: a listener that unbinds itself (or binds another) must not invalidate this walk.
        const std::vector<Bound> listeners = found->second;
        for ( const Bound& bound : listeners )
            bound.Call( payload );
        return Common::MakeSuccess( listeners.size() );
    }

    void ComponentEvents::Prune( const entt::registry& registry )
    {
        std::erase_if( m_Bound, [&]( const auto& entry ) { return !registry.valid( entry.first.Self ); } );
    }
} // namespace Desert::ECS
