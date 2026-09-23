#pragma once

// AN ENTITY TAKES ITS JOLT BODY WITH IT.
//
// PhysicsECSSystem creates a Jolt body for every RigidBody+Collider entity and a CharacterVirtual for every
// CharacterController, and until WP6 nothing ever gave either back: `PhysicsWorld::RemoveBody` and
// `RemoveCharacter` had no caller anywhere in the tree. An entity destroyed while playing — by a script's
// `self:destroy()`, by the editor, or by WorldPartition unloading its cell — left a body behind that still
// collided, invisible and unselectable, until Stop tore the whole world down. Removing just the
// ColliderComponent did the same thing more quietly: the system's view stopped seeing the entity, and the
// body stayed where it was.
//
// The release rides EnTT's `on_destroy<Component>` signal, because that is the one event both paths share:
// it fires for `registry.destroy(entity)` and for `registry.remove<Component>(entity)` alike, before the
// component's storage is gone, so the handle is still readable. A per-frame sweep (AudioECSSystem's shape)
// would also work but costs O(bodies) every frame to find O(destroyed) bodies, which is the wrong way round
// for streaming.
//
// Kept out of PhysicsECSSystem.hpp so a suite can drive it with a bare registry and a real PhysicsWorld —
// that header includes Scene, and Scene includes the renderer.

#include <Engine/Physics/PhysicsWorld.hpp>

#include <entt/entt.hpp>

namespace Desert::ECS
{
    class PhysicsBodyLifetime final
    {
    public:
        explicit PhysicsBodyLifetime( Physics::PhysicsWorld& world ) : m_World( &world )
        {
        }
        ~PhysicsBodyLifetime();

        PhysicsBodyLifetime( const PhysicsBodyLifetime& )            = delete;
        PhysicsBodyLifetime& operator=( const PhysicsBodyLifetime& ) = delete;

        // Listens on @p registry from now on. Idempotent for the same registry, and moves the listeners if
        // the registry changes, so it can be called every frame.
        void Attach( entt::registry& registry );

        // Stops listening. Must run before the world it releases into is shut down.
        void Detach();

    private:
        void OnRigidBodyDestroyed( entt::registry& registry, entt::entity entity );
        void OnColliderDestroyed( entt::registry& registry, entt::entity entity );
        void OnCharacterDestroyed( entt::registry& registry, entt::entity entity );

        Physics::PhysicsWorld* m_World    = nullptr;
        entt::registry*        m_Registry = nullptr;
    };
} // namespace Desert::ECS
