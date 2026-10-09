#pragma once

// TRIGGER OVERLAPS AS ENTITY EVENTS, AND KINEMATIC BODIES THAT FOLLOW THEIR ENTITY.
//
// PhysicsWorld reports overlaps between BODIES (OverlapEvent: trigger handle, other handle). Gameplay wants
// them between ENTITIES, and on both sides: UE fires OnComponentBeginOverlap on the trigger's component and on
// the component that walked into it. EntityOverlapRouter keeps the body -> entity map PhysicsECSSystem fills
// as it creates bodies, turns each world event into one EntityOverlapEvent, queues it in BOTH entities'
// OverlapEventsComponent (ScriptSystem delivers those to Lua) and hands it to C++ subscribers.
//
// An entity destroyed inside a trigger: PhysicsBodyLifetime removes its body, the world ends that body's
// overlaps at its next fixed step, and the router still knows whose body it was — the map entry of a dead
// entity is dropped only by a Deliver whose Step ran a fixed step (the one that produced that End), so the End
// is never lost to a frame whose Step only added to the backlog.
//
// DriveKinematicBodies is the other half of the physics sync: a Kinematic RigidBody is moved by its entity's
// world transform (UE: kinematic bodies follow the component), handed to the world as a kinematic target so
// the body travels over the step and contacts and triggers see it move.
//
// Kept out of PhysicsECSSystem.hpp so a suite can drive both with a bare registry and a real PhysicsWorld —
// that header includes Scene, and Scene includes the renderer.

#include <Engine/Physics/PhysicsWorld.hpp>

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Desert::ECS
{
    /// One overlap change between two entities. @c Other may already be destroyed (an End its removal caused),
    /// or entt::null when the other body belongs to no entity (a landscape tile, a destructible chunk).
    struct EntityOverlapEvent
    {
        Physics::OverlapPhase Phase   = Physics::OverlapPhase::Begin;
        entt::entity          Trigger = entt::null;
        entt::entity          Other   = entt::null;
    };

    using EntityOverlapCallback     = std::function<void( const EntityOverlapEvent& )>;
    using EntityOverlapSubscription = uint32_t;

    /// An entity's pose in the world: its transform composed with every parent's, scale split off.
    struct EntityWorldPose
    {
        glm::vec3 Position = glm::vec3( 0.0f );
        glm::quat Rotation = glm::quat( 1.0f, 0.0f, 0.0f, 0.0f );
        glm::vec3 Scale    = glm::vec3( 1.0f );
    };

    [[nodiscard]] EntityWorldPose ComputeEntityWorldPose( const entt::registry& registry, entt::entity entity );

    /// Hands every Kinematic RigidBody's world pose to @p world as its kinematic target. Call before Step.
    void DriveKinematicBodies( entt::registry& registry, Physics::PhysicsWorld& world );

    class EntityOverlapRouter final
    {
    public:
        explicit EntityOverlapRouter( Physics::PhysicsWorld& world );
        ~EntityOverlapRouter();

        EntityOverlapRouter( const EntityOverlapRouter& )            = delete;
        EntityOverlapRouter& operator=( const EntityOverlapRouter& ) = delete;

        /// @p body belongs to @p entity from now on (a RigidBody's body, a character's inner body).
        void Track( Physics::BodyHandle body, entt::entity entity );

        /// Empties every entity's OverlapEventsComponent queue. Call before the frame's Step: whatever no script
        /// took during the frame (an entity without scripts) is not carried into the next.
        void BeginFrame( entt::registry& registry );

        /// Turns the world events of the Step that just ran into entity events: queued on both entities (the
        /// live ones), then given to the subscribers. @p fixedSteps is what Step returned.
        void Deliver( entt::registry& registry, uint32_t fixedSteps );

        EntityOverlapSubscription SubscribeOverlaps( EntityOverlapCallback callback );
        void                      UnsubscribeOverlaps( EntityOverlapSubscription subscription );

    private:
        [[nodiscard]] entt::entity EntityOf( Physics::BodyHandle body ) const;

        Physics::PhysicsWorld*                                m_World             = nullptr;
        Physics::OverlapSubscription                          m_WorldSubscription = 0u;
        std::unordered_map<Physics::BodyHandle, entt::entity> m_Owners;
        std::vector<Physics::OverlapEvent>                    m_StepEvents; // filled by the world during Step
        std::vector<std::pair<EntityOverlapSubscription, EntityOverlapCallback>> m_Subscribers;
        EntityOverlapSubscription                                                m_NextSubscription = 1u;
    };
} // namespace Desert::ECS
