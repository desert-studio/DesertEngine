// ANIM-UI2: every edit of an anim graph is ONE entry of the editor's CommandHistory (UE: FScopedTransaction on
// the AnimBlueprint). An edit then Undo leaves the graph's stored bytes as they were; a drag of many frames is
// one entry; a move the history made itself is not recorded as an edit.
#include <Editor/Core/Commands/AnimGraphEdit.hpp>
#include <Editor/Panels/Animation/AnimGraphCanvasPlan.hpp>
#include <Editor/Panels/Animation/PoseGraphEdit.hpp>

#include <Engine/Animation/Graph/AnimGraph.hpp>

#include <gtest/gtest.h>

namespace G  = Desert::Animation::Graph;
namespace EG = Desert::Editor::Graph;
using Desert::Editor::AnimGraphEditTransaction;
using Desert::Editor::AnimGraphOwner;
using Desert::Editor::CommandHistory;

namespace
{
    struct Fixture
    {
        G::AnimGraph graph;
        uint32_t     revision = 1;

        Fixture()
        {
            CommandHistory::Get().Clear();
            // A graph the loader accepts (one player at Output Pose), round-tripped so it carries a header as a
            // loaded file does.
            G::AnimGraph seed;
            const auto   added = EG::AddPoseNode( seed, seed.Nodes, G::PoseNodeKind::SequencePlayer, G::GraphScope::Host,
                                                  0.0f, 0.0f, "Idle" );
            EXPECT_TRUE( added.IsSuccess() );
            EXPECT_TRUE( EG::ConnectOutput( seed.Nodes, seed.OutputPose, added.GetValue() ).IsSuccess() );
            const auto loaded = G::Deserialize( G::Serialize( seed ) );
            EXPECT_TRUE( loaded.IsSuccess() ) << loaded.GetError();
            graph = loaded.IsSuccess() ? loaded.GetValue() : seed;
            EXPECT_TRUE( graph.Header.has_value() );
        }
        AnimGraphOwner Owner()
        {
            AnimGraphOwner owner;
            owner.Identity     = &graph;
            owner.Name         = "Test";
            owner.Volatile     = false;
            owner.Resolve      = [this] { return &graph; };
            owner.AfterRestore = [this] { ++revision; };
            return owner;
        }
    };
} // namespace

TEST( AnimGraphUndo, AnAddedNodeUndoesToTheBytesItHadBefore )
{
    Fixture                  f;
    AnimGraphEditTransaction tx;
    const std::string        before = G::Serialize( f.graph );
    {
        AnimGraphEditTransaction::Scope scope( tx, f.Owner() );
        ASSERT_TRUE( EG::AddPoseNode( f.graph, f.graph.Nodes, G::PoseNodeKind::SequencePlayer, G::GraphScope::Host,
                                      0.0f, 0.0f, "Walk" )
                          .IsSuccess() );
    }
    ASSERT_EQ( CommandHistory::Get().UndoStack().size(), 1u );
    const std::string after = G::Serialize( f.graph );
    ASSERT_NE( before, after );
    ASSERT_TRUE( CommandHistory::Get().Undo() );
    EXPECT_EQ( G::Serialize( f.graph ), before );
    ASSERT_TRUE( CommandHistory::Get().Redo() );
    EXPECT_EQ( G::Serialize( f.graph ), after );
}

TEST( AnimGraphUndo, AScopeThatChangedNothingPushesNothing )
{
    Fixture                  f;
    AnimGraphEditTransaction tx;
    {
        AnimGraphEditTransaction::Scope scope( tx, f.Owner() );
    }
    EXPECT_TRUE( CommandHistory::Get().UndoStack().empty() );
}

TEST( AnimGraphUndo, ADragOfManyFramesIsOneEntry )
{
    Fixture f;
    ASSERT_TRUE( EG::AddPoseNode( f.graph, f.graph.Nodes, G::PoseNodeKind::SequencePlayer, G::GraphScope::Host, 0.0f,
                                  0.0f, "Walk" )
                      .IsSuccess() );
    AnimGraphEditTransaction tx;
    EXPECT_EQ( tx.Observe( f.Owner(), f.revision, false ), 0u ); // the baseline
    const std::string before = G::Serialize( f.graph );
    for ( int frame = 0; frame < 40; ++frame )
    {
        f.graph.Nodes.back().X += 5.0f; // the node editor writes the position every frame of the drag
        ++f.revision;
        EXPECT_EQ( tx.Observe( f.Owner(), f.revision, true ), 0u );
    }
    EXPECT_EQ( tx.Observe( f.Owner(), f.revision, false ), 1u ); // the release closes ONE entry
    EXPECT_EQ( CommandHistory::Get().UndoStack().size(), 1u );
    ASSERT_TRUE( CommandHistory::Get().Undo() );
    EXPECT_EQ( G::Serialize( f.graph ), before );
}

TEST( AnimGraphUndo, AKeyEditInASettledFrameIsOneEntryAndAnUndoIsNotRecordedAsAnEdit )
{
    Fixture f;
    ASSERT_TRUE( EG::AddPoseNode( f.graph, f.graph.Nodes, G::PoseNodeKind::SequencePlayer, G::GraphScope::Host, 0.0f,
                                  0.0f, "Walk" )
                      .IsSuccess() );
    AnimGraphEditTransaction tx;
    (void)tx.Observe( f.Owner(), f.revision, false );
    const std::string before = G::Serialize( f.graph );
    const std::string name   = f.graph.Nodes.back().Name;
    ASSERT_TRUE( EG::RemovePoseNode( f.graph.Nodes, f.graph.OutputPose, name ).IsSuccess() ); // Delete key
    ++f.revision;
    EXPECT_EQ( tx.Observe( f.Owner(), f.revision, false ), 1u );
    ASSERT_TRUE( CommandHistory::Get().Undo() );
    EXPECT_EQ( G::Serialize( f.graph ), before );
    // The restore bumped the revision; the next frame must follow it, not push it (that would kill Redo).
    EXPECT_EQ( tx.Observe( f.Owner(), f.revision, false ), 0u );
    EXPECT_EQ( CommandHistory::Get().RedoStack().size(), 1u );
}

TEST( AnimGraphUndo, ParametersStatesAndSettingsAreOneEntryEach )
{
    Fixture                  f;
    AnimGraphEditTransaction tx;
    f.graph.Nodes.push_back( G::PoseNode{ .Name = "Machine", .Machine = G::StateMachine{} } );
    f.graph.OutputPose = "Machine";
    const std::string before = G::Serialize( f.graph );
    {
        AnimGraphEditTransaction::Scope scope( tx, f.Owner() );
        f.graph.Parameters.push_back( G::Parameter{ .Name = "Speed" } );
    }
    {
        AnimGraphEditTransaction::Scope scope( tx, f.Owner() );
        G::State state;
        state.Name = EG::MakeUniqueStateName( f.graph.Nodes.back().Machine->States, "State", -1 );
        f.graph.Nodes.back().Machine->States.push_back( state );
    }
    EXPECT_EQ( CommandHistory::Get().UndoStack().size(), 2u );
    ASSERT_TRUE( CommandHistory::Get().Undo() );
    ASSERT_TRUE( CommandHistory::Get().Undo() );
    EXPECT_EQ( G::Serialize( f.graph ), before );
}

TEST( AnimGraphUndo, ANestedMachineIsPlannedOnItsOwnStates )
{
    G::StateMachine machine;
    machine.States.push_back( G::State{ .Name = "A" } );
    machine.States.push_back( G::State{ .Name = "B" } );
    EG::ElementIdMap ids;
    const auto       canvas = EG::PlanStateMachine( machine.States, ids );
    EXPECT_EQ( canvas.StateNodes.size(), 2u );
    EXPECT_NE( EG::MakeUniqueStateName( machine.States, "A", -1 ), "A" );
    EXPECT_EQ( EG::MakeUniqueStateName( machine.States, "C", -1 ), "C" );
}
