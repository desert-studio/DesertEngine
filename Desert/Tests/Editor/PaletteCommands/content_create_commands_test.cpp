// "Assets / New Level Sequence" (ANIM-I11d): the palette entry exists under the words the context menu uses,
// and running it IS the creation route it was handed - its call and its outcome, not a copy of either.
#include <Editor/Core/ContentCreateCommands.hpp>

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace
{
    using Desert::Editor::ContentCreatePaletteCommands;
    using Desert::Editor::kNewLevelSequenceLabel;
    using Desert::Editor::PaletteCommand;

    std::function<Common::BoolResultStr()> NotCalled()
    {
        return [] { return Common::MakeError( "a creation the test did not run was run" ); };
    }

    const PaletteCommand* FindAsset( const std::vector<PaletteCommand>& commands, const std::string_view label )
    {
        for ( const PaletteCommand& command : commands )
            if ( command.Group == "Assets" && command.Label == label )
                return &command;
        return nullptr;
    }

    const PaletteCommand* FindNewLevelSequence( const std::vector<PaletteCommand>& commands )
    {
        for ( const PaletteCommand& command : commands )
            if ( command.Group == "Assets" && command.Label == kNewLevelSequenceLabel )
                return &command;
        return nullptr;
    }
} // namespace

TEST( ContentCreateCommands, NewLevelSequenceIsOfferedAndRunsTheCreationItWasHanded )
{
    int        calls    = 0;
    const auto commands = ContentCreatePaletteCommands(
         [&calls]
         {
             ++calls;
             return Common::MakeSuccess( true );
         },
         NotCalled(), NotCalled() );
    const PaletteCommand* command = FindNewLevelSequence( commands );
    ASSERT_NE( command, nullptr ) << "no \"Assets / New Level Sequence\" palette entry";
    EXPECT_EQ( calls, 0 ) << "building the palette created an asset";
    const auto outcome = command->Run();
    EXPECT_TRUE( outcome );
    EXPECT_EQ( calls, 1 ) << "the entry did not call CreateNewLevelSequence exactly once";
}

TEST( ContentCreateCommands, ACreationThatFailedAnswersWithItsOwnReason )
{
    const auto commands = ContentCreatePaletteCommands(
         [] { return Common::MakeError( "New Level Sequence: the Assets window has no folder open" ); },
         NotCalled(), NotCalled() );
    const PaletteCommand* command = FindNewLevelSequence( commands );
    ASSERT_NE( command, nullptr );
    const auto outcome = command->Run();
    ASSERT_FALSE( outcome ) << "a failed creation answered like one that worked";
    EXPECT_EQ( std::string( outcome.GetError() ), "New Level Sequence: the Assets window has no folder open" );
}

// GP1d: "Assets / New Input Action" and "Assets / New Input Mapping Context" each run their own creation once.
TEST( ContentCreateCommands, NewInputActionAndMappingContextRunTheirOwnCreation )
{
    int        actions  = 0;
    int        contexts = 0;
    const auto commands = ContentCreatePaletteCommands(
         NotCalled(),
         [&actions]
         {
             ++actions;
             return Common::MakeSuccess( true );
         },
         [&contexts]
         {
             ++contexts;
             return Common::MakeSuccess( true );
         } );
    const PaletteCommand* action  = FindAsset( commands, Desert::Editor::kNewInputActionLabel );
    const PaletteCommand* context = FindAsset( commands, Desert::Editor::kNewInputMappingContextLabel );
    ASSERT_NE( action, nullptr ) << "no \"Assets / New Input Action\" palette entry";
    ASSERT_NE( context, nullptr ) << "no \"Assets / New Input Mapping Context\" palette entry";
    EXPECT_TRUE( action->Run() );
    EXPECT_EQ( actions, 1 );
    EXPECT_EQ( contexts, 0 );
    EXPECT_TRUE( context->Run() );
    EXPECT_EQ( contexts, 1 );
}
