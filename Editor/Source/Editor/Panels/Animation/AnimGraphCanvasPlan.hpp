#pragma once

#include <Editor/Core/GraphCanvas/GraphCanvas.hpp>

#include <Engine/Animation/Graph/AnimGraph.hpp>
#include <Engine/Animation/Graph/AnimGraphValidation.hpp>

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

    /// Where on the canvas one finding of `Animation::Graph::Validate` points.
    ///
    /// AT MOST ONE OF THE TWO IS SET, and they are two fields rather than an id plus a kind on purpose:
    /// the caller has to select a node and a link through different entry points of the node editor, so
    /// a single id it would then have to decode is a decode that can go wrong. An all-`Invalid` target
    /// is a warning about a state this plan does not contain, which one frame cannot produce.
    struct WarningTarget
    {
        ElementId Node = ElementId::Invalid;
        ElementId Link = ElementId::Invalid;

        [[nodiscard]] bool Valid() const
        {
            return Node != ElementId::Invalid || Link != ElementId::Invalid;
        }
    };

    /// The element @p warning is about, looked up in @p canvas.
    ///
    /// `GraphWarning::State` AND `::Transition` HAD NO READER AT ALL until this existed. Every finding
    /// carried them, the strip drew only `Text`, and a reader who wanted to act on "'Run' names no clip"
    /// had to find `Run` by eye — which at the thirty states 07 §8 exists to make readable is the same
    /// as not being told. A field computed and never read is the data-side of a dead knob, and it is the
    /// half that no frame can show is missing.
    ///
    /// A WARNING ABOUT A TRANSITION POINTS AT ITS LINK, not at its state: the link is what the side panel
    /// turns into the transition inspector, so selecting it puts the Blend/Exit-time/Conditions the
    /// warning is about under the reader's hand in the same frame. It falls back to the state's node when
    /// the transition names a target no state carries — `PlanAnimGraph` draws no link for one, and W3
    /// fires on it — because the state is then the only thing there is to go to.
    ///
    /// THE STATE IS FOUND BY NAME, FIRST MATCH — the rule `Evaluator::FindState` and this unit's own link
    /// planner already follow. A `.danimgraph` edited by hand can carry two states of one name; the
    /// second is unreachable to the runtime, so sending its reader to the first is sending them to the
    /// one that actually runs.
    [[nodiscard]] WarningTarget WarningTargetOf( const AnimGraphCanvas&                canvas,
                                                 const Animation::Graph::AnimGraph&    graph,
                                                 const Animation::Graph::GraphWarning& warning );

    /// @p desired, or @p desired with a numeric suffix, such that no OTHER state of @p graph carries it.
    /// @p selfIndex is the state being named (-1 when the state does not exist yet), so renaming a state
    /// to what it is already called is not treated as a collision.
    ///
    /// NAMES ARE THE ANIM GRAPH'S IDENTITY — `Entry`, `Transition::To` and `Evaluator::FindState` all
    /// resolve by string and all take the FIRST match — so two states sharing a name means the second is
    /// unreachable and plays the first one's clip with nothing said. Nothing enforced it before.
    [[nodiscard]] std::string MakeUniqueStateName( const Animation::Graph::AnimGraph& graph,
                                                   const std::string& desired, int selfIndex );

    /// @p desired, or @p desired with a numeric suffix, such that no OTHER parameter of @p graph carries
    /// it. @p selfIndex is the parameter being named (-1 when it does not exist yet).
    ///
    /// THE SAME ARGUMENT AS `MakeUniqueStateName`, ABOUT THE OTHER HALF OF THE GRAPH. `Condition` names a
    /// parameter by string and `Evaluator::FindParameter` takes the FIRST match, so two parameters sharing
    /// a name means the second one's declared TYPE is never consulted while its `Default` still overwrites
    /// the first one's in `Evaluator::Reset` — one live value, two authors, and nothing said. `+ Parameter`
    /// pushed the literal name "Param" every time, so two presses produced exactly that.
    [[nodiscard]] std::string MakeUniqueParameterName( const Animation::Graph::AnimGraph& graph,
                                                       const std::string& desired, int selfIndex );

    /// Renames `graph.Parameters[index]` to @p desired — uniquified as above — AND rewrites every
    /// `Condition` that named it. Returns the name actually given.
    ///
    /// RENAMING A PARAMETER USED TO BREAK EVERY CONDITION ON IT, IN SILENCE (07 §17.4). The panel wrote
    /// `Parameter::Name` and stopped there, unlike the state rename beside it which carries the new name
    /// into `Entry` and every `Transition::To`. What made it worse than "stopped working" is the read
    /// side: `Evaluator::GetFloat` answers an undeclared name with 0.0 and `EvaluateCondition` compares
    /// against that, so `>`/`>=` against a positive threshold go permanently false while
    /// `<`/`<=`/`is false` go permanently TRUE. One rename can either kill a transition or jam the state
    /// machine into taking it every tick, and neither outcome writes a line anywhere.
    ///
    /// ONLY THE FIRST PARAMETER OF A GIVEN NAME CARRIES ITS CONDITIONS, because that is the one
    /// `Evaluator::FindParameter` resolves them against. Renaming a later duplicate (which only a
    /// hand-edited file can produce) must not steal conditions that were never reading it.
    std::string RenameParameter( Animation::Graph::AnimGraph& graph, int index, const std::string& desired );

    /// A canvas position, in the node editor's coordinates. Not `ImVec2`: this header is compiled by a
    /// suite that links no ImGui, which is the whole reason the unit exists apart from its panel.
    struct StatePosition
    {
        float X = 0.0f;
        float Y = 0.0f;
    };

    /// Where a state ADDED to @p graph right now should be put, so that it does not land on top of one
    /// that is already there.
    ///
    /// `+ State` used to write (0, 0) into every new state. The second one therefore covered the first
    /// exactly, and a node hidden under another node is not a cosmetic defect: it cannot be clicked, so
    /// it cannot be given a clip, renamed or deleted, and the only way to reach it is to drag the one on
    /// top away first — which nobody does, because nothing says it is there. The graph the layout is for
    /// is the one §8 exists to make readable at thirty states.
    ///
    /// A GRID SCAN AND NOT "TO THE RIGHT OF THE LAST ONE": the rightmost-plus-a-step rule walks a graph
    /// off into a strip nobody can frame, and it puts the new state back on top of a neighbour as soon
    /// as the user has dragged things around. This asks which grid cell is free, which is true whatever
    /// the user did with the mouse.
    [[nodiscard]] StatePosition NextStatePosition( const Animation::Graph::AnimGraph& graph );

    /// The grid `NextStatePosition` places on. Named here because the test asserts separation in terms
    /// of them, and a test that spelled its own numbers would pass while the panel drifted.
    inline constexpr float kStateGridStepX   = 240.0f;
    inline constexpr float kStateGridStepY   = 130.0f;
    inline constexpr int   kStateGridColumns = 5;
} // namespace Desert::Editor::Graph
