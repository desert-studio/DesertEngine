// ANIM-UI: the pose graph's edits as the AnimGraph panel makes them (PoseGraphEdit, no ImGui). Every
// command must build a node of the kind it names, a wire it accepts must leave a graph the ENGINE plans
// (PlanPoseGraph), and what it refuses — a Linked Input Pose in the graph's own AnimGraph, a loop — it
// refuses with the reason. Plus the census: the panel reaches these through the unit and the ⚠ strip
// carries the engine's plan refusal.
#include <Editor/Panels/Animation/PoseGraphEdit.hpp>

#include <Engine/Animation/Graph/AnimGraph.hpp>
#include <Engine/Animation/Graph/AnimGraphValidation.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <string>
#include <tuple>

namespace G  = Desert::Animation::Graph;
namespace EG = Desert::Editor::Graph;

namespace
{
    std::string Add( G::AnimGraph& graph, G::PoseNodeKind kind, G::GraphScope scope = G::GraphScope::Host )
    {
        const auto added = EG::AddPoseNode( graph, graph.Nodes, kind, scope, 0.0f, 0.0f, "Walk" );
        EXPECT_TRUE( added.IsSuccess() ) << added.GetError();
        return added.IsSuccess() ? added.GetValue() : std::string();
    }

    // The repository root, found as the sibling suite finds it: walking up from the working directory.
    std::string ReadSource( const char* relative )
    {
        std::filesystem::path prefix = ".";
        for ( int up = 0; up < 6 && !std::filesystem::exists( prefix / relative ); ++up )
            prefix /= "..";
        const std::ifstream in( prefix / relative );
        std::ostringstream  text;
        text << in.rdbuf();
        return text.str();
    }
} // namespace

TEST( PoseGraphEdit, EachAddCommandBuildsANodeOfItsKindWithItsPayloadAndPins )
{
    G::AnimGraph graph = G::MakeStateMachineGraph( "Fox" );
    for ( const G::PoseNodeKind kind : EG::AddableKinds( G::GraphScope::Host ) )
    {
        const std::string  name = Add( graph, kind );
        const G::PoseNode* node = G::FindNode( graph, name );
        ASSERT_NE( node, nullptr );
        EXPECT_EQ( node->Kind, static_cast<int>( kind ) );
        EXPECT_EQ( static_cast<int>( node->PoseInputs.size() ), G::PoseInputCountOf( *node ) );
        EXPECT_EQ( node->Machine.has_value(), kind == G::PoseNodeKind::StateMachine );
        EXPECT_EQ( node->LayeredBlend.has_value(), kind == G::PoseNodeKind::LayeredBlendPerBone );
        EXPECT_EQ( node->Sequence.has_value(), kind == G::PoseNodeKind::SequencePlayer );
        EXPECT_EQ( node->LinkedLayer.has_value(), kind == G::PoseNodeKind::LinkedAnimLayer );
    }
    const std::vector<G::PoseNodeKind> addable = EG::AddableKinds( G::GraphScope::Host );
    EXPECT_EQ( std::count( addable.begin(), addable.end(), G::PoseNodeKind::LinkedInputPose ), 0 );
    // Two of one kind are two names: a wire to a shared name would mean either.
    const std::string a = Add( graph, G::PoseNodeKind::SequencePlayer );
    const std::string b = Add( graph, G::PoseNodeKind::SequencePlayer );
    EXPECT_NE( a, b );
}

TEST( PoseGraphEdit, SequencePlayerIntoLayeredBlendIntoOutputIsAGraphTheEnginePlans )
{
    G::AnimGraph      graph   = G::MakeStateMachineGraph( "Fox" );
    const std::string player  = Add( graph, G::PoseNodeKind::SequencePlayer );
    const std::string blend   = Add( graph, G::PoseNodeKind::LayeredBlendPerBone );
    const std::string machine = std::string( G::kDefaultStateMachineNode );

    ASSERT_TRUE( EG::ConnectPose( graph.Nodes, machine, blend, 0 ).IsSuccess() );
    ASSERT_TRUE( EG::ConnectPose( graph.Nodes, player, blend, 1 ).IsSuccess() );
    ASSERT_TRUE( EG::ConnectOutput( graph.Nodes, graph.OutputPose, blend ).IsSuccess() );

    const auto plan = G::PlanPoseGraph( graph );
    ASSERT_TRUE( plan.IsSuccess() ) << plan.GetError();
    EXPECT_EQ( plan.GetValue().size(), 3u );
    EXPECT_EQ( G::FindNode( graph, blend )->PoseInputs[1], player );

    // And the ⚠ strip is quiet about it: no PoseGraphRefused finding.
    const auto warnings = G::Validate( graph, G::ClipSet{ false, {} } );
    EXPECT_TRUE( std::none_of( warnings.begin(), warnings.end(), []( const G::GraphWarning& w )
                               { return w.Kind == G::WarningKind::PoseGraphRefused; } ) );
}

TEST( PoseGraphEdit, AnUnwiredPinIsNamedInTheStripByTheEnginesOwnPlan )
{
    G::AnimGraph      graph = G::MakeStateMachineGraph( "Fox" );
    const std::string blend = Add( graph, G::PoseNodeKind::LayeredBlendPerBone );
    ASSERT_TRUE( EG::ConnectOutput( graph.Nodes, graph.OutputPose, blend ).IsSuccess() );
    const auto warnings = G::Validate( graph, G::ClipSet{ false, {} } );
    const auto refused  = std::find_if( warnings.begin(), warnings.end(), []( const G::GraphWarning& w )
                                        { return w.Kind == G::WarningKind::PoseGraphRefused; } );
    ASSERT_NE( refused, warnings.end() );
    EXPECT_EQ( refused->Text, G::PlanPoseGraph( graph ).GetError() );
}

TEST( PoseGraphEdit, ALinkedInputPoseIsRefusedInTheHostGraphAndAddedInALayerGraph )
{
    G::AnimGraph graph = G::MakeStateMachineGraph( "Fox" );
    const auto   host = EG::AddPoseNode( graph, graph.Nodes, G::PoseNodeKind::LinkedInputPose, G::GraphScope::Host,
                                         0.0f, 0.0f, "" );
    ASSERT_FALSE( host.IsSuccess() );
    EXPECT_NE( host.GetError().find( "layer graph" ), std::string::npos ) << host.GetError();
    EXPECT_EQ( graph.Nodes.size(), 1u ) << "a refused add must leave the graph as it was";

    std::vector<G::PoseNode> layerNodes;
    const auto layer = EG::AddPoseNode( graph, layerNodes, G::PoseNodeKind::LinkedInputPose, G::GraphScope::Layer,
                                        0.0f, 0.0f, "" );
    ASSERT_TRUE( layer.IsSuccess() ) << layer.GetError();
    EXPECT_EQ( layerNodes.front().Kind, static_cast<int>( G::PoseNodeKind::LinkedInputPose ) );
}

TEST( PoseGraphEdit, AWireThatClosesALoopIsRefusedAndNamesTheLoop )
{
    G::AnimGraph      graph = G::MakeStateMachineGraph( "Fox" );
    const std::string a     = Add( graph, G::PoseNodeKind::ApplyAdditive );
    const std::string b     = Add( graph, G::PoseNodeKind::LayeredBlendPerBone );
    ASSERT_TRUE( EG::ConnectPose( graph.Nodes, a, b, 0 ).IsSuccess() );

    const auto loop = EG::ConnectPose( graph.Nodes, b, a, 0 );
    ASSERT_FALSE( loop.IsSuccess() );
    EXPECT_NE( loop.GetError().find( "cycle" ), std::string::npos ) << loop.GetError();
    EXPECT_NE( loop.GetError().find( std::format( "{0} -> {1} -> {0}", a, b ) ), std::string::npos )
         << loop.GetError();
    EXPECT_TRUE( G::FindNode( graph, a )->PoseInputs[0].empty() ) << "a refused wire must not be made";

    EXPECT_FALSE( EG::ConnectPose( graph.Nodes, a, a, 1 ).IsSuccess() ) << "a node wired into itself";
    EXPECT_FALSE( EG::ConnectPose( graph.Nodes, a, b, 7 ).IsSuccess() ) << "a pin the node does not have";
}

TEST( PoseGraphEdit, TheQuestionAnswersAsTheEditRefusesAndWritesNothing )
{
    // THE RELATION, not either function: the Wire actions are CanConnectPose's answers and the drop is
    // ConnectPose's, so the two must refuse the same wires with the same words -- and the question must
    // leave the graph as it found it.
    G::AnimGraph      graph = G::MakeStateMachineGraph( "Fox" );
    const std::string a     = Add( graph, G::PoseNodeKind::ApplyAdditive );
    const std::string b     = Add( graph, G::PoseNodeKind::LayeredBlendPerBone );
    ASSERT_TRUE( EG::ConnectPose( graph.Nodes, a, b, 0 ).IsSuccess() );

    const std::string before = G::Serialize( graph );
    for ( const auto& [from, to, pin] : { std::tuple{ b, a, 0 }, std::tuple{ a, a, 1 }, std::tuple{ a, b, 7 },
                                          std::tuple{ std::string( "nobody" ), a, 0 }, std::tuple{ b, a, 1 } } )
    {
        SCOPED_TRACE( std::format( "{} -> {} pin {}", from, to, pin ) );
        const auto asked = EG::CanConnectPose( graph.Nodes, from, to, pin );
        EXPECT_EQ( G::Serialize( graph ), before ) << "asking wrote into the graph";
        std::vector<G::PoseNode> trial = graph.Nodes;
        const auto               done  = EG::ConnectPose( trial, from, to, pin );
        ASSERT_EQ( asked.IsSuccess(), done.IsSuccess() );
        if ( !asked.IsSuccess() )
            EXPECT_EQ( asked.GetError(), done.GetError() );
    }
}

TEST( PoseGraphEdit, DeletingANodeTakesItsWiresAndTheOutputWithIt )
{
    G::AnimGraph      graph  = G::MakeStateMachineGraph( "Fox" );
    const std::string player = Add( graph, G::PoseNodeKind::SequencePlayer );
    const std::string add    = Add( graph, G::PoseNodeKind::ApplyAdditive );
    ASSERT_TRUE( EG::ConnectPose( graph.Nodes, player, add, 1 ).IsSuccess() );
    ASSERT_TRUE( EG::ConnectOutput( graph.Nodes, graph.OutputPose, player ).IsSuccess() );

    ASSERT_TRUE( EG::RemovePoseNode( graph.Nodes, graph.OutputPose, player ).IsSuccess() );
    EXPECT_EQ( G::FindNode( graph, player ), nullptr );
    EXPECT_TRUE( G::FindNode( graph, add )->PoseInputs[1].empty() );
    EXPECT_TRUE( graph.OutputPose.empty() );
    EXPECT_FALSE( EG::RemovePoseNode( graph.Nodes, graph.OutputPose, player ).IsSuccess() );
}

TEST( PoseGraphEdit, LayersGrowThePinsAndARenameCarriesTheWires )
{
    G::AnimGraph graph = G::MakeStateMachineGraph( "Fox" );
    graph.Parameters.push_back( { "UpperBody", static_cast<int>( G::ParamType::Float ), 1.0f } );
    const std::string blend = Add( graph, G::PoseNodeKind::LayeredBlendPerBone );
    G::PoseNode*      node  = G::FindNode( graph, blend );
    EG::SetLayerCount( *node, 2 );
    EXPECT_EQ( node->PoseInputs.size(), 3u );
    ASSERT_TRUE( EG::BindParameterPin( graph, *node, G::LayerWeightPin( 1 ), "UpperBody" ).IsSuccess() );
    EXPECT_FALSE( EG::BindParameterPin( graph, *node, G::LayerWeightPin( 1 ), "Nope" ).IsSuccess() );
    EG::SetLayerCount( *node, 1 );
    EXPECT_EQ( node->PoseInputs.size(), 2u );
    EXPECT_TRUE( EG::BoundParameter( *node, G::LayerWeightPin( 1 ) ).empty() )
         << "a removed layer's binding stays";

    ASSERT_TRUE( EG::ConnectOutput( graph.Nodes, graph.OutputPose, blend ).IsSuccess() );
    const int         index = static_cast<int>( node - graph.Nodes.data() );
    const std::string given = EG::RenamePoseNode( graph.Nodes, graph.OutputPose, index, "UpperBlend" );
    EXPECT_EQ( given, "UpperBlend" );
    EXPECT_EQ( graph.OutputPose, "UpperBlend" );
}

TEST( PoseGraphEdit, TheCanvasPlansOneNodePerPoseNodeAndTheOutputSinkLast )
{
    G::AnimGraph      graph  = G::MakeStateMachineGraph( "Fox" );
    const std::string player = Add( graph, G::PoseNodeKind::SequencePlayer );
    const std::string blend  = Add( graph, G::PoseNodeKind::LayeredBlendPerBone );
    ASSERT_TRUE( EG::ConnectPose( graph.Nodes, player, blend, 1 ).IsSuccess() );
    ASSERT_TRUE( EG::ConnectOutput( graph.Nodes, graph.OutputPose, blend ).IsSuccess() );

    EG::ElementIdMap          ids;
    const EG::PoseGraphCanvas canvas = EG::PlanPoseCanvas( graph.Nodes, graph.OutputPose, ids );
    ASSERT_EQ( canvas.Plan.Nodes.size(), graph.Nodes.size() + 1 );
    EXPECT_EQ( EG::PoseNodeOf( canvas, canvas.Plan.Nodes.back().Id ), EG::kOutputSink );
    ASSERT_EQ( canvas.Plan.Links.size(), 2u );
    const EG::PosePinRef in = EG::PinOf( canvas, canvas.Plan.Links.front().ToPin );
    EXPECT_EQ( graph.Nodes[static_cast<size_t>( in.Node )].Name, blend );
    EXPECT_EQ( in.Pin, 1 );
}

TEST( PoseGraphEdit, ThePanelEditsThePoseGraphThroughTheUnit )
{
    const std::string source = ReadSource( "Editor/Source/Editor/Panels/Animation/AnimGraphPanelPoseGraph.cpp" );
    ASSERT_FALSE( source.empty() );
    EXPECT_NE( source.find( "Graph::AddPoseNode( graph, target.Nodes, kind, target.Scope" ), std::string::npos );
    EXPECT_NE( source.find( "Graph::CanConnectPose( nodes, name, into.Name, pin ).IsSuccess()" ),
               std::string::npos )
         << "the Wire actions are not the unit's own answer to \"would this wire be accepted\"";
    EXPECT_NE( source.find( "Graph::ConnectPose( trial, from" ), std::string::npos )
         << "the canvas drag does not ask the unit before it wires";
    EXPECT_NE( source.find( "ed::ShowBackgroundContextMenu()" ), std::string::npos );
    EXPECT_NE( source.find( "\"Add {}\", Graph::PoseNodeTitle( kind )" ), std::string::npos );
}
