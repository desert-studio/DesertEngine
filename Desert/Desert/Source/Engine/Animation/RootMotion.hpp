#pragma once

#include "Pose.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>

// ROOT MOTION, UE's model (UAnimSequence::ExtractRootMotion, FRootMotionMovementParams,
// USkeletalMeshComponent::ConvertLocalRootMotionToWorld) on this engine's types.
//
// THE DELTA. Per clock step the root bone's track is sampled at the step's two ends and the movement between
// them becomes the delta; a looping step that wrapped is two pieces (to the end, then from the start), so a
// clip whose root walks forward and snaps back at the loop keeps walking forward. The root bone in the POSE
// is locked to its reference (UE ERootMotionRootLock::RefPose), so the mesh stays on its component and the
// component moves instead.
//
// THE SPACE. The delta is in COMPONENT space (the skinned mesh's own space — the root bone's parent space,
// since a root's global matrix is its local one, Pose.cpp): `Lock * (Root_A^-1 * Root_B) * Lock^-1`, the
// animated root's own movement re-expressed around where the locked root stands. A root whose reference is
// turned (a Z-up export, a character authored facing another axis) therefore moves its component along the
// direction it is actually walking. Rotation is about the component origin, as UE applies it.
namespace Desert::Animation
{
    /**
     * @brief UE ERootMotionMode, per animated component. Serialized by NAME (reflect-cpp); append only.
     *
     * UE's fourth mode, RootMotionFromMontagesOnly, is NOT here: the engine has no montages, and a mode that
     * can only ever behave as IgnoreRootMotion would be a choice that changes nothing. It arrives with them.
     */
    enum class RootMotionMode
    {
        NoExtraction             = 0, ///< the root plays as authored; nothing is extracted (UE's default)
        IgnoreRootMotion         = 1, ///< the root is locked in the pose and its movement is discarded
        RootMotionFromEverything = 2, ///< locked and extracted from every clip that plays, weighted by blend
    };

    [[nodiscard]] constexpr bool LocksRoot( const RootMotionMode mode )
    {
        return mode != RootMotionMode::NoExtraction;
    }

    /// The identity delta: a frame with no root movement.
    [[nodiscard]] inline BoneTransform NoRootMotion()
    {
        return BoneTransform{};
    }

    /**
     * @brief The component-space movement of a root that went from @p from to @p to while the pose shows it
     *        at @p lock: `lock * (from^-1 * to) * lock^-1`, rotation about the component origin.
     *        @p translationScale scales the movement (a retargeted clip: target / source pelvis height).
     *        Scale is not root motion (UE's character movement ignores it too); the result's is 1.
     */
    [[nodiscard]] inline BoneTransform RootMotionBetween( const BoneTransform& from, const BoneTransform& to,
                                                          const BoneTransform& lock, const float translationScale )
    {
        const glm::quat fromInverse = glm::inverse( from.Rotation );
        const glm::quat relative    = fromInverse * to.Rotation;
        const glm::vec3 moved       = fromInverse * ( to.Translation - from.Translation );

        BoneTransform delta;
        delta.Rotation    = glm::normalize( lock.Rotation * relative * glm::inverse( lock.Rotation ) );
        delta.Translation = ( lock.Rotation * moved ) * translationScale;
        return delta;
    }

    /// @p first then @p next, both in the component's frame at their own start (UE FTransform accumulation:
    /// the second delta is taken in the frame the first one left the component in).
    [[nodiscard]] inline BoneTransform AccumulateRootMotion( const BoneTransform& first,
                                                             const BoneTransform& next )
    {
        BoneTransform sum;
        sum.Rotation    = glm::normalize( first.Rotation * next.Rotation );
        sum.Translation = first.Translation + first.Rotation * next.Translation;
        return sum;
    }
} // namespace Desert::Animation
