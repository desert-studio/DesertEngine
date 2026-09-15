// T3.1 — WHAT IT TAKES FOR A SCRIPT TO DRIVE A STATE MACHINE, AND THE TWO THINGS THAT WERE SILENT.
//
// Before this task `Evaluator::SetFloat`/`SetBool` were reachable from exactly two places, both of them the
// editor panel's live-value sliders, so nothing in Play mode or in a packaged game could move a parameter at
// all. Handing that power to Lua is one file; making it SAFE to hand over is this suite, because the two
// ways it goes wrong both used to produce no diagnostic whatsoever:
//
//   1. A NAME THAT DOES NOT EXIST. `m_Params[name] = value` created the parameter, held the value, and was
//      read by no condition. A typo was a parameter that worked.
//   2. THE MIRROR, ON THE AUTHORING SIDE. A CONDITION on a misspelled parameter reads 0.0 through the
//      tolerant GetFloat and compares against it: permanently false, never fires, nothing in the log.
//
// The suite also pins the ORDER the one-frame latency rests on. That claim is written in a comment in
// AnimationECSSystem, and a comment is not the code: what makes it true is the order two OTHER files
// register their systems in, so those files are read here as text. If somebody reorders them the latency
// changes and the comment becomes a lie — this is what says so.

#include <Engine/Animation/Graph/AnimGraph.hpp>

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>

using Desert::Animation::Graph::AnimGraph;
using Desert::Animation::Graph::CompareOp;
using Desert::Animation::Graph::DeclaredParameterList;
using Desert::Animation::Graph::Evaluator;
using Desert::Animation::Graph::ParamType;
using Desert::Animation::Graph::State;
using Desert::Animation::Graph::TypeName;

namespace
{
    // One state machine with one parameter of each declared type, so a type refusal has somewhere to land.
    // Idle --(Go is true)--> Moving, and nothing else: no exit time, so the ONLY thing that can move this
    // machine is a parameter write. That is the property the scene witness rests on too.
    AnimGraph ScriptedGraph()
    {
        AnimGraph g;
        g.Name  = "Scripted";
        g.Entry = "Idle";
        g.Parameters.push_back( { "Go", static_cast<int>( ParamType::Bool ), 0.0F } );
        g.Parameters.push_back( { "Weapon", static_cast<int>( ParamType::Int ), 0.0F } );
        g.Parameters.push_back( { "Speed", static_cast<int>( ParamType::Float ), 0.0F } );

        State idle;
        idle.Name = "Idle";
        idle.Clip = "idle_clip";
        idle.Transitions.push_back(
             { "Moving", 0.2F, false, 1.0F, { { "Go", static_cast<int>( CompareOp::IsTrue ), 0.0F } } } );

        State moving;
        moving.Name = "Moving";
        moving.Clip = "moving_clip";

        g.States = { idle, moving };
        return g;
    }

    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            const std::ifstream probe( prefix + "Editor/Source/EditorLayer.cpp" );
            if ( probe )
            {
                return prefix;
            }
            prefix += "../";
        }
        return {};
    }

    std::string ReadFile( const std::string& path )
    {
        const std::ifstream in( path, std::ios::binary );
        if ( !in )
        {
            return {};
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }
} // namespace

// ---------------------------------------------------------------- 1. the name

TEST( AnimGraphScript, AParameterTheGraphDoesNotDeclareIsRefusedAndTheRefusalListsWhatExists )
{
    Evaluator eval( ScriptedGraph() );

    const auto refused = eval.SetFloat( "Sped", 1.0F );
    ASSERT_FALSE( refused.IsSuccess() );

    // The name that was wrong, AND the ones that are right: "no such parameter" alone sends the reader to
    // open the graph, which is the one thing the message could have saved them.
    EXPECT_NE( refused.GetError().find( "'Sped'" ), std::string::npos ) << refused.GetError();
    EXPECT_NE( refused.GetError().find( "'Speed'" ), std::string::npos ) << refused.GetError();
    EXPECT_NE( refused.GetError().find( "'Go'" ), std::string::npos ) << refused.GetError();
    EXPECT_NE( refused.GetError().find( "Scripted" ), std::string::npos ) << refused.GetError();

    // AND NOTHING WAS STORED. This is the actual defect: the old setter created the parameter, so a later
    // read found it and the typo looked like it had worked.
    EXPECT_FLOAT_EQ( eval.GetFloat( "Sped" ), 0.0F );
}

TEST( AnimGraphScript, AGraphWithNoParametersSaysSoRatherThanListingNothing )
{
    AnimGraph bare;
    bare.Name  = "Bare";
    bare.Entry = "Only";
    State only;
    only.Name   = "Only";
    only.Clip   = "c";
    bare.States = { only };

    Evaluator  eval( bare );
    const auto refused = eval.SetFloat( "Speed", 1.0F );
    ASSERT_FALSE( refused.IsSuccess() );
    EXPECT_NE( refused.GetError().find( "none at all" ), std::string::npos ) << refused.GetError();
    EXPECT_EQ( DeclaredParameterList( bare ), "none at all" );
}

// ---------------------------------------------------------------- 2. the type

TEST( AnimGraphScript, TheGRAPHDecidesTheTypeAndASetterThatDisagreesIsRefused )
{
    Evaluator eval( ScriptedGraph() );

    // Each setter on its own declared type: accepted.
    EXPECT_TRUE( eval.SetBool( "Go", true ).IsSuccess() );
    EXPECT_TRUE( eval.SetInt( "Weapon", 2 ).IsSuccess() );
    EXPECT_TRUE( eval.SetFloat( "Speed", 3.5F ).IsSuccess() );

    // Every other pairing: refused, naming the declared type. A store that accepted them would be a
    // SECOND answer to "what is this parameter" — the graph already gave the first.
    const auto boolAsFloat = eval.SetFloat( "Go", 1.0F );
    EXPECT_FALSE( boolAsFloat.IsSuccess() );
    EXPECT_NE( boolAsFloat.GetError().find( "Bool" ), std::string::npos ) << boolAsFloat.GetError();

    EXPECT_FALSE( eval.SetBool( "Speed", true ).IsSuccess() );
    EXPECT_FALSE( eval.SetInt( "Speed", 1 ).IsSuccess() );
    EXPECT_FALSE( eval.SetFloat( "Weapon", 2.0F ).IsSuccess() );
    EXPECT_FALSE( eval.SetBool( "Weapon", true ).IsSuccess() );
    EXPECT_FALSE( eval.SetInt( "Go", 1 ).IsSuccess() );

    // A refused write changes NOTHING — the accepted values above are still what is stored.
    EXPECT_FLOAT_EQ( eval.GetFloat( "Go" ), 1.0F );
    EXPECT_FLOAT_EQ( eval.GetFloat( "Weapon" ), 2.0F );
    EXPECT_FLOAT_EQ( eval.GetFloat( "Speed" ), 3.5F );
}

TEST( AnimGraphScript, EveryDeclaredTypeHasAName )
{
    // These names go into refusals an artist is meant to act on; "declares it as 2" is unreadable.
    for ( const auto type : { ParamType::Bool, ParamType::Int, ParamType::Float } )
    {
        EXPECT_STRNE( TypeName( type ), "?" );
    }
}

// ---------------------------------------------------------------- 3. the mirror: conditions

TEST( AnimGraphScript, AConditionOnAnUndeclaredParameterIsReportedOnceRatherThanReadingZeroForever )
{
    AnimGraph broken                                        = ScriptedGraph();
    broken.States[0].Transitions[0].Conditions[0].Parameter = "Gone"; // the typo, on the AUTHORING side

    Evaluator eval( broken );
    ASSERT_FALSE( eval.GetStructureError().empty() );
    EXPECT_NE( eval.GetStructureError().find( "Gone" ), std::string::npos ) << eval.GetStructureError();
    EXPECT_NE( eval.GetStructureError().find( "Idle" ), std::string::npos ) << eval.GetStructureError();

    // And the behaviour it explains: the transition can never fire, because the condition reads 0.
    EXPECT_FALSE( eval.Update( 1.0F ).Changed );

    // A healthy graph says nothing at all — a structure report that is never empty is noise.
    const Evaluator healthy( ScriptedGraph() );
    EXPECT_TRUE( healthy.GetStructureError().empty() ) << healthy.GetStructureError();
}

TEST( AnimGraphScript, TheStructureVerdictIsRecomputedWhenTheGraphIsReplaced )
{
    Evaluator eval( ScriptedGraph() );
    ASSERT_TRUE( eval.GetStructureError().empty() );

    // The editor edits a live graph through SyncGraph; a verdict computed once at construction would go on
    // describing a graph that no longer exists — in both directions.
    AnimGraph broken                                        = ScriptedGraph();
    broken.States[0].Transitions[0].Conditions[0].Parameter = "Gone";
    eval.SyncGraph( broken );
    EXPECT_FALSE( eval.GetStructureError().empty() );

    eval.SyncGraph( ScriptedGraph() );
    EXPECT_TRUE( eval.GetStructureError().empty() ) << eval.GetStructureError();
}

// ---------------------------------------------------------------- 4. the whole point

TEST( AnimGraphScript, AParameterWriteIsTheONLYThingThatCanMoveThisMachine )
{
    Evaluator eval( ScriptedGraph() );

    // No exit time and no other transition: ticking forever changes nothing. This is what "the graph could
    // only sit in its entry state" meant before T3.1, and it is the state a packaged game shipped in.
    for ( int frame = 0; frame < 120; ++frame )
    {
        EXPECT_FALSE( eval.Update( static_cast<float>( frame ) / 120.0F ).Changed );
    }
    ASSERT_NE( eval.CurrentState(), nullptr );
    EXPECT_EQ( eval.CurrentState()->Name, "Idle" );

    ASSERT_TRUE( eval.SetBool( "Go", true ).IsSuccess() );

    const auto fired = eval.Update( 0.0F );
    EXPECT_TRUE( fired.Changed );
    ASSERT_NE( fired.Current, nullptr );
    EXPECT_EQ( fired.Current->Name, "Moving" );
    EXPECT_FLOAT_EQ( fired.Blend, 0.2F );
}

TEST( AnimGraphScript, AWriteActsOnTheVeryNEXTTickAndNotTheOneAfterIt )
{
    // The half of the ordering decision that lives in this class: a value stored before Update is seen BY
    // that Update. AnimationECSSystem drains the script's queue immediately before calling it for exactly
    // this reason — so the latency is one frame and not two.
    Evaluator eval( ScriptedGraph() );
    ASSERT_TRUE( eval.SetBool( "Go", true ).IsSuccess() );
    EXPECT_TRUE( eval.Update( 0.0F ).Changed );
}

// ---------------------------------------------------------------- 5. the order the latency rests on

TEST( AnimGraphScript, TheSystemOrderTheOneFrameLatencyIsDocumentedAgainstStillHolds )
{
    ASSERT_FALSE( RepoRoot().empty() ) << "could not locate the repository root from the working directory";

    // Both hosts that build a gameplay scene. A claim about frame ordering that is true in the editor and
    // false in the packaged runtime is worse than no claim.
    for ( const char* host : { "Editor/Source/EditorLayer.cpp", "Runtime/Source/RuntimeLayer.cpp" } )
    {
        SCOPED_TRACE( host );
        const std::string code = ReadFile( RepoRoot() + host );
        ASSERT_FALSE( code.empty() ) << "could not read " << host;

        const std::size_t animation  = code.find( "AddSystem<ECS::AnimationECSSystem>" );
        const std::size_t attachment = code.find( "AddSystem<ECS::AttachmentSystem>" );
        const std::size_t script     = code.find( "AddSystem<ECS::ScriptSystem>" );
        const std::size_t physics    = code.find( "AddSystem<ECS::PhysicsECSSystem>" );

        ASSERT_NE( animation, std::string::npos );
        ASSERT_NE( attachment, std::string::npos );
        ASSERT_NE( script, std::string::npos );
        ASSERT_NE( physics, std::string::npos );

        // THE LATENCY. Scripts run after animation, so a parameter set in OnUpdate on frame N is drained at
        // the top of frame N+1. One frame, stated in AnimationECSSystem::DrainGraphParams.
        EXPECT_LT( animation, script ) << "AnimationECSSystem no longer runs before ScriptSystem, so the "
                                          "one-frame parameter latency documented in "
                                          "AnimationECSSystem::DrainGraphParams is no longer what happens.";

        // THE TWO REASONS IT WAS NOT REORDERED INSTEAD. Both are other features' same-frame guarantees, and
        // if either stops being true the decision above should be revisited rather than inherited.
        EXPECT_LT( animation, attachment ) << "AttachmentSystem must follow animation to place a socket on "
                                              "THIS frame's pose.";
        EXPECT_LT( script, physics ) << "ScriptSystem must precede physics so move intent executes on THIS "
                                        "frame.";
    }
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
