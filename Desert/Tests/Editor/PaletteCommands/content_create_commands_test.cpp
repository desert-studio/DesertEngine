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
             return Common::MakeSuccess( true );
         },
         [] { return Common::MakeError( "not this entry" ); } );
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
         [] { return Common::MakeSuccess( true ); } );
    const PaletteCommand* command = FindNewLevelSequence( commands );
    ASSERT_NE( command, nullptr );
    const auto outcome = command->Run();
    ASSERT_FALSE( outcome ) << "a failed creation answered like one that worked";
    EXPECT_EQ( std::string( outcome.GetError() ), "New Level Sequence: the Assets window has no folder open" );
}

// VFX-10b: "Assets / New VFX Data Channel" runs the channel creation it was handed - not the level sequence's.
TEST( ContentCreateCommands, NewVFXDataChannelIsOfferedAndRunsItsOwnCreation )
{
    int        sequences = 0;
    int        channels  = 0;
    const auto commands  = Desert::Editor::ContentCreatePaletteCommands(
         [&sequences]
         {
             ++sequences;
             return Common::MakeSuccess( true );
         },
         [&channels]
         {
             ++channels;
             return Common::MakeSuccess( true );
         } );
    const PaletteCommand* command = nullptr;
    for ( const PaletteCommand& c : commands )
        if ( c.Group == "Assets" && c.Label == Desert::Editor::kNewVFXDataChannelLabel )
            command = &c;
    ASSERT_NE( command, nullptr ) << "no \"Assets / New VFX Data Channel\" palette entry";
    EXPECT_TRUE( command->Run() );
    EXPECT_EQ( channels, 1 ) << "the entry did not call CreateNewVFXDataChannel exactly once";
    EXPECT_EQ( sequences, 0 ) << "the channel entry ran the level sequence creation";
}
