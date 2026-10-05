#include <Engine/Animation/Graph/AnimGraph.hpp>

#include <gtest/gtest.h>

#include <algorithm>

using Desert::Animation::Graph::AnimGraph;
using Desert::Animation::Graph::CompareOp;
using Desert::Animation::Graph::Condition;
using Desert::Animation::Graph::Evaluator;
using Desert::Animation::Graph::Parameter;
using Desert::Animation::Graph::ParamType;
using Desert::Animation::Graph::State;
using Desert::Animation::Graph::Transition;

namespace
{
    // Idle <-> Run, gated on a float "Speed": Idle --(Speed > 0.5)--> Run, Run --(Speed < 0.1)--> Idle.
    AnimGraph LocomotionGraph()
    {
        AnimGraph g               = ::Desert::Animation::Graph::MakeStateMachineGraph();
        g.Name  = "Locomotion";
        OutputMachine( g )->Entry = "Idle";
        g.Parameters.push_back( { "Speed", static_cast<int>( ParamType::Float ), 0.0f } );

        State idle;
        idle.Name = "Idle";
        idle.Clip = "idle_clip";
        idle.Transitions.push_back(
             { "Run", 0.25f, false, 1.0f, { { "Speed", static_cast<int>( CompareOp::Greater ), 0.5f } } } );

        State run;
        run.Name = "Run";
        run.Clip = "run_clip";
        run.Transitions.push_back(
             { "Idle", 0.2f, false, 1.0f, { { "Speed", static_cast<int>( CompareOp::Less ), 0.1f } } } );

        OutputMachine( g )->States = { idle, run };
        return g;
    }
} // namespace

namespace
{
    // A --(GoB)--> B --(GoC)--> C, each a 1-second fade: the shortest graph in which a transition can fire
    // while another is still blending.
    AnimGraph ChainGraph( bool abCanInterrupt = true, int abCurve = 0 )
    {
        AnimGraph g               = ::Desert::Animation::Graph::MakeStateMachineGraph();
        g.Name                    = "Chain";
        OutputMachine( g )->Entry = "A";
        g.Parameters.push_back( { "GoB", static_cast<int>( ParamType::Bool ), 0.0f } );
        g.Parameters.push_back( { "GoC", static_cast<int>( ParamType::Bool ), 0.0f } );

        State      a{ .Name = "A", .Clip = "a", .Transitions = {} };
        Transition ab{
             .To = "B", .Blend = 1.0f, .Conditions = { { "GoB", static_cast<int>( CompareOp::IsTrue ), 0.0f } } };
        ab.BlendCurve   = abCurve;
        ab.CanInterrupt = abCanInterrupt;
        a.Transitions.push_back( ab );
        State b{ .Name = "B", .Clip = "b", .Transitions = {} };
        b.Transitions.push_back( { .To         = "C",
                                   .Blend      = 1.0f,
                                   .Conditions = { { "GoC", static_cast<int>( CompareOp::IsTrue ), 0.0f } } } );
        const State c{ .Name = "C", .Clip = "c", .Transitions = {} };
        OutputMachine( g )->States = { a, b, c };
        return g;
    }
} // namespace

TEST( AnimGraph, AnInterruptingTransitionStacksOnTheOneStillBlending )
{
    Evaluator eval( ChainGraph() );
    ASSERT_TRUE( eval.SetBool( "GoB", true ) );
    ASSERT_TRUE( eval.Update( 0.0f ).Changed );
    eval.AdvanceTransitions( 0.5f );

    ASSERT_TRUE( eval.SetBool( "GoC", true ) );
    const auto fired = eval.Update( 0.0f );
    ASSERT_TRUE( fired.Changed ) << "a transition out of the target was refused while the first one blended";
    EXPECT_EQ( fired.Current->Name, "C" );

    eval.AdvanceTransitions( 0.25f ); // A->B at 0.75, B->C at 0.25
    const auto layers = eval.ActiveStateWeights();
    ASSERT_EQ( layers.size(), 3U ) << "the interrupted transition was cut instead of stacked";
    EXPECT_EQ( layers[0].Of->Name, "A" );
    EXPECT_EQ( layers[1].Of->Name, "B" );
    EXPECT_EQ( layers[2].Of->Name, "C" );
    EXPECT_NEAR( layers[0].Weight, 0.25f * 0.75f, 1e-5f ) << "the oldest pose is not fading out under both";
    EXPECT_NEAR( layers[1].Weight, 0.75f * 0.75f, 1e-5f );
    EXPECT_NEAR( layers[2].Weight, 0.25f, 1e-5f );
    EXPECT_NEAR( layers[0].Weight + layers[1].Weight + layers[2].Weight, 1.0f, 1e-5f );

    eval.AdvanceTransitions( 0.25f ); // A->B finishes: A is gone, B fades out under C
    const auto later = eval.ActiveStateWeights();
    ASSERT_EQ( later.size(), 2U );
    EXPECT_EQ( later[0].Of->Name, "B" );
    EXPECT_NEAR( later[0].Weight, 0.5f, 1e-5f );
    EXPECT_NEAR( later[1].Weight, 0.5f, 1e-5f );

    eval.AdvanceTransitions( 0.5f );
    const auto settled = eval.ActiveStateWeights();
    ASSERT_EQ( settled.size(), 1U );
    EXPECT_EQ( settled[0].Of->Name, "C" );
    EXPECT_EQ( settled[0].Weight, 1.0f );
}

TEST( AnimGraph, ATransitionThatCannotBeInterruptedHoldsTheMachineUntilItEnds )
{
    Evaluator eval( ChainGraph( /*abCanInterrupt=*/false ) );
    ASSERT_TRUE( eval.SetBool( "GoB", true ) );
    ASSERT_TRUE( eval.SetBool( "GoC", true ) );
    ASSERT_TRUE( eval.Update( 0.0f ).Changed ); // A -> B

    eval.AdvanceTransitions( 0.5f );
    const auto held = eval.Update( 0.0f );
    EXPECT_FALSE( held.Changed ) << "B -> C fired over a transition marked CanInterrupt = false";
    EXPECT_EQ( held.Current->Name, "B" );

    eval.AdvanceTransitions( 0.5f ); // A -> B ends
    const auto freed = eval.Update( 0.0f );
    ASSERT_TRUE( freed.Changed ) << "the machine stayed held after the uninterruptible fade ended";
    EXPECT_EQ( freed.Current->Name, "C" );
}

TEST( AnimGraph, ABlendCurveShapesTheTransitionsWeight )
{
    // CubicInOut is symmetric (0.5 at the midpoint), so the curve is told apart from Linear at a quarter.
    Evaluator eval( ChainGraph( true, static_cast<int>( Desert::Animation::AlphaBlendOption::CubicInOut ) ) );
    ASSERT_TRUE( eval.SetBool( "GoB", true ) );
    const auto fired = eval.Update( 0.0f );
    ASSERT_TRUE( fired.Changed );
    EXPECT_EQ( fired.Curve, Desert::Animation::AlphaBlendOption::CubicInOut )
         << "the consumer is not told the curve";

    eval.AdvanceTransitions( 0.25f );
    auto layers = eval.ActiveStateWeights();
    ASSERT_EQ( layers.size(), 2U );
    EXPECT_NEAR( layers[1].Weight, 4.0f * 0.25f * 0.25f * 0.25f, 1e-5f ) << "the fade ignored its curve";
    EXPECT_NEAR( layers[0].Weight + layers[1].Weight, 1.0f, 1e-5f );

    eval.AdvanceTransitions( 0.25f );
    layers = eval.ActiveStateWeights();
    ASSERT_EQ( layers.size(), 2U );
    EXPECT_NEAR( layers[1].Weight, 0.5f, 1e-5f );
}

namespace
{
    // The live Fox graph's first hop: Survey --(Speed > 0.1, Blend 0.5 s)--> Walk.
    AnimGraph SurveyWalkGraph()
    {
        AnimGraph g               = ::Desert::Animation::Graph::MakeStateMachineGraph();
        g.Name                    = "FoxLocomotion";
        OutputMachine( g )->Entry = "Survey";
        g.Parameters.push_back( { "Speed", static_cast<int>( ParamType::Float ), 0.0f } );
        State survey{ .Name = "Survey", .Clip = "Survey", .Transitions = {} };
        survey.Transitions.push_back(
             { .To         = "Walk",
               .Blend      = 0.5f,
               .Conditions = { { "Speed", static_cast<int>( CompareOp::Greater ), 0.1f } } } );
        const State walk{ .Name = "Walk", .Clip = "Walk", .Transitions = {} };
        OutputMachine( g )->States = { survey, walk };
        return g;
    }
} // namespace

// ANIM-FIX6a-BLEND. The caller's loop at a fixed 1/60 s (AnimationECSSystem under --play): Update, then
// AdvanceTransitions by the same step. The transition must begin on the tick its condition first holds, and
// the target's weight must climb on every one of the 30 ticks a 0.5 s blend spans, never jump to 1.
TEST( AnimGraph, AHalfSecondBlendClimbsOverThirtyTicksFromTheTickItsConditionHolds )
{
    constexpr float kStep  = 1.0f / 60.0f;
    constexpr int   kSetOn = 5;
    Evaluator       eval( SurveyWalkGraph() );

    float lastWeight = 0.0f;
    int   firedOn    = -1;
    for ( int tick = 1; tick <= kSetOn + 28; ++tick ) // the blend's first 29 ticks
    {
        if ( tick == kSetOn )
            ASSERT_TRUE( eval.SetFloat( "Speed", 0.3f ) );
        const auto res = eval.Update( 0.0f );
        if ( res.Changed )
        {
            ASSERT_EQ( firedOn, -1 ) << "the transition fired twice";
            firedOn = tick;
        }
        if ( tick < kSetOn )
        {
            EXPECT_EQ( res.Current->Name, "Survey" ) << "tick " << tick;
            EXPECT_FALSE( eval.EnteringTransition().has_value() );
            eval.AdvanceTransitions( kStep );
            continue;
        }
        EXPECT_EQ( res.Current->Name, "Walk" ) << "the state changed late, tick " << tick;

        // What a clip arriving on this tick joins: the transition at its own elapsed time.
        const auto entering = eval.EnteringTransition();
        if ( !entering.has_value() )
        {
            FAIL() << "nothing is fading into Walk on tick " << tick;
        }
        EXPECT_FLOAT_EQ( entering->Duration, 0.5f );
        EXPECT_NEAR( entering->Elapsed, static_cast<float>( tick - kSetOn ) * kStep, 1e-5f ) << "tick " << tick;

        eval.AdvanceTransitions( kStep );
        const auto layers = eval.ActiveStateWeights();
        ASSERT_EQ( layers.size(), 2U ) << "the blend ended early, tick " << tick;
        EXPECT_EQ( layers[0].Of->Name, "Survey" );
        EXPECT_EQ( layers[1].Of->Name, "Walk" );
        EXPECT_GT( layers[1].Weight, lastWeight ) << "the weight did not climb on tick " << tick;
        EXPECT_LT( layers[1].Weight, 1.0f ) << "the blend jumped to its target on tick " << tick;
        EXPECT_NEAR( layers[1].Weight, static_cast<float>( tick - kSetOn + 1 ) * kStep / 0.5f, 1e-4f );
        lastWeight = layers[1].Weight;
    }
    EXPECT_EQ( firedOn, kSetOn ) << "the transition did not begin on the tick its condition first held";

    // Tick 30 of the blend (or 31: the float sum of thirty 1/60 s may fall a hair short of 0.5) retires it.
    for ( int extra = 0; extra < 2 && eval.EnteringTransition().has_value(); ++extra )
    {
        static_cast<void>( eval.Update( 0.0f ) );
        eval.AdvanceTransitions( kStep );
    }
    EXPECT_FALSE( eval.EnteringTransition().has_value() ) << "the 0.5 s blend outlived 31 ticks of 1/60 s";
    const auto settled = eval.ActiveStateWeights();
    ASSERT_EQ( settled.size(), 1U );
    EXPECT_EQ( settled[0].Of->Name, "Walk" );
}

TEST( AnimGraph, StartsAtEntryState )
{
    Evaluator eval( LocomotionGraph() );
    ASSERT_NE( eval.CurrentState(), nullptr );
    EXPECT_EQ( eval.CurrentState()->Name, "Idle" );
}

TEST( AnimGraph, EntryFallsBackToFirstStateWhenUnset )
{
    AnimGraph g = LocomotionGraph();
    OutputMachine( g )->Entry.clear();
    Evaluator eval( g );
    ASSERT_NE( eval.CurrentState(), nullptr );
    EXPECT_EQ( eval.CurrentState()->Name, "Idle" ); // first state
}

TEST( AnimGraph, TransitionsWhenConditionMet )
{
    Evaluator eval( LocomotionGraph() );

    // Speed below threshold: no transition.
    ASSERT_TRUE( eval.SetFloat( "Speed", 0.0f ).IsSuccess() );
    auto r0 = eval.Update( 0.0f );
    EXPECT_FALSE( r0.Changed );
    EXPECT_EQ( r0.Current->Name, "Idle" );

    // Speed above threshold: Idle -> Run, with the transition's blend duration.
    ASSERT_TRUE( eval.SetFloat( "Speed", 1.0f ).IsSuccess() );
    auto r1 = eval.Update( 0.0f );
    EXPECT_TRUE( r1.Changed );
    ASSERT_NE( r1.Current, nullptr );
    EXPECT_EQ( r1.Current->Name, "Run" );
    EXPECT_FLOAT_EQ( r1.Blend, 0.25f );

    // Back to idle when speed drops.
    ASSERT_TRUE( eval.SetFloat( "Speed", 0.0f ).IsSuccess() );
    auto r2 = eval.Update( 0.0f );
    EXPECT_TRUE( r2.Changed );
    EXPECT_EQ( r2.Current->Name, "Idle" );
}

TEST( AnimGraph, ExitTimeGatesTransition )
{
    AnimGraph g               = ::Desert::Animation::Graph::MakeStateMachineGraph();
    OutputMachine( g )->Entry = "A";
    State a;
    a.Name = "A";
    a.Clip = "a";
    // Unconditional transition, but only after the clip reaches 80%.
    a.Transitions.push_back( { "B", 0.1f, true, 0.8f, {} } );
    State b;
    b.Name   = "B";
    b.Clip   = "b";
    OutputMachine( g )->States = { a, b };

    Evaluator eval( g );
    EXPECT_FALSE( eval.Update( 0.5f ).Changed ); // before exit time -> hold
    auto r = eval.Update( 0.9f );                // past exit time -> fire
    EXPECT_TRUE( r.Changed );
    EXPECT_EQ( r.Current->Name, "B" );
}

TEST( AnimGraph, DanglingAndSelfTargetsIgnored )
{
    AnimGraph g               = ::Desert::Animation::Graph::MakeStateMachineGraph();
    OutputMachine( g )->Entry = "Only";
    State s;
    s.Name = "Only";
    s.Clip = "c";
    s.Transitions.push_back( { "DoesNotExist", 0.2f, false, 1.0f, {} } ); // dangling
    s.Transitions.push_back( { "Only", 0.2f, false, 1.0f, {} } );         // self
    OutputMachine( g )->States = { s };

    Evaluator eval( g );
    auto      r = eval.Update( 1.0f );
    EXPECT_FALSE( r.Changed );
    EXPECT_EQ( r.Current->Name, "Only" );
}

TEST( AnimGraph, SyncGraphPreservesStateAndParams )
{
    Evaluator eval( LocomotionGraph() );
    ASSERT_TRUE( eval.SetFloat( "Speed", 1.0f ).IsSuccess() );
    ASSERT_EQ( eval.Update( 0.0f ).Current->Name, "Run" ); // now in Run, Speed = 1

    // Edit the graph (bump a blend duration) and re-sync: the active state + live param must survive.
    AnimGraph edited                      = LocomotionGraph();
    OutputMachine( edited )->States[0].Transitions[0].Blend = 0.9f;
    eval.SyncGraph( edited );

    EXPECT_EQ( eval.CurrentState()->Name, "Run" );     // preserved by name
    EXPECT_FLOAT_EQ( eval.GetFloat( "Speed" ), 1.0f ); // live value preserved

    // Removing the active state re-enters at the entry.
    AnimGraph idleOnly               = ::Desert::Animation::Graph::MakeStateMachineGraph();
    OutputMachine( idleOnly )->Entry = "Idle";
    State idle;
    idle.Name       = "Idle";
    idle.Clip       = "idle_clip";
    OutputMachine( idleOnly )->States = { idle };
    eval.SyncGraph( idleOnly );
    EXPECT_EQ( eval.CurrentState()->Name, "Idle" );
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}

// ОТКУДА ПРИШЛИ — и почему это утверждается парой, а не одним «имя правильное».
//
// Заголовок панели показывает переход как «откуда → куда NN%», и без второго имени он утверждает
// больше, чем знает: пока кроссфейд идёт, поза на экране есть СМЕСЬ двух состояний, а надпись
// называет одно. Второе имя может дать только тот, кто помнит, где переход сработал.
//
// Обе половины нужны. «Называет Idle после перехода» проходит и на реализации, которая просто держит
// первое состояние графа: в локомоционном графе вход и есть Idle. Поэтому здесь ДВА перехода подряд,
// и второй обязан сдвинуть память — иначе это не память, а константа.
TEST( AnimGraph, PreviousStateNamesWhereTheTransitionCameFrom )
{
    Evaluator eval( LocomotionGraph() );

    // ДО первого перехода предыдущего нет — и это не «то же, что текущее». Состояние «переход ещё не
    // случался» обязано быть отличимо, иначе панель нарисует «Idle -> Idle 0%» на нетронутом графе.
    EXPECT_EQ( eval.PreviousState(), nullptr );

    ASSERT_TRUE( eval.SetFloat( "Speed", 1.0f ).IsSuccess() );
    ASSERT_EQ( eval.Update( 0.0f ).Current->Name, "Run" );
    ASSERT_NE( eval.PreviousState(), nullptr );
    EXPECT_EQ( eval.PreviousState()->Name, "Idle" );

    // Второй переход: память обязана СДВИНУТЬСЯ. Это и есть положительный контроль — реализация,
    // возвращающая вход графа, здесь покраснеет.
    ASSERT_TRUE( eval.SetFloat( "Speed", 0.0f ).IsSuccess() );
    ASSERT_EQ( eval.Update( 0.0f ).Current->Name, "Idle" );
    ASSERT_NE( eval.PreviousState(), nullptr );
    EXPECT_EQ( eval.PreviousState()->Name, "Run" );
}

// ── THE POSE GRAPH (ANIM-I12): a state machine is ONE node, evaluated from Output Pose back ─────────────

namespace PG = Desert::Animation::Graph;

// The shape every ANGR 1 file became: one StateMachine node, wired into Output Pose, and it is the plan.
TEST( PoseGraph, TheMigratedShapeIsOneMachineAtTheOutput )
{
    const AnimGraph graph = PG::MakeStateMachineGraph( "Hero" );
    ASSERT_EQ( graph.Nodes.size(), 1u );
    EXPECT_EQ( graph.OutputPose, PG::kDefaultStateMachineNode );
    ASSERT_NE( PG::OutputMachine( graph ), nullptr );

    const auto plan = PG::PlanPoseGraph( graph );
    ASSERT_TRUE( plan.IsSuccess() ) << plan.GetError();
    EXPECT_EQ( plan.GetValue(), std::vector<int>{ 0 } );
}

TEST( PoseGraph, NothingAtOutputPoseIsRefusedByName )
{
    AnimGraph graph  = PG::MakeStateMachineGraph( "Hero" );
    graph.OutputPose = "Locomotion";
    const auto plan  = PG::PlanPoseGraph( graph );
    ASSERT_FALSE( plan.IsSuccess() );
    EXPECT_NE( plan.GetError().find( "'Locomotion'" ), std::string::npos ) << plan.GetError();

    graph.OutputPose.clear();
    EXPECT_FALSE( PG::PlanPoseGraph( graph ).IsSuccess() );
}

// A wire into a pin the kind does not have, a duplicate node name, and a parameter pin the kind does not
// have are each refused, naming the node.
TEST( PoseGraph, AWireTheKindHasNoPinForIsRefused )
{
    AnimGraph graph = PG::MakeStateMachineGraph( "Hero" );
    graph.Nodes[0].PoseInputs.emplace_back( "Elsewhere" );
    const auto plan = PG::PlanPoseGraph( graph );
    ASSERT_FALSE( plan.IsSuccess() );
    EXPECT_NE( plan.GetError().find( "'StateMachine'" ), std::string::npos ) << plan.GetError();
}

TEST( PoseGraph, TwoNodesOfOneNameAreRefused )
{
    AnimGraph graph = PG::MakeStateMachineGraph( "Hero" );
    graph.Nodes.push_back( graph.Nodes[0] );
    const auto plan = PG::PlanPoseGraph( graph );
    ASSERT_FALSE( plan.IsSuccess() );
    EXPECT_NE( plan.GetError().find( "two pose nodes" ), std::string::npos ) << plan.GetError();
}

TEST( PoseGraph, AParameterPinTheKindLacksIsRefused )
{
    AnimGraph graph = PG::MakeStateMachineGraph( "Hero" );
    graph.Parameters.push_back( Parameter{ "Speed", static_cast<int>( ParamType::Float ), 0.0f } );
    graph.Nodes[0].ParameterInputs.push_back( PG::ParameterPin{ "Weight", "Speed" } );
    const auto plan = PG::PlanPoseGraph( graph );
    ASSERT_FALSE( plan.IsSuccess() );
    EXPECT_NE( plan.GetError().find( "'Weight'" ), std::string::npos ) << plan.GetError();
}

// A machine nothing wires to Output Pose is authored but not evaluated: it is not in the plan, its
// transitions never fire, and the evaluator's answers are the output machine's.
TEST( PoseGraph, AnUnwiredMachineIsNotEvaluated )
{
    AnimGraph graph = PG::MakeStateMachineGraph( "Hero" );
    State     idle;
    idle.Name                          = "Idle";
    PG::OutputMachine( graph )->States = { idle };

    PG::PoseNode spare = graph.Nodes[0];
    spare.Name         = "Spare";
    State a;
    a.Name = "A";
    State b;
    b.Name = "B";
    Transition always;
    always.To = "B";
    a.Transitions.push_back( always );
    if ( !spare.Machine )
    {
        FAIL() << "a state-machine graph's node carries its machine";
    }
    spare.Machine->States = { a, b };
    graph.Nodes.push_back( spare );

    const auto plan = PG::PlanPoseGraph( graph );
    ASSERT_TRUE( plan.IsSuccess() ) << plan.GetError();
    EXPECT_EQ( plan.GetValue(), std::vector<int>{ 0 } );

    Evaluator  eval( graph );
    const auto result = eval.Update( 1.0f );
    ASSERT_NE( result.Current, nullptr );
    EXPECT_EQ( result.Current->Name, "Idle" );
    ASSERT_NE( eval.CurrentState( "Spare" ), nullptr );
    EXPECT_EQ( eval.CurrentState( "Spare" )->Name, "A" ); // its unconditional transition never ran
}

// An unplannable graph updates nothing and says why, once, in the structure error.
TEST( PoseGraph, AnUnplannableGraphUpdatesNothingAndSaysWhy )
{
    AnimGraph graph  = LocomotionGraph();
    graph.OutputPose = "Gone";
    Evaluator eval( graph );
    EXPECT_NE( eval.GetStructureError().find( "'Gone'" ), std::string::npos ) << eval.GetStructureError();
    EXPECT_EQ( eval.Update( 0.0f ).Current, nullptr );
}

// ── The pose graph evaluates node by node (ANIM-I13b): any composition plans, a loop is refused by name ──
namespace
{
    PG::PoseNode SequenceNode( std::string name, std::string clip )
    {
        PG::PoseNode node;
        node.Name     = std::move( name );
        node.Kind     = static_cast<int>( PG::PoseNodeKind::SequencePlayer );
        node.Sequence = PG::SequencePlayerNode{ .Clip = std::move( clip ), .Loop = true };
        return node;
    }

    PG::PoseNode LayeredNode( std::string name, std::string base, std::string layer )
    {
        PG::PoseNode node;
        node.Name         = std::move( name );
        node.Kind         = static_cast<int>( PG::PoseNodeKind::LayeredBlendPerBone );
        node.PoseInputs   = { std::move( base ), std::move( layer ) };
        node.LayeredBlend = PG::LayeredBlendPerBoneNode{};
        node.LayeredBlend->Layers.push_back( PG::LayerSetup{ { PG::BranchFilter{ "spine", 0 } } } );
        return node;
    }

    PG::PoseNode AdditiveNode( std::string name, std::string base, std::string additive )
    {
        PG::PoseNode node;
        node.Name       = std::move( name );
        node.Kind       = static_cast<int>( PG::PoseNodeKind::ApplyAdditive );
        node.PoseInputs = { std::move( base ), std::move( additive ) };
        return node;
    }
} // namespace

TEST( PoseGraph, ABlendOfBlendsAndAnAdditiveOverItArePlannedAndPlayed )
{
    AnimGraph graph = PG::MakeStateMachineGraph( "Hero" );
    State     idle;
    idle.Name                          = "Idle";
    PG::OutputMachine( graph )->States = { idle };
    graph.Nodes.push_back( SequenceNode( "Aim", "aim" ) );
    graph.Nodes.push_back( SequenceNode( "Wave", "wave" ) );
    graph.Nodes.push_back( SequenceNode( "Breathe", "breathe" ) );
    graph.Nodes.push_back( LayeredNode( "Inner", std::string( PG::kDefaultStateMachineNode ), "Aim" ) );
    graph.Nodes.push_back( LayeredNode( "Outer", "Inner", "Wave" ) ); // a blend whose base is a blend
    graph.Nodes.push_back( AdditiveNode( "Add", "Outer", "Breathe" ) );
    graph.OutputPose = "Add";

    const auto plan = PG::PlanPoseGraph( graph );
    ASSERT_TRUE( plan.IsSuccess() ) << plan.GetError();
    ASSERT_EQ( plan.GetValue().size(), 7U );
    EXPECT_EQ( plan.GetValue().back(), 6 ) << "Output Pose's node is evaluated last";

    const auto position = [&]( int node )
    { return std::find( plan.GetValue().begin(), plan.GetValue().end(), node ) - plan.GetValue().begin(); };
    EXPECT_LT( position( 4 ), position( 5 ) ) << "the inner blend has to be evaluated before the blend reading it";

    Evaluator eval( graph );
    EXPECT_TRUE( eval.GetStructureError().empty() )
         << "the evaluator still refuses a composition: " << eval.GetStructureError();
    ASSERT_NE( PG::BaseSourceNode( graph ), nullptr );
    EXPECT_EQ( PG::BaseSourceNode( graph )->Name, PG::kDefaultStateMachineNode )
         << "the base chain runs down pin 0 of the additive and of both blends";
    ASSERT_NE( eval.Update( 0.0f ).Current, nullptr );
}

TEST( PoseGraph, ALoopThroughBlendNodesIsRefusedNamingItsNodes )
{
    AnimGraph graph = PG::MakeStateMachineGraph( "Hero" );
    graph.Nodes.push_back( LayeredNode( "Upper", "Plus", std::string( PG::kDefaultStateMachineNode ) ) );
    graph.Nodes.push_back( AdditiveNode( "Plus", "Upper", std::string( PG::kDefaultStateMachineNode ) ) );
    graph.OutputPose = "Upper";

    const auto plan = PG::PlanPoseGraph( graph );
    ASSERT_FALSE( plan.IsSuccess() ) << "a pose graph with a loop has no first node";
    EXPECT_NE( plan.GetError().find( "Upper" ), std::string::npos ) << plan.GetError();
    EXPECT_NE( plan.GetError().find( "Plus" ), std::string::npos ) << plan.GetError();
    EXPECT_NE( plan.GetError().find( "->" ), std::string::npos ) << plan.GetError();
    EXPECT_FALSE( Evaluator( graph ).GetStructureError().empty() );
}

TEST( PoseGraph, ASequencePlayerWithNoClipAndAnAdditiveMissingAWireAreRefusedByName )
{
    AnimGraph noClip = PG::MakeStateMachineGraph( "Hero" );
    noClip.Nodes.push_back( SequenceNode( "Silent", "" ) );
    noClip.OutputPose = "Silent";
    const auto silent = PG::PlanPoseGraph( noClip );
    ASSERT_FALSE( silent.IsSuccess() );
    EXPECT_NE( silent.GetError().find( "Silent" ), std::string::npos ) << silent.GetError();

    AnimGraph    oneWire = PG::MakeStateMachineGraph( "Hero" );
    PG::PoseNode add     = AdditiveNode( "Add", std::string( PG::kDefaultStateMachineNode ), "x" );
    add.PoseInputs.pop_back();
    oneWire.Nodes.push_back( add );
    oneWire.OutputPose = "Add";
    const auto missing = PG::PlanPoseGraph( oneWire );
    ASSERT_FALSE( missing.IsSuccess() );
    EXPECT_NE( missing.GetError().find( "'Add' (ApplyAdditive) has 2 Pose pin(s) and 1 wire(s)" ),
               std::string::npos )
         << missing.GetError();
}

// ── Linked anim layers (UE Anim Layer Interface): the graph-side rules, refused by name ─────────────────
namespace
{
    PG::PoseNode KindNode( std::string name, PG::PoseNodeKind kind, std::vector<std::string> inputs = {} )
    {
        PG::PoseNode node;
        node.Name       = std::move( name );
        node.Kind       = static_cast<int>( kind );
        node.PoseInputs = std::move( inputs );
        if ( kind == PG::PoseNodeKind::SequencePlayer )
            node.Sequence = PG::SequencePlayerNode{ .Clip = "clip", .Loop = true };
        return node;
    }

    /// Output Pose = LinkedAnimLayer "Call" (Weapon.UpperBody) over the SequencePlayer "Walk".
    AnimGraph CallingGraph()
    {
        AnimGraph graph;
        graph.Name                 = "Hero";
        graph.Nodes                = { KindNode( "Walk", PG::PoseNodeKind::SequencePlayer ),
                                       KindNode( "Call", PG::PoseNodeKind::LinkedAnimLayer, { "Walk" } ) };
        graph.Nodes[1].LinkedLayer = PG::LinkedAnimLayerNode{ "Weapon", "UpperBody" };
        graph.OutputPose           = "Call";
        graph.Layers = PG::AnimGraphLayers{ { PG::AnimLayerInterface{ "Weapon", { "UpperBody", "Hands" } } }, {} };
        return graph;
    }

    PG::AnimLayerGraph PassLayer( std::string layer )
    {
        return PG::AnimLayerGraph{
             "Weapon", std::move( layer ), { KindNode( "In", PG::PoseNodeKind::LinkedInputPose ) }, "In" };
    }
} // namespace

TEST( LinkedAnimLayer, ACallPlansAndAnUnknownInterfaceOrLayerIsRefusedByName )
{
    AnimGraph  graph = CallingGraph();
    const auto plan  = PG::PlanPoseGraph( graph );
    ASSERT_TRUE( plan.IsSuccess() ) << plan.GetError();
    EXPECT_EQ( plan.GetValue(), ( std::vector<int>{ 0, 1 } ) );
    EXPECT_EQ( PG::BaseSourceNode( graph ), graph.Nodes.data() ) << "the base chain runs through the call's input";

    PG::PoseNode& call = graph.Nodes[1];
    if ( !call.LinkedLayer )
    {
        FAIL() << "the calling graph's node 1 is the layer call";
    }
    call.LinkedLayer->Layer = "Legs";
    const auto noLayer      = PG::PlanPoseGraph( graph );
    ASSERT_FALSE( noLayer.IsSuccess() );
    EXPECT_NE( noLayer.GetError().find( "'Legs'" ), std::string::npos ) << noLayer.GetError();
    EXPECT_NE( noLayer.GetError().find( "'UpperBody'" ), std::string::npos )
         << "lists the layers: " << noLayer.GetError();

    graph.Nodes[1].LinkedLayer = PG::LinkedAnimLayerNode{ "Shield", "UpperBody" };
    const auto noInterface     = PG::PlanPoseGraph( graph );
    ASSERT_FALSE( noInterface.IsSuccess() );
    EXPECT_NE( noInterface.GetError().find( "'Shield'" ), std::string::npos ) << noInterface.GetError();
    EXPECT_NE( noInterface.GetError().find( "'Weapon'" ), std::string::npos ) << noInterface.GetError();

    graph.Nodes[1].LinkedLayer.reset();
    EXPECT_FALSE( PG::PlanPoseGraph( graph ).IsSuccess() ) << "a LinkedAnimLayer node naming no layer";
}

TEST( LinkedAnimLayer, AnImplementationIsWholeAndAnInputPoseLivesOnlyInALayerGraph )
{
    AnimGraph graph = CallingGraph();
    if ( !graph.Layers )
    {
        FAIL() << "the calling graph declares its layer interface";
    }
    PG::AnimGraphLayers& layers = *graph.Layers;
    layers.Implemented.push_back( PassLayer( "UpperBody" ) );
    const auto half = PG::PlanPoseGraph( graph );
    ASSERT_FALSE( half.IsSuccess() );
    EXPECT_NE( half.GetError().find( "'Hands'" ), std::string::npos ) << half.GetError();

    layers.Implemented.push_back( PassLayer( "Hands" ) );
    ASSERT_TRUE( PG::PlanPoseGraph( graph ).IsSuccess() ) << PG::PlanPoseGraph( graph ).GetError();

    layers.Implemented.push_back( PassLayer( "Tail" ) );
    const auto unknown = PG::PlanPoseGraph( graph );
    ASSERT_FALSE( unknown.IsSuccess() );
    EXPECT_NE( unknown.GetError().find( "'Tail'" ), std::string::npos ) << unknown.GetError();
    layers.Implemented.pop_back();

    layers.Implemented[0].Nodes.push_back( KindNode( "Nested", PG::PoseNodeKind::LinkedAnimLayer, { "In" } ) );
    layers.Implemented[0].Nodes.back().LinkedLayer = PG::LinkedAnimLayerNode{ "Weapon", "Hands" };
    layers.Implemented[0].OutputPose               = "Nested";
    const auto nestedPlan                          = PG::PlanPoseGraph( graph );
    EXPECT_TRUE( nestedPlan.IsSuccess() ) << "a layer graph may call another layer: " << nestedPlan.GetError();

    // UpperBody calls Hands and Hands calls UpperBody: a cycle within the graph, refused with its path.
    layers.Implemented[1].Nodes.push_back( KindNode( "Back", PG::PoseNodeKind::LinkedAnimLayer, { "In" } ) );
    layers.Implemented[1].Nodes.back().LinkedLayer = PG::LinkedAnimLayerNode{ "Weapon", "UpperBody" };
    layers.Implemented[1].OutputPose               = "Back";
    const auto cycle                               = PG::PlanPoseGraph( graph );
    ASSERT_FALSE( cycle.IsSuccess() );
    EXPECT_NE( cycle.GetError().find( "Weapon.UpperBody -> Weapon.Hands -> Weapon.UpperBody" ), std::string::npos )
         << cycle.GetError();
    layers.Implemented[1] = PassLayer( "Hands" );
    layers.Implemented[0].Nodes.pop_back();
    layers.Implemented[0].OutputPose = "In";

    AnimGraph host = CallingGraph();
    host.Nodes.push_back( KindNode( "In", PG::PoseNodeKind::LinkedInputPose ) );
    host.OutputPose    = "In";
    const auto outside = PG::PlanPoseGraph( host );
    ASSERT_FALSE( outside.IsSuccess() );
    EXPECT_NE( outside.GetError().find( "LinkedInputPose" ), std::string::npos ) << outside.GetError();
}
