// WHAT THE ANIM GRAPH REFUSES TO SAY, AND THE ONE INSTRUMENT THAT CAN SEE WHETHER IT SAYS IT.
//
// ── THE TRAP THIS SUITE IS BUILT AROUND ──────────────────────────────────────────────────────────────
//
// A control that is drawn but reaches nothing, and a rule that is computed but drawn nowhere, are the
// same defect wearing two hats — and a suite that builds the subject IN CODE and asserts about it cannot
// tell either of them from working software. That has already happened on this tree: a conduction was
// broken so that no entity received a graph at all, and both named suites stayed green, because one
// built graphs in code and the other read the scene as TEXT. Neither ran the conduction.
//
// So this file is in two halves and they are different KINDS of evidence:
//
//   PART 1  the rules themselves, run directly. Every rule has a graph that must trip it and a graph
//           that must NOT — the negative control is half the assertion, because a validator that
//           returns a warning for everything is as useless as one that returns none.
//   PART 2  a CENSUS over the panel's source text, because the panel is the one place on this machine
//           that no test can run: synthetic input is closed and a build machine has no
//           `ed::EditorContext`. The census asserts the WIRING — that what Part 1 measures is what the
//           panel computes and draws. Delete the draw call and Part 1 stays green; that is precisely
//           the hole the census fills, and it is the same instrument `ArgumentOrder` uses for a defect
//           no run can see.
//
// ── AND THE PLACEMENT RULE ───────────────────────────────────────────────────────────────────────────
//
// `NextStatePosition` is in the third part. It is in this suite rather than in `GraphCanvasIdentity`
// because it is not about identity: it is about a node that cannot be clicked because another node is
// exactly on top of it.

#include <Editor/Panels/Animation/AnimGraphCanvasPlan.hpp>

#include <Engine/Animation/Graph/AnimGraph.hpp>
#include <Engine/Animation/Graph/AnimGraphValidation.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace G  = Desert::Animation::Graph;
namespace EG = Desert::Editor::Graph;

namespace
{
    G::Condition Cond( const char* parameter, G::CompareOp op, float value = 0.0f )
    {
        G::Condition condition;
        condition.Parameter = parameter;
        condition.Op        = static_cast<int>( op );
        condition.Value     = value;
        return condition;
    }

    G::State Playing( const char* name, const char* clip )
    {
        G::State state;
        state.Name = name;
        state.Clip = clip;
        return state;
    }

    /// A graph with nothing wrong with it: two states, both with clips, one parameter, one transition
    /// that tests it. Every negative control starts here, so "the validator went quiet" and "the graph
    /// under test was trivial" cannot be confused.
    G::AnimGraph HealthyGraph()
    {
        G::AnimGraph graph;
        graph.Name  = "Locomotion";
        graph.Entry = "Idle";
        graph.Parameters.push_back( { "Speed", static_cast<int>( G::ParamType::Float ), 0.0f } );
        graph.States.push_back( Playing( "Idle", "Idle" ) );
        graph.States.push_back( Playing( "Run", "Run" ) );

        G::Transition toRun;
        toRun.To = "Run";
        toRun.Conditions.push_back( Cond( "Speed", G::CompareOp::Greater, 3.0f ) );
        graph.States[0].Transitions.push_back( toRun );
        return graph;
    }

    const std::vector<std::string> kClips{ "Idle", "Run" };

    G::ClipSet Known()
    {
        return G::ClipSet{ true, kClips };
    }

    [[nodiscard]] size_t CountOf( const std::vector<G::GraphWarning>& warnings, G::WarningKind kind )
    {
        return static_cast<size_t>( std::count_if( warnings.begin(), warnings.end(),
                                                   [kind]( const G::GraphWarning& warning )
                                                   { return warning.Kind == kind; } ) );
    }

    [[nodiscard]] std::string Joined( const std::vector<G::GraphWarning>& warnings )
    {
        std::string all;
        for ( const auto& warning : warnings )
        {
            all += warning.Text + "\n";
        }
        return all;
    }

    /// The repository root, found by walking up to an anchor only this repository has. A suite is run
    /// from wherever the sweep happens to stand, so a relative path alone would be a coin toss.
    std::filesystem::path RepoRoot()
    {
        std::filesystem::path prefix = ".";
        for ( int up = 0; up < 6; ++up )
        {
            if ( std::filesystem::exists( prefix / "Desert/Desert/Source/Engine/Core/SceneSettings.hpp" ) )
            {
                return prefix;
            }
            prefix /= "..";
        }
        return {};
    }

    std::string ReadAll( const std::filesystem::path& path )
    {
        std::ifstream      in( path, std::ios::binary );
        std::ostringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }

    std::string PanelSource()
    {
        return ReadAll( RepoRoot() / "Editor/Source/Editor/Panels/Animation/AnimGraphPanel.cpp" );
    }
} // namespace

// ═══ PART 1 — THE RULES ══════════════════════════════════════════════════════════════════════════════

TEST( AnimGraphValidation, AHealthyGraphProducesNoWarningAtAll )
{
    // THE NEGATIVE CONTROL FOR EVERY TEST BELOW. Without it, a validator that answered "one warning" for
    // any input would satisfy all of them.
    const auto warnings = G::Validate( HealthyGraph(), Known() );
    EXPECT_TRUE( warnings.empty() ) << "a graph with nothing wrong with it was reported on:\n"
                                    << Joined( warnings );
}

TEST( AnimGraphValidation, W1_AStateWithNoClipIsNamed )
{
    G::AnimGraph graph = HealthyGraph();
    graph.States[1].Clip.clear();

    const auto warnings = G::Validate( graph, Known() );
    ASSERT_EQ( CountOf( warnings, G::WarningKind::StateHasNoClip ), 1u ) << Joined( warnings );
    EXPECT_NE( Joined( warnings ).find( "Run" ), std::string::npos )
         << "the warning does not name the state, so it cannot be acted on: " << Joined( warnings );
}

TEST( AnimGraphValidation, W1_AClipThisSkeletonCannotPlayIsNamed )
{
    G::AnimGraph graph   = HealthyGraph();
    graph.States[1].Clip = "Run_v2_FINAL";

    const auto warnings = G::Validate( graph, Known() );
    ASSERT_EQ( CountOf( warnings, G::WarningKind::StateClipNotAvailable ), 1u ) << Joined( warnings );
    EXPECT_NE( Joined( warnings ).find( "Run_v2_FINAL" ), std::string::npos ) << Joined( warnings );
}

TEST( AnimGraphValidation, W1_AnUnknownClipListIsNotAnEmptyClipList )
{
    // THE DIFFERENCE THAT DECIDES BETWEEN A WARNING AND A SILENCE. Before an entity's Animator exists
    // there is no skeleton to ask, and collapsing "could not ask" into "this skeleton has no clips"
    // would light up every state of every graph for the first frames after a scene loads — which is how
    // a warning strip gets learned as noise and stops being read at all.
    const G::AnimGraph graph = HealthyGraph();

    const auto unknown = G::Validate( graph, G::ClipSet{} );
    EXPECT_EQ( CountOf( unknown, G::WarningKind::StateClipNotAvailable ), 0u ) << Joined( unknown );

    const std::vector<std::string> none;
    const auto                     knownEmpty = G::Validate( graph, G::ClipSet{ true, none } );
    EXPECT_EQ( CountOf( knownEmpty, G::WarningKind::StateClipNotAvailable ), 2u )
         << "a skeleton that really has no clips must still be reported: " << Joined( knownEmpty );
}

TEST( AnimGraphValidation, W2_ALaterTransitionBehindALooserOneCanNeverFire )
{
    // THE DOC'S OWN CASE (§8.2): `Speed > 0.1` is authored first, `Speed > 3.0` second. Every Speed that
    // satisfies the second satisfies the first, `Evaluator::Update` takes the first eligible transition
    // and breaks, so the second is dead code in the graph and nothing says so.
    G::AnimGraph graph = HealthyGraph();
    graph.States.push_back( Playing( "Walk", "Idle" ) );

    G::Transition toWalk;
    toWalk.To = "Walk";
    toWalk.Conditions.push_back( Cond( "Speed", G::CompareOp::Greater, 0.1f ) );
    graph.States[0].Transitions.insert( graph.States[0].Transitions.begin(), toWalk );

    const auto warnings = G::Validate( graph, Known() );
    ASSERT_EQ( CountOf( warnings, G::WarningKind::TransitionNeverFires ), 1u ) << Joined( warnings );
    EXPECT_NE( Joined( warnings ).find( "Run" ), std::string::npos ) << Joined( warnings );
}

TEST( AnimGraphValidation, W2_TheSameTwoTransitionsInTheOtherOrderAreFine )
{
    // ORDER IS A PRIORITY AND THE AUTHOR MEANT IT. `Speed > 3.0` first and `Speed > 0.1` second is the
    // correct way to write run-before-walk: at Speed = 5 the first wins, at Speed = 1 the second does,
    // and BOTH are reachable. A rule that flagged mere overlap would fire here, on the shape every real
    // graph is built out of, and be learned as noise within a day.
    G::AnimGraph graph = HealthyGraph();
    graph.States.push_back( Playing( "Walk", "Idle" ) );

    G::Transition toWalk;
    toWalk.To = "Walk";
    toWalk.Conditions.push_back( Cond( "Speed", G::CompareOp::Greater, 0.1f ) );
    graph.States[0].Transitions.push_back( toWalk ); // AFTER the Speed > 3 one

    const auto warnings = G::Validate( graph, Known() );
    EXPECT_EQ( CountOf( warnings, G::WarningKind::TransitionNeverFires ), 0u ) << Joined( warnings );
}

TEST( AnimGraphValidation, W2_TransitionsOnDifferentParametersAreNotShadowing )
{
    // The second discriminates on an axis the first says nothing about, so there are Jump-values at
    // which the first does not hold and the second does. Both are reachable.
    G::AnimGraph graph = HealthyGraph();
    graph.Parameters.push_back( { "Jump", static_cast<int>( G::ParamType::Bool ), 0.0f } );
    graph.States.push_back( Playing( "Jump", "Idle" ) );

    G::Transition toJump;
    toJump.To = "Jump";
    toJump.Conditions.push_back( Cond( "Jump", G::CompareOp::IsTrue ) );
    graph.States[0].Transitions.push_back( toJump );

    const auto warnings = G::Validate( graph, Known() );
    EXPECT_EQ( CountOf( warnings, G::WarningKind::TransitionNeverFires ), 0u ) << Joined( warnings );
}

TEST( AnimGraphValidation, W2_AnUnconditionalTransitionKillsEverythingAfterIt )
{
    // The strongest case and the one a "conditions overlap" rule cannot see: the earlier transition
    // constrains NOTHING, so it fires on the first tick, every tick, and no later transition out of this
    // state will ever be reached.
    G::AnimGraph graph = HealthyGraph();
    graph.States.push_back( Playing( "Walk", "Idle" ) );

    G::Transition always;
    always.To = "Walk";
    graph.States[0].Transitions.insert( graph.States[0].Transitions.begin(), always );

    const auto warnings = G::Validate( graph, Known() );
    ASSERT_EQ( CountOf( warnings, G::WarningKind::TransitionNeverFires ), 1u ) << Joined( warnings );
}

TEST( AnimGraphValidation, W2_AnExitTimeGateThatOpensLaterShadowsNothing )
{
    // `Evaluator::Update` SKIPS a transition whose exit time has not been reached, so the later one gets
    // its turn while the earlier one is held back. Ignoring this would have produced a false report on
    // the commonest authored shape there is — "finish the clip, then go here; interrupt for that".
    G::AnimGraph graph = HealthyGraph();
    graph.States.push_back( Playing( "Walk", "Idle" ) );

    G::Transition gated;
    gated.To          = "Walk";
    gated.HasExitTime = true;
    gated.ExitTime    = 0.9f;
    gated.Conditions.push_back( Cond( "Speed", G::CompareOp::Greater, 0.1f ) );
    graph.States[0].Transitions.insert( graph.States[0].Transitions.begin(), gated );

    const auto warnings = G::Validate( graph, Known() );
    EXPECT_EQ( CountOf( warnings, G::WarningKind::TransitionNeverFires ), 0u ) << Joined( warnings );
}

TEST( AnimGraphValidation, W2_ATransitionWhoseOwnConditionsContradictIsNamed )
{
    G::AnimGraph graph = HealthyGraph();
    graph.States[0].Transitions[0].Conditions.push_back( Cond( "Speed", G::CompareOp::Less, 1.0f ) );

    const auto warnings = G::Validate( graph, Known() );
    ASSERT_EQ( CountOf( warnings, G::WarningKind::TransitionNeverFires ), 1u ) << Joined( warnings );
    EXPECT_NE( Joined( warnings ).find( "contradict" ), std::string::npos ) << Joined( warnings );
}

TEST( AnimGraphValidation, W2_ATransitionToAMissingStateShadowsNothing )
{
    // `Evaluator::Update` resolves `To` and CONTINUES when it does not exist, so a dangling transition
    // does not consume the tick. Blaming it for a later transition's silence would point the artist at
    // the wrong line.
    G::AnimGraph graph = HealthyGraph();

    G::Transition dangling;
    dangling.To = "StateThatWasDeletedByHand";
    graph.States[0].Transitions.insert( graph.States[0].Transitions.begin(), dangling );

    const auto warnings = G::Validate( graph, Known() );
    EXPECT_EQ( CountOf( warnings, G::WarningKind::TransitionNeverFires ), 0u ) << Joined( warnings );
}

TEST( AnimGraphValidation, W3_AConditionOnAnUndeclaredParameterIsNamed )
{
    G::AnimGraph graph                                     = HealthyGraph();
    graph.States[0].Transitions[0].Conditions[0].Parameter = "Sped";

    const auto warnings = G::Validate( graph, Known() );
    ASSERT_EQ( CountOf( warnings, G::WarningKind::UndeclaredConditionParam ), 1u ) << Joined( warnings );
    EXPECT_NE( Joined( warnings ).find( "Sped" ), std::string::npos ) << Joined( warnings );
}

TEST( AnimGraphValidation, W3_IsOneRuleAndNotTwo )
{
    // ONE SPELLING, AND THIS IS THE ASSERTION THAT HOLDS IT TO ONE. The evaluator checks the graph once
    // at construction because the place the conditions are read cannot refuse; the panel checks the same
    // graph every frame because that is where a person is looking. If those two ever answered
    // differently about one graph, the strip and the log would disagree and both would be doubted.
    G::AnimGraph graph                                     = HealthyGraph();
    graph.States[0].Transitions[0].Conditions[0].Parameter = "Sped";

    const G::Evaluator evaluator{ graph };
    EXPECT_EQ( evaluator.GetStructureError(), G::UndeclaredConditionParameters( graph ) );
    EXPECT_FALSE( evaluator.GetStructureError().empty() );

    const G::Evaluator healthy{ HealthyGraph() };
    EXPECT_EQ( healthy.GetStructureError(), G::UndeclaredConditionParameters( HealthyGraph() ) );
    EXPECT_TRUE( healthy.GetStructureError().empty() );
}

// ═══ PART 2 — THE CENSUS OVER THE PANEL, BECAUSE NOTHING HERE CAN RUN IT ══════════════════════════════

TEST( AnimGraphValidation, ThePanelIsTheOneThatAsksAndTheOneThatDraws )
{
    const std::string source = PanelSource();
    ASSERT_FALSE( source.empty() ) << "AnimGraphPanel.cpp was not found from " << RepoRoot();

    // It asks the validator...
    EXPECT_NE( source.find( "G::Validate( *anim->Graph, clips )" ), std::string::npos )
         << "the panel does not call the validator, so every rule measured above is computed by nobody";

    // ...it hands the answer to the strip...
    EXPECT_NE( source.find( "DrawWarningStrip( *anim->Graph, warnings )" ), std::string::npos )
         << "the panel computes warnings and never draws them -- a rule with no reader";

    // ...and the strip puts the sentence on screen. `warning.Text` is what carries the state name and
    // the clip name; a strip that drew only an icon and a count would be unactionable. The line is
    // composed before it is drawn, because the hit box under it has to be measured as what is DRAWN and
    // not as the sentence alone -- see the panel.
    EXPECT_NE( source.find( "+ warning.Text;" ), std::string::npos )
         << "the strip does not draw the warning's own sentence";
    EXPECT_NE( source.find( "ImGui::TextWrapped( \"%s\", line.c_str() )" ), std::string::npos )
         << "the strip's sentence is not wrapped any more: un-wrapped it runs off the document's right "
            "edge and the half that names the clip or the parameter is cut";
}

TEST( AnimGraphValidation, DraggingAStateIsAnEditToTheFile )
{
    // `State.X/Y` are serialized into the .danimgraph, so a drag authors the file. The panel used to
    // discard `PullNodePosition`'s answer with a `(void)`, which meant a layout the user arranged was
    // never marked unsaved and died with the window, silently. No run of this binary can see a drag;
    // the source text can.
    const std::string source = PanelSource();
    ASSERT_FALSE( source.empty() );

    EXPECT_NE( source.find( "dirty |= Graph::PullNodePosition( planned, s.X, s.Y )" ), std::string::npos )
         << "a node drag no longer marks the graph edited";
    EXPECT_EQ( source.find( "(void)Graph::PullNodePosition" ), std::string::npos )
         << "the discarded-result form is back";
}

TEST( AnimGraphValidation, TheParameterDefaultIsAuthoredAndNotJustStored )
{
    // 07 §3.1 / §17.3: `Parameter::Default` existed in the model and in the file format with nothing
    // anywhere able to set it, so every parameter of every shipped graph started at 0 and a built game's
    // state machine could not leave its entry state except by exit-time.
    const std::string source = PanelSource();
    ASSERT_FALSE( source.empty() );

    EXPECT_NE( source.find( "DrawTypedValue( \"##pd\", declaredType, p.Default )" ), std::string::npos )
         << "there is no control bound to Parameter::Default";
    EXPECT_NE( source.find( "dirty |= DrawTypedValue( \"##pd\"" ), std::string::npos )
         << "editing the default does not mark the graph edited, so it would never reach the file";
}

TEST( AnimGraphValidation, TheDefaultSurvivesTheFileAndSeedsTheEvaluator )
{
    // The other end of the same chain: a value the control can now write has to arrive somewhere. This
    // is the READER the §8.3 table names for that row.
    G::AnimGraph graph          = HealthyGraph();
    graph.Parameters[0].Default = 4.5f;

    const G::Evaluator evaluator{ graph };
    EXPECT_FLOAT_EQ( evaluator.GetFloat( "Speed" ), 4.5f )
         << "Evaluator::Reset does not seed live values from the authored default";
}

// ═══ PART 3 — WHERE A NEW STATE LANDS ════════════════════════════════════════════════════════════════

TEST( AnimGraphValidation, NewStatesNeverLandOnTopOfEachOther )
{
    // `+ State` used to write (0, 0) into every new state, so the second one covered the first exactly.
    // A node under another node cannot be clicked: it cannot be renamed, given a clip or deleted, and
    // nothing says it is there.
    G::AnimGraph graph;
    for ( int i = 0; i < 17; ++i )
    {
        const EG::StatePosition where = EG::NextStatePosition( graph );
        G::State                state;
        state.Name = "State_" + std::to_string( i );
        state.X    = where.X;
        state.Y    = where.Y;
        graph.States.push_back( state );
    }

    ASSERT_EQ( graph.States.size(), 17u );
    for ( size_t a = 0; a < graph.States.size(); ++a )
    {
        for ( size_t b = a + 1; b < graph.States.size(); ++b )
        {
            const bool apart = std::abs( graph.States[a].X - graph.States[b].X ) >= EG::kStateGridStepX ||
                               std::abs( graph.States[a].Y - graph.States[b].Y ) >= EG::kStateGridStepY;
            EXPECT_TRUE( apart ) << "states " << a << " and " << b << " overlap at (" << graph.States[a].X << ", "
                                 << graph.States[a].Y << ")";
        }
    }
}

TEST( AnimGraphValidation, ANewStateAvoidsWhereTheUserDraggedTheOthers )
{
    // THE GRID IS ASKED WHICH CELL IS FREE, NOT HOW MANY STATES THERE ARE, and this is the case that
    // tells the two apart. The one existing state has been dragged onto the SECOND cell, so a rule that
    // counted ("one state, therefore cell one") would drop the new node exactly on top of it, while cell
    // zero sits empty. Written this way after a mutation: with the dragged state left at the origin,
    // counting and scanning give the same answer and the counting rule survived the test.
    G::AnimGraph graph;
    G::State     dragged;
    dragged.Name = "Idle";
    dragged.X    = EG::kStateGridStepX; // cell 1
    dragged.Y    = 0.0f;
    graph.States.push_back( dragged );

    const EG::StatePosition second = EG::NextStatePosition( graph );
    EXPECT_FLOAT_EQ( second.X, 0.0f ) << "the free cell zero was passed over";
    EXPECT_FLOAT_EQ( second.Y, 0.0f );
}

TEST( AnimGraphValidation, TheFirstStateOfAnEmptyGraphSitsAtTheOrigin )
{
    // The negative control for the two above: the rule must not push the FIRST state somewhere arbitrary,
    // or every graph would open with its entry state off to one side of an empty canvas.
    const G::AnimGraph      empty;
    const EG::StatePosition first = EG::NextStatePosition( empty );
    EXPECT_FLOAT_EQ( first.X, 0.0f );
    EXPECT_FLOAT_EQ( first.Y, 0.0f );
}

TEST( AnimGraphValidation, ThePanelUsesThePlacementRuleRatherThanZero )
{
    const std::string source = PanelSource();
    ASSERT_FALSE( source.empty() );
    EXPECT_NE( source.find( "Graph::NextStatePosition( *anim->Graph )" ), std::string::npos )
         << "+ State does not use the placement rule, so new states land on top of each other again";
}

TEST( AnimGraphValidation, TheButtonAndTheDocumentActionAddTheSameState )
{
    // WHAT A CLIENT DRIVES IS WHAT A PERSON PRESSES. `+ State` is also a document action, because a
    // toolbar button cannot be pressed on this machine -- synthetic input is closed -- and that is
    // precisely why "a new state lands on top of its neighbour" survived until now: nothing but a person
    // with a mouse could produce one. Two code paths here would be two behaviours to keep in step, and
    // the checked one would be the one nobody uses.
    const std::string source = PanelSource();
    ASSERT_FALSE( source.empty() );

    // `AddState();` WITH THE SEMICOLON, which is a call and not a definition: the definition's own
    // `void AnimGraphPanel::AddState()` contains the bare spelling and would be counted with it. Written
    // this way because the first version of this assertion counted three and its comment claimed the
    // definition was excluded -- a comment asserting something the code did not do, which is the class of
    // defect this project keeps paying for, caught here by its own test.
    size_t calls = 0;
    for ( size_t at = source.find( "AddState();" ); at != std::string::npos;
          at        = source.find( "AddState();", at + 1 ) )
    {
        ++calls;
    }
    EXPECT_EQ( calls, 2u ) << "the toolbar button and the 'Add State' document action are not the same "
                              "call any more (found "
                           << calls << ")";
}

// ═══ PART 4 — RENAMING A PARAMETER, WHICH IS THE DEFECT W3 EXISTS TO REPORT ══════════════════════════
//
// W3 tells a reader that a condition names a parameter the graph does not declare. The commonest way to
// PRODUCE one was the panel itself: it moved `Parameter::Name` and left every `Condition` behind. These
// tests are about the cure rather than the report, and the last one asserts the two agree.

namespace
{
    /// Two parameters and one transition whose two conditions name one of them each, so that every test
    /// below has a control it must not disturb.
    G::AnimGraph TwoParameterGraph()
    {
        G::AnimGraph graph;
        graph.Parameters.push_back( { "Speed", static_cast<int>( G::ParamType::Float ), 0.0f } );
        graph.Parameters.push_back( { "Armed", static_cast<int>( G::ParamType::Bool ), 0.0f } );

        G::State idle = Playing( "Idle", "Idle" );
        G::State run  = Playing( "Run", "Run" );

        G::Transition toRun;
        toRun.To = "Run";
        toRun.Conditions.push_back( Cond( "Speed", G::CompareOp::Greater, 3.0f ) );
        toRun.Conditions.push_back( Cond( "Armed", G::CompareOp::IsTrue ) );
        idle.Transitions.push_back( toRun );

        graph.Entry = "Idle";
        graph.States.push_back( idle );
        graph.States.push_back( run );
        return graph;
    }
} // namespace

TEST( AnimGraphValidation, RenamingAParameterCarriesItsConditionsWithIt )
{
    G::AnimGraph graph = TwoParameterGraph();

    EXPECT_EQ( EG::RenameParameter( graph, 0, "GroundSpeed" ), "GroundSpeed" );

    EXPECT_EQ( graph.Parameters[0].Name, "GroundSpeed" );
    EXPECT_EQ( graph.States[0].Transitions[0].Conditions[0].Parameter, "GroundSpeed" )
         << "the condition was left naming a parameter that no longer exists, which reads 0.0 through the "
            "tolerant GetFloat and compares against it";
    // THE NEGATIVE CONTROL. A rename that rewrote every condition rather than the matching ones would
    // pass the line above and destroy the graph.
    EXPECT_EQ( graph.States[0].Transitions[0].Conditions[1].Parameter, "Armed" );
    EXPECT_EQ( graph.Parameters[1].Name, "Armed" );
}

TEST( AnimGraphValidation, ARenameOntoAnOccupiedNameIsGivenAFreeOneAndTheConditionsFollowTHAT )
{
    // Renaming `Speed` to `Armed` cannot be granted: `Evaluator::FindParameter` takes the first match, so
    // the second `Armed` would declare a type nobody consults while its default still overwrote the
    // first's in `Reset`. What matters here is that the conditions follow the name actually GIVEN and not
    // the name asked for -- a rename that uniquified the parameter and propagated the request would break
    // exactly the conditions it was meant to keep.
    G::AnimGraph graph = TwoParameterGraph();

    const std::string given = EG::RenameParameter( graph, 0, "Armed" );
    EXPECT_NE( given, "Armed" );
    EXPECT_EQ( graph.Parameters[0].Name, given );
    EXPECT_EQ( graph.States[0].Transitions[0].Conditions[0].Parameter, given );
    EXPECT_EQ( graph.States[0].Transitions[0].Conditions[1].Parameter, "Armed" );
}

TEST( AnimGraphValidation, RenamingALaterDuplicateDoesNotStealTheFirstOnesConditions )
{
    // Only a hand-edited `.danimgraph` can hold two parameters of one name now, and in such a file the
    // conditions belong to the FIRST -- that is the one `Evaluator::FindParameter` resolves them against.
    // Renaming the second one must therefore move nothing.
    G::AnimGraph graph = TwoParameterGraph();
    graph.Parameters.push_back( { "Speed", static_cast<int>( G::ParamType::Int ), 0.0f } );

    EXPECT_EQ( EG::RenameParameter( graph, 2, "Cadence" ), "Cadence" );
    EXPECT_EQ( graph.States[0].Transitions[0].Conditions[0].Parameter, "Speed" )
         << "the condition was reading parameter 0 and was moved onto a rename of parameter 2";
}

TEST( AnimGraphValidation, ANewParameterIsNamedFreeOfTheOnesAlreadyThere )
{
    G::AnimGraph      graph;
    const std::string first = EG::MakeUniqueParameterName( graph, "Param", -1 );
    EXPECT_EQ( first, "Param" );
    graph.Parameters.push_back( { first, static_cast<int>( G::ParamType::Float ), 0.0f } );

    const std::string second = EG::MakeUniqueParameterName( graph, "Param", -1 );
    EXPECT_NE( second, first ) << "two presses of '+ Parameter' produced two parameters nothing can tell "
                                  "apart";
}

TEST( AnimGraphValidation, RenamingIsWhatMakesTheStripGoQUIET )
{
    // THE RELATION, AND IT IS THE POINT OF THE WHOLE PART. Part 1 measures that W3 fires; this measures
    // that the panel's rename is the thing that stops it firing. Two separately-correct halves that
    // disagreed about which name a condition carries would leave a strip permanently lit on a graph the
    // user had just fixed.
    G::AnimGraph graph = TwoParameterGraph();

    // The defect, reproduced exactly as the panel used to produce it: the name moves, the condition does
    // not.
    graph.Parameters[0].Name = "GroundSpeed";
    const auto broken        = G::Validate( graph, Known() );
    ASSERT_FALSE( broken.empty() );
    EXPECT_EQ( broken.front().Kind, G::WarningKind::UndeclaredConditionParam );

    // And the cure. Rename it back THROUGH THE RULE and the strip has nothing left to say.
    graph.Parameters[0].Name = "Speed";
    EXPECT_EQ( EG::RenameParameter( graph, 0, "GroundSpeed" ), "GroundSpeed" );
    const auto cured = G::Validate( graph, Known() );
    EXPECT_TRUE( cured.empty() ) << "the rename left the graph in a state the validator still objects "
                                    "to:\n"
                                 << Joined( cured );
}

TEST( AnimGraphValidation, ThePanelRenamesThroughTheRuleRatherThanWritingTheNameItself )
{
    // The census half. Part 4 above stays green if the panel goes back to writing `p.Name` directly --
    // which is what it did, and what 07 §17.4 is about.
    const std::string source = PanelSource();
    ASSERT_FALSE( source.empty() );

    EXPECT_NE( source.find( "Graph::RenameParameter( graph, i, typed )" ), std::string::npos )
         << "the parameter rename does not go through the rule, so conditions are left behind again";
    EXPECT_EQ( source.find( "InputText( p.Name" ), std::string::npos )
         << "the panel edits Parameter::Name in place, which is the defect itself";
    EXPECT_NE( source.find( "Graph::MakeUniqueParameterName( *anim->Graph, \"Param\", -1 )" ), std::string::npos )
         << "'+ Parameter' pushes a fixed name again, so two presses produce two indistinguishable "
            "parameters";
}

TEST( AnimGraphValidation, TheParameterButtonAndTheDocumentActionAddTheSameParameter )
{
    // THE SAME ARGUMENT AS `AddState`, AND THE SAME SPELLING OF IT. `+ Parameter` is a document action
    // because a toolbar button is unreachable to every client and every check on this machine, and that
    // is precisely why "two presses make two parameters nothing can tell apart" survived as long as the
    // state one did. `AddParameter();` WITH THE SEMICOLON, so the definition's own
    // `void AnimGraphPanel::AddParameter()` is not counted with the calls.
    const std::string source = PanelSource();
    ASSERT_FALSE( source.empty() );

    size_t calls = 0;
    for ( size_t at = source.find( "AddParameter();" ); at != std::string::npos;
          at        = source.find( "AddParameter();", at + 1 ) )
    {
        ++calls;
    }
    EXPECT_EQ( calls, 2u ) << "the toolbar button and the 'Add Parameter' document action are not the "
                              "same call any more (found "
                           << calls << ")";
}

// ═══ PART 5 — A FINDING IS A PLACE ON THE CANVAS, NOT JUST A SENTENCE ════════════════════════════════
//
// `GraphWarning::State` and `::Transition` were computed for every finding and read by NOTHING: the strip
// drew `Text` and stopped. That is a dead knob on the data side, and it is the half no frame can show is
// missing -- the picture looks complete, because what is absent is a READER for a field. These are the
// tests for the reader.

TEST( AnimGraphValidation, W1PointsAtTheStatesOwnNode )
{
    G::AnimGraph graph = HealthyGraph();
    graph.States[1].Clip.clear();

    EG::ElementIdMap ids;
    const auto       canvas = EG::PlanAnimGraph( graph, ids );

    const auto warnings = G::Validate( graph, Known() );
    ASSERT_EQ( warnings.size(), 1u ) << Joined( warnings );

    const EG::WarningTarget target = EG::WarningTargetOf( canvas, graph, warnings[0] );
    ASSERT_TRUE( target.Valid() ) << "the finding names a state the canvas cannot be pointed at";
    EXPECT_EQ( target.Link, EG::ElementId::Invalid ) << "a state warning selected a transition";

    // THE RELATION, NOT THE VALUE. Asserting the id equals some number would pass while the id named a
    // different state; asking the canvas which state that id IS cannot.
    EXPECT_EQ( EG::StateOfNode( canvas, target.Node ), 1 )
         << "clicking the warning about 'Run' would select a different state";
}

TEST( AnimGraphValidation, W3PointsAtTheTransitionsOwnLink )
{
    G::AnimGraph graph = HealthyGraph();
    graph.States[0].Transitions[0].Conditions[0] = Cond( "Velocity", G::CompareOp::Greater, 3.0f );

    EG::ElementIdMap ids;
    const auto       canvas = EG::PlanAnimGraph( graph, ids );

    const auto warnings = G::Validate( graph, Known() );
    ASSERT_EQ( CountOf( warnings, G::WarningKind::UndeclaredConditionParam ), 1u ) << Joined( warnings );

    const EG::WarningTarget target = EG::WarningTargetOf( canvas, graph, warnings[0] );
    ASSERT_NE( target.Link, EG::ElementId::Invalid )
         << "a transition warning must select the LINK: the link is what opens the transition inspector, "
            "which holds the conditions the warning is about";
    EXPECT_EQ( target.Node, EG::ElementId::Invalid );

    const EG::TransitionRef ref = EG::TransitionOfLink( canvas, target.Link );
    ASSERT_TRUE( ref.Valid() );
    EXPECT_EQ( ref.State, 0 );
    EXPECT_EQ( ref.Index, warnings[0].Transition )
         << "the selected link is a different transition than the one the finding is about";
}

TEST( AnimGraphValidation, TheNthTransitionIsNotTheNthLink )
{
    // THE REASON THE LOOKUP GOES THROUGH `LinkRefs` AND NOT THROUGH ARITHMETIC. `PlanAnimGraph` draws no
    // link for a transition whose target name no state carries, so the transitions of a state and the
    // links out of it are NOT the same list -- and W3 fires on exactly such a transition, because a
    // condition is checked whatever its target is. Indexing the links by the warning's transition number
    // would select the wrong transition here, or run off the end.
    G::AnimGraph graph = HealthyGraph();
    graph.States[0].Transitions.clear();

    G::Transition toGhost; // a target no state carries: planned as no link at all
    toGhost.To = "Ghost";
    toGhost.Conditions.push_back( Cond( "Velocity", G::CompareOp::Greater, 1.0f ) );
    graph.States[0].Transitions.push_back( toGhost );

    G::Transition toRun;
    toRun.To = "Run";
    toRun.Conditions.push_back( Cond( "Velocity", G::CompareOp::Greater, 3.0f ) );
    graph.States[0].Transitions.push_back( toRun );

    EG::ElementIdMap ids;
    const auto       canvas = EG::PlanAnimGraph( graph, ids );
    ASSERT_EQ( canvas.Plan.Links.size(), 1u ) << "the ghost transition was drawn as a link after all";

    const auto warnings = G::Validate( graph, Known() );

    // The finding about transition 1 must reach the ONE link, which is transition 1's.
    const auto onOne = std::find_if( warnings.begin(), warnings.end(), []( const G::GraphWarning& w )
                                     { return w.Transition == 1; } );
    ASSERT_NE( onOne, warnings.end() ) << Joined( warnings );
    const EG::WarningTarget one = EG::WarningTargetOf( canvas, graph, *onOne );
    ASSERT_NE( one.Link, EG::ElementId::Invalid );
    EXPECT_EQ( EG::TransitionOfLink( canvas, one.Link ).Index, 1 );

    // And the finding about transition 0 -- which has no link -- must still be reachable, through the
    // state it belongs to. A finding nothing can be clicked on is a finding the strip cannot act on.
    const auto onZero = std::find_if( warnings.begin(), warnings.end(), []( const G::GraphWarning& w )
                                      { return w.Transition == 0; } );
    ASSERT_NE( onZero, warnings.end() ) << Joined( warnings );
    const EG::WarningTarget zero = EG::WarningTargetOf( canvas, graph, *onZero );
    ASSERT_TRUE( zero.Valid() );
    EXPECT_EQ( zero.Link, EG::ElementId::Invalid );
    EXPECT_EQ( EG::StateOfNode( canvas, zero.Node ), 0 );
}

TEST( AnimGraphValidation, EveryFindingOfABadlyBrokenGraphCanBeReached )
{
    // THE PROPERTY THAT MATTERS FOR THE STRIP AS A WHOLE: no finding is a dead end. A single unreachable
    // line teaches its reader that clicking does nothing, which costs the whole control.
    G::AnimGraph graph;
    graph.Entry = "Idle";
    graph.States.push_back( Playing( "Idle", "Idle" ) );
    graph.States.push_back( Playing( "Run", "NoSuchClip" ) );
    graph.States.push_back( Playing( "Aim", "" ) );

    G::Transition wide; // earlier and weaker: shadows the one below
    wide.To = "Run";
    wide.Conditions.push_back( Cond( "Speed", G::CompareOp::Greater, 0.1f ) );
    graph.States[0].Transitions.push_back( wide );

    G::Transition narrow;
    narrow.To = "Run";
    narrow.Conditions.push_back( Cond( "Speed", G::CompareOp::Greater, 3.0f ) );
    graph.States[0].Transitions.push_back( narrow );

    EG::ElementIdMap ids;
    const auto       canvas = EG::PlanAnimGraph( graph, ids );

    const auto warnings = G::Validate( graph, Known() );
    ASSERT_GE( warnings.size(), 4u ) << Joined( warnings );
    for ( const auto& warning : warnings )
    {
        EXPECT_TRUE( EG::WarningTargetOf( canvas, graph, warning ).Valid() )
             << "this finding points at nothing on the canvas: " << warning.Text;
    }
}

TEST( AnimGraphValidation, DeletingAStateDoesNotSendAWarningToItsNeighbour )
{
    // THE DEFECT CLASS THIS WHOLE UNIT EXISTS FOR, in the strip's own terms. Under `NodeId( i ) = i + 1`
    // every id after a deletion named the neighbour, so a warning about 'Aim' would have selected 'Run'
    // -- and the reader would have gone and edited the wrong state, which is worse than not being told.
    G::AnimGraph graph;
    graph.Entry = "Idle";
    graph.States.push_back( Playing( "Idle", "Idle" ) );
    graph.States.push_back( Playing( "Walk", "Idle" ) );
    graph.States.push_back( Playing( "Run", "Idle" ) );
    graph.States.push_back( Playing( "Aim", "" ) ); // the one warning in this graph

    EG::ElementIdMap ids;
    auto             canvas = EG::PlanAnimGraph( graph, ids ); // frame 1: everything present

    graph.States.erase( graph.States.begin() + 1 ); // the user deletes 'Walk'
    canvas = EG::PlanAnimGraph( graph, ids );       // frame 2, through the SAME id map

    const auto warnings = G::Validate( graph, Known() );
    ASSERT_EQ( warnings.size(), 1u ) << Joined( warnings );
    EXPECT_EQ( warnings[0].State, "Aim" );

    const EG::WarningTarget target = EG::WarningTargetOf( canvas, graph, warnings[0] );
    ASSERT_TRUE( target.Valid() );
    const int selected = EG::StateOfNode( canvas, target.Node );
    ASSERT_GE( selected, 0 );
    EXPECT_EQ( graph.States[static_cast<size_t>( selected )].Name, "Aim" )
         << "the warning about 'Aim' selects '"
         << graph.States[static_cast<size_t>( selected )].Name << "' instead";
}

TEST( AnimGraphValidation, TheStripIsAControlAndNotJustText )
{
    // The census half of Part 5, for the same reason every other census here exists: no run of this
    // binary can click a line. What it pins is that the line IS clickable and that the click goes
    // through the plan -- both of which can be removed without a single rule above going red.
    const std::string source = PanelSource();
    ASSERT_FALSE( source.empty() );

    EXPECT_NE( source.find( "RevealWarning( graph, warning )" ), std::string::npos )
         << "clicking a warning does nothing again, so GraphWarning::State/::Transition are back to "
            "having no reader";
    EXPECT_NE( source.find( "Graph::WarningTargetOf( m_Canvas, graph, warning )" ), std::string::npos )
         << "the panel resolves the warning some other way than through the plan -- which is the index "
            "arithmetic this unit was split out to delete";
    EXPECT_NE( source.find( "ed::SelectLink" ), std::string::npos )
         << "a transition finding no longer selects its link, so the conditions it is about stay closed";
    EXPECT_NE( source.find( "ed::SelectNode" ), std::string::npos )
         << "a state finding no longer selects its node";
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
