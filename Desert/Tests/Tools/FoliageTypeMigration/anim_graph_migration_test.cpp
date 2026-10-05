// TAIL-ANIM: ANGR 2 -> 3. The Output Pose node became a node of the canvas with its position in the file
// (OutputPoseX/Y, UE's Root NodePosX/Y). The step places it where the v2 editor drew it - one column right of
// the rightmost node, level with the node wired into it - keeps everything else and the GUID, and what it writes
// is what the engine reads.

#include <SceneMigration.hpp>

#include <Engine/Animation/Graph/AnimGraph.hpp>
#include <Engine/Assets/TextAssetHeaderStamp.hpp>

#include <gtest/gtest.h>

#include <regex>
#include <string>

namespace
{
    namespace G = Desert::Animation::Graph;

    // An ANGR 2 file: the current writer's text with the v3 fields taken out and the generation stated as 2.
    std::string AsV2( const G::AnimGraph& graph )
    {
        std::string text = G::Serialize( graph );
        text = std::regex_replace( text, std::regex( R"(,\s*"OutputPose[XY]"\s*:\s*[-0-9.eE+]+)" ), "" );
        text = std::regex_replace( text, std::regex( R"("ANGR"\s*:\s*3)" ), "\"ANGR\": 2" );
        return text;
    }
} // namespace

TEST( AnimGraphMigration, AV2GraphGetsItsOutputPoseRightOfTheRightmostNodeLevelWithTheWiredOne )
{
    G::AnimGraph graph = G::MakeStateMachineGraph( "Fox" );
    graph.Nodes[0].X   = 520.0f;
    graph.Nodes[0].Y   = 150.0f;
    const auto minted  = G::Deserialize( G::Serialize( graph ) ); // the header minted, as on disk
    ASSERT_TRUE( minted.IsSuccess() ) << minted.GetError();
    graph = minted.GetValue();
    ASSERT_TRUE( graph.Header );
    const std::string v2 = AsV2( graph );
    ASSERT_EQ( v2.find( "OutputPoseX" ), std::string::npos );
    ASSERT_NE( v2.find( "\"ANGR\": 2" ), std::string::npos );
    ASSERT_FALSE( G::Deserialize( v2 ).IsSuccess() ) << "the engine reads ANGR 3 only";

    const auto raised = Desert::Migration::MigrateAnimGraphV2ToV3( v2 );
    ASSERT_TRUE( raised.IsSuccess() ) << raised.GetError();
    const auto read = G::Deserialize( raised.GetValue() );
    ASSERT_TRUE( read.IsSuccess() ) << read.GetError();
    const G::AnimGraph& now = read.GetValue();
    EXPECT_FLOAT_EQ( now.OutputPoseX, 520.0f + G::kOutputPoseSpacingX );
    EXPECT_FLOAT_EQ( now.OutputPoseY, 150.0f );
    EXPECT_EQ( now.OutputPose, "StateMachine" );
    ASSERT_TRUE( now.Header );
    EXPECT_EQ( now.Header->Guid, graph.Header->Guid ) << "the graph's identity survives the raise";
    EXPECT_EQ( now.Header->Versions.at( "ANGR" ), Desert::Assets::kAnimGraphSchemaVersion );
}

TEST( AnimGraphMigration, AFileAlreadyAtV3IsRefusedByTheStep )
{
    const std::string v3     = G::Serialize( G::MakeStateMachineGraph( "Fox" ) );
    const auto        raised = Desert::Migration::MigrateAnimGraphV2ToV3( v3 );
    EXPECT_FALSE( raised.IsSuccess() );
}

TEST( AnimGraphMigration, ANewGraphPlacesItsOutputPoseOneColumnRightOfTheMachine )
{
    const G::AnimGraph graph = G::MakeStateMachineGraph( "Fox" );
    EXPECT_FLOAT_EQ( graph.OutputPoseX, G::kOutputPoseSpacingX );
    EXPECT_FLOAT_EQ( graph.OutputPoseY, 0.0f );
}
