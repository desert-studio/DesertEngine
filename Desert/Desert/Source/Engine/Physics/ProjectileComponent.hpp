#pragma once

#include <entt/entt.hpp>

#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/UUID.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

#include <CoreReflection/ReflectionMacros.hpp>

namespace Desert::ECS
{
    // A FLYING PROJECTILE (bullet / grenade / arrow) — UE UProjectileMovementComponent
    // (ProjectileMovementComponent.cpp). ECS/System/ProjectileMovement.cpp moves it in Play, after the physics
    // step: velocity Verlet with the world's gravity times Gravity Scale, swept as a sphere through the physics
    // world in sub-steps of at most kMaxSimulationTimeStep; a blocking hit either bounces it (Should Bounce) or
    // stops it (UE StopSimulating). Every blocking hit is a Hit fact of the frame's PhysicsEventQueue for BOTH
    // sides (UE DispatchBlockingHit -> ReceiveHit), so the scripts of the projectile and of what it struck get
    // OnHit(other, point, normal, impulse). Mechanism here; what a hit MEANS (damage, despawn) is the script's.
    // Speeds are cm/s, lengths cm.
    struct ProjectileData
    {
        REFLECT()

        // UE Velocity: the world-space velocity it is launched with (and, in Play, the current one).
        PROPERTY( DisplayName( "Velocity" ), Category( "Projectile" ), Units( "cm/s" ), Summary )
        glm::vec3 Velocity = { 0.0f, 0.0f, 0.0f };

        // UE MaxSpeed: the speed the velocity is clamped to; 0 = no limit.
        PROPERTY( DisplayName( "Max Speed" ), Category( "Projectile" ), Range( 0.0f, 100000.0f ), Units( "cm/s" ) )
        float MaxSpeed = 0.0f;

        // UE ProjectileGravityScale: times the physics world's gravity (0 = a straight line).
        PROPERTY( DisplayName( "Gravity Scale" ), Category( "Projectile" ), Range( 0.0f, 10.0f ), Summary )
        float GravityScale = 1.0f;

        // The radius of the sphere swept through the world (UE: the UpdatedComponent's collision sphere).
        PROPERTY( DisplayName( "Collision Radius" ), Category( "Projectile" ), Range( 0.1f, 500.0f ), Length )
        float CollisionRadius = 2.0f;

        // UE bRotationFollowsVelocity: the entity faces along its velocity (its -Z, the engine's forward).
        PROPERTY( DisplayName( "Rotation Follows Velocity" ), Category( "Projectile" ) )
        bool RotationFollowsVelocity = false;

        // UE InitialLifeSpan: seconds after Play / spawn before the entity is destroyed; 0 = it lives on.
        PROPERTY( DisplayName( "Life Span" ), Category( "Projectile" ), Range( 0.0f, 600.0f ), Units( "s" ) )
        float LifeSpan = 5.0f;

        // UE bShouldBounce: a blocking hit reflects the velocity instead of stopping the projectile.
        PROPERTY( DisplayName( "Should Bounce" ), Category( "Projectile Bounces" ) )
        bool ShouldBounce = false;

        // UE Bounciness: the fraction of the velocity along the hit normal kept after a bounce.
        PROPERTY( DisplayName( "Bounciness" ), Category( "Projectile Bounces" ), Range( 0.0f, 1.0f ) )
        float Bounciness = 0.6f;

        // UE Friction: the fraction of the velocity along the surface lost in a bounce.
        PROPERTY( DisplayName( "Friction" ), Category( "Projectile Bounces" ), Range( 0.0f, 1.0f ) )
        float Friction = 0.2f;

        // UE BounceVelocityStopSimulatingThreshold: below this speed after a bounce it stops.
        PROPERTY( DisplayName( "Bounce Stop Speed" ), Category( "Projectile Bounces" ), Range( 0.0f, 1000.0f ),
                  Units( "cm/s" ), Advanced )
        float BounceVelocityStopSimulatingThreshold = 5.0f;
    };

    struct ProjectileComponent
    {
        COMPONENT( Key( "Projectile" ), Block( Data ), Run( ActorsAndUI ) )
        ProjectileData Data;

        // Transient (Play only). UE GetInstigator / MoveIgnoreActors: the shooter, whose bodies the sweep goes
        // through; stamped by the firing script (entity:launch). Null = nothing ignored.
        entt::entity Instigator = entt::null;
        float        Age        = 0.0f;  // seconds since Play / spawn, against Life Span
        bool         Stopped    = false; // UE HasStoppedSimulation: a blocking hit ended the flight
    };
} // namespace Desert::ECS
