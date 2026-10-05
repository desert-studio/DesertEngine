#pragma once

/**
 * THE SKELETAL CONTROL NODES OF THE POSE GRAPH (UE: FAnimNode_SkeletalControlBase's TwoBoneIK and LookAt).
 *
 * A node takes its input pose and hands it on with a few bones overridden in component space, blended by
 * Alpha in local space. Both halves are the bone-control stage's own: the chain solve is
 * `SolveTwoBoneIKChain` (TwoBoneIKControl), the write-back and the blend `ApplyBoneOverrides`
 * (BoneControl) — the graph node and the stage cannot come to disagree about what "reach the goal" means.
 */

#include <Engine/Animation/BoneControl.hpp>
#include <Engine/Animation/Graph/AnimGraph.hpp>
#include <Engine/Animation/Pose.hpp>

#include <Common/Core/ResultStr.hpp>

#include <glm/glm.hpp>

#include <vector>

namespace Desert::Animation
{
    class Skeleton;
}

namespace Desert::Animation::Graph
{
    /// Reused between frames so a node's solve allocates nothing after the first.
    struct BoneControlScratch
    {
        std::vector<BoneOverride>  Overrides;
        std::vector<BoneTransform> Before;
    };

    /// `target` in component space, read against `component` (the input pose): its position as is when it
    /// names no bone, else carried by that bone. Refuses a bone the skeleton lacks.
    [[nodiscard]] Common::ResultStr<glm::vec3>
    ComponentSpaceTarget( const BoneControlTarget& target, const Skeleton& skeleton, ComponentPose& component );

    /// UE FAnimNode_TwoBoneIK on `pose` (in place) at `alpha` (clamped; 0 leaves the pose bit-identical).
    [[nodiscard]] Common::BoolResultStr ApplyTwoBoneIKNode( const TwoBoneIKNode& node, const Skeleton& skeleton,
                                                            float alpha, LocalPose& pose,
                                                            BoneControlScratch& scratch );

    /// UE FAnimNode_LookAt on `pose` (in place): the bone turns so its local `AimAxis` points at the target.
    [[nodiscard]] Common::BoolResultStr ApplyLookAtNode( const LookAtNode& node, const Skeleton& skeleton,
                                                         float alpha, LocalPose& pose,
                                                         BoneControlScratch& scratch );
} // namespace Desert::Animation::Graph
