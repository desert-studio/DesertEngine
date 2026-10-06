// THE CHANNEL ADDRESSES THE COMMAND PALETTE, AND ONLY THE COMMAND PALETTE.
//
// The editor grew a control surface nobody designed: eighteen command-line flags, four of them purely about
// driving the interface, each added by whichever developer needed a picture that week. A flag is spent once
// at boot, so the family could only ever grow -- one flag per task, for ever.
//
// The channel replaces it, and the decision that shapes everything is that it executes PALETTE ENTRIES
// rather than a command table of its own. A table of its own would be a second execution path, and a second
// path is the defect this codebase spends its days removing: two sides that must agree, one of which falls
// behind. So the relations worth asserting are about the addressing, not about any particular command:
//
//   1. NOTHING IN THE DICTIONARY IS UNREACHABLE. Every entry the palette offers can be named and found.
//      This is the "everything a person can do, an agent can do" claim, stated as an expression.
//   2. THE MATCH IS EXACT, ON BOTH HALVES. Fuzzy matching decides SUGGESTIONS, never what runs.
//   3. A MISS IS A NAMED REFUSAL WITH NEAR MISSES, never silence -- and an EMPTY dictionary says so
//      specifically, because "no such command" against zero candidates means the editor is still loading
//      and is a different problem with a different fix.
//   4. THE NAMED VIEWPOINTS ARE A TABLE, and the table is what makes "Front" mean Front in every run --
//      the property `--preview-orbit 160,15` never had.

#include <Editor/Core/Control/ControlDispatch.hpp>
#include <Editor/Core/PreviewViewpoints.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <set>
#include <string>
#include <vector>

using Desert::Editor::FindPreviewViewpoint;
using Desert::Editor::kPreviewPitchLimitDegrees;
using Desert::Editor::kPreviewViewpoints;
using Desert::Editor::Control::CommandAddress;
using Desert::Editor::Control::DescribeUnknownCommand;
using Desert::Editor::Control::kMaxSuggestions;
using Desert::Editor::Control::Resolution;
using Desert::Editor::Control::ResolveCommand;

namespace
{
    // The shape ResolveCommand works over: anything with a Group and a Label. PaletteCommand is one; this
    // is deliberately NOT PaletteCommand, because dragging std::function and its header in would tie the
    // addressing rule to the editor's own types for no gain. If the two ever disagree about the field
    // names, EditorLayer stops compiling -- which is the right place to find out.
    struct Entry
    {
        std::string Group;
        std::string Label;
    };

    std::vector<Entry> Dictionary()
    {
        return {
             { "Panel", "Open Details" },
             { "Panel", "Open Outliner" },
             { "Panel", "Open Logs" },
             { "Document", "Go to M_Crate" },
             { "Document", "Close M_Crate" },
             { "Document", "Reopen M_Barrel" },
             { "Entity", "Directional Light" },
             { "Menu", "Open the View menu" },
             { "Preview", "Front" },
             { "Action", "Save Scene" },
        };
    }
} // namespace

// ---------------------------------------------------------------------------------------------------
// 1. Nothing in the dictionary is unreachable.
// ---------------------------------------------------------------------------------------------------

// THE CLAIM OF THE WHOLE SUBSYSTEM, as an expression: every entry the palette offers is addressable by the
// pair the palette shows. Not "a few of them" -- every one, walked.
//
// It is a relation and not a list of names on purpose. A test naming "Panel"/"Open Details" would still
// pass on the day some filter dropped every Document entry on the floor.
TEST( ControlDispatch, EveryEntryInTheDictionaryCanBeAddressed )
{
    const std::vector<Entry> dictionary = Dictionary();

    for ( std::size_t i = 0; i < dictionary.size(); ++i )
    {
        const CommandAddress wanted{ dictionary[i].Group, dictionary[i].Label };
        const Resolution     resolved = ResolveCommand( dictionary, wanted );

        ASSERT_TRUE( resolved.Found ) << "'" << wanted.Group << "' / '" << wanted.Label
                                      << "' is offered by the palette and cannot be reached by the channel";
        EXPECT_EQ( resolved.Index, i ) << "resolved to the wrong entry, which would RUN the wrong command";
        EXPECT_EQ( resolved.Candidates, dictionary.size() );
    }
}

// ---------------------------------------------------------------------------------------------------
// 2. The match is exact, on both halves.
// ---------------------------------------------------------------------------------------------------

// A label that fuzzy-matches must NOT run. "Close M_Crate" is a few characters from "Go to M_Crate", and a
// channel that helpfully ran the nearest thing would eventually destroy a window unattended. A person
// picking from a ranked list is present to see what they picked; an agent is not.
TEST( ControlDispatch, ANearMissDoesNotRunAnything )
{
    const std::vector<Entry> dictionary = Dictionary();

    const Resolution resolved = ResolveCommand( dictionary, CommandAddress{ "Document", "Close M_Crat" } );
    EXPECT_FALSE( resolved.Found ) << "a label one character short of a real one must not resolve";
    EXPECT_FALSE( resolved.Suggestions.empty() ) << "...but it must say what it nearly matched";
}

// The GROUP is half the address. The same label in another group is a different command -- "Front" under
// Preview and a hypothetical "Front" under Entity are not the same thing, and running either for the other
// is exactly the confusion the two-part address exists to prevent.
TEST( ControlDispatch, TheGroupIsPartOfTheAddress )
{
    const std::vector<Entry> dictionary = Dictionary();

    EXPECT_FALSE( ResolveCommand( dictionary, CommandAddress{ "Action", "Open Details" } ).Found );
    EXPECT_FALSE( ResolveCommand( dictionary, CommandAddress{ "", "Open Details" } ).Found );
    EXPECT_TRUE( ResolveCommand( dictionary, CommandAddress{ "Panel", "Open Details" } ).Found );
}

// ---------------------------------------------------------------------------------------------------
// 3. A miss is a named refusal.
// ---------------------------------------------------------------------------------------------------

TEST( ControlDispatch, AMissOffersNearMissesFromTheSameGroupFirst )
{
    const std::vector<Entry> dictionary = Dictionary();
    const CommandAddress     wanted{ "Panel", "Open Detail" };
    const Resolution         resolved = ResolveCommand( dictionary, wanted );

    ASSERT_FALSE( resolved.Found );
    ASSERT_FALSE( resolved.Suggestions.empty() );

    // The group that was asked for ranks first: a typo in the label with the group right is nearly always
    // the entry that was meant.
    EXPECT_EQ( resolved.Suggestions.front().Group, "Panel" );
    EXPECT_EQ( resolved.Suggestions.front().Label, "Open Details" );

    EXPECT_LE( resolved.Suggestions.size(), kMaxSuggestions )
         << "a refusal nobody reads is a refusal that said nothing";
}

TEST( ControlDispatch, ARefusalNamesTheCommandAndIsNeverEmpty )
{
    const std::vector<Entry> dictionary = Dictionary();
    const CommandAddress     wanted{ "Panel", "Open Detail" };
    const std::string        message = DescribeUnknownCommand( wanted, ResolveCommand( dictionary, wanted ) );

    EXPECT_FALSE( message.empty() );
    EXPECT_NE( message.find( "Open Detail" ), std::string::npos );
    EXPECT_NE( message.find( "Open Details" ), std::string::npos ) << "the near miss must be offered";
    EXPECT_NE( message.find( std::to_string( dictionary.size() ) ), std::string::npos )
         << "how many commands exist is part of the diagnosis";
}

// AN EMPTY DICTIONARY IS A DIFFERENT ANSWER. Against zero candidates, "no such command" is misleading: the
// command may be spelled perfectly and the editor simply has not finished loading. Telling the client to
// ask 'state' is the difference between a retry that works and a retry that fails identically.
TEST( ControlDispatch, AnEmptyDictionarySaysSoRatherThanBlamingTheSpelling )
{
    const std::vector<Entry> nothing;
    const CommandAddress     wanted{ "Panel", "Open Details" };
    const Resolution         resolved = ResolveCommand( nothing, wanted );

    EXPECT_FALSE( resolved.Found );
    EXPECT_EQ( resolved.Candidates, 0u );

    const std::string message = DescribeUnknownCommand( wanted, resolved );
    EXPECT_NE( message.find( "EMPTY" ), std::string::npos );
    EXPECT_NE( message.find( "state" ), std::string::npos ) << "it must say what to do next";
}

TEST( ControlDispatch, ANothingLikeItRefusalStillPointsAtTheFullList )
{
    const std::vector<Entry> dictionary = Dictionary();
    const CommandAddress     wanted{ "Panel", "zzzzzzzz" };
    const Resolution         resolved = ResolveCommand( dictionary, wanted );

    ASSERT_FALSE( resolved.Found );
    ASSERT_TRUE( resolved.Suggestions.empty() );
    EXPECT_NE( DescribeUnknownCommand( wanted, resolved ).find( "commands" ), std::string::npos );
}

// ---------------------------------------------------------------------------------------------------
// 4. The named viewpoints.
// ---------------------------------------------------------------------------------------------------

// `--preview-orbit yaw,pitch` is gone, because a PaletteCommand is a group, a label and a closure with
// nowhere for an argument to go -- and giving it one would be a second way to invoke a command, which is
// the shape the whole channel exists to remove. The capability was decomposed into names instead.
//
// THE TRADE RUNS THE RIGHT WAY, and this test is what makes that true rather than hopeful: a name resolves
// to the same two numbers in every run, on every machine, for every developer. "160,15" had to be
// remembered and passed between people. What was given up is an arbitrary angle, not repeatability.
TEST( ControlDispatch, EveryViewpointNameResolvesToItsOwnAngles )
{
    for ( const auto& viewpoint : kPreviewViewpoints )
    {
        const auto* found = FindPreviewViewpoint( viewpoint.Name );
        ASSERT_NE( found, nullptr ) << "'" << viewpoint.Name << "' is offered and cannot be looked up";
        EXPECT_EQ( found->YawDegrees, viewpoint.YawDegrees );
        EXPECT_EQ( found->PitchDegrees, viewpoint.PitchDegrees );
    }
}

// A name nobody serves must answer NOTHING rather than falling back to the first entry. A silent fall back
// to Front would look exactly like a preview that ignored the request -- and the capture taken of it would
// be a picture of the failure presented as a picture of the feature.
TEST( ControlDispatch, AnUnknownViewpointResolvesToNothingRatherThanToFront )
{
    EXPECT_EQ( FindPreviewViewpoint( "Frontal" ), nullptr );
    EXPECT_EQ( FindPreviewViewpoint( "" ), nullptr );
    EXPECT_EQ( FindPreviewViewpoint( "front" ), nullptr ) << "the names are the labels the palette shows";
}

// Two viewpoints with one name would make the palette offer the same label twice, and the second would be
// unreachable -- a command in the dictionary that cannot be addressed, which is relation 1 above broken
// from the other end.
TEST( ControlDispatch, ViewpointNamesAreUnique )
{
    std::set<std::string> seen;
    for ( const auto& viewpoint : kPreviewViewpoints )
    {
        EXPECT_TRUE( seen.insert( std::string( viewpoint.Name ) ).second )
             << "'" << viewpoint.Name << "' appears twice; the second one could never be run";
    }
}

// The poles must sit inside the limit the MOUSE obeys. A viewpoint authored outside it would be clamped on
// arrival and silently become a different angle -- so "Top" would not be the Top this table promises, and
// the reproducibility the whole decomposition was justified by would be gone.
TEST( ControlDispatch, EveryViewpointIsInsideThePitchLimitTheMouseObeys )
{
    for ( const auto& viewpoint : kPreviewViewpoints )
    {
        EXPECT_LE( std::fabs( viewpoint.PitchDegrees ), kPreviewPitchLimitDegrees )
             << "'" << viewpoint.Name << "' would be clamped, and would not be the angle it names";
    }
}
