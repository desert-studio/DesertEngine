#pragma once

/**
 * MODEL SPACE AS TRS, BECAUSE THE MATRIX ROUND TRIP IS A FAILURE PATH — T6.2.
 *
 * `ComponentPose` already resolves a `LocalPose` through the parent chain, and it is the right type for
 * its three callers: they want ONE bone as a `glm::mat4` and hand it to a socket, a gizmo or the GPU.
 * A retargeter is the fourth kind of caller and it wants something none of them do — to resolve a pose,
 * REWRITE it, and convert the result back to a `LocalPose`. That last hop is what makes the matrix the
 * wrong currency here, and `08_retarget_measurement.md` §3.1 measured the price:
 *
 *     "`FromMatrix` is a FAILURE PATH: it rejects a non-positive determinant rather than straightening a
 *      mirror. Mirrored bones occur constantly in character rigs. Embedding the mapper as a stage gives
 *      us a pipeline that CAN REFUSE EVERY FRAME where today there is nothing to refuse."
 *
 * That is not a hypothetical. `BoneTransform::FromMatrix` refuses `determinant <= 0` by design, and a
 * left/right-mirrored bone has exactly that. A retargeter built on `ComponentPose` would therefore refuse
 * every frame on a perfectly ordinary rig — and the cost of avoiding it is this file, which never builds
 * a matrix at all.
 *
 * ── THE COMPOSITION IS UE's `FTransform`, NOT A MATRIX PRODUCT ───────────────────────────────────────
 *
 * Composing two TRS triples directly (rotations as quaternions, scales component-wise, translation
 * through the parent's rotation and scale) is exact for uniform scale and is the nearest TRS for a
 * non-uniform one — the same contract `BoneTransform::FromMatrix` documents for shear, reached without
 * the decomposition. `Relative()` is its exact inverse, so `ToLocal(FromLocal(p)) == p` to float noise,
 * with NO determinant test anywhere on the path.
 *
 * ── AND IT MUST AGREE WITH `ComponentPose`, WHICH IS A TEST, NOT A COMMENT ───────────────────────────
 *
 * Two resolvers for the same question is how two answers come to exist (this tree has closed that defect
 * repeatedly). The relation is asserted rather than asserted-in-prose: `Tests/Engine/RetargetPipeline`'s
 * `ModelPoseAgreesWithComponentPose` runs both over the corpus rig and pins the disagreement.
 */

#include <Engine/Animation/Pose.hpp>

#include <Common/Core/ResultStr.hpp>

#include <cstdint>
#include <vector>

namespace Desert::Animation
{
    class Skeleton;
}

namespace Desert::Animation::Retarget
{
    /// `parent * child` as TRS. Rotation composes, scale multiplies component-wise, and the child's
    /// translation is taken through the parent's scale and then its rotation — the order a matrix product
    /// would apply them in.
    [[nodiscard]] BoneTransform Compose( const BoneTransform& parent, const BoneTransform& child );

    /**
     * @brief The exact inverse of `Compose`: the local transform `child` must have under `parent`.
     *
     * REFUSES ON A ZERO SCALE COMPONENT rather than dividing. A parent bone scaled to nothing on one axis
     * genuinely has no local frame to express a child in, and a silent 1.0 there is the "empty successful
     * answer" the contract forbids — the caller cannot tell it from a bone that was really unscaled.
     */
    [[nodiscard]] Common::ResultStr<BoneTransform> Relative( const BoneTransform& parent,
                                                             const BoneTransform& child );

    /**
     * @brief A whole pose in model (mesh-local) space, as TRS per bone, index-for-index with the skeleton.
     *
     * Eager, unlike `ComponentPose`, and deliberately: every consumer here wants all of it. The FK stage
     * reads every source bone, and the chain stage reads runs of them.
     */
    class ModelPose
    {
    public:
        ModelPose() = default;

        /// Resolve `local` through `skeleton`'s parent chain, in its cached parent-before-child order.
        /// Refuses when the pose and the skeleton disagree about how many bones there are — the one
        /// mistake that would otherwise read every bone of a rig against the wrong bone of a pose.
        [[nodiscard]] static Common::ResultStr<ModelPose> FromLocal( const Skeleton& skeleton,
                                                                     const LocalPose& local );

        /// Re-resolve only the tail of the resolve order, from `startRank` onwards. Every descendant of a
        /// bone has a higher resolve rank than it does, so re-running the suffix is exactly "fix this bone
        /// and everything under it" without a child list the `Skeleton` does not keep.
        [[nodiscard]] Common::BoolResultStr PropagateFrom( const Skeleton& skeleton, const LocalPose& local,
                                                           uint32_t startRank );

        /// The parent-relative pose that produces this one. Refuses for the same reason `Relative` does,
        /// naming the bone.
        [[nodiscard]] Common::ResultStr<LocalPose> ToLocal( const Skeleton& skeleton ) const;

        [[nodiscard]] size_t Size() const
        {
            return m_Bones.size();
        }

        [[nodiscard]] const BoneTransform& operator[]( size_t bone ) const
        {
            return m_Bones[bone];
        }
        [[nodiscard]] BoneTransform& operator[]( size_t bone )
        {
            return m_Bones[bone];
        }

    private:
        std::vector<BoneTransform> m_Bones;
    };
} // namespace Desert::Animation::Retarget
