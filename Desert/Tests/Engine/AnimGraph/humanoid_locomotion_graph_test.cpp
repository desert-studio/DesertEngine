// GP2c: the engine's default Humanoid locomotion graph (Editor/Resources/Engine/Meshes/Skinned/
// Humanoid_Locomotion.danimgraph), fed the parameters CharacterMovement::PublishAnimGraphParameters writes:
// Speed (cm/s) picks Idle / Walk / Run, IsFalling enters Jump and landing leaves it.
#include <Engine/Animation/Graph/AnimGraph.hpp>

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>

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

TEST( HumanoidLocomotionGraph, SpeedZeroThreeHundredSixHundredSettleOnIdleWalkRun )
{
    Evaluator evaluator( LoadLocomotionGraph() );
    ASSERT_TRUE( evaluator.GetStructureError().empty() ) << evaluator.GetStructureError();

    ASSERT_TRUE( evaluator.SetFloat( "Speed", 0.0f ).IsSuccess() );
    EXPECT_EQ( SettledState( evaluator ), "Idle" );

    ASSERT_TRUE( evaluator.SetFloat( "Speed", 300.0f ).IsSuccess() );
    EXPECT_EQ( SettledState( evaluator ), "Walk" );

    ASSERT_TRUE( evaluator.SetFloat( "Speed", 600.0f ).IsSuccess() );
    EXPECT_EQ( SettledState( evaluator ), "Run" );

    ASSERT_TRUE( evaluator.SetFloat( "Speed", 300.0f ).IsSuccess() );
    EXPECT_EQ( SettledState( evaluator ), "Walk" ) << "slowing down must leave Run";

    ASSERT_TRUE( evaluator.SetFloat( "Speed", 0.0f ).IsSuccess() );
    EXPECT_EQ( SettledState( evaluator ), "Idle" );
}

TEST( HumanoidLocomotionGraph, FallingEntersJumpFromEveryGroundStateAndLandingLeavesIt )
{
    Evaluator evaluator( LoadLocomotionGraph() );
    ASSERT_TRUE( evaluator.GetStructureError().empty() ) << evaluator.GetStructureError();

    for ( const float speed : { 0.0f, 300.0f, 600.0f } )
    {
        ASSERT_TRUE( evaluator.SetBool( "IsFalling", false ).IsSuccess() );
        ASSERT_TRUE( evaluator.SetFloat( "Speed", speed ).IsSuccess() );
        const std::string ground = SettledState( evaluator );
        ASSERT_FALSE( ground.empty() );

        ASSERT_TRUE( evaluator.SetBool( "IsFalling", true ).IsSuccess() );
        EXPECT_EQ( SettledState( evaluator ), "Jump" ) << "falling out of " << ground;

        ASSERT_TRUE( evaluator.SetBool( "IsFalling", false ).IsSuccess() );
        EXPECT_EQ( SettledState( evaluator ), speed > 10.0f ? ( speed > 450.0f ? "Run" : "Walk" ) : "Idle" )
             << "landing at " << speed << " cm/s";
    }
}
