#pragma once

/**
 * THE FIRST SOLVER, AND THE THING IT IS ACTUALLY FOR IS PROVING THE SUBSTRATE UNDER IT.
 *
 * Everything anatomical about two-bone IK lives in `Solvers/TwoBoneIK.hpp`, which knows nothing about
 * skeletons, poses or bones. What is left here is the WIRING, and the wiring is the part that generalises:
 * find the chain, read three component-space positions out of the pose, hand them to the maths, and turn
 * the three points that come back into three bone overrides. The next solver (an N-bone limb, a look-at)
 * replaces the middle line and reuses the rest.
 */

#include <Engine/Animation/BoneControl.hpp>
#include <Engine/Animation/Skeleton.hpp>
#include <Engine/Animation/Solvers/TwoBoneIK.hpp>

#include <glm/glm.hpp>

#include <string>

namespace Desert::Animation
{
    class TwoBoneIKControl final : public BoneControl
    {
    public:
        [[nodiscard]] BoneControlKind GetKind() const override
        {
            return BoneControlKind::TwoBoneIK;
        }

        /**
         * @brief The ONE authored bone. The chain is the named bone, its parent and its grandparent.
         *
         * UE authors it the same way and has no "root bone" pin at all
         * (`AnimNode_TwoBoneIK.cpp:239-249`), and the reason is worth keeping: a chain authored at both
         * ends can name two bones that are not related, and nothing can be done about that except detect it
         * and refuse. Deriving the chain makes the invalid case unrepresentable — the only remaining failure
         * is "this rig does not give the named bone two ancestors", which is a sentence an artist can act on.
         *
         * Setting a name does NOT resolve it: call `Resolve` with the rig. (Report 03 §1.4 — indices are
         * cached on rig change, never looked up inside a solve.)
         */
        void                             SetEndBone( std::string name );
        [[nodiscard]] const std::string& GetEndBoneName() const
        {
            return m_EndBone.GetName();
        }

        /// Where the end bone should land, in COMPONENT (mesh-local) space, centimetres. Component space and
        /// not world: the pose the solver reads is component-space, and converting a world goal here would
        /// mean this class knowing about the entity's transform — a dependency the whole point of
        /// `Solvers/` is to keep out. The caller that has a world goal has the entity matrix; it converts.
        void SetGoal( const glm::vec3& componentSpaceGoal )
        {
            m_Goal = componentSpaceGoal;
        }
        [[nodiscard]] const glm::vec3& GetGoal() const
        {
            return m_Goal;
        }

        /// The pole target, a POINT in component space that the joint bends towards (report 03 §785). A
        /// point rather than a direction so it can later be a bone or a socket without changing this API.
        void SetPoleTarget( const glm::vec3& componentSpacePoleTarget )
        {
            m_PoleTarget = componentSpacePoleTarget;
        }
        [[nodiscard]] const glm::vec3& GetPoleTarget() const
        {
            return m_PoleTarget;
        }

        [[nodiscard]] Common::BoolResultStr Resolve( const Skeleton& skeleton ) override;

        /// The three bones the chain resolved to, or `BoneOverride::NO_BONE`. For the editor's readout and
        /// for the suite that asserts the chain is derived rather than guessed.
        [[nodiscard]] uint32_t GetRootBone() const
        {
            return m_RootBone;
        }
        [[nodiscard]] uint32_t GetJointBone() const
        {
            return m_JointBone;
        }
        [[nodiscard]] uint32_t GetEndBone() const
        {
            return m_EndBone.IsResolved() ? m_EndBone.GetIndex() : BoneOverride::NO_BONE;
        }

        /// What the last solve did. `Reach` is the load-bearing one: "the arm is reaching but never arrives"
        /// and "the arm is folded as far as it goes" look identical in a frame and are different problems.
        [[nodiscard]] Solvers::TwoBoneIKReach GetLastReach() const
        {
            return m_LastReach;
        }
        [[nodiscard]] Solvers::TwoBoneIKPlane GetLastPlane() const
        {
            return m_LastPlane;
        }

    protected:
        [[nodiscard]] Common::BoolResultStr Solve( const Skeleton& skeleton, ComponentPose& component,
                                                   std::vector<BoneOverride>& out ) override;

    private:
        BoneRef  m_EndBone;
        uint32_t m_JointBone = BoneOverride::NO_BONE;
        uint32_t m_RootBone  = BoneOverride::NO_BONE;

        /// Why the chain is unusable, or empty. Produced by `Resolve` and re-reported by every `Solve`,
        /// because an unresolved chain is a standing condition rather than a one-off event: the frame after
        /// a bad bone name is just as broken as the first one, and a caller that only hears about it once
        /// has no way to find out later.
        std::string m_ChainError = "two-bone IK has not been resolved against a rig yet.";

        glm::vec3 m_Goal       = glm::vec3( 0.0F );
        glm::vec3 m_PoleTarget = glm::vec3( 0.0F );

        Solvers::TwoBoneIKReach m_LastReach = Solvers::TwoBoneIKReach::DegenerateChain;
        Solvers::TwoBoneIKPlane m_LastPlane = Solvers::TwoBoneIKPlane::NotChosen;
    };
} // namespace Desert::Animation
