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
             return Desert::Common::MakeSuccess( true );
         } );
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
         [] { return Desert::Common::MakeError( "New Level Sequence: the Assets window has no folder open" ); } );
    const PaletteCommand* command = FindNewLevelSequence( commands );
    ASSERT_NE( command, nullptr );
    const auto outcome = command->Run();
    ASSERT_FALSE( outcome ) << "a failed creation answered like one that worked";
    EXPECT_EQ( std::string( outcome.GetError() ), "New Level Sequence: the Assets window has no folder open" );
}
