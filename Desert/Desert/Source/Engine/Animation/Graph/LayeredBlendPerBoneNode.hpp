#pragma once

// The AUTHORED half of the layered blend per bone node (UE: FAnimNode_LayeredBoneBlend's properties): what the
// AnimGraph asset stores. Its own header so the graph format (AnimGraph.hpp) does not pull the runtime pose
// (LayeredBlendPerBone.hpp evaluates it on a GraphPose).

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Desert::Animation::Graph
{
    struct BranchFilter
    {
        std::string BoneName;
        int32_t     BlendDepth = 0;
    };

    /// One layer's reach: its own branch filters OR a bone mask of the skeleton by name (UE
    /// FAnimNode_LayeredBoneBlend's BlendMode: BranchFilter vs BlendMask, the mask a USkeleton blend profile).
    /// Never both - two statements of one reach would be two truths; BuildPerBoneWeights refuses that, and a mask
    /// name the skeleton lacks, by name.
    struct LayerSetup
    {
        std::vector<BranchFilter>  Filters;
        std::optional<std::string> BoneMask; ///< Skeleton::FindBoneMask; absent = the filters above
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

    struct LayeredBlendPerBoneNode
    {
        std::vector<LayerSetup> Layers; // one per blend-pose input
        bool                    MeshSpaceRotationBlend = false;
        bool                    MeshSpaceScaleBlend    = false;
        /// A CurveBlendOption, stored as int like every enum of the AnimGraph format (AnimGraph.hpp).
        int CurveBlend = static_cast<int>( CurveBlendOption::Override );
        /// Root motion comes from a layer only in proportion to that layer's weight ON THE ROOT BONE
        /// (UE's bBlendRootMotionBasedOnRootBone): an upper-body layer never steers the character.
        bool BlendRootMotionBasedOnRootBone = true;
    };
} // namespace Desert::Animation::Graph
