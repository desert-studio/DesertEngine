#pragma once

// FROM A PHYSICS ASSET AND A SKELETON TO WHAT JOLT'S RAGDOLL IS BUILT FROM — device-free and PhysicsSystem-free,
// so it is tested as a pure function (UE: UPhysicsAsset + the skeletal mesh's ref pose -> FBodyInstance /
// FConstraintInstance setup in USkeletalMeshComponent::InitArticulated).
//
// RagdollDesc mirrors JPH::RagdollSettings field for field, in Jolt's units, so the runtime (RAG1b) copies it
// into JPH::Skeleton (one joint per part, Parent), JPH::RagdollSettings::Part (a BodyCreationSettings with the
// shape, Position / Rotation, MassKg as mMassPropertiesOverride with EOverrideMassProperties::CalculateInertia)
// and Part::mToParent (a JPH::SwingTwistConstraintSettings in EConstraintSpace::WorldSpace) with no arithmetic
// of its own. Every unit conversion of the ragdoll path happens HERE, once:
//
//   lengths:  centimetres * kJoltUnitsPerCentimetre (see below),
//   angles:   authored degrees -> radians,
//   mass:     MassKg as authored, or the shape's volume (cm^3) * DensityGramsPerCm3 / 1000 -> kg,
//   capsule:  authored along the body's local Z (UE FKSphylElem) -> Jolt's CapsuleShape, built along Y, under
//             a +90 degree turn about X; Length (between the cap centres) -> Jolt's half-height = Length / 2.

#include <Engine/Physics/PhysicsAssetFormat.hpp>

#include <Common/Core/ResultStr.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace Desert::Animation
{
    class Skeleton;
}

namespace Desert::Physics
{
    /// ONE JOLT UNIT IS ONE CENTIMETRE. PhysicsWorld runs Jolt in the engine's world unit — its gravity arrives in
    /// cm/s^2 (PhysicsWorld::SetGravity) and every BodyDesc length is centimetres — so a ragdoll body is placed in
    /// the same unit as the floor it falls on. A metre conversion here would make a 10 cm capsule radius 0.1
    /// units in a world whose floor is 1000 units wide. The factor is stated so the conversion is one named step.
    inline constexpr float kJoltUnitsPerCentimetre = 1.0f;

    /// JPH::SwingTwistConstraintSettings with mSpace = EConstraintSpace::WorldSpace, in the bind pose: both
    /// bodies' frames coincide at the authored joint, so 1 and 2 are equal at rest.
    struct RagdollConstraintDesc
    {
        glm::vec3 Position1           = glm::vec3( 0.0f ); ///< Jolt units, component space of the rig
        glm::vec3 TwistAxis1          = glm::vec3( 1.0f, 0.0f, 0.0f );
        glm::vec3 PlaneAxis1          = glm::vec3( 0.0f, 1.0f, 0.0f );
        glm::vec3 Position2           = glm::vec3( 0.0f );
        glm::vec3 TwistAxis2          = glm::vec3( 1.0f, 0.0f, 0.0f );
        glm::vec3 PlaneAxis2          = glm::vec3( 0.0f, 1.0f, 0.0f );
        float     NormalHalfConeAngle = 0.0f; ///< radians; UE Swing1 (about the frame's +Z = twist x plane)
        float     PlaneHalfConeAngle  = 0.0f; ///< radians; UE Swing2 (about the frame's +Y = the plane axis)
        float     TwistMinAngle       = 0.0f; ///< radians; -UE TwistLimit
        float     TwistMaxAngle       = 0.0f; ///< radians; +UE TwistLimit
    };

    /// One JPH::RagdollSettings::Part and its JPH::Skeleton joint.
    struct RagdollPartDesc
    {
        std::string      Bone;             ///< the JPH::Skeleton joint name
        uint32_t         SkeletonBone;     ///< index into the engine Skeleton's bones (pose read-back in RAG1b)
        int32_t          Parent      = -1; ///< index into RagdollDesc::Parts of the parent body; -1 for a root
        PhysicsBodyShape Shape       = PhysicsBodyShape::Capsule;
        float            Radius      = 0.0f;              ///< Jolt units; Sphere and Capsule
        float            HalfHeight  = 0.0f;              ///< Jolt units; Capsule (JPH::CapsuleShape half height)
        glm::vec3        HalfExtents = glm::vec3( 0.0f ); ///< Jolt units; Box (JPH::BoxShape)
        // The shape inside the body (JPH::RotatedTranslatedShape), body space, Jolt units.
        glm::vec3 ShapeOffset   = glm::vec3( 0.0f );
        glm::quat ShapeRotation = glm::quat( 1.0f, 0.0f, 0.0f, 0.0f );
        // The body in the bind pose (BodyCreationSettings::mPosition / mRotation): the bone's component-space
        // bind transform, Jolt units.
        glm::vec3 Position = glm::vec3( 0.0f );
        glm::quat Rotation = glm::quat( 1.0f, 0.0f, 0.0f, 0.0f );
        float     MassKg   = 0.0f;
        // Part::mToParent; absent for a root and for a body the author left unjointed (UE: a body with no
        // constraint simulates free of its parent).
        bool                  HasConstraint = false;
        RagdollConstraintDesc ToParent;
    };

    /// JPH::RagdollSettings: the parts in parent-before-child order (JPH::Skeleton requires it), so every
    /// Parent is smaller than its own index.
    struct RagdollDesc
    {
        std::vector<RagdollPartDesc> Parts;
    };

    /**
     * @brief Everything wrong with @p asset against @p skeleton, one message per fault, each naming the bone or
     * the constraint it is about. Empty means the asset builds a ragdoll.
     *
     * Checked: a null skeleton GUID; no bodies; a body on a bone the skeleton does not have; two bodies on one
     * bone; a non-positive radius, length or box extent, mass below zero, or a non-positive density where the
     * density is what gives the mass; a constraint naming a bone the skeleton does not have or that carries no
     * body; a constraint whose bodies are not parent and child — the parent body of a body is the body on the
     * nearest ancestor bone that carries one (UE's physics asset tool links a body to it the same way); two
     * constraints on one child; a swing or twist limit outside [0, 180] degrees.
     */
    [[nodiscard]] std::vector<std::string> ValidatePhysicsAsset( const PhysicsAssetData&    asset,
                                                                 const Animation::Skeleton& skeleton );

    /// The ragdoll description of @p asset on @p skeleton's bind pose; refused with every ValidatePhysicsAsset
    /// message when there is any.
    [[nodiscard]] Common::ResultStr<RagdollDesc> BuildRagdollDesc( const PhysicsAssetData&    asset,
                                                                   const Animation::Skeleton& skeleton );

    /// The body's mass in kilograms: MassKg when positive, else its shape's volume times its density.
    [[nodiscard]] float PhysicsBodyMassKg( const PhysicsAssetBody& body );
} // namespace Desert::Physics
