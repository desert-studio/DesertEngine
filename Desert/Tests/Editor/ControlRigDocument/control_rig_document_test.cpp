#include <gtest/gtest.h>

#include <Editor/Panels/ControlRig/ControlRigDocumentModel.hpp>

#include <filesystem>
#include <fstream>

// ANIM-FIX11: the .derig document's model — open, edit, undo, save, re-read — without a window.
using namespace Desert;
using Editor::ControlRigDocumentModel;
namespace Serialization = Desert::Assets::Serialization;

namespace
{
    std::filesystem::path WriteRig( const std::string& stem )
    {
        const auto dir = std::filesystem::temp_directory_path() / "ControlRigDocumentTest";
        std::filesystem::create_directories( dir );
        const auto path = dir / ( stem + ".derig" );
        std::ofstream( path ) << R"({
      "Header": { "Kind": "ControlRig", "Guid": "0123456789abcdef0123456789abcdef", "Versions": { "CRIG": 4 }, "Dependencies": [] },
      "TargetSkeleton": { "Guid": "fedcba9876543210fedcba9876543210", "Path": "Meshes/ArmRig.skeleton" },
      "Name": "Doc",
      "Controls": [
        { "Name": "Hand_CTRL", "ShapeName": "CircleXY",
          "Offset": { "Translation": [0.0, 0.0, 0.0], "Rotation": [1.0, 0.0, 0.0, 0.0], "Scale": [1.0, 1.0, 1.0] },
          "Pose":   { "Translation": [0.0, 0.0, 0.0], "Rotation": [1.0, 0.0, 0.0, 0.0], "Scale": [1.0, 1.0, 1.0] },
          "Parents": [ { "Kind": "Component", "Target": "", "Weight": 1.0 } ], "Limits": [] },
        { "Name": "Elbow_CTRL", "ShapeName": "CircleXY",
          "Offset": { "Translation": [0.0, 0.0, 0.0], "Rotation": [1.0, 0.0, 0.0, 0.0], "Scale": [1.0, 1.0, 1.0] },
          "Pose":   { "Translation": [0.0, 0.0, 0.0], "Rotation": [1.0, 0.0, 0.0, 0.0], "Scale": [1.0, 1.0, 1.0] },
          "Parents": [ { "Kind": "Control", "Target": "Hand_CTRL", "Weight": 1.0 } ], "Limits": [] }
      ],
      "Drives": [ { "Control": "Hand_CTRL", "Bone": "Hand" } ],
      "Graphs": []
    })";
        return path;
    }
} // namespace

TEST( ControlRigDocumentTest, OpenEditUndoSaveRereadIsEqual )
{
    Editor::CommandHistory history;
    const auto             path   = WriteRig( "RoundTrip" );
    auto                   opened = ControlRigDocumentModel::Open( path, history );
    ASSERT_TRUE( opened.IsSuccess() ) << opened.GetError();
    auto& model = *opened.GetValue();
    EXPECT_FALSE( model.IsDirty() );

    auto get = model.AddNode( Animation::RigNodeKind::GetControl, "Hand_CTRL", {} );
    auto set = model.AddNode( Animation::RigNodeKind::SetControl, "Elbow_CTRL", {} );
    ASSERT_TRUE( get.IsSuccess() && set.IsSuccess() );
    ASSERT_TRUE( model.Connect( get.GetValue(), "Transform", set.GetValue(), "Transform" ).IsSuccess() );
    EXPECT_TRUE( model.IsDirty() );
    ASSERT_TRUE( model.Validate().IsSuccess() ) << model.Validate().GetError();
    const auto wired = model.GetData();

    // Undo cuts exactly the wire; redo puts it back — the record holds the whole value, not a guess.
    ASSERT_TRUE( history.Undo() );
    ASSERT_FALSE( model.GetData().Graphs.empty() );
    EXPECT_FALSE( model.GetData().Graphs.front().Nodes[1].Inputs[0].Link.has_value() );
    ASSERT_TRUE( history.Redo() );
    EXPECT_EQ( model.GetData(), wired );

    ASSERT_TRUE( model.Save().IsSuccess() );
    EXPECT_FALSE( model.IsDirty() );
    auto reread = ControlRigDocumentModel::Open( path, history );
    ASSERT_TRUE( reread.IsSuccess() ) << reread.GetError();
    EXPECT_EQ( reread.GetValue()->GetData(), wired );

    // Undo back past the save makes the document dirty again: dirty is equality with disk, not a flag.
    ASSERT_TRUE( history.Undo() );
    EXPECT_TRUE( model.IsDirty() );
}

TEST( ControlRigDocumentTest, RenameCarriesEveryReference )
{
    Editor::CommandHistory history;
    auto                   opened = ControlRigDocumentModel::Open( WriteRig( "Rename" ), history );
    ASSERT_TRUE( opened.IsSuccess() ) << opened.GetError();
    auto& model = *opened.GetValue();
    ASSERT_TRUE( model.AddNode( Animation::RigNodeKind::GetControl, "Hand_CTRL", {} ).IsSuccess() );

    auto renamed = model.GetData().Controls[0];
    renamed.Name = "Wrist_CTRL";
    ASSERT_TRUE( model.SetControl( "Hand_CTRL", renamed ).IsSuccess() );
    const auto& data = model.GetData();
    EXPECT_EQ( data.Controls[1].Parents[0].Target, "Wrist_CTRL" );
    EXPECT_EQ( data.Drives[0].Control, "Wrist_CTRL" );
    EXPECT_EQ( data.Graphs.front().Nodes[0].Target, "Wrist_CTRL" );
}

TEST( ControlRigDocumentTest, WiresThatCanNeverBeValidAreRefusedWithoutARecord )
{
    Editor::CommandHistory history;
    auto                   opened = ControlRigDocumentModel::Open( WriteRig( "Refusals" ), history );
    ASSERT_TRUE( opened.IsSuccess() ) << opened.GetError();
    auto& model = *opened.GetValue();
    auto  get   = model.AddNode( Animation::RigNodeKind::GetControl, "Hand_CTRL", {} );
    auto  remap = model.AddNode( Animation::RigNodeKind::RemapFloat, "", {} );
    ASSERT_TRUE( get.IsSuccess() && remap.IsSuccess() );
    const auto before = model.GetRevision();

    EXPECT_FALSE( model.Connect( get.GetValue(), "Transform", remap.GetValue(), "Value" ).IsSuccess() )
         << "a Transform into a Float pin";
    EXPECT_FALSE( model.Connect( remap.GetValue(), "Value", remap.GetValue(), "InMin" ).IsSuccess() )
         << "a node into itself is a loop";
    EXPECT_FALSE( model.AddControl( "Hand_CTRL" ).IsSuccess() ) << "a second control of the same name";
    EXPECT_EQ( model.GetRevision(), before );
}

TEST( ControlRigDocumentTest, PositionsLimitsAndEventsSurviveSaveAndLoadAndUndo )
{
    Editor::CommandHistory history;
    const auto             path   = WriteRig( "Crig4" );
    auto                   opened = ControlRigDocumentModel::Open( path, history );
    ASSERT_TRUE( opened.IsSuccess() ) << opened.GetError();
    auto& model = *opened.GetValue();

    // A node in each of Construction and Forwards: the event switch is a view choice, not a record.
    model.SetEvent( Animation::RigEvent::Construction );
    EXPECT_FALSE( model.IsDirty() );
    auto setup = model.AddNode( Animation::RigNodeKind::SetControl, "Elbow_CTRL", { 10.0f, 20.0f } );
    ASSERT_TRUE( setup.IsSuccess() ) << setup.GetError();
    model.SetEvent( Animation::RigEvent::Forwards );
    EXPECT_EQ( model.GetGraph(), nullptr ) << "Forwards has no node yet; the Construction one leaked across";
    auto solve = model.AddNode( Animation::RigNodeKind::SetControl, "Hand_CTRL", { 300.0f, 40.0f } );
    ASSERT_TRUE( solve.IsSuccess() ) << solve.GetError();

    // A drag is one record; undo puts the node back where it was.
    ASSERT_TRUE( model.MoveNode( solve.GetValue(), { 512.0f, -64.0f } ).IsSuccess() );
    ASSERT_NE( model.GetGraph(), nullptr );
    EXPECT_FLOAT_EQ( model.GetGraph()->Nodes[0].Position.X, 512.0f );
    ASSERT_TRUE( history.Undo() );
    EXPECT_FLOAT_EQ( model.GetGraph()->Nodes[0].Position.X, 300.0f );
    ASSERT_TRUE( history.Redo() );

    ASSERT_TRUE(
         model.SetLimits( "Hand_CTRL", { Serialization::ControlLimitData{ "TX", -1.0f, 1.0f } } ).IsSuccess() );
    EXPECT_FALSE( model.SetLimits( "Hand_CTRL", { { "TX", 2.0f, 1.0f } } ).IsSuccess() ) << "Min above Max";
    EXPECT_FALSE( model.SetLimits( "Hand_CTRL", { { "TW", 0.0f, 1.0f } } ).IsSuccess() ) << "no such channel";
    EXPECT_FALSE( model.SetLimits( "Hand_CTRL", { { "TX", 0.0f, 1.0f }, { "TX", 0.0f, 2.0f } } ).IsSuccess() )
         << "one channel limited twice";

    ASSERT_TRUE( model.Validate().IsSuccess() ) << model.Validate().GetError();
    const auto authored = model.GetData();
    ASSERT_TRUE( model.Save().IsSuccess() );
    auto reread = ControlRigDocumentModel::Open( path, history );
    ASSERT_TRUE( reread.IsSuccess() ) << reread.GetError();
    EXPECT_EQ( reread.GetValue()->GetData(), authored )
         << "positions / limits / events changed on the way to disk";
    const auto* construction = Serialization::FindRigGraph( reread.GetValue()->GetData(), "Construction" );
    ASSERT_NE( construction, nullptr );
    EXPECT_FLOAT_EQ( construction->Nodes[0].Position.Y, 20.0f );
    EXPECT_EQ( reread.GetValue()->GetData().Controls[0].Limits.size(), 1U );

    // Removing the last node of an event drops the event: the file spells "no solve" by leaving it out.
    model.SetEvent( Animation::RigEvent::Construction );
    ASSERT_TRUE( model.RemoveNode( setup.GetValue() ).IsSuccess() );
    EXPECT_EQ( Serialization::FindRigGraph( model.GetData(), "Construction" ), nullptr );
    EXPECT_TRUE( model.Validate().IsSuccess() ) << model.Validate().GetError();
}

TEST( ControlRigDocumentTest, ALiteralOfTheWrongTypeIsRefusedAtTheEdit )
{
    Editor::CommandHistory history;
    auto                   opened = ControlRigDocumentModel::Open( WriteRig( "Literal" ), history );
    ASSERT_TRUE( opened.IsSuccess() ) << opened.GetError();
    auto& model = *opened.GetValue();
    auto  remap = model.AddNode( Animation::RigNodeKind::RemapFloat, "", {} );
    ASSERT_TRUE( remap.IsSuccess() ) << remap.GetError();
    const auto before = model.GetRevision();

    Serialization::RigGraphInputData wrong;
    wrong.Pin  = "Value";
    wrong.Vec3 = glm::vec3( 1.0f );
    EXPECT_FALSE( model.SetLiteral( remap.GetValue(), wrong ).IsSuccess() ) << "a Vec3 into a Float pin";
    EXPECT_EQ( model.GetRevision(), before );

    Serialization::RigGraphInputData right;
    right.Pin   = "Value";
    right.Float = 0.5f;
    EXPECT_TRUE( model.SetLiteral( remap.GetValue(), right ).IsSuccess() );
}

TEST( ControlRigDocumentTest, ClosingTheDocumentDropsItsUndoRecords )
{
    Editor::CommandHistory history;
    {
        auto opened = ControlRigDocumentModel::Open( WriteRig( "Drop" ), history );
        ASSERT_TRUE( opened.IsSuccess() ) << opened.GetError();
        ASSERT_TRUE( opened.GetValue()->AddControl( "Pole_CTRL" ).IsSuccess() );
    }
    EXPECT_FALSE( history.Undo() ) << "a record pointing into a closed document would write freed memory";
}
