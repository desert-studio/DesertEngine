// "THE HOST THE PLAYER RUNS HAS TO BE THE ONE WE CAN MEASURE."
//
// WHAT WAS MISSING, ON `dev` @ 514cee91. `Editor/Source/EditorLayer.cpp` timed twelve startup stages and
// logged each one; `Runtime/Source/RuntimeLayer.cpp` — the process a player actually starts — ran
// thirteen preload calls and a scene load as a flat sequence of statements, with one log line
// (`[Runtime] Scene loaded`) that arrives after all of it. `Docs/World/PROGRAMME.md` §0 makes "start-up
// time does not grow with the size of the map" one of four acceptance criteria, so the host the criterion
// was about was the host nobody could measure.
//
// WHY THE RULE IS A TYPE AND NOT A `chrono` PAIR IN EACH LAYER. The accumulation rule is not obvious and
// getting it wrong is silent: `ElapsedMs()` is the SUM OF THE STAGES, not wall clock between the first
// and the last, because the editor runs one stage per FRAME and wall clock there also counts every frame
// in between. Two copies of that rule would have produced two numbers that got compared anyway. So the
// rule lives once, in `Engine/Core/BootTimeline.hpp`, and both hosts use it.

#include "../SettingConsumers/setting_consumers_reader.hpp"

#include <Engine/Core/BootTimeline.hpp>

#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>
#include <thread>

#include <gtest/gtest.h>

namespace
{
    namespace fs = std::filesystem;
    using Desert::Core::BootTimeline;

    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            const std::ifstream probe( prefix + "Desert/Desert/Source/Engine/Core/BootTimeline.hpp" );
            if ( probe )
                return prefix;
            prefix += "../";
        }
        return {};
    }

    std::string ReadAll( const fs::path& path )
    {
        const std::ifstream in( path, std::ios::binary );
        std::ostringstream  buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }
    /// One message naming every offender. A FREE FUNCTION and not the `<< [&] { ... }()` lambda it
    /// replaces: a parameter-less multi-line lambda is the construct on which clang-format 18.1.3 (CI)
    /// and 18.1.8 (this machine) disagree, so the changed-lines gate can go red for code that is
    /// locally clean, and the repair people reach for is to hand-format until CI stops complaining.
    std::string Listing( const char* lead, const std::vector<std::string>& names )
    {
        std::string message = lead;
        for ( const std::string& name : names )
            message += "\n  " + name;
        return message;
    }
} // namespace

TEST( BootTimelineType, TheElapsedTotalIsTheSumOfTheStagesAndNotWallClock )
{
    BootTimeline boot( "Test" );
    boot.Record( "first", 100.0 );
    // The gap stands for the frames between two stages of the editor's per-frame scheduler. Wall clock
    // across this function would include it; the boot number must not, because the question it answers
    // is "which stage is spending the boot".
    std::this_thread::sleep_for( std::chrono::milliseconds( 30 ) );
    boot.Record( "second", 50.0 );

    EXPECT_DOUBLE_EQ( boot.ElapsedMs(), 150.0 );
    ASSERT_EQ( boot.Stages().size(), 2u );
}

TEST( BootTimelineType, RunTimesTheWorkAndKeepsItsResult )
{
    BootTimeline boot( "Test" );

    // THE RESULT HAS TO SURVIVE. Every scene-load stage in the runtime returns a `BoolResultStr` that
    // the caller then checks; a timing wrapper that swallowed it would turn a refused scene into a game
    // that starts on an empty world — the silent substitution the contract's §1.4 forbids, introduced by
    // a detector.
    const int answer = boot.Run( "with a result",
                                 []
                                 {
                                     std::this_thread::sleep_for( std::chrono::milliseconds( 12 ) );
                                     return 42;
                                 } );
    EXPECT_EQ( answer, 42 );

    // And a void stage must compile and record too — nine of the runtime's thirteen preloads return void.
    bool ran = false;
    boot.Run( "void", [&ran] { ran = true; } );
    EXPECT_TRUE( ran );

    ASSERT_EQ( boot.Stages().size(), 2u );
    EXPECT_GE( boot.Stages().front().Ms, 8.0 );
}

TEST( BootTimelineType, TheSlowestStageIsNamed )
{
    BootTimeline boot( "Test" );
    boot.Record( "cheap", 1.0 );
    boot.Record( "expensive", 3527.0 ); // the cloud-type preload's measured share of a 3908 ms boot
    boot.Record( "middling", 40.0 );
    EXPECT_EQ( boot.Slowest().Label, "expensive" );
    EXPECT_DOUBLE_EQ( boot.Slowest().Ms, 3527.0 );
}

TEST( BootTimelineType, AnEmptyTimelineHasAnAnswerRatherThanUndefinedBehaviour )
{
    // A host torn down before its first stage is a legitimate state, and it is the state that actually
    // produces an empty timeline. Reading `Stages().front()` there would be undefined behaviour in the
    // exact case worth reporting.
    const BootTimeline boot( "Test" );
    EXPECT_TRUE( boot.Stages().empty() );
    EXPECT_DOUBLE_EQ( boot.ElapsedMs(), 0.0 );
    EXPECT_TRUE( boot.Slowest().Label.empty() );
    EXPECT_DOUBLE_EQ( boot.Slowest().Ms, 0.0 );
}

// ── THE CENSUS: BOTH HOSTS ACTUALLY USE IT ─────────────────────────────────────────────────────────

TEST( BootStageTimingCensus, TheShippingRuntimeWrapsEveryBootContentCallInAStage )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    // DERIVED FROM BootContent.hpp, not from a list here — the same source `BootContentCensus` uses, and
    // for the same reason: a fourth boot function must be covered by existing, not by somebody remembering
    // this file. An unstaged preload is a phase of the player's boot that the log cannot name,
    // which is the entire defect.
    const std::string header = Desert::Tests::ConsumerText::StripComments(
         ReadAll( fs::path( root ) / "Desert/Desert/Source/Engine/Assets/BootContent.hpp" ) );
    ASSERT_FALSE( header.empty() );

    // A boot function either loads silently (void) or refuses the boot (BoolResultStr, CompileShaders with

    // no Default Surface).

    static const std::regex  declaration( R"(\b(?:void|Common::BoolResultStr)\s+(\w+)\s*\()" );
    std::vector<std::string> declared;
    for ( auto it = std::sregex_iterator( header.begin(), header.end(), declaration );
          it != std::sregex_iterator(); ++it )
    {
        declared.push_back( ( *it )[1].str() );
    }
    ASSERT_EQ( declared.size(), 3u ) << "the scan found " << declared.size() << " boot functions";

    const std::string layer = Desert::Tests::ConsumerText::StripComments(
         ReadAll( fs::path( root ) / "Runtime/Source/RuntimeLayer.cpp" ) );
    ASSERT_FALSE( layer.empty() );

    std::vector<std::string> unstaged;
    for ( const std::string& name : declared )
    {
        // The call has to appear INSIDE an `m_Boot.Run(` argument list. Checked by looking for the call
        // and then walking back to the nearest statement boundary — crude, and conservative in the safe
        // direction: a staged call written some other way reads as unstaged and the author has to say so.
        const std::size_t call = layer.find( "Assets::" + name + "(" );
        if ( call == std::string::npos )
        {
            unstaged.push_back( name + " (never called at all)" );
            continue;
        }
        const std::size_t lineStart = layer.rfind( ';', call ) == std::string::npos ? 0 : layer.rfind( ';', call );
        if ( layer.find( "m_Boot.Run", lineStart ) == std::string::npos ||
             layer.find( "m_Boot.Run", lineStart ) > call )
        {
            unstaged.push_back( name );
        }
    }

    EXPECT_TRUE( unstaged.empty() ) << Listing(
         "these boot functions are not wrapped in a timed stage in the shipping runtime:", unstaged );
}

TEST( BootStageTimingCensus, BothHostsUseTheOneAccumulationRule )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    // A host is the set of files that make it up: the editor's boot moved out of EditorLayer.cpp into
    // LevelEditor/EditorStartup.cpp (EDL-7), and the rule is about the host, not about one file name.
    // ONE RULE PER HOST, NEVER TWO: the Runtime times its boot with BootTimeline (m_Boot) and prints its
    // summary; the editor's boot is the phases of Core::StartupProgress, whose Finish prints the summary
    // (STARTUP-PROGRESS), and the editor keeps no BootTimeline next to it.
    struct Host
    {
        const char*              Name;
        std::vector<const char*> Files;
        std::vector<const char*> Required;  // what the host's one rule is written with
        std::vector<const char*> Forbidden; // the other rule, which must not sit next to it
    };
    const Host hosts[] = {
         { "the editor (EditorLayer.cpp + LevelEditor/EditorStartup.cpp)",
           { "Editor/Source/EditorLayer.cpp", "Editor/Source/Editor/LevelEditor/EditorStartup.cpp" },
           { "m_Startup->Finish" },
           { "m_Boot" } },
         { "Runtime/Source/RuntimeLayer.cpp",
           { "Runtime/Source/RuntimeLayer.cpp" },
           { "m_Boot", "m_Boot.LogSummary" },
           {} },
    };
    for ( const Host& host : hosts )
    {
        const char* layer = host.Name;
        std::string text;
        for ( const char* file : host.Files )
        {
            const std::string part =
                 Desert::Tests::ConsumerText::StripComments( ReadAll( fs::path( root ) / file ) );
            ASSERT_FALSE( part.empty() ) << "could not read " << file;
            text += part;
            text += '\n';
        }
        for ( const char* token : host.Required )
            EXPECT_NE( text.find( token ), std::string::npos )
                 << layer << " has no '" << token
                 << "': its boot is not timed by its one rule, or never prints the per-stage summary";
        for ( const char* token : host.Forbidden )
            EXPECT_EQ( text.find( token ), std::string::npos )
                 << layer << " still has '" << token << "' next to its own startup timing: two timing rules";
    }

    // AND THE OLD ACCUMULATOR IS GONE, not merely unused. The contract forbids keeping the replaced path:
    // a second elapsed total that nothing updates is a number a reader will quote.
    for ( const char* header :
          { "Editor/Source/EditorLayer.hpp", "Editor/Source/Editor/LevelEditor/EditorStartup.hpp" } )
    {
        const std::string editorHeader =
             Desert::Tests::ConsumerText::StripComments( ReadAll( fs::path( root ) / header ) );
        ASSERT_FALSE( editorHeader.empty() ) << "could not read " << header;
        EXPECT_EQ( editorHeader.find( "m_StartupElapsedMs" ), std::string::npos )
             << header << ": the editor's old elapsed accumulator is still declared alongside the shared one";
    }
}
