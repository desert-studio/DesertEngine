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

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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

    class LinkedLayerTable;

    /// What the owner of the clocks and the parameters supplies to one evaluation (UE: the AnimInstance proxy).
    struct PoseGraphSources
    {
        /// Fills `out` with source node `node`'s pose this tick (`IsSourceKind`), sized to the skeleton.
        std::function<void( size_t node, GraphPose& out )> Sample;
        /// The same for source node `node` of the layer graph linked in `slot` of `Linked` (its own clock).
        std::function<void( size_t slot, size_t node, GraphPose& out )> SampleLinked;
        /// The layers linked on this character; nullptr or no link for a node's layer = its input passes.
        LinkedLayerTable* Linked = nullptr;
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
        [[nodiscard]] Common::BoolResultStr Bind( AnimGraph graph, const Skeleton& skeleton,
                                                  GraphScope scope = GraphScope::Host );

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

        /// A LAYER graph's evaluation (bound in `GraphScope::Layer`): its LinkedInputPose nodes output `input`,
        /// its sources are `sources.SampleLinked( slot, ... )`, its pins read `sources.Parameter`.
        void EvaluateLayer( const PoseGraphSources& sources, const Skeleton& skeleton, size_t slot,
                            const GraphPose& input, GraphPose& out );

    private:
        void Run( const PoseGraphSources& sources, const Skeleton& skeleton, std::optional<size_t> slot,
                  const GraphPose* input, GraphPose& out );

        AnimGraph                                    m_Graph;
        std::vector<int>                             m_Plan;
        std::vector<std::vector<int>>                m_Inputs; ///< per node: the node index behind each Pose pin
        std::vector<std::vector<PerBoneBlendWeight>> m_Tables; ///< per node: a layered blend's per-bone table
        std::vector<GraphPose>                       m_Poses;  ///< per node: its pose this evaluation
        std::vector<GraphPose>                       m_LayerScratch;
        std::vector<float>                           m_WeightScratch;
    };

    /**
     * @brief The layers linked on one character (UE: the linked instances of a UAnimInstance, what
     *        LinkAnimClassLayers fills): per (interface, layer) the implementing graph's layer graph, BOUND.
     *        A LinkedAnimLayer node resolves here by name at evaluation; no entry = its input passes through.
     */
    class LinkedLayerTable
    {
    public:
        struct Layer
        {
            /// The implementing graph's identity — its .danimgraph asset GUID, what Unlink matches (UE: the
            /// linked class). Two graphs may share a Name; they never share a GUID.
            uint64_t          Implementation = 0;
            std::string       ImplementationName; ///< for messages only
            std::string       Interface;
            std::string       Name;
            PoseGraphInstance Instance; ///< bound in GraphScope::Layer against the host's skeleton
            /// The layer graph's state machines, run per link (UE: the linked instance's own state); empty
            /// when the layer graph has none. Its parameters are fed from the host's by name each tick.
            std::optional<Evaluator> Machines;
        };

        /**
         * @brief UE LinkAnimClassLayers: every interface `implementation` (asset GUID `implementationId`)
         *        implements now resolves to its layer graphs, replacing whatever was linked for those
         *        interfaces. All or nothing; refuses, naming it: a zero GUID, an interface `host` does not
         *        declare or declares with other layers, a parameter a layer graph reads (a pin or a state
         *        machine's condition) that `host` does not declare as the same type, a layer graph that does
         *        not bind, a graph implementing no interface at all, and a link after which linked layers
         *        would call each other in a cycle.
         */
        [[nodiscard]] Common::BoolResultStr Link( const AnimGraph& host, uint64_t implementationId,
                                                  const AnimGraph& implementation, const Skeleton& skeleton );
        /// UE UnlinkAnimClassLayers: the interfaces the graph with GUID `implementationId` linked go back to
        /// pass-through. A no-op for interfaces another graph has linked since.
        void Unlink( uint64_t implementationId );
        void Clear()
        {
            m_Layers.clear();
        }

        /// The slot of (interface, layer), or empty when nothing is linked for it.
        [[nodiscard]] std::optional<size_t>  Find( std::string_view anInterface, std::string_view layer ) const;
        [[nodiscard]] std::span<const Layer> Layers() const
        {
            return m_Layers;
        }
        [[nodiscard]] Layer& At( size_t slot )
        {
            return m_Layers[slot];
        }

    private:
        std::vector<Layer> m_Layers;
    };
} // namespace Desert::Animation::Graph
