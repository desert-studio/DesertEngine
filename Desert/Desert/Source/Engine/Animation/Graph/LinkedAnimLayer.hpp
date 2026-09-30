#pragma once

/**
 * LINKED ANIM LAYERS — SWAPPABLE SUB-GRAPHS BEHIND A DECLARED INTERFACE (UE: UAnimLayerInterface +
 * FAnimNode_LinkedAnimLayer + FAnimNode_LinkedInputPose + LinkAnimClassLayers).
 *
 * An INTERFACE is a name and the names of its layers; every layer takes ONE input pose and returns a pose.
 * A graph that CALLS layers declares the interface and places `LinkedAnimLayer` nodes naming interface +
 * layer — never an implementation. A graph that IMPLEMENTS an interface declares it too and carries one
 * layer graph per layer of it (`AnimLayerGraph`, AnimGraph.hpp): a small pose graph whose
 * `LinkedInputPose` node is the pose the caller handed in. At runtime an implementing graph is LINKED on
 * the character (`LinkedLayerTable::Link`, `Animator::LinkLayers`) — "armed with a rifle" links the rifle
 * graph — and every node calling that interface's layers now evaluates the implementation's layer graph,
 * without an edit to the calling graph. Unlinked, a layer is the interface's DEFAULT: its input pose passed
 * through, so a character with nothing linked still animates.
 *
 * Interfaces match BY NAME, and a link is refused unless the two graphs declare the interface with the
 * same layers (the name is the contract both sides were written against; UE's asset reference is that
 * contract there). Declarations and implementations are optional in a .danimgraph: absent, the graph
 * neither calls nor implements a layer.
 */

#include <string>
#include <vector>

namespace Desert::Animation::Graph
{
    /// UE UAnimLayerInterface: a named set of layers, each taking one input pose.
    struct AnimLayerInterface
    {
        std::string              Name;
        std::vector<std::string> Layers;
    };

    /// The payload of a LinkedAnimLayer node: which layer of which declared interface it calls. Pose pin 0
    /// is the layer's input pose, and what the node outputs while no implementation is linked.
    struct LinkedAnimLayerNode
    {
        std::string Interface;
        std::string Layer;
    };
} // namespace Desert::Animation::Graph
