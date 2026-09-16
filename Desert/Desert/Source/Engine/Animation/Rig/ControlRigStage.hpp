#pragma once

/**
 * THE RIG AS A STAGE OF THE POSE PIPELINE — T5.4.
 *
 * Report 05 §650-658 answers "where does a Control Rig sit" without ambiguity: it does NOT sit inside the
 * character's AnimGraph. Sequencer swaps the anim instance for a `UControlRigLayerInstance` whose graph is
 * `[CR node N] -> ... -> [CR node 0] -> FAnimNode_ControlRigInputPose`, and the input-pose node is linked
 * at run time to the character's original graph. §658 states the seam in one sentence:
 *
 *     "the animation source is an input pose; the rig is a pose -> pose operator; the tool drives the
 *      operator's inputs."
 *
 * That is this file. The operator has exactly the shape `FAnimNode_ControlRigBase::ExecuteControlRig`
 * has (report 05 §640-646):
 *
 *     UpdateInput(rig, pose);      // pose -> rig hierarchy      -> ControlHierarchy::Evaluate
 *     rig->Evaluate_AnyThread();   // the solve                  -> T5.5, and NOT here
 *     UpdateOutput(rig, pose);     // rig hierarchy -> pose      -> ApplyBoneOverrides
 *
 * ── WHY THE CONTROL -> BONE MAP LIVES HERE AND NOT ON `ControlElement` ───────────────────────────────
 *
 * It is tempting to hang a `DrivenBone` field on the control, next to its parents. Report 01 §798 says not
 * to, in the course of explaining something else: the hierarchy carries the previous frame's pose into the
 * next solve, and "the anim node compensates by pushing the incoming pose in every frame before executing
 * (`UpdateInput`) and pulling it out after (`UpdateOutput`) — **but that is the *node's* policy, not the
 * rig's**".
 *
 * Both hops are the STAGE's policy. A `ControlHierarchy` is the same object whether it is being drawn by a
 * manipulator, keyed by a Sequencer, or run as a pipeline stage, and only the third of those has an opinion
 * about bones being written. So the map is a member of this class, `ControlHierarchy` is untouched by T5.4,
 * and the one place that decides what reaches the skeleton is the one place that can be reordered.
 *
 * ── THE OUTPUT HOP IS `ApplyBoneOverrides`, WHICH ALREADY EXISTED ────────────────────────────────────
 *
 * "This bone's component transform becomes that" is precisely `BoneOverride` (T2.1), and the arithmetic
 * that turns a sparse list of component-space transforms into a local pose — convert against the SOLVED
 * parent, refuse an unordered list, refuse a basis that will not decompose, roll back a mid-chain failure —
 * is `ApplyBoneOverrides` and is already proven by `Tests/Engine/BoneControlContract`. Writing a second
 * copy of it for controls would be the "two ends of a chain look right, the link between them loses
 * something" defect this project keeps closing. The alpha argument is passed 1.0F and is not a knob: the
 * rig is the authored override and there is nothing today that would set a weight, so there is no dial
 * here for a dial's sake.
 *
 * ── A RIG WITH NO DRIVES IS REFUSED, NOT ATTACHED ────────────────────────────────────────────────────
 *
 * `Animator::GetStages()` means "what will run". A stage that provably cannot change the pose would make
 * that sentence false, and — worse — it is exactly the rig a test cannot tell from a working one: a stage
 * that silently does nothing passes every "the pipeline still produces the old pose" assertion. So
 * `SetDrives` refuses an empty list and `Animator::AttachRig` refuses a rig that has none, which is what
 * lets the suite's positive control be "the pose came out CHANGED, by this much" rather than "it ran".
 */

#include <Engine/Animation/BoneControl.hpp>
#include <Engine/Animation/Pose.hpp>
#include <Engine/Animation/Rig/ControlHierarchy.hpp>

#include <Common/Core/ResultStr.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace Desert::Animation
{
    class Skeleton;

    /**
     * @brief One output hop: this control's component-space transform BECOMES this bone's.
     *
     * The whole of the rig's forwards solve, for now. T5.5's graph is what will let a control reach a bone
     * through arithmetic instead of through identity, and when it does it replaces the body of the loop in
     * `Evaluate` — not the seam, which is the point of doing T5.4 first.
     */
    struct ControlBoneDrive
    {
        uint32_t Control = ControlHierarchy::INVALID;
        uint32_t Bone    = BoneOverride::NO_BONE;
    };

    /**
     * @brief A control rig, as the pipeline sees it: a pose in, the same pose with driven bones replaced.
     *
     * Owns its `ControlHierarchy` because the stage is the hierarchy's lifetime: an `Animator` holding a
     * pointer to a rig somebody else owns is the ownership argument this project refuses to make on the
     * other three T5 classes, and there is no reason to make it here.
     */
    class ControlRigStage
    {
    public:
        [[nodiscard]] ControlHierarchy& GetHierarchy()
        {
            return m_Hierarchy;
        }

        [[nodiscard]] const ControlHierarchy& GetHierarchy() const
        {
            return m_Hierarchy;
        }

        /**
         * @brief Declare which control drives which bone. SORTED HERE, ONCE, AGAINST THIS SKELETON.
         *
         * `ApplyBoneOverrides` refuses a list that is not in resolve order, and it is right to: a child's
         * component-space override is converted against its parent's ALREADY-SOLVED transform. Sorting at
         * authoring time rather than per frame means the refusal can never be reached by a rig that was
         * written down in a sensible order and happens to disagree with the skeleton's — which would be a
         * rig failing for a reason its author cannot see.
         *
         * Refuses: an empty list (see the file note); an unknown control; a bone this skeleton does not
         * have; and two drives on the SAME bone, because "which control wins" has no answer that is not
         * invented here.
         */
        [[nodiscard]] Common::BoolResultStr SetDrives( const Skeleton&              skeleton,
                                                       std::vector<ControlBoneDrive> drives );

        [[nodiscard]] const std::vector<ControlBoneDrive>& GetDrives() const
        {
            return m_Drives;
        }

        /**
         * @brief The operator. `pose` in, `pose` out, with every driven bone replaced.
         *
         * `component` must be a view over `pose` — the Animator's is. It is invalidated on entry rather
         * than by the caller: the input hop READS bone transforms, and a rig that read the cache a previous
         * stage left behind would be posing against last frame's skeleton. Keeping that inside the operator
         * is what makes the operator correct when the editor panel calls it too.
         */
        [[nodiscard]] Common::BoolResultStr Evaluate( const Skeleton& skeleton, LocalPose& pose,
                                                      ComponentPose& component );

        /// The last refusal, or empty. Kept for the reason `BoneControl::GetLastError` is: a per-frame
        /// failure that exists only in a log line is a failure an editor cannot show.
        [[nodiscard]] const std::string& GetLastError() const
        {
            return m_LastError;
        }

    private:
        /// Logs `m_LastError` only when it DIFFERS from the last logged one. A rig refuses every frame for
        /// as long as the condition stands; sixty identical lines a second is how a real diagnostic becomes
        /// something people filter out. Same shape, and the same reason, as `BoneControl`'s.
        void ReportErrorOnChange();

        ControlHierarchy              m_Hierarchy;
        std::vector<ControlBoneDrive> m_Drives;

        /// Reused between frames so an evaluation allocates nothing after the first, as `BoneControl` does.
        std::vector<BoneOverride>  m_Overrides;
        std::vector<BoneTransform> m_Scratch;

        std::string m_LastError;
        std::string m_ReportedError;
    };
} // namespace Desert::Animation
