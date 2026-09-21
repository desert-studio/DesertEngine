#pragma once

/**
 * THE RETARGET POSE — T6.2, AND IT NEEDED NO NEW ARITHMETIC.
 *
 * `06_gap_analysis.md` §4.4 describes UE's retarget pose in one line: *"a sparse `map<name, quat>` of
 * local deltas plus one pelvis translation"*. This is that, and nothing more, because
 * `08_retarget_measurement.md` §1 measured the reason it can be nothing more:
 *
 *     "`Initialize(skeleton1, neutralPose1, skeleton2, neutralPose2)` takes ARBITRARY rest poses — so
 *      `neutralPose1/2` ARE `SourceInitial`/`TargetInitial`, and retarget poses are expressible without a
 *      single new line. A target differing from the source ONLY in rest orientation (the elbow's local
 *      rest rotated 45 degrees) retargets with a worst limb-length error of 2.98e-05 % — the rest-pose
 *      difference is absorbed EXACTLY. An A-pose against a T-pose is free."
 *
 * So this class does not participate in the retarget equation at all. It produces the `LocalPose` that
 * the equation calls `SourceInitial` or `TargetInitial`, and the equation absorbs whatever it says.
 *
 * ── WHY IT REFUSES AN UNKNOWN BONE NAME ─────────────────────────────────────────────────────────────
 *
 * A retarget pose is the one piece of a retargeter an artist edits by hand, and the edit is a bone NAME.
 * A typo that silently changes nothing produces a retargeter that runs, reports success, and is wrong by
 * exactly the correction the artist thought they had made — the "empty successful answer" the contract
 * forbids, in the place most likely to be reached. `Apply` names the bone it could not find.
 */

#include <Engine/Animation/Pose.hpp>

#include <Common/Core/ResultStr.hpp>

#include <glm/glm.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/quaternion.hpp>

#include <cstdint>
#include <map>
#include <string>

namespace Desert::Animation
{
    class Skeleton;
}

namespace Desert::Animation::Retarget
{
    class RetargetPose
    {
    public:
        /**
         * @brief "Rotate this bone by this much, in its own space, relative to the rig's bind pose."
         *
         * POST-MULTIPLIED, not assigned. An authored delta means "turn the arm down from where the rig
         * has it", which is a rotation in the bone's own local frame; assigning an absolute local
         * rotation instead would make every entry depend on the bind pose it was authored against and
         * break the moment a mesh is re-exported.
         */
        void SetBoneRotationOffset( std::string bone, const glm::quat& localDelta );

        /// The one translation in the whole structure (§4.4). Moves the pelvis in ITS OWN local frame,
        /// which is what raising a rig's hips means; everything below it follows through the chain.
        void SetPelvisTranslationOffset( const glm::vec3& localOffset );

        [[nodiscard]] const std::map<std::string, glm::quat>& GetBoneRotationOffsets() const
        {
            return m_BoneRotationOffsets;
        }

        [[nodiscard]] const glm::vec3& GetPelvisTranslationOffset() const
        {
            return m_PelvisTranslationOffset;
        }

        /**
         * @brief The rig's bind pose with every authored offset applied. This is `SourceInitial` /
         *        `TargetInitial`.
         *
         * Refuses: a bind pose that cannot be built (see `LocalPose::FromBindPose`), an offset naming a
         * bone this skeleton does not have, and a pelvis index out of range.
         */
        [[nodiscard]] Common::ResultStr<LocalPose> Apply( const Skeleton& skeleton, uint32_t pelvisBone ) const;

    private:
        std::map<std::string, glm::quat> m_BoneRotationOffsets;
        glm::vec3                        m_PelvisTranslationOffset = glm::vec3( 0.0F );
    };
} // namespace Desert::Animation::Retarget
