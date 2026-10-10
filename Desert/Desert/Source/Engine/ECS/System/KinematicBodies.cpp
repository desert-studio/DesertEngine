#include <Engine/ECS/System/KinematicBodies.hpp>

#include <Engine/ECS/Components.hpp>

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
} // namespace Desert::ECS
