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
//
// It is also the one owner of the body -> entity map (contacts, projectile hits and the pose write-back name
// a Jolt body by its entity). UE Chaos keeps that link on the body itself, set when the body is created and
// gone with it; here an entry is added where the body is made (Track) and erased by the same release that
// gives the body back, so the map is never rebuilt per frame and never names a body that is gone.

#include <PhysicsCore/PhysicsWorld.hpp>

#include <entt/entt.hpp>

#include <unordered_map>

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

        // @p body (a rigid body, or a character's inner body) now belongs to @p entity. Called once, where the
        // body is created; the entry goes with the body's release below.
        void Track( Physics::BodyHandle body, entt::entity entity );

        using BodyEntityMap = std::unordered_map<Physics::BodyHandle, entt::entity>;
        // Every live tracked body of the attached registry, by handle.
        [[nodiscard]] const BodyEntityMap& GetBodyEntities() const noexcept
        {
            return m_BodyEntities;
        }

    private:
        void OnRigidBodyDestroyed( entt::registry& registry, entt::entity entity );
        void OnColliderDestroyed( entt::registry& registry, entt::entity entity );
        void OnCharacterDestroyed( entt::registry& registry, entt::entity entity );

        Physics::PhysicsWorld* m_World    = nullptr;
        entt::registry*        m_Registry = nullptr;
        BodyEntityMap          m_BodyEntities;
    };
} // namespace Desert::ECS
