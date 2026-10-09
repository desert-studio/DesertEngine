// GP2c: the engine's default Humanoid locomotion graph (Editor/Resources/Engine/Meshes/Skinned/
// Humanoid_Locomotion.danimgraph), fed the parameters CharacterMovement::PublishAnimGraphParameters writes:
// Speed (cm/s) is the axis of the Locomotion state's Idle / Walk / Run blend space, IsFalling enters Jump and
// landing leaves it.
#include <Engine/Animation/Graph/AnimGraph.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace Desert::Animation::Graph;

namespace
{
    const char* const kGraphPath = "Editor/Resources/Engine/Meshes/Skinned/Humanoid_Locomotion.danimgraph";

    std::string ReadGraphText()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            std::ifstream in( prefix + kGraphPath, std::ios::binary );
            if ( in )
            {
                std::ostringstream text;
                text << in.rdbuf();
                return text.str();
            }
            prefix += "../";
        }
        return {};
    }

    // Ticks the machine at 60 Hz until every cross-fade has retired, then returns the state holding the
    // whole weight ("" if the blend never settled on one state).
    std::string SettledState( Evaluator& evaluator )
    {
        for ( int tick = 0; tick < 240; ++tick )
        {
            evaluator.Update( 0.0f );
            evaluator.AdvanceTransitions( 1.0f / 60.0f );
        }
        const auto weights = evaluator.ActiveStateWeights();
        if ( weights.size() != 1 || weights[0].Of == nullptr || weights[0].Weight < 0.999f )
            return {};
        return weights[0].Of->Name;
    }

    AnimGraph LoadLocomotionGraph()
    {
        const std::string text = ReadGraphText();
        EXPECT_FALSE( text.empty() ) << kGraphPath << " not found from the working directory";
        auto graph = Deserialize( text );
        EXPECT_TRUE( graph.IsSuccess() ) << "Deserialize refused " << kGraphPath;
        return graph.IsSuccess() ? graph.GetValue() : AnimGraph{};
    }
} // namespace

// GP2f: the ground is ONE state playing a Blend Space 1D over Speed (UE's locomotion Blend Space Player), not
// three clip states switched at thresholds: red if the file went back to Idle / Walk / Run states, or if its row
// stopped planning (a sample out of order, the axis not the Float Speed).
TEST( HumanoidLocomotionGraph, TheGroundIsOneLocomotionBlendSpaceOverSpeed )
{
    const AnimGraph graph = LoadLocomotionGraph();
    const auto      plan  = PlanPoseGraph( graph );
    ASSERT_TRUE( plan.IsSuccess() ) << plan.GetError();

    const StateMachine* machine = OutputMachine( graph );
    ASSERT_NE( machine, nullptr );
    EXPECT_EQ( machine->Entry, "Locomotion" );
    const auto ground = std::find_if( machine->States.begin(), machine->States.end(),
                                      []( const State& s ) { return s.Name == "Locomotion"; } );
    ASSERT_NE( ground, machine->States.end() );
    EXPECT_TRUE( ground->Clip.empty() );
    ASSERT_TRUE( ground->BlendSpace.has_value() );
    EXPECT_EQ( ground->BlendSpace->Axis, "Speed" );
    const std::vector<BlendSample>& row = ground->BlendSpace->Space.Samples;
    ASSERT_EQ( row.size(), 3u );
    EXPECT_EQ( row[0].Clip, "Idle" );
    EXPECT_EQ( row[0].Value, 0.0F );
    EXPECT_EQ( row[1].Clip, "Walk" );
    EXPECT_EQ( row[1].Value, 300.0F );
    EXPECT_EQ( row[2].Clip, "Run" );
    EXPECT_EQ( row[2].Value, 600.0F );
    EXPECT_TRUE( ground->BlendSpace->Space.Loop );
    EXPECT_EQ( machine->States.size(), 2u ) << "Locomotion and Jump only";
}

// Red if any speed switched states: the blend space is what changes with Speed, the machine stays put.
TEST( HumanoidLocomotionGraph, EverySpeedSettlesOnLocomotion )
{
    Evaluator evaluator( LoadLocomotionGraph() );
    ASSERT_TRUE( evaluator.GetStructureError().empty() ) << evaluator.GetStructureError();

    for ( const float speed : { 0.0f, 300.0f, 600.0f, 150.0f, 0.0f } )
    {
        ASSERT_TRUE( evaluator.SetFloat( "Speed", speed ).IsSuccess() );
        EXPECT_EQ( SettledState( evaluator ), "Locomotion" ) << "at " << speed << " cm/s";
    }
}

TEST( HumanoidLocomotionGraph, FallingEntersJumpAtEverySpeedAndLandingReturnsToLocomotion )
{
    Evaluator evaluator( LoadLocomotionGraph() );
    ASSERT_TRUE( evaluator.GetStructureError().empty() ) << evaluator.GetStructureError();

    for ( const float speed : { 0.0f, 300.0f, 600.0f } )
    {
        ASSERT_TRUE( evaluator.SetBool( "IsFalling", false ).IsSuccess() );
        ASSERT_TRUE( evaluator.SetFloat( "Speed", speed ).IsSuccess() );
        ASSERT_EQ( SettledState( evaluator ), "Locomotion" );

        ASSERT_TRUE( evaluator.SetBool( "IsFalling", true ).IsSuccess() );
        EXPECT_EQ( SettledState( evaluator ), "Jump" ) << "falling at " << speed << " cm/s";

        ASSERT_TRUE( evaluator.SetBool( "IsFalling", false ).IsSuccess() );
        EXPECT_EQ( SettledState( evaluator ), "Locomotion" ) << "landing at " << speed << " cm/s";
    }
}

// GP2d: a Blend Space 1D node survives the .danimgraph writer and the strict reader field for field (ANGR 4: the
// payload is optional, so the corpus reads unchanged).
TEST( HumanoidLocomotionGraph, ABlendSpace1DNodeRoundTripsThroughTheDanimgraphText )
{
    AnimGraph graph = LoadLocomotionGraph();
    PoseNode  node;
    node.Name            = "SpeedBlend";
    node.Kind            = static_cast<int>( PoseNodeKind::BlendSpace1D );
    node.ParameterInputs = { ParameterPin{ std::string( kBlendSpaceAxisPin ), "Speed" } };
    node.BlendSpace      = BlendSpace1DNode{
              .Samples = { BlendSample{ "Idle", 0.0F }, BlendSample{ "Walk", 300.0F }, BlendSample{ "Run", 600.0F } },
              .WeightSpeed = 4.0F,
              .Loop        = false };
    node.X = 40.0F;
    graph.Nodes.push_back( node );

    const auto back = Deserialize( Serialize( graph ) );
    ASSERT_TRUE( back.IsSuccess() ) << back.GetError();
    const PoseNode* read = FindNode( back.GetValue(), "SpeedBlend" );
    ASSERT_NE( read, nullptr );
    EXPECT_EQ( read->Kind, node.Kind );
    ASSERT_TRUE( read->BlendSpace.has_value() );
    ASSERT_EQ( read->BlendSpace->Samples.size(), 3u );
    for ( size_t s = 0; s < 3; ++s )
    {
        EXPECT_EQ( read->BlendSpace->Samples[s].Clip, node.BlendSpace->Samples[s].Clip );
        EXPECT_EQ( read->BlendSpace->Samples[s].Value, node.BlendSpace->Samples[s].Value );
    }
    EXPECT_EQ( read->BlendSpace->WeightSpeed, 4.0F );
    EXPECT_FALSE( read->BlendSpace->Loop );
    ASSERT_EQ( read->ParameterInputs.size(), 1u );
    EXPECT_EQ( read->ParameterInputs[0].Parameter, "Speed" );
}
