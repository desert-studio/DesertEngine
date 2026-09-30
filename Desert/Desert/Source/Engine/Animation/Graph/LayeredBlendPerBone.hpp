#pragma once

/**
 * LAYERED BLEND PER BONE — THE ANIMGRAPH NODE LAYERS LIVE IN (UE: FAnimNode_LayeredBoneBlend).
 *
 * Layers are NOT a timeline concept (Timeline/Binding.hpp says why) and they are not the Animator's
 * `AnimationLayer` list either: that list (Animator.hpp, clip + weight + additive + a per-bone byte mask)
 * is this node with its inputs hard-wired to clips, its mask stuck at 0/1, no depth ramp, no mesh-space
 * rotation, no curve rule and no root-motion rule. It becomes this node when the AnimGraph gains a pose
 * graph (today the AnimGraph is a state machine only; the pose-node DAG is the prerequisite piece).
 *
 * ── WHAT THE NODE DOES ────────────────────────────────────────────────────────────────────────────────
 *
 * Inputs: a BASE pose and N BLEND poses, each blend pose with a weight and a layer setup of branch
 * filters. Per bone b, the layer whose filter reaches b gives a mask weight m(b) ∈ [0, 1]; the output is
 *     out(b) = blend( base(b), layer(b), m(b) * layerWeight )
 * in LOCAL space, except where MeshSpaceRotationBlend / MeshSpaceScaleBlend say mesh space — then the
 * layer's rotation is blended as a mesh-space rotation and converted back, so an upper-body aim layer
 * keeps the head pointing where the layer says even when the base pose bends the pelvis.
 *
 * BRANCH FILTER DEPTH (UE's FBranchFilter::BlendDepth, our semantics stated):
 *     0     the filter bone and every descendant at full weight
 *     N > 0 a ramp: a bone d levels below the filter bone gets min( 1, ( d + 1 ) / N )
 *     N < 0 EXCLUDE: the filter bone and its descendants get 0 from this layer, overriding a wider filter
 * Two layers reaching one bone: the LATER layer wins that bone (UE's per-bone SourceIndex).
 */

#include <Engine/Animation/Pose.hpp>

#include <Common/Core/ResultStr.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace Desert::Animation
{
    class Skeleton;
}

namespace Desert::Animation::Graph
{
    struct BranchFilter
    {
        std::string BoneName;
        int32_t     BlendDepth = 0;
    };

    struct LayerSetup
    {
        std::vector<BranchFilter> Filters;
    };

    /// UE's ECurveBlendOption: how the base's and the layers' named float curves combine. Append only.
    enum class CurveBlendOption : uint8_t
    {
        Override          = 0, ///< the last layer that has the curve wins
        DoNotOverride     = 1, ///< the first that has it wins
        NormalizeByWeight = 2,
        BlendByWeight     = 3,
        UseBasePose       = 4,
        UseMaxValue       = 5,
        UseMinValue       = 6,
    };

    /// A pose as it travels between graph nodes (UE: FPoseContext): bones, named curves, root motion.
    struct GraphPose
    {
        LocalPose                Pose;
        std::vector<std::string> CurveNames; // parallel to CurveValues
        std::vector<float>       CurveValues;
        BoneTransform            RootMotion; ///< this frame's root delta
    };

    struct LayeredBlendPerBoneNode
    {
        std::vector<LayerSetup> Layers; // one per blend-pose input
        bool                    MeshSpaceRotationBlend = false;
        bool                    MeshSpaceScaleBlend    = false;
        CurveBlendOption        CurveBlend             = CurveBlendOption::Override;
        /// Root motion comes from a layer only in proportion to that layer's weight ON THE ROOT BONE
        /// (UE's bBlendRootMotionBasedOnRootBone): an upper-body layer never steers the character.
        bool BlendRootMotionBasedOnRootBone = true;
    };

    /// Per bone: which layer reaches it (-1 = none, the base shows) and the mask weight.
    struct PerBoneBlendWeight
    {
        int32_t Layer  = -1;
        float   Weight = 0.0F;
    };

    /**
     * @brief The per-bone table, built once per (node, skeleton) — not per frame.
     * Refuses a filter bone the skeleton lacks, by name (a typo would otherwise be a layer that does nothing).
     */
    [[nodiscard]] Common::ResultStr<std::vector<PerBoneBlendWeight>>
    BuildPerBoneWeights( const LayeredBlendPerBoneNode& node, const Skeleton& skeleton );

    /**
     * @brief The node's evaluation. `layers.size() == layerWeights.size() == node.Layers.size()`, refused
     * otherwise. A layer weight of 0 leaves the output bit-identical to the base (short-circuited).
     */
    [[nodiscard]] Common::BoolResultStr BlendLayeredPerBone( const LayeredBlendPerBoneNode&      node,
                                                             std::span<const PerBoneBlendWeight> weights,
                                                             const Skeleton& skeleton, const GraphPose& base,
                                                             std::span<const GraphPose> layers,
                                                             std::span<const float> layerWeights, GraphPose& out );
} // namespace Desert::Animation::Graph
