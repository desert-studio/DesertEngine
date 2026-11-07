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
        m_Registry = &registry;
    }

    void PhysicsBodyLifetime::Detach()
    {
        if ( m_Registry == nullptr )
            return;
        m_Registry->on_destroy<RigidBodyComponent>().disconnect( this );
        m_Registry->on_destroy<ColliderComponent>().disconnect( this );
        m_Registry->on_destroy<CharacterControllerComponent>().disconnect( this );
        m_Registry = nullptr;
        // The entries name entities of the registry let go of; a new one tracks its own bodies as they are made.
        m_BodyEntities.clear();
    }

    void PhysicsBodyLifetime::Track( Physics::BodyHandle body, entt::entity entity )
    {
        if ( body != Physics::kInvalidBody )
            m_BodyEntities.insert_or_assign( body, entity );
    }

    void PhysicsBodyLifetime::OnRigidBodyDestroyed( entt::registry& registry, entt::entity entity )
    {
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
        if ( auto* rb = registry.try_get<RigidBodyComponent>( entity ) )
        {
            m_BodyEntities.erase( rb->RuntimeBody );
            m_World->RemoveBody( rb->RuntimeBody );
            rb->RuntimeBody = Physics::kInvalidBody;
        }
    }

    void PhysicsBodyLifetime::OnCharacterDestroyed( entt::registry& registry, entt::entity entity )
    {
        auto& cc = registry.get<CharacterControllerComponent>( entity );
        m_BodyEntities.erase( m_World->GetCharacterBody( cc.RuntimeCharacter ) );
        m_World->RemoveCharacter( cc.RuntimeCharacter );
        cc.RuntimeCharacter = Physics::kInvalidCharacter;
    }
} // namespace Desert::ECS
