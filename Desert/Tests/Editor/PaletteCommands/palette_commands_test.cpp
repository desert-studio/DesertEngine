// A COMMAND THAT FAILED MUST NOT ANSWER LIKE ONE THAT WORKED.
//
// `PaletteCommand::Run` returned `void`. The palette's dictionary is also the control channel's
// vocabulary (Г14), so `run` over the socket answered `{"ok":true}` for a command that had FAILED — a
// document that would not resolve, a scene that would not save, an Apply that published nothing — and the
// only trace was a line in a log the client was not reading. That is the Ф4/Г13 shape this codebase spent
// a day removing: a result that EXISTS, is KNOWN, and is thrown away at the boundary. `SetData` on the
// buffers was the same defect one layer down.
//
// Measured on the live editor before the fix: `desertctl run Action Undo` against an empty history exited
// 0 with `{"ok":true}`, having undone nothing.
//
// WHAT IS ASSERTED HERE, and why each is a relation rather than a spot value:
//
//   1. THE TWO SPELLINGS OF SUCCESS ARE DISTINGUISHABLE IN THE SOURCE AND IDENTICAL ON THE WIRE.
//      `PaletteCommandDone()` marks an entry with no failure mode; `PaletteCommandOutcome(moved, why)`
//      carries an answer something actually gave back. A reader can grep which is which; a client cannot
//      tell them apart, and must not be able to — "it worked" is one fact.
//   2. A FALSE ALWAYS CARRIES WORDS. A refusal with nothing said is the shape the whole channel exists to
//      make impossible, and `bool`-returning editor operations are exactly where it would creep back in.
//   3. THE OUTCOME SURVIVES THE DICTIONARY. Resolution and execution are two steps over one list, and a
//      failure has to reach the caller THROUGH them — this is the property EditorLayer's `Op::Run` and
//      CommandPalette::Draw both depend on and neither can assert (EditorLayer.cpp and CommandPalette.cpp
//      are compiled by no suite: scripts/CI/UnreachedSources.sh).

#include <Editor/Core/CommandPalette.hpp>
#include <Editor/Core/Control/ControlDispatch.hpp>

#include <gtest/gtest.h>

#include <string>
#include <vector>

using Desert::Editor::ClampPaletteSelection;
using Desert::Editor::CommandPalette;
using Desert::Editor::PaletteCommand;
using Desert::Editor::PaletteCommandDone;
using Desert::Editor::PaletteCommandOutcome;
using Desert::Editor::PaletteHit;
using Desert::Editor::RankPaletteCommands;
using Desert::Editor::WrapPaletteSelection;
using Desert::Editor::Control::CommandAddress;
using Desert::Editor::Control::ResolveCommand;

// ---------------------------------------------------------------------------------------------------
// 1 & 2. The outcome protocol.
// ---------------------------------------------------------------------------------------------------

TEST( PaletteCommands, AnEntryWithNoFailureModeSucceeds )
{
    EXPECT_TRUE( PaletteCommandDone().IsSuccess() );
}

// THE `bool` HALF, which is where the defect lived: three ISubjectDocument operations and the scene save
// answer "did anything move" and nothing else. True is a success; FALSE MUST CARRY WORDS, because "the
// document declined" is all the editor knows and a client told nothing at all would read it as done.
TEST( PaletteCommands, AFalseOutcomeCarriesTheReasonItWasGiven )
{
    const auto refused = PaletteCommandOutcome( false, "the scene was NOT saved." );

    ASSERT_FALSE( refused.IsSuccess() );
    EXPECT_EQ( refused.GetError(), "the scene was NOT saved." );
}

TEST( PaletteCommands, ATrueOutcomeIsASuccessAndSaysNothingElse )
{
    EXPECT_TRUE( PaletteCommandOutcome( true, "unused" ).IsSuccess() );
}

// THE TWO SPELLINGS OF SUCCESS ARE ONE FACT ON THE WIRE. They exist so a READER can tell "this cannot
// fail" from "this was asked and said yes"; a client must not be able to, or "ok" would mean two things.
TEST( PaletteCommands, BothSpellingsOfSuccessAreIndistinguishableToACaller )
{
    const auto cannotFail = PaletteCommandDone();
    const auto didWork    = PaletteCommandOutcome( true, "unused" );

    EXPECT_EQ( cannotFail.IsSuccess(), didWork.IsSuccess() );
    EXPECT_TRUE( cannotFail.IsSuccess() );
}

// A REFUSAL IS NEVER EMPTY. Asserted as a property over the messages the editor actually passes in,
// rather than one of them, because the failure this guards is somebody adding a fifth call site and
// leaving the string blank — which produces a refusal a client can see and not act on.
TEST( PaletteCommands, NoRefusalIsEverWordless )
{
    for ( const char* reason :
          { "there was nothing left to undo.", "the document published nothing: it had no outstanding edit.",
            "the scene was NOT saved; the log line above says why." } )
    {
        const auto refused = PaletteCommandOutcome( false, reason );
        ASSERT_FALSE( refused.IsSuccess() );
        EXPECT_FALSE( refused.GetError().empty() );
    }
}

// ---------------------------------------------------------------------------------------------------
// 3. The outcome survives the dictionary.
// ---------------------------------------------------------------------------------------------------

namespace
{
    // A dictionary shaped like the editor's: entries that cannot fail beside entries that can, which is
    // the mixture both consumers walk.
    std::vector<PaletteCommand> Dictionary( bool saveWillWork )
    {
        std::vector<PaletteCommand> commands;
        commands.push_back( { "View", "Toggle the grid", [] { return PaletteCommandDone(); } } );
        commands.push_back( { "Action", "Undo",
                              [] { return PaletteCommandOutcome( false, "there was nothing left to undo." ); } } );
        commands.push_back( { "Action", "Save Scene", [saveWillWork]
                              { return PaletteCommandOutcome( saveWillWork, "the scene was NOT saved." ); } } );
        return commands;
    }
} // namespace

// THE RELATION EditorLayer's Op::Run RESTS ON: one dictionary, resolved against and run out of, with the
// failure reaching the caller through both steps. It used to be resolved, run, and answered `ok`.
TEST( PaletteCommands, AResolvedEntryThatRefusesReachesTheCallerAsARefusal )
{
    const std::vector<PaletteCommand> dictionary = Dictionary( /*saveWillWork=*/false );

    const auto resolved = ResolveCommand( dictionary, CommandAddress{ "Action", "Undo" } );
    ASSERT_TRUE( resolved.Found );

    const auto ran = dictionary[resolved.Index].Run();
    ASSERT_FALSE( ran.IsSuccess() ) << "the entry refused and the dictionary reported success";
    EXPECT_NE( ran.GetError().find( "nothing left to undo" ), std::string::npos );
}

// ...and the other direction, which is the one an over-eager change breaks: an entry that WORKED must
// still come back as success through the same two steps.
TEST( PaletteCommands, AResolvedEntryThatWorksStillSucceeds )
{
    const std::vector<PaletteCommand> dictionary = Dictionary( /*saveWillWork=*/true );

    for ( const CommandAddress& wanted :
          { CommandAddress{ "View", "Toggle the grid" }, CommandAddress{ "Action", "Save Scene" } } )
    {
        const auto resolved = ResolveCommand( dictionary, wanted );
        ASSERT_TRUE( resolved.Found ) << wanted.Group << " / " << wanted.Label;
        EXPECT_TRUE( dictionary[resolved.Index].Run().IsSuccess() ) << wanted.Group << " / " << wanted.Label;
    }
}

// ONE ENTRY, ONE RUN. The dictionary is rebuilt per request, so a command that is resolved must be the
// command that runs — asserted by counting, because "it ran the right one" and "it ran it once" are two
// different claims and a loop that fell through would satisfy the first.
TEST( PaletteCommands, ResolvingAndRunningTouchesExactlyTheOneEntry )
{
    int grid = 0;
    int undo = 0;

    std::vector<PaletteCommand> dictionary;
    dictionary.push_back( { "View", "Toggle the grid", [&grid]
                            {
                                ++grid;
                                return PaletteCommandDone();
                            } } );
    dictionary.push_back( { "Action", "Undo", [&undo]
                            {
                                ++undo;
                                return PaletteCommandOutcome( false, "nothing to undo." );
                            } } );

    const auto resolved = ResolveCommand( dictionary, CommandAddress{ "Action", "Undo" } );
    ASSERT_TRUE( resolved.Found );
    EXPECT_FALSE( dictionary[resolved.Index].Run().IsSuccess() );

    EXPECT_EQ( undo, 1 );
    EXPECT_EQ( grid, 0 ) << "resolving one entry ran another";
}

// ---------------------------------------------------------------------------------------------------
// 4. THE PALETTE'S OWN DECISIONS - A6-2 point 3.
//
// `CommandPalette.cpp` is compiled by no suite, which alone says little: 85 % of this repository's
// translation units are not. What made it worth acting on is that A6-1 had just put two things worth
// checking INTO it - WHEN the dictionary is built, and WHAT the ranking does with it - so the only
// evidence either worked was a photograph.
//
// The decisions are free functions now and are asserted here. What is left in the .cpp is a drawing: the
// popup's lifecycle, the keyboard focus, the scroll. That half is NAMED as unreachable rather than
// pretended about, and it is checked the one way it can be - by looking at a frame, which the control
// channel can now take because the palette has a name in its own dictionary.
// ---------------------------------------------------------------------------------------------------

namespace
{
    std::vector<std::string> LabelsOf( const std::vector<PaletteHit>& hits )
    {
        std::vector<std::string> labels;
        labels.reserve( hits.size() );
        for ( const PaletteHit& hit : hits )
            labels.push_back( hit.Command->Label );
        return labels;
    }

    std::vector<PaletteCommand> Named( const std::vector<std::string>& labels )
    {
        std::vector<PaletteCommand> commands;
        for ( const std::string& label : labels )
            commands.push_back( { "Action", label, [] { return PaletteCommandDone(); } } );
        return commands;
    }
} // namespace

// AN EMPTY QUERY OFFERS EVERYTHING, in the dictionary's own order. This is the first thing a person sees
// on Ctrl+P and the first thing a picture of the overlay shows.
TEST( PaletteCommands, AnEmptyQueryOffersTheWholeDictionaryInOrder )
{
    const auto commands = Named( { "Save Scene", "Undo", "Redo" } );
    EXPECT_EQ( LabelsOf( RankPaletteCommands( commands, "" ) ),
               ( std::vector<std::string>{ "Save Scene", "Undo", "Redo" } ) );
}

TEST( PaletteCommands, AQueryNothingMatchesOffersNothing )
{
    EXPECT_TRUE( RankPaletteCommands( Named( { "Save Scene", "Undo" } ), "zzzz" ).empty() );
}

// THE ORDER IS BEST-FIRST, asserted as a property of the whole result rather than by naming a winner:
// the scores are FuzzyMatch's business and pinning one would make this suite red for an honest change
// to it.
TEST( PaletteCommands, TheOfferedListIsBestFirst )
{
    const auto commands = Named( { "Save Scene", "Open Scene Arena", "Undo", "Save this document" } );
    const auto hits     = RankPaletteCommands( commands, "save" );

    ASSERT_FALSE( hits.empty() );
    for ( const PaletteHit& hit : hits )
        ASSERT_NE( hit.Command, nullptr );
    for ( std::size_t i = 1; i < hits.size(); ++i )
        EXPECT_GE( hits[i - 1].Score, hits[i].Score ) << "the list is not best-first at " << i;
}

// STABLE, AND THAT IS LOAD-BEARING RATHER THAN TIDY. The first row is what Enter runs, so an unstable
// sort would let two openings that found the same entries with the same scores run DIFFERENT commands
// for the same keystrokes.
TEST( PaletteCommands, EntriesOfEqualScoreKeepTheDictionaryOrder )
{
    // Identical labels: whatever FuzzyMatch scores them, they score the same, so only stability decides.
    std::vector<PaletteCommand> commands;
    for ( const char* group : { "First", "Second", "Third" } )
        commands.push_back( { group, "Toggle the grid", [] { return PaletteCommandDone(); } } );

    const auto hits = RankPaletteCommands( commands, "grid" );
    ASSERT_EQ( hits.size(), 3u );
    EXPECT_EQ( hits[0].Command->Group, "First" );
    EXPECT_EQ( hits[1].Command->Group, "Second" );
    EXPECT_EQ( hits[2].Command->Group, "Third" );
}

// The hit points INTO the dictionary it was given, which is what lets Draw run the entry without a copy
// - and is why the caller must not let that vector die first. Stated as a property rather than left for
// a reader to infer.
TEST( PaletteCommands, AHitPointsAtTheEntryItCameFrom )
{
    const auto commands = Named( { "Undo", "Redo" } );
    const auto hits     = RankPaletteCommands( commands, "Redo" );

    ASSERT_FALSE( hits.empty() );
    EXPECT_EQ( hits[0].Command, &commands[1] );
}

// -- the selection arithmetic ----------------------------------------------------------------------

TEST( PaletteCommands, AnEmptyListSelectsNothingRatherThanRowZero )
{
    EXPECT_EQ( ClampPaletteSelection( 0, 0 ), 0 );
    EXPECT_EQ( ClampPaletteSelection( 7, 0 ), 0 );
    EXPECT_EQ( ClampPaletteSelection( -3, 0 ), 0 );
    EXPECT_EQ( WrapPaletteSelection( 4, 0 ), 0 );
}

// THE CLAMP KEEPS A SELECTION INSIDE A LIST THAT SHRANK UNDER IT - the ordinary case, since the list is
// re-ranked on every keystroke and one more letter usually makes it shorter.
TEST( PaletteCommands, ASelectionSurvivesTheListShrinkingUnderIt )
{
    EXPECT_EQ( ClampPaletteSelection( 9, 3 ), 2 );
    EXPECT_EQ( ClampPaletteSelection( 2, 3 ), 2 );
    EXPECT_EQ( ClampPaletteSelection( 0, 3 ), 0 );
    EXPECT_EQ( ClampPaletteSelection( -1, 3 ), 0 );
}

// UP FROM THE FIRST ROW REACHES THE LAST, AND DOWN FROM THE LAST REACHES THE FIRST. The wrap is fed a
// clamped selection moved by one, so its whole domain is [-1, hitCount] - asserted over all of it.
TEST( PaletteCommands, TheSelectionWrapsAtBothEndsOverItsWholeDomain )
{
    constexpr std::size_t kCount = 4;

    EXPECT_EQ( WrapPaletteSelection( -1, kCount ), 3 ) << "Up from the first row must reach the last";
    EXPECT_EQ( WrapPaletteSelection( static_cast<int>( kCount ), kCount ), 0 )
         << "Down from the last row must reach the first";

    for ( int selected = -1; selected <= static_cast<int>( kCount ); ++selected )
    {
        const int wrapped = WrapPaletteSelection( selected, kCount );
        EXPECT_GE( wrapped, 0 ) << "selected " << selected;
        EXPECT_LT( wrapped, static_cast<int>( kCount ) ) << "selected " << selected;
    }
}

// -- the flag protocol A6-1 introduced, which had no test and one photograph ------------------------
//
// The dictionary is rebuilt on the frame the palette OPENS and not on every frame it is open - it walks
// the scene's entities, the levels on disk and every openable file under the content root, and doing
// that sixty times a second while somebody types was two recursive directory walks per frame.

TEST( PaletteCommands, TheDictionaryIsAskedForExactlyOncePerOpening )
{
    CommandPalette palette;
    EXPECT_FALSE( palette.TakeJustOpened() ) << "a palette nobody opened must not ask for a dictionary";

    palette.Open();
    EXPECT_TRUE( palette.IsOpen() );
    EXPECT_TRUE( palette.TakeJustOpened() ) << "the frame it opens on must ask";

    for ( int frame = 0; frame < 60; ++frame )
        EXPECT_FALSE( palette.TakeJustOpened() ) << "rebuilt again on frame " << frame;

    palette.Open();
    EXPECT_TRUE( palette.TakeJustOpened() ) << "the NEXT opening must ask again, or it would show the "
                                               "previous session's list";
}
