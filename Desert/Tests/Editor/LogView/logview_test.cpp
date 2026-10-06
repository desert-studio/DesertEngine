#include <gtest/gtest.h>

#include <Editor/Core/LogView.hpp>

using namespace Desert::Editor;

TEST( LogView, LogMatchesIsCaseInsensitiveSubstring )
{
    EXPECT_TRUE( LogMatches( "[Project] Opened Foo", "project" ) );
    EXPECT_TRUE( LogMatches( "[Project] Opened Foo", "OPENED" ) );
    EXPECT_TRUE( LogMatches( "anything", "" ) ); // empty query matches all
    EXPECT_FALSE( LogMatches( "hello", "world" ) );
    EXPECT_FALSE( LogMatches( "hi", "longer than line" ) );
}

TEST( LogView, CollapseMergesConsecutiveDuplicates )
{
    std::vector<std::pair<std::string, int>> lines = {
        { "tick", 0 }, { "tick", 0 }, { "tick", 0 }, { "warn", 1 }, { "tick", 0 }
    };
    const auto runs = CollapseConsecutive( lines );

    ASSERT_EQ( runs.size(), 3u );
    EXPECT_EQ( runs[0].Text, "tick" );
    EXPECT_EQ( runs[0].Count, 3 );
    EXPECT_EQ( runs[1].Text, "warn" );
    EXPECT_EQ( runs[1].Count, 1 );
    // The trailing "tick" is a separate event, not merged with the earlier run.
    EXPECT_EQ( runs[2].Text, "tick" );
    EXPECT_EQ( runs[2].Count, 1 );
}

TEST( LogView, CollapseDoesNotMergeAcrossLevels )
{
    std::vector<std::pair<std::string, int>> lines = { { "msg", 0 }, { "msg", 2 } };
    const auto runs = CollapseConsecutive( lines );
    ASSERT_EQ( runs.size(), 2u );
    EXPECT_EQ( runs[0].Level, 0 );
    EXPECT_EQ( runs[1].Level, 2 );
}

TEST( LogView, CollapseEmptyIsEmpty )
{
    EXPECT_TRUE( CollapseConsecutive( {} ).empty() );
}

// --- ParseLogLine ------------------------------------------------------------------------------
// THE DEFECT THESE PIN. The Logs panel used to decide a row's severity by searching the WHOLE line for
// "[warning]" / "[error]" -- so any message that QUOTED one of those words was painted as the fault it
// was merely describing. The severity has to come from the line's own level field and from nowhere else,
// which is a relation between two parts of one line, not a property of either.

TEST( LogView, ParsesTheLoggerPattern )
{
    const auto p = ParseLogLine( "[16:55:20.047][info][Desert]: [Vulkan] GPU: Apple M1 Pro" );
    EXPECT_TRUE( p.Parsed );
    EXPECT_EQ( p.Time, "16:55:20.047" );
    EXPECT_EQ( p.Level, "info" );
    EXPECT_EQ( p.Category, "Vulkan" );
    EXPECT_EQ( p.Message, "GPU: Apple M1 Pro" );
    EXPECT_EQ( p.Severity, LogSeverity::Info );
}

TEST( LogView, SeverityComesFromTheLevelFieldNotFromTheMessage )
{
    // The whole point: the words are in the MESSAGE, and the message is not the level.
    const auto quoted = ParseLogLine( "[16:55:20.047][info][Desert]: shader said [error] near line 12" );
    EXPECT_TRUE( quoted.Parsed );
    EXPECT_EQ( quoted.Severity, LogSeverity::Info );

    const auto real = ParseLogLine( "[16:55:20.047][error][Desert]: shader failed" );
    EXPECT_EQ( real.Severity, LogSeverity::Error );

    const auto warn = ParseLogLine( "[16:55:20.047][warning][Desert]: mips disabled" );
    EXPECT_EQ( warn.Severity, LogSeverity::Warning );

    // critical is an error too -- there is no third colour, and silently calling it Info would hide the
    // single most important line the engine can write.
    EXPECT_EQ( ParseLogLine( "[1][critical][Desert]: dead" ).Severity, LogSeverity::Error );
    // trace/debug are not complaints.
    EXPECT_EQ( ParseLogLine( "[1][debug][Desert]: x" ).Severity, LogSeverity::Info );
    EXPECT_EQ( ParseLogLine( "[1][trace][Desert]: x" ).Severity, LogSeverity::Info );
}

TEST( LogView, SeverityOfLevelAgreesWithParse )
{
    // The panel's counters call SeverityOfLevel and its rows call ParseLogLine. Two entry points, one
    // answer -- assert the agreement rather than each side (they were allowed to differ once already).
    for ( const char* level : { "trace", "debug", "info", "warning", "error", "critical", "future-level" } )
    {
        const std::string line = std::string( "[00:00:00.000][" ) + level + "][Desert]: text";
        EXPECT_EQ( ParseLogLine( line ).Severity, SeverityOfLevel( level ) ) << level;
    }
}

TEST( LogView, MessageWithoutATagGetsNoCategory )
{
    const auto p = ParseLogLine( "[16:55:20.037][trace][Desert]: VulkanRenderingContext::CreateVKInstance()" );
    EXPECT_TRUE( p.Parsed );
    EXPECT_TRUE( p.Category.empty() ); // never invented
    EXPECT_EQ( p.Message, "VulkanRenderingContext::CreateVKInstance()" );
}

TEST( LogView, ABracketedSentenceIsNotACategory )
{
    // A chip is a short subsystem name. A message that merely opens with a bracketed phrase must not put
    // that phrase in the chip -- the row would become a paragraph in a pill.
    const auto p = ParseLogLine( "[1][info][Desert]: [this is a long bracketed phrase] rest" );
    EXPECT_TRUE( p.Category.empty() );
    EXPECT_EQ( p.Message, "[this is a long bracketed phrase] rest" );
}

TEST( LogView, UnparsedLineKeepsItsWholeText )
{
    // A continuation line, or anything not written by the logger, is SHOWN IN FULL rather than dropped or
    // half-parsed: losing a line from a log is worse than showing it unstyled.
    const char* raw = "  VK_LAYER_KHRONOS_validation";
    const auto  p   = ParseLogLine( raw );
    EXPECT_FALSE( p.Parsed );
    EXPECT_EQ( p.Message, raw );
    EXPECT_TRUE( p.Time.empty() );
    EXPECT_EQ( p.Severity, LogSeverity::Info );

    // Bracket groups that are not the logger's header (no ": " terminator) also fall through whole.
    const auto q = ParseLogLine( "[a][b][c] no colon" );
    EXPECT_FALSE( q.Parsed );
    EXPECT_EQ( q.Message, "[a][b][c] no colon" );
}

TEST( LogView, ParseIsTotalOnDegenerateInput )
{
    for ( const char* line : { "", "[", "[]", "[][]", "[][][]", "[][][]:", "[][][]: " } )
    {
        const auto p = ParseLogLine( line );
        EXPECT_EQ( p.Severity, LogSeverity::Info ) << "line: '" << line << "'";
    }
}

// --- CollapseRepeats (RDG-FAULT1) --------------------------------------------------------------------
// THE DEFECT THIS PINS. An error logged every frame flooded the panel although Collapse was on: the timestamp
// made every line distinct. The relation under test: two lines that differ ONLY in time are one run.

TEST( LogView, CollapseRepeatsIgnoresTheTimestamp )
{
    const std::vector<std::pair<std::string, int>> lines = {
         { "[16:55:20.047][error][Desert]: [RDG] graph 'Scene' pass 'Glass' failed", 2 },
         { "[16:55:20.064][error][Desert]: [RDG] graph 'Scene' pass 'Glass' failed", 2 },
         { "[16:55:20.081][error][Desert]: [RDG] graph 'Scene' pass 'Glass' failed", 2 },
    };
    const auto runs = CollapseRepeats( lines );
    ASSERT_EQ( runs.size(), 1u );
    EXPECT_EQ( runs[0].Count, 3 );
    EXPECT_EQ( runs[0].FirstTime, "16:55:20.047" );
    EXPECT_EQ( runs[0].LastTime, "16:55:20.081" );
    EXPECT_EQ( runs[0].Text, lines[0].first );
}

TEST( LogView, CollapseRepeatsKeepsDifferentMessagesAndLevelsApart )
{
    const std::vector<std::pair<std::string, int>> lines = {
         { "[16:55:20.047][error][Desert]: pass 'Glass' failed", 2 },
         { "[16:55:20.064][warning][Desert]: pass 'Glass' failed", 1 }, // same message, other level
         { "[16:55:20.081][error][Desert]: pass 'Fog' failed", 2 },     // other message
         { "[16:55:20.098][error][Desert]: pass 'Glass' failed", 2 },   // not adjacent to the first
    };
    EXPECT_EQ( CollapseRepeats( lines ).size(), 4u );
}

TEST( LogView, CollapseRepeatsComparesAnUnparsedLineWhole )
{
    const auto runs = CollapseRepeats( { { "  continuation", 0 }, { "  continuation", 0 }, { "  other", 0 } } );
    ASSERT_EQ( runs.size(), 2u );
    EXPECT_EQ( runs[0].Count, 2 );
    EXPECT_TRUE( runs[0].FirstTime.empty() );
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
