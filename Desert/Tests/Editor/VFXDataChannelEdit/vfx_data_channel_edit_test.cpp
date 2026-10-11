// VFX-10c. The `.dfxch` field-list editor's command layer (Editor/Core/Commands/VFXDataChannelEdit.hpp),
// device-free: every add / remove / rename / retype / reorder is ONE undo step, an edit the channel validator
// refuses changes nothing and pushes nothing, and the window can forget its entries by its working copy.

#include <gtest/gtest.h>

#include <Editor/Core/CommandHistory.hpp>
#include <Editor/Core/Commands/VFXDataChannelEdit.hpp>

#include <Engine/Assets/Serialization/VFXDataChannel.hpp>

using namespace Desert;
namespace S = Desert::Assets::Serialization;

namespace
{
    S::VFXDataChannelData TwoFields()
    {
        S::VFXDataChannelData data;
        data.Fields = { { "Pos", S::VFXDataChannelFieldType::Position },
                        { "Strength", S::VFXDataChannelFieldType::Float } };
        return data;
    }
} // namespace

TEST( VFXDataChannelEdit, EveryFieldEditIsOneUndoStep )
{
    Editor::CommandHistory history;
    S::VFXDataChannelData  working = TwoFields();
    const auto             start   = working.Fields;

    ASSERT_TRUE( Editor::AddChannelField( working, history, S::VFXDataChannelFieldType::Color ) );
    ASSERT_EQ( working.Fields.size(), 3u );
    EXPECT_EQ( working.Fields[2].Name, "Field" );
    EXPECT_EQ( working.Fields[2].Type, S::VFXDataChannelFieldType::Color );
    ASSERT_TRUE( Editor::RenameChannelField( working, history, 2, "Tint" ) );
    ASSERT_TRUE( Editor::RetypeChannelField( working, history, 1, S::VFXDataChannelFieldType::Int ) );
    ASSERT_TRUE( Editor::MoveChannelField( working, history, 2, 0 ) );
    EXPECT_EQ( working.Fields[0].Name, "Tint" );
    EXPECT_EQ( working.Fields[1].Name, "Pos" );
    EXPECT_EQ( working.Fields[2].Type, S::VFXDataChannelFieldType::Int );
    ASSERT_TRUE( Editor::RemoveChannelField( working, history, 1 ) );
    ASSERT_EQ( working.Fields.size(), 2u );

    // Five edits, five steps: each Undo takes back exactly one.
    ASSERT_TRUE( history.Undo() );
    EXPECT_EQ( working.Fields.size(), 3u ) << "remove was not its own step";
    ASSERT_TRUE( history.Undo() );
    EXPECT_EQ( working.Fields[0].Name, "Pos" ) << "reorder was not its own step";
    ASSERT_TRUE( history.Undo() );
    EXPECT_EQ( working.Fields[1].Type, S::VFXDataChannelFieldType::Float ) << "retype was not its own step";
    ASSERT_TRUE( history.Undo() );
    EXPECT_EQ( working.Fields[2].Name, "Field" ) << "rename was not its own step";
    ASSERT_TRUE( history.Undo() );
    EXPECT_EQ( working.Fields, start ) << "add was not its own step";
    EXPECT_FALSE( history.Undo() ) << "an edit pushed more than one entry";

    ASSERT_TRUE( history.Redo() );
    EXPECT_EQ( working.Fields.size(), 3u );
}

TEST( VFXDataChannelEdit, ARefusedEditChangesNothingAndPushesNothing )
{
    Editor::CommandHistory history;
    S::VFXDataChannelData  working = TwoFields();
    const auto             start   = working.Fields;

    EXPECT_FALSE( Editor::RenameChannelField( working, history, 1, "Pos" ) ) << "a duplicate field name accepted";
    EXPECT_FALSE( Editor::RenameChannelField( working, history, 1, "" ) ) << "an empty field name accepted";
    EXPECT_FALSE( Editor::RemoveChannelField( working, history, 7 ) ) << "a field that does not exist removed";
    EXPECT_FALSE( Editor::MoveChannelField( working, history, 0, 5 ) ) << "a move past the end accepted";
    EXPECT_EQ( working.Fields, start ) << "a refused edit changed the working copy";
    EXPECT_FALSE( history.Undo() ) << "a refused edit pushed an undo entry";

    // Renaming to the same name changes nothing, so it is no step either.
    ASSERT_TRUE( Editor::RenameChannelField( working, history, 0, "Pos" ) );
    EXPECT_FALSE( history.Undo() ) << "an edit that changed nothing pushed an entry";
}

TEST( VFXDataChannelEdit, TheWindowForgetsItsEntriesByItsWorkingCopy )
{
    Editor::CommandHistory history;
    S::VFXDataChannelData  working = TwoFields();
    ASSERT_TRUE( Editor::AddChannelField( working, history, S::VFXDataChannelFieldType::Float ) );
    ASSERT_TRUE( Editor::AddChannelField( working, history, S::VFXDataChannelFieldType::Float ) );
    EXPECT_EQ( working.Fields[3].Name, "Field1" ) << "the free-name pick reused a taken name";
    history.DropFor( &working );
    EXPECT_FALSE( history.Undo() ) << "the closed window's entries outlived it (EditedObject is not the copy)";
}
