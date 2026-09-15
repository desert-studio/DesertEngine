#include <Engine/Animation/Graph/AnimGraph.hpp>

#include <gtest/gtest.h>

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
        AnimGraph g;
        g.Name  = "Locomotion";
        g.Entry = "Idle";
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

        g.States = { idle, run };
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
    g.Entry.clear();
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
    AnimGraph g;
    g.Entry = "A";
    State a;
    a.Name = "A";
    a.Clip = "a";
    // Unconditional transition, but only after the clip reaches 80%.
    a.Transitions.push_back( { "B", 0.1f, true, 0.8f, {} } );
    State b;
    b.Name   = "B";
    b.Clip   = "b";
    g.States = { a, b };

    Evaluator eval( g );
    EXPECT_FALSE( eval.Update( 0.5f ).Changed ); // before exit time -> hold
    auto r = eval.Update( 0.9f );                // past exit time -> fire
    EXPECT_TRUE( r.Changed );
    EXPECT_EQ( r.Current->Name, "B" );
}

TEST( AnimGraph, DanglingAndSelfTargetsIgnored )
{
    AnimGraph g;
    g.Entry = "Only";
    State s;
    s.Name = "Only";
    s.Clip = "c";
    s.Transitions.push_back( { "DoesNotExist", 0.2f, false, 1.0f, {} } ); // dangling
    s.Transitions.push_back( { "Only", 0.2f, false, 1.0f, {} } );         // self
    g.States = { s };

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
    edited.States[0].Transitions[0].Blend = 0.9f;
    eval.SyncGraph( edited );

    EXPECT_EQ( eval.CurrentState()->Name, "Run" );     // preserved by name
    EXPECT_FLOAT_EQ( eval.GetFloat( "Speed" ), 1.0f ); // live value preserved

    // Removing the active state re-enters at the entry.
    AnimGraph idleOnly;
    idleOnly.Entry = "Idle";
    State idle;
    idle.Name       = "Idle";
    idle.Clip       = "idle_clip";
    idleOnly.States = { idle };
    eval.SyncGraph( idleOnly );
    EXPECT_EQ( eval.CurrentState()->Name, "Idle" );
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
