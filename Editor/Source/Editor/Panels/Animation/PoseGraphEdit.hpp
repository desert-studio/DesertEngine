#pragma once

#include <Editor/Core/GraphCanvas/GraphCanvas.hpp>

#include <Engine/Animation/Graph/AnimGraph.hpp>

#include <Common/Core/ResultStr.hpp>

#include <string>
#include <string_view>
#include <vector>

// ── THE POSE GRAPH AS AN EDITOR EDITS IT, WITHOUT AN IMGUI CONTEXT ────────────────────────────────────
//
// UE's AnimGraph editor: pose nodes of every kind added from a menu, their Pose pins wired by dragging,
// a node or a wire deleted, Output Pose fed by one node. Every operation here is a whole edit of the
// graph — the panel's canvas, its context menu and its document actions all call the SAME function, so
// what a client drives is what a person drags — and every refusal names what is wrong, in the engine's
// own terms (`PlanPoseGraph`'s sentences where the rule is the engine's).
//
// No ImGui, no ed::EditorContext: the AnimGraphValidation suite compiles this unit and measures it.
namespace Desert::Editor::Graph
{
    /// @p desired, or @p desired with a numeric suffix, such that no node of @p nodes carries it — wires
    /// name nodes, so two nodes of one name make a wire mean either (PlanPoseGraph refuses that graph).
    [[nodiscard]] std::string MakeUniquePoseNodeName( const std::vector<Animation::Graph::PoseNode>& nodes,
                                                      const std::string&                             desired );

    /// The menu / document-action spelling of a kind, "Sequence Player" (UE's node titles).
    [[nodiscard]] const char* PoseNodeTitle( Animation::Graph::PoseNodeKind kind );

    /// The kinds a person may add in @p scope, in menu order. LinkedInputPose only in a layer graph.
    [[nodiscard]] std::vector<Animation::Graph::PoseNodeKind> AddableKinds( Animation::Graph::GraphScope scope );

    /// Appends a node of @p kind to @p nodes at (@p x, @p y), with its payload and one EMPTY wire slot per
    /// Pose pin (an empty wire is an unwired pin). A Sequence Player plays @p clip; a Layered Blend Per
    /// Bone starts with one layer (UE's default: base + one blend pose); a Linked Anim Layer calls the
    /// first layer of the first interface @p graph declares (none declared: named empty, and the plan
    /// says which interfaces exist). Refuses a LinkedInputPose outside a layer graph. Returns the name.
    [[nodiscard]] Common::ResultStr<std::string> AddPoseNode( const Animation::Graph::AnimGraph&       graph,
                                                              std::vector<Animation::Graph::PoseNode>& nodes,
                                                              Animation::Graph::PoseNodeKind           kind,
                                                              Animation::Graph::GraphScope scope, float x, float y,
                                                              const std::string& clip );

    /// The question ConnectPose asks before it writes, asked without writing (UE's schema
    /// CanCreateConnection): the same refusals, word for word, and nothing changed. The canvas's
    /// wire offers are this answer, not a trial edit on a copy of the graph.
    [[nodiscard]] Common::BoolResultStr CanConnectPose( const std::vector<Animation::Graph::PoseNode>& nodes,
                                                        std::string_view from, std::string_view to, int pin );

    /// Wires node @p from into Pose pin @p pin of node @p to. Refuses a missing node, a pin the node does
    /// not have, a node wired into itself and a wire that would close a loop — the loop spelled as
    /// PlanPoseGraph spells one, "A -> B -> A". Replaces what the pin was wired to (a pin takes one wire).
    [[nodiscard]] Common::BoolResultStr ConnectPose( std::vector<Animation::Graph::PoseNode>& nodes,
                                                     std::string_view from, std::string_view to, int pin );

    /// Wires node @p from into Output Pose. Refuses a node that does not exist.
    [[nodiscard]] Common::BoolResultStr ConnectOutput( const std::vector<Animation::Graph::PoseNode>& nodes,
                                                       std::string& outputPose, std::string_view from );

    /// Unwires Pose pin @p pin of @p to (the slot stays, empty). Refuses a missing node or pin.
    [[nodiscard]] Common::BoolResultStr DisconnectPose( std::vector<Animation::Graph::PoseNode>& nodes,
                                                        std::string_view to, int pin );

    /// Deletes node @p name and every wire into it (their pins become unwired; Output Pose too).
    [[nodiscard]] Common::BoolResultStr RemovePoseNode( std::vector<Animation::Graph::PoseNode>& nodes,
                                                        std::string& outputPose, std::string_view name );

    /// Renames node @p index to @p desired (uniquified) and carries every wire naming it, Output Pose
    /// included — the parameter rename's rule on the pose half. Returns the name given.
    std::string RenamePoseNode( std::vector<Animation::Graph::PoseNode>& nodes, std::string& outputPose, int index,
                                const std::string& desired );

    /// Gives a Layered Blend Per Bone @p count layers: the Pose pins grow or shrink with them (a new pin
    /// unwired), and a weight-pin binding of a removed layer is dropped with it.
    void SetLayerCount( Animation::Graph::PoseNode& node, size_t count );

    /// Binds parameter pin @p pin of @p node to @p parameter; an empty @p parameter unbinds it (UE: the
    /// pin's default, 1 for Alpha and for a layer weight). Refuses a pin the node does not have and a
    /// parameter the graph does not declare.
    [[nodiscard]] Common::BoolResultStr BindParameterPin( const Animation::Graph::AnimGraph& graph,
                                                          Animation::Graph::PoseNode& node, std::string_view pin,
                                                          const std::string& parameter );

    /// The parameter bound to @p pin of @p node, empty when unbound.
    [[nodiscard]] std::string BoundParameter( const Animation::Graph::PoseNode& node, std::string_view pin );

    /// The label of Pose pin @p pin of @p node: "Base Pose", "Blend Pose 0", "Additive", "In Pose".
    [[nodiscard]] std::string PosePinLabel( const Animation::Graph::PoseNode& node, int pin );

    // ── THE CANVAS PLAN OF THE POSE GRAPH ─────────────────────────────────────────────────────────────

    /// A pin of the pose canvas: node index into the nodes (kOutputSink for the Output Pose sink), the
    /// Pose pin index for an input, -1 for a node's output pin.
    struct PosePinRef
    {
        int  Node   = -1;
        int  Pin    = -1;
        bool Output = false;

        [[nodiscard]] bool Valid() const
        {
            return Node != -1;
        }
    };
    inline constexpr int kOutputSink = -2;

    /// One wire of the canvas: node @p From's output into Pose pin @p Pin of node @p To (kOutputSink: the
    /// Output Pose sink).
    struct PoseWireRef
    {
        int From = -1;
        int To   = -1;
        int Pin  = -1;
    };

    struct PoseGraphCanvas
    {
        CanvasPlan                          Plan;    // Plan.Nodes: one per pose node, then the sink LAST
        std::vector<ElementId>              OutPins; // parallel to the nodes
        std::vector<std::vector<ElementId>> InPins;  // parallel to the nodes, one per Pose pin
        ElementId                           SinkPin = ElementId::Invalid;
        std::vector<PoseWireRef>            Wires; // parallel to Plan.Links
    };

    /// The canvas of @p nodes and @p outputPose, ids out of @p ids by node NAME. The Output Pose sink is planned
    /// LAST, right of the rightmost node as UE places it (its position is not part of the file).
    [[nodiscard]] PoseGraphCanvas PlanPoseCanvas( const std::vector<Animation::Graph::PoseNode>& nodes,
                                                  const std::string& outputPose, ElementIdMap& ids );

    /// The pin a canvas pin id names; invalid when it is none of this frame's.
    [[nodiscard]] PosePinRef PinOf( const PoseGraphCanvas& canvas, uint64_t pin );

    /// The node a canvas node id names (kOutputSink for the sink), -1 when none.
    [[nodiscard]] int PoseNodeOf( const PoseGraphCanvas& canvas, ElementId node );

    /// The wire a canvas link id names; From == -1 when none.
    [[nodiscard]] PoseWireRef WireOf( const PoseGraphCanvas& canvas, ElementId link );

    /// Where a node added to @p nodes should sit so that it covers no other (the state grid's rule).
    [[nodiscard]] std::pair<float, float>
    NextPoseNodePosition( const std::vector<Animation::Graph::PoseNode>& nodes );
} // namespace Desert::Editor::Graph
