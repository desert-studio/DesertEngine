#include <Engine/ECS/System/SpringArm.hpp>

#include <Engine/ECS/Entity.hpp>

namespace Desert::ECS::SpringArm
{
    ArmSolve Solve( SpringArmComponent& arm, const glm::vec3& origin, const glm::quat& rotation, float dt,
                    const Physics::PhysicsWorld* world )
    {
        const SpringArmData& data = arm.Data;

        // UE bEnableCameraLag: VInterpTo of the arm's origin toward where it should be.
        glm::vec3 armOrigin = origin;
        if ( data.EnableCameraLag && arm.HasLaggedOrigin && data.CameraLagSpeed > 0.0f )
            armOrigin = arm.LaggedOrigin +
                        ( origin - arm.LaggedOrigin ) * glm::clamp( dt * data.CameraLagSpeed, 0.0f, 1.0f );
        arm.LaggedOrigin    = armOrigin;
        arm.HasLaggedOrigin = true;

        ArmSolve out;
        out.CameraPosition =
             armOrigin + rotation * glm::vec3( 0.0f, 0.0f, data.TargetArmLength ) + rotation * data.SocketOffset;

        if ( data.DoCollisionTest && world != nullptr && data.ProbeSize > 0.0f )
        {
            const glm::vec3 path = out.CameraPosition - origin;
            const float     len  = glm::length( path );
            if ( len > 1e-3f )
                if ( const auto hit = world->CastSphere( origin, path, data.ProbeSize, len ) )
                {
                    out.CameraPosition = origin + path / len * hit->Distance;
                    out.Blocked        = true;
                }
        }
        out.ArmLength        = glm::length( out.CameraPosition - origin );
        arm.CurrentArmLength = out.ArmLength;
        return out;
    }

    void UpdateAll( entt::registry& registry, const Physics::PhysicsWorld* world, float dt )
    {
        auto arms = registry.view<SpringArmComponent>();
        for ( const entt::entity entity : arms )
        {
            const glm::mat4 armWorld = Entity( entity, registry ).GetWorldTransform();
            const glm::vec3 origin( armWorld[3] );
            const glm::mat3 basis( glm::normalize( glm::vec3( armWorld[0] ) ),
                                   glm::normalize( glm::vec3( armWorld[1] ) ),
                                   glm::normalize( glm::vec3( armWorld[2] ) ) );
            const ArmSolve  solve =
                 Solve( arms.get<SpringArmComponent>( entity ), origin, glm::quat_cast( basis ), dt, world );

            if ( !registry.has<RelationshipComponent>( entity ) )
                continue;
            const glm::vec3 local( glm::inverse( armWorld ) * glm::vec4( solve.CameraPosition, 1.0f ) );
            for ( const entt::entity child : registry.get<RelationshipComponent>( entity ).Children )
                if ( registry.has<CameraComponent>( child ) && registry.has<TransformComponent>( child ) )
                    registry.get<TransformComponent>( child ).Translation = local;
        }
    }
} // namespace Desert::ECS::SpringArm
