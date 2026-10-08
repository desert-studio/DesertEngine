#include <Engine/ECS/System/EntityOverlaps.hpp>

#include <Engine/ECS/Components.hpp>

#include <algorithm>

namespace Desert::ECS
{
    EntityWorldPose ComputeEntityWorldPose( const entt::registry& registry, entt::entity entity )
    {
        // Walk the parents so a body on a CHILD entity (a wall inside a "House" prefab root) is where it is,
        // not at its local offset.
        glm::mat4    world = registry.has<TransformComponent>( entity )
                                  ? registry.get<TransformComponent>( entity ).GetTransform()
                                  : glm::mat4( 1.0f );
        entt::entity cur   = entity;
        while ( registry.has<RelationshipComponent>( cur ) )
        {
            const auto& rel = registry.get<RelationshipComponent>( cur );
            if ( rel.Parent == entt::null )
                break;
            cur = rel.Parent;
            if ( registry.has<TransformComponent>( cur ) )
                world = registry.get<TransformComponent>( cur ).GetTransform() * world;
        }

        EntityWorldPose pose;
        pose.Position = glm::vec3( world[3] );
        glm::mat3 basis( world ); // strip scale so quat_cast gives a clean rotation
        pose.Scale = glm::vec3( glm::length( basis[0] ), glm::length( basis[1] ), glm::length( basis[2] ) );
        for ( int axis = 0; axis < 3; ++axis )
            if ( pose.Scale[axis] > 1e-6f )
                basis[axis] /= pose.Scale[axis];
        pose.Rotation = glm::quat_cast( basis );
        return pose;
    }

    void DriveKinematicBodies( entt::registry& registry, Physics::PhysicsWorld& world )
    {
        auto bodies = registry.view<TransformComponent, RigidBodyComponent>();
        for ( auto entity : bodies )
        {
            const auto& rb = bodies.get<RigidBodyComponent>( entity );
            if ( rb.RuntimeBody == Physics::kInvalidBody || rb.Data.Type != Physics::BodyType::Kinematic )
                continue;
            const EntityWorldPose pose = ComputeEntityWorldPose( registry, entity );
            world.SetKinematicTarget( rb.RuntimeBody, pose.Position, pose.Rotation );
        }
    }

    EntityOverlapRouter::EntityOverlapRouter( Physics::PhysicsWorld& world ) : m_World( &world )
    {
        m_WorldSubscription = m_World->SubscribeOverlaps( [this]( const Physics::OverlapEvent& event )
                                                          { m_StepEvents.push_back( event ); } );
    }

    EntityOverlapRouter::~EntityOverlapRouter()
    {
        m_World->UnsubscribeOverlaps( m_WorldSubscription );
    }

    void EntityOverlapRouter::Track( Physics::BodyHandle body, entt::entity entity )
    {
        if ( body != Physics::kInvalidBody )
            m_Owners[body] = entity;
    }

    void EntityOverlapRouter::BeginFrame( entt::registry& registry )
    {
        for ( auto entity : registry.view<OverlapEventsComponent>() )
            registry.get<OverlapEventsComponent>( entity ).Pending.clear();
    }

    entt::entity EntityOverlapRouter::EntityOf( Physics::BodyHandle body ) const
    {
        const auto found = m_Owners.find( body );
        return found == m_Owners.end() ? entt::null : found->second;
    }

    void EntityOverlapRouter::Deliver( entt::registry& registry, uint32_t fixedSteps )
    {
        std::vector<Physics::OverlapEvent> events;
        events.swap( m_StepEvents );
        for ( const Physics::OverlapEvent& raw : events )
        {
            EntityOverlapEvent event;
            event.Phase   = raw.Phase;
            event.Trigger = EntityOf( raw.Trigger );
            event.Other   = EntityOf( raw.Other );
            if ( event.Trigger == entt::null && event.Other == entt::null )
                continue; // two bodies no entity owns: nobody to tell

            const bool begin = raw.Phase == Physics::OverlapPhase::Begin;
            if ( event.Trigger != entt::null && registry.valid( event.Trigger ) )
                registry.get_or_emplace<OverlapEventsComponent>( event.Trigger )
                     .Pending.push_back( { begin, event.Other } );
            if ( event.Other != entt::null && registry.valid( event.Other ) )
                registry.get_or_emplace<OverlapEventsComponent>( event.Other )
                     .Pending.push_back( { begin, event.Trigger } );

            // A copy: a subscriber may unsubscribe (or subscribe) from inside its callback.
            const auto subscribers = m_Subscribers;
            for ( const auto& [id, callback] : subscribers )
                callback( event );
        }

        // Every End a destroyed entity's body was owed came out of the fixed step that followed its removal, so
        // once a fixed step has run the entry has nothing left to name.
        if ( fixedSteps > 0u )
            std::erase_if( m_Owners,
                           [&registry]( const auto& owner ) { return !registry.valid( owner.second ); } );
    }

    EntityOverlapSubscription EntityOverlapRouter::SubscribeOverlaps( EntityOverlapCallback callback )
    {
        const EntityOverlapSubscription id = m_NextSubscription++;
        m_Subscribers.emplace_back( id, std::move( callback ) );
        return id;
    }

    void EntityOverlapRouter::UnsubscribeOverlaps( EntityOverlapSubscription subscription )
    {
        std::erase_if( m_Subscribers,
                       [subscription]( const auto& entry ) { return entry.first == subscription; } );
    }
} // namespace Desert::ECS
