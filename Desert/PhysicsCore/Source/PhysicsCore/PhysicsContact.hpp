#pragma once

// What a step reports and a query returns, by value: contact impulses and events, ray hits. Their own
// header so a consumer of the events (the ECS relay, destruction, gameplay) names them without the world
// API (PhysicsWorld.hpp).

#include <PhysicsCore/PhysicsTypes.hpp>

#include <glm/vec3.hpp>

#include <cstdint>

namespace Desert::Physics
{
    struct ContactImpulse
    {
        BodyHandle Body1   = kInvalidBody;
        BodyHandle Body2   = kInvalidBody;
        uint32_t   Part1   = 0u; ///< Index into Body1's CompoundBodyDesc::Parts; 0 for any other shape.
        uint32_t   Part2   = 0u;
        glm::vec3  Point   = { 0.0f, 0.0f, 0.0f }; ///< World, on Body1's surface
        glm::vec3  Normal  = { 0.0f, 1.0f, 0.0f }; ///< World, from Body1 towards Body2
        float      Impulse = 0.0f;
    };

    /// What a ContactEvent reports (UE OnComponentHit / OnComponentBeginOverlap / OnComponentEndOverlap).
    enum class ContactEventKind : uint8_t
    {
        Hit,          ///< Two bodies began a blocking (solved) contact
        BeginOverlap, ///< Two bodies began to overlap (a sensor contact: an Overlap pair, found and not solved)
        EndOverlap,   ///< The pair stopped overlapping, or one of its bodies left the world
    };

    /**
     * @brief One body-pair event of a fixed step, named by body (ChaosEventRelay / FCollisionNotifyInfo's
     * pattern: collected on the solver's threads, handed to the game thread after the step).
     *
     * A Hit is reported when either body's profile GeneratesHitEvents, once per contact begun (a touch kept
     * across steps is one Hit); Notify1 / Notify2 say whose profile asked. Begin / EndOverlap are reported when
     * both profiles GeneratesOverlapEvents, once per body pair however many shape pairs touch, and every Begin
     * is followed by exactly one End — also when a body is removed while overlapping.
     */
    struct ContactEvent
    {
        ContactEventKind Kind    = ContactEventKind::Hit;
        BodyHandle       Body1   = kInvalidBody;
        BodyHandle       Body2   = kInvalidBody;
        bool             Notify1 = false; ///< Body1's profile asked for this kind of event
        bool             Notify2 = false;
        glm::vec3        Point   = { 0.0f, 0.0f, 0.0f }; ///< World, on Body1's surface; zero for EndOverlap
        glm::vec3        Normal  = { 0.0f, 0.0f, 0.0f }; ///< World, from Body1 towards Body2; zero for EndOverlap
        float            Impulse = 0.0f;                 ///< Hit only: estimated normal impulse, kg·cm/s
    };

    struct RayHit
    {
        BodyHandle Body     = kInvalidBody;
        float      Distance = 0.0f; ///< Along the ray, in units of |direction|.
        glm::vec3  Point    = { 0.0f, 0.0f, 0.0f };
        glm::vec3  Normal   = { 0.0f, 1.0f, 0.0f };
    };
} // namespace Desert::Physics
