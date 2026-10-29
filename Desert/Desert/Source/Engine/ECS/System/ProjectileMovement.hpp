#pragma once

// THE PROJECTILE'S MOVEMENT — UE UProjectileMovementComponent (Runtime/Engine/Private/Components/
// ProjectileMovementComponent.cpp): TickComponent's sub-stepped sweep loop, ComputeVelocity / ComputeMoveDelta
// (velocity Verlet), HandleImpact / ComputeBounceResult (bounce or StopSimulating) and HandleDeflection's slide.
// Run by PhysicsECSSystem once per frame after the physics step, against the world that step produced (UE ticks
// the component in the frame, outside the physics scene). Every speed is cm/s, every length cm.

#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/PhysicsEvents.hpp>
#include <PhysicsCore/PhysicsWorld.hpp>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <unordered_map>
#include <vector>

namespace Desert::ECS::ProjectileMovement
{
    /// UE MaxSimulationTimeStep: the longest sub-step of a curved (gravity-bent) flight, seconds.
    inline constexpr float kMaxSimulationTimeStep = 0.05f;
    /// UE MaxSimulationIterations: the most sub-steps one frame takes.
    inline constexpr int kMaxSimulationIterations = 4;
    /// UE BounceAdditionalIterations: impacts that earn the frame one more iteration.
    inline constexpr int kBounceAdditionalIterations = 1;
    /// UE MIN_TICK_TIME: less remaining time than this is not simulated.
    inline constexpr float kMinTickTime = 1e-6f;

    /// UE LimitVelocity: @p velocity clamped to Max Speed (0 = no limit).
    [[nodiscard]] glm::vec3 LimitVelocity( const ProjectileData& data, const glm::vec3& velocity );

    /// UE ComputeVelocity: v + a*dt, limited; a = the world's downward gravity @p gravity (cm/s^2) times Gravity
    /// Scale.
    [[nodiscard]] glm::vec3 ComputeVelocity( const ProjectileData& data, const glm::vec3& velocity, float gravity,
                                             float dt );

    /// UE ComputeMoveDelta: the velocity Verlet displacement over @p dt, v0*t + (v1 - v0)*t/2.
    [[nodiscard]] glm::vec3 ComputeMoveDelta( const ProjectileData& data, const glm::vec3& velocity, float gravity,
                                              float dt );

    /// UE ComputeBounceResult: @p velocity after striking a surface of @p normal — the normal part reflected and
    /// scaled by Bounciness, the tangential part reduced by Friction, the result limited.
    [[nodiscard]] glm::vec3 ComputeBounceResult( const ProjectileData& data, const glm::vec3& velocity,
                                                 const glm::vec3& normal );

    /// What one frame of the scene's projectiles produced.
    struct FrameResult
    {
        /// One Hit fact per side of every blocking hit (UE DispatchBlockingHit: both actors' ReceiveHit), named
        /// by entity; appended to the frame's PhysicsEventQueue by the caller.
        std::vector<PhysicsEvent> Hits;
        /// Projectiles whose Life Span ran out this frame (UE InitialLifeSpan): the caller destroys them.
        std::vector<entt::entity> Expired;
    };

    /**
     * @brief Advances every ProjectileComponent of @p registry by @p dt: ages it, and unless it has stopped moves
     * its TransformComponent through @p world by sub-stepped sphere sweeps of Collision Radius, ignoring its own
     * body and its Instigator's. @p bodies names a struck body's entity (PhysicsECSSystem's body map); a body it
     * lacks (a landscape tile) is struck as Other = null.
     */
    [[nodiscard]] FrameResult Step( entt::registry& registry, const Physics::PhysicsWorld& world,
                                    const std::unordered_map<Physics::BodyHandle, entt::entity>& bodies,
                                    float                                                        dt );
} // namespace Desert::ECS::ProjectileMovement
