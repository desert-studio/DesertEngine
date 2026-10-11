#include <Engine/ECS/System/ComponentEventSystem.hpp>

#include <Engine/ECS/ComponentEvents.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/DestructibleComponent.hpp>
#include <Engine/ECS/DestructionEvents.hpp>
#include <Engine/ECS/PhysicsEvents.hpp>
#include <Engine/Reflection/FunctionThunk.hpp>
#include <Engine/Reflection/ReflectionRegistry.hpp>

#include <Common/Core/Logger.hpp>

#include <array>
#include <span>

namespace Desert::ECS
{
    namespace
    {
        /// The registry's record of `type`.`event`. Every name below is an EVENT(...) of a header the generator
        /// reads, so a miss is a broken build of the reflection, refused loudly rather than delivered nowhere.
        const Reflection::EventInfo* RequireEvent( const char* type, const char* event )
        {
            const Reflection::TypeInfo*  info  = Reflection::ReflectionRegistry::Get().Find( type );
            const Reflection::EventInfo* found = info != nullptr ? info->FindEvent( event ) : nullptr;
            if ( found == nullptr )
                LOG_ERROR( "[Events] {}.{} is not reflected: its facts are not delivered", type, event );
            return found;
        }

        std::size_t Fire( const ComponentEvents& events, entt::entity self, const Reflection::EventInfo& event,
                          std::span<const Reflection::Value> payload )
        {
            auto called = events.Broadcast( self, event, payload );
            if ( !called.IsSuccess() )
            {
                LOG_ERROR( "[Events] {}", called.GetError() );
                return 0;
            }
            return called.GetValue();
        }

        std::size_t DeliverContacts( const PhysicsEventQueue& queue, const ComponentEvents& events )
        {
            const Reflection::EventInfo* hit   = RequireEvent( "RigidBodyData", "OnHit" );
            const Reflection::EventInfo* begin = RequireEvent( "RigidBodyData", "OnBeginOverlap" );
            const Reflection::EventInfo* end   = RequireEvent( "RigidBodyData", "OnEndOverlap" );
            if ( hit == nullptr || begin == nullptr || end == nullptr )
                return 0;

            std::size_t calls = 0;
            for ( const PhysicsEvent& fact : queue.Events )
            {
                switch ( fact.Kind )
                {
                    case PhysicsEventKind::Hit:
                        if ( events.IsBound( fact.Self, *hit ) )
                            calls += Fire( events, fact.Self, *hit,
                                           Reflection::EventPayload<RigidBodyData::OnHit>(
                                                fact.Other, fact.Point, fact.Normal, fact.Impulse ) );
                        break;
                    case PhysicsEventKind::BeginOverlap:
                        if ( events.IsBound( fact.Self, *begin ) )
                            calls += Fire( events, fact.Self, *begin,
                                           Reflection::EventPayload<RigidBodyData::OnBeginOverlap>( fact.Other ) );
                        break;
                    case PhysicsEventKind::EndOverlap:
                        if ( events.IsBound( fact.Self, *end ) )
                            calls += Fire( events, fact.Self, *end,
                                           Reflection::EventPayload<RigidBodyData::OnEndOverlap>( fact.Other ) );
                        break;
                }
            }
            return calls;
        }

        std::size_t DeliverBreaks( const DestructionEventQueue& queue, const ComponentEvents& events )
        {
            const Reflection::EventInfo* broke = RequireEvent( "DestructibleData", "OnBreak" );
            if ( broke == nullptr )
                return 0;
            std::size_t calls = 0;
            for ( const DestructionBreakEvent& fact : queue.Breaks )
                if ( events.IsBound( fact.Self, *broke ) )
                    calls += Fire( events, fact.Self, *broke,
                                   Reflection::EventPayload<DestructibleData::OnBreak>( fact.Node, fact.Position,
                                                                                        fact.Velocity ) );
            return calls;
        }
    } // namespace

    std::size_t DeliverComponentEvents( entt::registry& registry, ComponentEventCursor& cursor )
    {
        const auto* contacts    = registry.try_ctx<PhysicsEventQueue>();
        const auto* breaks      = registry.try_ctx<DestructionEventQueue>();
        const bool  newContacts = contacts != nullptr && contacts->Publication != cursor.Physics;
        const bool  newBreaks   = breaks != nullptr && breaks->Publication != cursor.Destruction;
        // Advanced whether or not anybody listens: a batch published while nobody listened is not news to a
        // listener that subscribes later.
        if ( contacts != nullptr )
            cursor.Physics = contacts->Publication;
        if ( breaks != nullptr )
            cursor.Destruction = breaks->Publication;

        auto* events = registry.try_ctx<ComponentEvents>();
        if ( events == nullptr || events->Empty() || ( !newContacts && !newBreaks ) )
            return 0;
        events->Prune( registry );

        std::size_t calls = 0;
        if ( newContacts )
            calls += DeliverContacts( *contacts, *events );
        if ( newBreaks )
            calls += DeliverBreaks( *breaks, *events );
        return calls;
    }
} // namespace Desert::ECS
