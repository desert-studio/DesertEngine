#pragma once

/**
 * THE POSE GRAPH, EVALUATED NODE BY NODE (UE: FAnimNode_Base::Evaluate_AnyThread(FPoseContext&)).
 *
 * Every node of the plan (`PlanPoseGraph`: inputs before the node that reads them) evaluates into its own
 * pose buffer from the buffers of the nodes wired into its Pose pins, so ANY composition is played: a blend
 * of blends, an additive over a layered blend, a sequence player in a layer. Output Pose's node's buffer is
 * the result.
 *
 * What a node is for is split the way UE splits it: the NODE math (Layered Blend Per Bone, Apply Additive)
 * is here and pure; the CLOCKS of the leaves (a state machine's running clip, a sequence player's clip) and
 * the graph's parameter values belong to whoever owns the character — the Animator — and are asked for
 * through `PoseGraphSources`, UE's AnimInstance proxy.
 */

#include <Engine/Animation/Graph/AnimGraph.hpp>
#include <Engine/Animation/Graph/LayeredBlendPerBone.hpp>

#include <Common/Core/ResultStr.hpp>

#include <functional>
#include <span>
#include <string>
#include <vector>

namespace Desert::Animation::Graph
{
    /**
     * @brief UE FAnimNode_ApplyAdditive in local space: per bone, the additive pose's difference from the
     *        `reference` pose (the rest pose the additive was authored against), scaled by `alpha` and put on
     *        top of `base` — rotation `slerp(1, add * ref^-1, alpha) * base`, translation `base + alpha x
     *        (add - ref)`, scale `base x mix(1, add / ref, alpha)`. Curves: `base + alpha x additive`, a curve
     *        only the additive has entering from 0. Root motion is the base's: an additive never steers.
     *        Alpha 0 leaves the output bit-identical to the base. Refuses poses of different bone counts.
     */
    [[nodiscard]] Common::BoolResultStr ApplyAdditive( const GraphPose& base, const GraphPose& additive,
                                                       const LocalPose& reference, float alpha, GraphPose& out );

    /// What the owner of the clocks and the parameters supplies to one evaluation (UE: the AnimInstance proxy).
    struct PoseGraphSources
    {
        /// Fills `out` with source node `node`'s pose this tick (`IsSourceKind`), sized to the skeleton.
        std::function<void( size_t node, GraphPose& out )> Sample;
        /// The live value of the graph parameter `parameter` (declared: `PlanPoseGraph` refused otherwise).
        std::function<float( const std::string& parameter )> Parameter;
        /// The pose an Apply Additive's additive input is a difference from (the rest pose on the target rig).
        const LocalPose* AdditiveReference = nullptr;
    };

    class PoseGraphInstance
    {
    public:
        /**
         * @brief Takes `graph` and prepares it for `skeleton`: the plan, each node's inputs by index and each
         *        Layered Blend Per Bone's per-bone table — built HERE, once, not per frame. Refuses an
         *        unplannable graph (PlanPoseGraph's refusal: a cycle is named as its loop of nodes) and a
         *        filter bone the skeleton lacks, naming the node.
         */
        [[nodiscard]] Common::BoolResultStr Bind( AnimGraph graph, const Skeleton& skeleton );

        /// Evaluates every planned node in order into its buffer; `out` becomes Output Pose's. Only after a
        /// successful Bind (an unbound instance leaves `out` untouched).
        void Evaluate( const PoseGraphSources& sources, const Skeleton& skeleton, GraphPose& out );

        [[nodiscard]] bool IsBound() const
        {
            return !m_Plan.empty();
        }
        [[nodiscard]] const AnimGraph& Graph() const
        {
            return m_Graph;
        }
        /// Node indices in evaluation order (PlanPoseGraph's), empty until bound.
        [[nodiscard]] std::span<const int> Plan() const
        {
            return m_Plan;
        }

        /// The value pin `pin` of `node` reads: its bound parameter's, else `unbound` (UE's pin default — 1 for
        /// a layer weight and for Apply Additive's Alpha).
        [[nodiscard]] static float PinValue( const PoseNode& node, std::string_view pin, float unbound,
                                             const std::function<float( const std::string& )>& parameter );

    private:
        AnimGraph                                    m_Graph;
        std::vector<int>                             m_Plan;
        std::vector<std::vector<int>>                m_Inputs; ///< per node: the node index behind each Pose pin
        std::vector<std::vector<PerBoneBlendWeight>> m_Tables; ///< per node: a layered blend's per-bone table
        std::vector<GraphPose>                       m_Poses;  ///< per node: its pose this evaluation
        std::vector<GraphPose>                       m_LayerScratch;
        std::vector<float>                           m_WeightScratch;
    };
} // namespace Desert::Animation::Graph
