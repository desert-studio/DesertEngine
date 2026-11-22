#pragma once

// One bone's TRS and its blend: what a channel, a key or a control names without the pose arrays of
// Pose.hpp (LocalPose / ComponentPose / SkinningMatrices).

#include <glm/glm.hpp>
#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif
#include <glm/gtx/quaternion.hpp>

namespace Desert::Animation
{
    /**
     * @brief One bone's transform, in the three quantities animation is actually authored and blended in.
     *
     * TRS RATHER THAN A MATRIX, and the argument is the tree's own code. `.anim` stores TRS keys
     * (Serialization::KeyPosition/KeyRotation/KeyScale); `BoneTrack::GetTransform` composes them into a
     * matrix; `Animator::SampleLocalTransform` hands that matrix on; and layer composition immediately
     * decomposes it again — twice per bone per layer, three times for an additive one — blends, and
     * recomposes. A round trip with no consumer of the matrix in the middle. Storing TRS deletes the trip
     * and makes the correct blend the only expressible one.
     */
    struct BoneTransform
    {
        glm::vec3 Translation = glm::vec3( 0.0F );
        glm::quat Rotation    = glm::quat( 1.0F, 0.0F, 0.0F, 0.0F );
        glm::vec3 Scale       = glm::vec3( 1.0F );

        [[nodiscard]] glm::mat4 ToMatrix() const;

        /**
         * @brief Matrix -> TRS, AND IT REFUSES RATHER THAN STRAIGHTENING A MIRROR.
         *
         * The decomposition this replaces took scale as the LENGTH of each basis column. A length is never
         * negative, so a mirrored bone (negative determinant) silently came back with positive scale and
         * the sign went into the rotation as a turn that does not exist. That happened on every blend, and
         * it is the "empty successful answer" the contract forbids: the caller cannot tell a pose that was
         * decomposed from one that was quietly repaired.
         *
         * Refuses on a determinant <= 0 — mirrored, or degenerate to the point where the basis cannot be
         * normalised — naming the determinant. It does NOT detect shear: a sheared basis decomposes to the
         * nearest TRS, which is what every consumer of a TRS pose means by it. When a task introduces
         * signed scale, this is the one place that has to learn about it.
         */
        [[nodiscard]] static Common::ResultStr<BoneTransform> FromMatrix( const glm::mat4& matrix );
    };

    /// Component-wise interpolation: lerp position and scale, slerp rotation. `alpha` is NOT clamped here —
    /// callers that need the endpoints bit-exact must short-circuit, because a round trip through this is
    /// not the identity on a matrix that was not exactly a TRS to begin with.
    [[nodiscard]] BoneTransform Blend( const BoneTransform& from, const BoneTransform& to, float alpha );
} // namespace Desert::Animation
