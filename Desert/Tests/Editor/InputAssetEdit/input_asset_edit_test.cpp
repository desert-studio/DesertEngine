// GP1d: every edit in the Input Action / Input Mapping Context editors is ONE entry of the editor's
// CommandHistory (UE: FScopedTransaction on the asset), the operations refuse what the subsystem could not
// evaluate, and what the editor saves reads back as the data it held. Device-free: no ImGui, no window.
#include <Editor/Core/CommandHistory.hpp>
#include <Editor/Core/Commands/InputAssetEdit.hpp>

#include <Engine/Assets/Serialization/InputAssets.hpp>

#include <Common/Content/TextAssetHeader.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

namespace
{
    using Desert::Editor::CommandHistory;
    using Desert::Editor::InputAssetOwner;
    using Desert::Editor::InputContextEditTransaction;
    namespace IE = Desert::Editor::InputEdit;
    namespace S  = Desert::Assets::Serialization;

    Desert::Assets::AssetGuidRef ActionRef( const uint64_t n, const std::string& stem )
    {
        return { Common::Content::AssetGuidToText( Common::Content::AssetGuid{ 0x6d00ull + n, 0x2ull } ),
                 "project:Input/" + stem + ".deinputaction" };
    }

    struct Fixture
    {
        std::shared_ptr<S::InputMappingContextData> context  = std::make_shared<S::InputMappingContextData>();
        int                                         restores = 0;

        Fixture()
        {
            CommandHistory::Get().Clear();
        }
        ~Fixture()
        {
            CommandHistory::Get().Clear();
        }

        InputAssetOwner<S::InputMappingContextData> Owner()
        {
            InputAssetOwner<S::InputMappingContextData> owner;
            owner.Label        = "Input Mapping Context 'IMC_Test'";
            owner.Volatile     = false;
            owner.Resolve      = [this] { return context.get(); };
            owner.AfterRestore = [this] { ++restores; };
            return owner;
        }
    };
} // namespace

TEST( InputAssetEdit, AnAddedMappingIsOneEntryAndUndoRestoresTheContextBeforeIt )
{
    Fixture                          f;
    InputContextEditTransaction      edits;
    const S::InputMappingContextData before = *f.context;
    ASSERT_TRUE( edits.Edit( f.Owner(), []( S::InputMappingContextData& d )
                             { return IE::AddMapping( d, ActionRef( 1, "IA_Jump" ), "Space" ); } ) );
    ASSERT_EQ( f.context->Mappings.size(), 1u );
    EXPECT_EQ( f.context->Mappings[0].Key, "Space" );
    EXPECT_EQ( CommandHistory::Get().UndoStack().size(), 1u );
    const S::InputMappingContextData after = *f.context;

    ASSERT_TRUE( CommandHistory::Get().Undo() );
    EXPECT_EQ( *f.context, before );
    ASSERT_TRUE( CommandHistory::Get().Redo() );
    EXPECT_EQ( *f.context, after );
    EXPECT_EQ( f.restores, 2 );
}

TEST( InputAssetEdit, ARemovedMappingUndoesBackInItsPlace )
{
    Fixture                     f;
    InputContextEditTransaction edits;
    ASSERT_TRUE( IE::AddMapping( *f.context, ActionRef( 1, "IA_Move" ), "W" ) );
    ASSERT_TRUE( IE::AddMapping( *f.context, ActionRef( 1, "IA_Move" ), "S" ) );
    ASSERT_TRUE( IE::AddMapping( *f.context, ActionRef( 2, "IA_Jump" ), "Space" ) );
    const S::InputMappingContextData before = *f.context;

    ASSERT_TRUE(
         edits.Edit( f.Owner(), []( S::InputMappingContextData& d ) { return IE::RemoveMapping( d, 1 ); } ) );
    ASSERT_EQ( f.context->Mappings.size(), 2u );
    EXPECT_EQ( f.context->Mappings[1].Key, "Space" );
    ASSERT_TRUE( CommandHistory::Get().Undo() );
    EXPECT_EQ( *f.context, before ) << "the undone removal did not put S back between W and Space";
}

TEST( InputAssetEdit, ModifiersReorderAsOneEntryAndKeepTheirParameters )
{
    Fixture                     f;
    InputContextEditTransaction edits;
    ASSERT_TRUE( IE::AddMapping( *f.context, ActionRef( 1, "IA_Move" ), "A" ) );
    ASSERT_TRUE( IE::AddModifier( *f.context, 0, S::InputModifierType::Negate ) );
    ASSERT_TRUE( IE::AddModifier( *f.context, 0, S::InputModifierType::Scalar ) );
    f.context->Mappings[0].Modifiers[1].Scalar->Scalar = glm::vec3( 0.5f );
    ASSERT_TRUE( f.context->Mappings[0].Modifiers[0].Negate.has_value() );
    EXPECT_FALSE( f.context->Mappings[0].Modifiers[0].Scalar.has_value() ) << "a Negate carries Scalar params";
    const S::InputMappingContextData before = *f.context;

    ASSERT_TRUE(
         edits.Edit( f.Owner(), []( S::InputMappingContextData& d ) { return IE::MoveModifier( d, 0, 1, 0 ); } ) );
    const auto& modifiers = f.context->Mappings[0].Modifiers;
    ASSERT_EQ( modifiers.size(), 2u );
    EXPECT_EQ( modifiers[0].Type, S::InputModifierType::Scalar );
    EXPECT_EQ( modifiers[0].Scalar->Scalar, glm::vec3( 0.5f ) );
    EXPECT_EQ( modifiers[1].Type, S::InputModifierType::Negate );
    EXPECT_EQ( CommandHistory::Get().UndoStack().size(), 1u );
    ASSERT_TRUE( CommandHistory::Get().Undo() );
    EXPECT_EQ( *f.context, before );

    EXPECT_FALSE( IE::MoveModifier( *f.context, 0, 0, 2 ) ) << "a move past the end was accepted";
}

TEST( InputAssetEdit, ATriggerChangedToHoldGainsItsSecondsAndKeepsItsThreshold )
{
    Fixture                     f;
    InputContextEditTransaction edits;
    ASSERT_TRUE( IE::AddMapping( *f.context, ActionRef( 1, "IA_Fire" ), "F" ) );
    ASSERT_TRUE( IE::AddTrigger( *f.context, 0, S::InputTriggerType::Pressed ) );
    f.context->Mappings[0].Triggers[0].ActuationThreshold = 0.25f;
    EXPECT_FALSE( IE::SetHoldSeconds( *f.context, 0, 0, 0.5f ) ) << "a Pressed trigger took Hold seconds";

    ASSERT_TRUE( edits.Edit( f.Owner(), []( S::InputMappingContextData& d )
                             { return IE::SetTriggerType( d, 0, 0, S::InputTriggerType::Hold ); } ) );
    const S::InputTriggerData& trigger = f.context->Mappings[0].Triggers[0];
    ASSERT_TRUE( trigger.Hold.has_value() );
    EXPECT_EQ( trigger.ActuationThreshold, 0.25f );
    ASSERT_TRUE( edits.Edit( f.Owner(), []( S::InputMappingContextData& d )
                             { return IE::SetHoldSeconds( d, 0, 0, 0.75f ); } ) );
    EXPECT_EQ( f.context->Mappings[0].Triggers[0].Hold->HoldTimeSeconds, 0.75f );
    EXPECT_FALSE( IE::SetHoldSeconds( *f.context, 0, 0, 0.0f ) ) << "a zero Hold time was accepted";

    EXPECT_EQ( CommandHistory::Get().UndoStack().size(), 2u );
    ASSERT_TRUE( CommandHistory::Get().Undo() );
    EXPECT_EQ( f.context->Mappings[0].Triggers[0].Hold->HoldTimeSeconds, 1.0f );
    ASSERT_TRUE( CommandHistory::Get().Undo() );
    EXPECT_EQ( f.context->Mappings[0].Triggers[0].Type, S::InputTriggerType::Pressed );
    EXPECT_FALSE( f.context->Mappings[0].Triggers[0].Hold.has_value() );
}

TEST( InputAssetEdit, ARefusedOrEmptyEditPushesNothingAndLeavesTheData )
{
    Fixture                     f;
    InputContextEditTransaction edits;
    ASSERT_TRUE( IE::AddMapping( *f.context, ActionRef( 1, "IA_Jump" ), "Space" ) );
    const S::InputMappingContextData before = *f.context;

    EXPECT_FALSE( edits.Edit( f.Owner(), []( S::InputMappingContextData& d )
                              { return IE::SetMappingKey( d, 0, "NotAKey" ); } ) );
    EXPECT_FALSE( edits.Edit( f.Owner(), []( S::InputMappingContextData& d )
                              { return IE::AddMapping( d, Desert::Assets::AssetGuidRef{ "", "x" }, "W" ); } ) );
    EXPECT_TRUE( edits.Edit( f.Owner(), []( S::InputMappingContextData& d )
                             { return IE::SetMappingKey( d, 0, "Space" ); } ) );
    EXPECT_EQ( *f.context, before );
    EXPECT_TRUE( CommandHistory::Get().UndoStack().empty() );
}

TEST( InputAssetEdit, ADragOfManyFramesIsOneEntryAndAnUndoIsNotRecordedAsAnEdit )
{
    Fixture                     f;
    InputContextEditTransaction edits;
    ASSERT_TRUE( IE::AddMapping( *f.context, ActionRef( 1, "IA_Charge" ), "E" ) );
    ASSERT_TRUE( IE::AddTrigger( *f.context, 0, S::InputTriggerType::Hold ) );
    EXPECT_EQ( edits.Observe( f.Owner(), false ), 0u ); // the settled baseline

    for ( int frame = 1; frame <= 40; ++frame )
    {
        f.context->Mappings[0].Triggers[0].Hold->HoldTimeSeconds = 1.0f + 0.01f * static_cast<float>( frame );
        EXPECT_EQ( edits.Observe( f.Owner(), true ), 0u ) << "an entry pushed mid-drag, frame " << frame;
    }
    EXPECT_EQ( edits.Observe( f.Owner(), false ), 1u );
    EXPECT_EQ( CommandHistory::Get().UndoStack().size(), 1u );

    ASSERT_TRUE( CommandHistory::Get().Undo() );
    EXPECT_EQ( f.context->Mappings[0].Triggers[0].Hold->HoldTimeSeconds, 1.0f );
    EXPECT_EQ( edits.Observe( f.Owner(), false ), 0u ) << "the Undo's restore was recorded as a new edit";
    EXPECT_EQ( CommandHistory::Get().RedoStack().size(), 1u );
}

TEST( InputAssetEdit, WhatTheEditorSavesReadsBackAsTheDataItHeld )
{
    S::InputMappingContextData context;
    ASSERT_TRUE( IE::AddMapping( context, ActionRef( 1, "IA_Move" ), "D" ) );
    ASSERT_TRUE( IE::AddModifier( context, 0, S::InputModifierType::Swizzle ) );
    ASSERT_TRUE( IE::AddModifier( context, 0, S::InputModifierType::DeadZone ) );
    ASSERT_TRUE( IE::AddTrigger( context, 0, S::InputTriggerType::Hold ) );
    ASSERT_TRUE( IE::SetHoldSeconds( context, 0, 0, 0.4f ) );
    ASSERT_TRUE( IE::AddMapping( context, ActionRef( 2, "IA_Jump" ), "Space" ) );
    ASSERT_TRUE( S::ValidateInputMappingContext( context ) );

    const std::filesystem::path file =
         std::filesystem::temp_directory_path() / "desert_input_asset_edit_test.deinputcontext";
    const auto saved = S::SaveInputMappingContextFile( file, context );
    ASSERT_TRUE( saved ) << saved.GetError();
    std::ostringstream text;
    text << std::ifstream( file, std::ios::binary ).rdbuf();
    std::error_code ec;
    std::filesystem::remove( file, ec );

    const auto parsed = S::ParseInputMappingContext( text.str() );
    ASSERT_TRUE( parsed ) << parsed.GetError();
    EXPECT_EQ( parsed.GetValue().Mappings, context.Mappings );
    ASSERT_TRUE( parsed.GetValue().Header.has_value() );

    S::InputActionData action;
    ASSERT_TRUE( IE::SetValueType( action, S::InputValueType::Axis2D ) );
    ASSERT_TRUE( IE::SetConsumeInput( action, false ) );
    const auto readAction = S::ParseInputAction( S::WriteInputAction( action ) );
    ASSERT_TRUE( readAction ) << readAction.GetError();
    EXPECT_EQ( readAction.GetValue().ValueType, S::InputValueType::Axis2D );
    EXPECT_FALSE( readAction.GetValue().ConsumeInput );
}
