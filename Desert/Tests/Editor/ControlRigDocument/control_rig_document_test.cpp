#include <gtest/gtest.h>

#include <Editor/Panels/ControlRig/ControlRigDocumentModel.hpp>

#include <filesystem>
#include <fstream>

// ANIM-FIX11: the .derig document's model — open, edit, undo, save, re-read — without a window.
using namespace Desert;
using Editor::ControlRigDocumentModel;

namespace
{
    std::filesystem::path WriteRig( const std::string& stem )
    {
        const auto dir = std::filesystem::temp_directory_path() / "ControlRigDocumentTest";
        std::filesystem::create_directories( dir );
        const auto path = dir / ( stem + ".derig" );
        std::ofstream( path ) << R"({
      "Header": { "Kind": "ControlRig", "Guid": "0123456789abcdef0123456789abcdef", "Versions": { "CRIG": 3 }, "Dependencies": [] },
      "TargetSkeleton": { "Guid": "fedcba9876543210fedcba9876543210", "Path": "Meshes/ArmRig.skeleton" },
      "Name": "Doc",
      "Controls": [
        { "Name": "Hand_CTRL", "ShapeName": "CircleXY",
          "Offset": { "Translation": [0.0, 0.0, 0.0], "Rotation": [1.0, 0.0, 0.0, 0.0], "Scale": [1.0, 1.0, 1.0] },
          "Pose":   { "Translation": [0.0, 0.0, 0.0], "Rotation": [1.0, 0.0, 0.0, 0.0], "Scale": [1.0, 1.0, 1.0] },
          "Parents": [ { "Kind": "Component", "Target": "", "Weight": 1.0 } ] },
        { "Name": "Elbow_CTRL", "ShapeName": "CircleXY",
          "Offset": { "Translation": [0.0, 0.0, 0.0], "Rotation": [1.0, 0.0, 0.0, 0.0], "Scale": [1.0, 1.0, 1.0] },
          "Pose":   { "Translation": [0.0, 0.0, 0.0], "Rotation": [1.0, 0.0, 0.0, 0.0], "Scale": [1.0, 1.0, 1.0] },
          "Parents": [ { "Kind": "Control", "Target": "Hand_CTRL", "Weight": 1.0 } ] }
      ],
      "Drives": [ { "Control": "Hand_CTRL", "Bone": "Hand" } ]
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

    auto get = model.AddNode( Animation::RigNodeKind::GetControl, "Hand_CTRL" );
    auto set = model.AddNode( Animation::RigNodeKind::SetControl, "Elbow_CTRL" );
    ASSERT_TRUE( get.IsSuccess() && set.IsSuccess() );
    ASSERT_TRUE( model.Connect( get.GetValue(), "Transform", set.GetValue(), "Transform" ).IsSuccess() );
    EXPECT_TRUE( model.IsDirty() );
    ASSERT_TRUE( model.Validate().IsSuccess() ) << model.Validate().GetError();
    const auto wired = model.GetData();

    // Undo cuts exactly the wire; redo puts it back — the record holds the whole value, not a guess.
    ASSERT_TRUE( history.Undo() );
    ASSERT_TRUE( model.GetData().Graph.has_value() );
    EXPECT_FALSE( model.GetData().Graph->Nodes[1].Inputs[0].Link.has_value() );
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
    ASSERT_TRUE( model.AddNode( Animation::RigNodeKind::GetControl, "Hand_CTRL" ).IsSuccess() );

    auto renamed = model.GetData().Controls[0];
    renamed.Name = "Wrist_CTRL";
    ASSERT_TRUE( model.SetControl( "Hand_CTRL", renamed ).IsSuccess() );
    const auto& data = model.GetData();
    EXPECT_EQ( data.Controls[1].Parents[0].Target, "Wrist_CTRL" );
    EXPECT_EQ( data.Drives[0].Control, "Wrist_CTRL" );
    EXPECT_EQ( data.Graph->Nodes[0].Target, "Wrist_CTRL" );
}

TEST( ControlRigDocumentTest, WiresThatCanNeverBeValidAreRefusedWithoutARecord )
{
    Editor::CommandHistory history;
    auto                   opened = ControlRigDocumentModel::Open( WriteRig( "Refusals" ), history );
    ASSERT_TRUE( opened.IsSuccess() ) << opened.GetError();
    auto& model = *opened.GetValue();
    auto  get   = model.AddNode( Animation::RigNodeKind::GetControl, "Hand_CTRL" );
    auto  remap = model.AddNode( Animation::RigNodeKind::RemapFloat, "" );
    ASSERT_TRUE( get.IsSuccess() && remap.IsSuccess() );
    const auto before = model.GetRevision();

    EXPECT_FALSE( model.Connect( get.GetValue(), "Transform", remap.GetValue(), "Value" ).IsSuccess() )
         << "a Transform into a Float pin";
    EXPECT_FALSE( model.Connect( remap.GetValue(), "Value", remap.GetValue(), "InMin" ).IsSuccess() )
         << "a node into itself is a loop";
    EXPECT_FALSE( model.AddControl( "Hand_CTRL" ).IsSuccess() ) << "a second control of the same name";
    EXPECT_EQ( model.GetRevision(), before );
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
