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
        AnimGraph g = ::Desert::Animation::Graph::MakeStateMachineGraph();
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
    AnimGraph g = ::Desert::Animation::Graph::MakeStateMachineGraph();
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
    AnimGraph g = ::Desert::Animation::Graph::MakeStateMachineGraph();
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
    AnimGraph idleOnly = ::Desert::Animation::Graph::MakeStateMachineGraph();
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
    graph.Nodes[0].PoseInputs.push_back( "Elsewhere" );
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
    idle.Name = "Idle";
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
    spare.Machine->States = { a, b };
    graph.Nodes.push_back( spare );

    const auto plan = PG::PlanPoseGraph( graph );
    ASSERT_TRUE( plan.IsSuccess() ) << plan.GetError();
    EXPECT_EQ( plan.GetValue(), std::vector<int>{ 0 } );

    Evaluator eval( graph );
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

    AnimGraph oneWire = PG::MakeStateMachineGraph( "Hero" );
    PG::PoseNode add  = AdditiveNode( "Add", std::string( PG::kDefaultStateMachineNode ), "x" );
    add.PoseInputs.pop_back();
    oneWire.Nodes.push_back( add );
    oneWire.OutputPose = "Add";
    const auto missing = PG::PlanPoseGraph( oneWire );
    ASSERT_FALSE( missing.IsSuccess() );
    EXPECT_NE( missing.GetError().find( "'Add' (ApplyAdditive) has 2 Pose pin(s) and 1 wire(s)" ), std::string::npos )
         << missing.GetError();
}
