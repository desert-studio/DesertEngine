#pragma once

// The physics value types a component or a system names without the world: body/shape enums and the
// opaque handles (PhysicsWorld.hpp owns the world API; including it costs ~every physics consumer).

#include <glm/vec3.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>

namespace Desert::Physics
{
    enum class BodyType
    {
        Static,    // never moves (ground, walls)
        Dynamic,   // simulated (falls, collides)
        Kinematic, // moved by code, pushes dynamics (platforms, the player controller later)
    };

    enum class ShapeType
    {
        Box,
        Sphere,
        Capsule,
        // UE's "complex as simple": the render triangles themselves. Jolt's MeshShape has no volume, so it
        // cannot carry mass — static and kinematic bodies only; a dynamic one is refused by name.
        Mesh,
        // UE's "simple" convex: the hull of the mesh's points, for anything that moves. Jolt's hull builder
        // caps the hull at ConvexHullShape::cMaxPointsInHull (256) and simplifies past it on its own.
        ConvexHull,
    };

    // The body-local axis a capsule's cylinder runs along. Jolt's capsule is built along Y; any other axis is
    // Jolt's capsule rotated onto it.
    enum class CapsuleAxis
    {
        X,
        Y,
        Z,
    };

    // Opaque handle wrapping a JPH::BodyID (its index+sequence uint32). kInvalidBody == not created.
    using BodyHandle = uint32_t;
    constexpr BodyHandle kInvalidBody = 0xFFFFFFFFu;

    // ---- Character controller (Jolt CharacterVirtual: a kinematic capsule that walks slopes/steps, is
    // blocked by world geometry, and reports ground contact — the basis for the playable player). ----
    using CharacterHandle                   = uint32_t;
    constexpr CharacterHandle kInvalidCharacter = 0xFFFFFFFFu;

    /// A ragdoll in the world (a JPH::Ragdoll: one body per part, a constraint per jointed part).
    using RagdollHandle                     = uint32_t;
    constexpr RagdollHandle kInvalidRagdoll = 0xFFFFFFFFu;

    /// What moves a ragdoll's bodies (UE: PhysicsBlendWeight 0 with kinematic bodies / SetSimulatePhysics).
    enum class RagdollMotion
    {
        Kinematic, ///< every part follows the target pose (MoveKinematic per fixed step); pushes dynamic bodies
        Simulated, ///< every part is dynamic: gravity, contacts and the constraints move it
    };

    /// One part's body in the world: its origin (the bone, not the centre of mass) and rotation, cm.
    struct RagdollPartTransform
    {
        glm::vec3 Position = { 0.0f, 0.0f, 0.0f };
        glm::quat Rotation = glm::quat( 1.0f, 0.0f, 0.0f, 0.0f );
    };
} // namespace Desert::Physics
