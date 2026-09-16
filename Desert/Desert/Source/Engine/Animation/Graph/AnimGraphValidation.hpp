#pragma once

#include "AnimGraph.hpp"

#include <span>
#include <string>
#include <vector>

// ── WHAT IS WRONG WITH A GRAPH, SAID ONCE, WHERE SOMEBODY IS READING ──────────────────────────────────
//
// Every rule here answers a defect the tree HAS and reports nothing about today. All three were named in
// Docs/Animation/07_panels_design.md §8.3 as W1/W2/W3; the shape they share is that the runtime's answer
// to each is silence:
//
//   W1  a state with no clip, or with a clip name that is not among the ones this skeleton can play.
//       `AnimationECSSystem` asks the library for the name, gets nothing, and the character stands there.
//   W2  a transition that can never fire, because an EARLIER transition out of the same state fires in
//       every case this one would. `Evaluator::Update` takes the first eligible transition and breaks;
//       the later one is dead and nothing says so.
//   W3  a condition naming a parameter the graph does not declare. `Evaluator::GetFloat` is deliberately
//       tolerant (its caller runs sixty times a second and has no channel to refuse on), so the condition
//       reads 0.0 and compares against it happily.
//
// NO IMGUI AND NO ENGINE STATE. The panel is where these are drawn, but the panel is also the one place
// on this machine that cannot be run by a test — synthetic input is closed, and an `ed::EditorContext`
// does not exist on a build machine. So the deciding lives here, where `AnimGraphValidation` compiles it
// directly and mutates it, exactly as `AnimGraphCanvasPlan` was split out of the same panel for the same
// reason.
namespace Desert::Animation::Graph
{
    enum class WarningKind : int
    {
        StateHasNoClip           = 0, // W1a
        StateClipNotAvailable    = 1, // W1b
        TransitionNeverFires     = 2, // W2
        UndeclaredConditionParam = 3, // W3
    };

    struct GraphWarning
    {
        WarningKind Kind = WarningKind::StateHasNoClip;

        /// The state the warning is about — the thing a reader has to click on. Empty only for a warning
        /// that belongs to no single state, of which there are none today.
        std::string State;

        /// The transition within `State`, or -1. An index and not an identity ON PURPOSE: a warning is
        /// produced and consumed inside one frame over one graph, and nothing stores it.
        int Transition = -1;

        /// One sentence, naming the state, the clip or the parameter. A warning that says "a state is
        /// misconfigured" is a warning an artist cannot act on without opening every state.
        std::string Text;
    };

    /// What clips the caller can see for the skeleton this graph runs on.
    ///
    /// `Known == false` means the caller COULD NOT ASK — the entity has no Animator yet, so there is no
    /// skeleton to ask about — and that is not the same fact as "this skeleton has no clips". Collapsing
    /// the two would make every state of every graph light up W1b for the first frames after a scene
    /// loads, and a warning that cries wolf is worse than no warning: it teaches its reader to skip the
    /// strip, which is where the real one will appear.
    struct ClipSet
    {
        bool                         Known = false;
        std::span<const std::string> Names;
    };

    /// Every W1/W2/W3 finding in @p graph, in state order then transition order. Empty for a graph with
    /// nothing wrong with it, which is the answer the panel draws as "no strip at all".
    [[nodiscard]] std::vector<GraphWarning> Validate( const AnimGraph& graph, const ClipSet& clips );

    /// W3 alone, as ONE sentence, or empty. This is what `Evaluator::CheckStructure` records: the
    /// evaluator checks the graph once at construction because the place the conditions are READ cannot
    /// refuse, and the panel checks the same graph every frame because that is where a person is looking.
    /// Two readers, two rhythms, ONE rule — a second spelling of "which parameters are undeclared" would
    /// be a strip and a log that disagree about the same graph.
    [[nodiscard]] std::string UndeclaredConditionParameters( const AnimGraph& graph );
} // namespace Desert::Animation::Graph
