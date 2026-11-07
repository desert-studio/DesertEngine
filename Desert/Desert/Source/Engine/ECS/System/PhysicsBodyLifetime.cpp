#include <Engine/ECS/System/PhysicsBodyLifetime.hpp>

#include <Engine/ECS/Components.hpp>

namespace Desert::ECS
{
    PhysicsBodyLifetime::~PhysicsBodyLifetime()
    {
        Detach();
    }

    void PhysicsBodyLifetime::Attach( entt::registry& registry )
    {
        if ( m_Registry == &registry )
            return;
        Detach();
        registry.on_destroy<RigidBodyComponent>().connect<&PhysicsBodyLifetime::OnRigidBodyDestroyed>( this );
        registry.on_destroy<ColliderComponent>().connect<&PhysicsBodyLifetime::OnColliderDestroyed>( this );
        registry.on_destroy<CharacterControllerComponent>().connect<&PhysicsBodyLifetime::OnCharacterDestroyed>(
             this );
        registry.on_construct<RigidBodyComponent>().connect<&PhysicsBodyLifetime::OnBodySourceChanged>( this );
        registry.on_update<RigidBodyComponent>().connect<&PhysicsBodyLifetime::OnBodySourceChanged>( this );
        registry.on_construct<ColliderComponent>().connect<&PhysicsBodyLifetime::OnBodySourceChanged>( this );
        registry.on_update<ColliderComponent>().connect<&PhysicsBodyLifetime::OnBodySourceChanged>( this );
        registry.on_construct<CharacterControllerComponent>().connect<&PhysicsBodyLifetime::OnCharacterChanged>(
             this );
        registry.on_update<CharacterControllerComponent>().connect<&PhysicsBodyLifetime::OnCharacterChanged>(
             this );
        // Construct only: a Transform is UPDATED every frame by the pose write-back, which must not mark.
        registry.on_construct<TransformComponent>().connect<&PhysicsBodyLifetime::OnTransformConstructed>( this );
        m_Registry = &registry;

        // What the registry already holds when it is attached (scene load, Play start, a swapped registry)
        // never fired a construct this listener heard; every candidate is looked at once.
        for ( const entt::entity entity :
              registry.view<TransformComponent, RigidBodyComponent, ColliderComponent>() )
            Mark( m_PendingBodies, entity );
        for ( const entt::entity entity : registry.view<TransformComponent, CharacterControllerComponent>() )
            Mark( m_PendingCharacters, entity );
    }

    void PhysicsBodyLifetime::Detach()
    {
        if ( m_Registry == nullptr )
            return;
        m_Registry->on_destroy<RigidBodyComponent>().disconnect( this );
        m_Registry->on_destroy<ColliderComponent>().disconnect( this );
        m_Registry->on_destroy<CharacterControllerComponent>().disconnect( this );
        m_Registry->on_construct<RigidBodyComponent>().disconnect( this );
        m_Registry->on_update<RigidBodyComponent>().disconnect( this );
        m_Registry->on_construct<ColliderComponent>().disconnect( this );
        m_Registry->on_update<ColliderComponent>().disconnect( this );
        m_Registry->on_construct<CharacterControllerComponent>().disconnect( this );
        m_Registry->on_update<CharacterControllerComponent>().disconnect( this );
        m_Registry->on_construct<TransformComponent>().disconnect( this );
        m_Registry = nullptr;
        // The entries name entities of the registry let go of; a new one tracks its own bodies as they are made,
        // and Attach seeds its own pending set.
        m_BodyEntities.clear();
        m_PendingBodies.clear();
        m_PendingCharacters.clear();
    }

    void PhysicsBodyLifetime::Track( Physics::BodyHandle body, entt::entity entity )
    {
        if ( body != Physics::kInvalidBody )
            m_BodyEntities.insert_or_assign( body, entity );
    }

    void PhysicsBodyLifetime::Mark( PendingSet& set, entt::entity entity )
    {
        if ( !set.contains( entity ) )
            set.emplace( entity );
    }

    void PhysicsBodyLifetime::Unmark( PendingSet& set, entt::entity entity )
    {
        if ( set.contains( entity ) )
            set.erase( entity );
    }

    void PhysicsBodyLifetime::OnBodySourceChanged( entt::registry&, entt::entity entity )
    {
        // Marked even while the other component is still missing: the creator drops an entity that is not a
        // full Transform + RigidBody + Collider, and the missing one marks it again when it arrives.
        Mark( m_PendingBodies, entity );
    }

    void PhysicsBodyLifetime::OnCharacterChanged( entt::registry&, entt::entity entity )
    {
        Mark( m_PendingCharacters, entity );
    }

    void PhysicsBodyLifetime::OnTransformConstructed( entt::registry& registry, entt::entity entity )
    {
        if ( registry.has<RigidBodyComponent>( entity ) || registry.has<ColliderComponent>( entity ) )
            Mark( m_PendingBodies, entity );
        if ( registry.has<CharacterControllerComponent>( entity ) )
            Mark( m_PendingCharacters, entity );
    }

    void PhysicsBodyLifetime::OnRigidBodyDestroyed( entt::registry& registry, entt::entity entity )
    {
        Unmark( m_PendingBodies, entity );
        auto& rb = registry.get<RigidBodyComponent>( entity );
        m_BodyEntities.erase( rb.RuntimeBody );
        m_World->RemoveBody( rb.RuntimeBody );
        rb.RuntimeBody = Physics::kInvalidBody;
    }

    void PhysicsBodyLifetime::OnColliderDestroyed( entt::registry& registry, entt::entity entity )
    {
        // The body is built from BOTH components, and PhysicsECSSystem's view needs both to keep driving it.
        // Losing the shape leaves a body the system can no longer see, so it goes too; the handle is reset
        // so a collider added back later builds a fresh one rather than being skipped as "already built".
        Unmark( m_PendingBodies, entity );
        if ( auto* rb = registry.try_get<RigidBodyComponent>( entity ) )
        {
            m_BodyEntities.erase( rb->RuntimeBody );
            m_World->RemoveBody( rb->RuntimeBody );
            rb->RuntimeBody = Physics::kInvalidBody;
        }
    }

    void PhysicsBodyLifetime::OnCharacterDestroyed( entt::registry& registry, entt::entity entity )
    {
        Unmark( m_PendingCharacters, entity );
        auto& cc = registry.get<CharacterControllerComponent>( entity );
        m_BodyEntities.erase( m_World->GetCharacterBody( cc.RuntimeCharacter ) );
        m_World->RemoveCharacter( cc.RuntimeCharacter );
        cc.RuntimeCharacter = Physics::kInvalidCharacter;
    }
} // namespace Desert::ECS
