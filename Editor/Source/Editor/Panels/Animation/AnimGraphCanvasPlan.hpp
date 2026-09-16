#pragma once

#include <Editor/Core/GraphCanvas/GraphCanvas.hpp>

#include <Engine/Animation/Graph/AnimGraph.hpp>

#include <string>
#include <vector>

// ── WHAT THE ANIM GRAPH ASKS THE CANVAS TO DRAW, WITHOUT AN IMGUI CONTEXT ─────────────────────────────
//
// Separated from `AnimGraphPanel` so that the part with a defect in it can be MEASURED. The defect was
// `NodeId( i ) = i + 1` — canvas identity taken from a position in a vector — and no test that needs a
// live `ed::EditorContext` could ever have run here. This translation unit touches ImGui not at all and
// the engine only through the graph's plain structs, so `GraphCanvasIdentity` compiles it directly.
//
// It is also where the panel's index arithmetic went to die: every "which state is this id" question is
// answered by a lookup through the tables below rather than by subtracting one.
namespace Desert::Editor::Graph
{
    /// Where a link on the canvas came from: `graph.States[State].Transitions[Index]`.
    struct TransitionRef
    {
        int State = -1;
        int Index = -1;

        [[nodiscard]] bool Valid() const
        {
            return State >= 0 && Index >= 0;
        }
    };

    /// One frame's worth of anim-graph canvas, plus the tables that map an id the canvas hands back to
    /// the element it names.
    struct AnimGraphCanvas
    {
        CanvasPlan Plan;

        // Parallel to `graph.States`. A state with no node (there is no such case today) would hold
        // `ElementId::Invalid`, which every lookup below treats as "no match" rather than as index 0.
        std::vector<ElementId> StateNodes;
        std::vector<ElementId> StateInPins;
        std::vector<ElementId> StateOutPins;

        /// Parallel to `Plan.Links`.
        std::vector<TransitionRef> LinkRefs;
    };

    /// Builds the plan for @p graph, issuing (or re-using) ids out of @p ids. Calls `BeginFrame` and
    /// `EndFrame` on the map itself: the plan IS the frame, and a caller that had to remember to bracket
    /// it is a caller that will one day forget.
    [[nodiscard]] AnimGraphCanvas PlanAnimGraph( const Animation::Graph::AnimGraph& graph, ElementIdMap& ids );

    /// The state a node id names, or -1. NOT `id - 1`.
    [[nodiscard]] int StateOfNode( const AnimGraphCanvas& canvas, ElementId node );

    /// The state an input / output pin id names, or -1.
    [[nodiscard]] int StateOfInPin( const AnimGraphCanvas& canvas, uint64_t pin );
    [[nodiscard]] int StateOfOutPin( const AnimGraphCanvas& canvas, uint64_t pin );

    /// The transition a link id names. Invalid when the id is not one of this frame's links.
    [[nodiscard]] TransitionRef TransitionOfLink( const AnimGraphCanvas& canvas, ElementId link );

    /// @p desired, or @p desired with a numeric suffix, such that no OTHER state of @p graph carries it.
    /// @p selfIndex is the state being named (-1 when the state does not exist yet), so renaming a state
    /// to what it is already called is not treated as a collision.
    ///
    /// NAMES ARE THE ANIM GRAPH'S IDENTITY — `Entry`, `Transition::To` and `Evaluator::FindState` all
    /// resolve by string and all take the FIRST match — so two states sharing a name means the second is
    /// unreachable and plays the first one's clip with nothing said. Nothing enforced it before.
    [[nodiscard]] std::string MakeUniqueStateName( const Animation::Graph::AnimGraph& graph,
                                                   const std::string& desired, int selfIndex );
} // namespace Desert::Editor::Graph
