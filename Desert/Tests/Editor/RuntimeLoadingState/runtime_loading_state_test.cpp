// THE STATE THAT KEEPS A HALF-READ WORLD OFF A PLAYER'S SCREEN, asserted where a compiler cannot look.
//
// Demand-driven loading (В6) took start-up from 3347.5 ms to 310.5 ms by moving the read of the world's
// content out of the boot and into the first frame that asks for it. The editor already had a loading
// overlay and simply kept it up; the SHIPPING host -- the process a player starts, on the platform this
// engine actually targets -- got the pump and a log line and no state at all, so its first frames could
// present a world whose sky had not landed. A volumetric cloud with no volume falls back to a
// PROCEDURAL sky, which is a picture and not an error: nothing is logged, nothing looks broken, and the
// only person who can tell is the one who authored the sky. Measured, on this tree, at frame 1 of
// Clouds_HeroTrio: with the state removed the frame is a cloudless sky (Docs/World/Shots/B7).
//
// Four of the five relations below are between a type in Engine/Assets and call sites in two .cpp files
// that no header includes. A unit test cannot see them and neither can the compiler.
//
//   1. the runtime's gate is constructed SHUT -- the one that defaults the wrong way presents the frames
//   2. the scene blit sits INSIDE the branch on that gate -- a gate nothing consults is a log line
//   3. both hosts tick it with BOTH counters -- the argument list is the second condition
//   4. neither host kept a copy of the rule -- one implementation, or one of them loses a condition
//   5. a level switch re-arms it -- otherwise the defect returns at the first door
//
// The fifth group tests the capture flags as code: they are the only instrument that can photograph the
// shipping host at all, and an unattended capture that silently does not happen leaves a windowed game
// running with nobody watching it.

#include <RuntimeShot.hpp>

#include <gtest/gtest.h>

#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    constexpr const char* kGateHeader    = "Desert/Desert/Source/Engine/Assets/ContentGate.hpp";
    constexpr const char* kRuntimeLayer  = "Runtime/Source/RuntimeLayer.cpp";
    constexpr const char* kRuntimeHeader = "Runtime/Source/RuntimeLayer.hpp";
    constexpr const char* kEditorLayer   = "Editor/Source/EditorLayer.cpp";

    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            const std::ifstream probe( prefix + kGateHeader );
            if ( probe )
                return prefix;
            prefix += "../";
        }
        return {};
    }

    std::string ReadFile( const std::string& path )
    {
        const std::ifstream in( path, std::ios::binary );
        if ( !in )
            return {};
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    /// The source with its comments blanked out, so a check about CODE cannot be satisfied by prose.
    /// THIS FILE IS THE PROOF THAT IT MATTERS: the block at the top quotes the very things it forbids,
    /// and two censuses in one week went red on their own explanations -- one of them drowning a real
    /// finding in the same run.
    std::string WithoutComments( const std::string& source )
    {
        std::string out;
        out.reserve( source.size() );
        bool inLine  = false;
        bool inBlock = false;
        for ( size_t i = 0; i < source.size(); ++i )
        {
            if ( inLine )
            {
                if ( source[i] == '\n' )
                {
                    inLine = false;
                    out.push_back( '\n' );
                }
                continue;
            }
            if ( inBlock )
            {
                if ( source[i] == '*' && i + 1 < source.size() && source[i + 1] == '/' )
                {
                    inBlock = false;
                    ++i;
                }
                else if ( source[i] == '\n' )
                {
                    out.push_back( '\n' );
                }
                continue;
            }
            if ( source[i] == '/' && i + 1 < source.size() && source[i + 1] == '/' )
            {
                inLine = true;
                continue;
            }
            if ( source[i] == '/' && i + 1 < source.size() && source[i + 1] == '*' )
            {
                inBlock = true;
                ++i;
                continue;
            }
            out.push_back( source[i] );
        }
        return out;
    }

    /// The braced block that follows the first occurrence of @p opener, by brace matching. An opener
    /// that is not found returns empty, which makes the assertions red rather than green.
    std::string BlockAfter( const std::string& source, const std::string& opener )
    {
        const size_t at = source.find( opener );
        if ( at == std::string::npos )
            return {};
        const size_t open = source.find( '{', at + opener.size() );
        if ( open == std::string::npos )
            return {};
        int    depth = 0;
        size_t i     = open;
        for ( ; i < source.size(); ++i )
        {
            if ( source[i] == '{' )
                ++depth;
            else if ( source[i] == '}' && --depth == 0 )
                break;
        }
        return source.substr( open, i - open );
    }

    /// The body of a free-standing `Class::Name( ... ) { ... }`.
    std::string FunctionBody( const std::string& source, const std::string& name )
    {
        const std::regex pattern( R"(::)" + name + R"(\s*\()" );
        std::smatch      match;
        if ( !std::regex_search( source, match, pattern ) )
            return {};
        return BlockAfter( source.substr( match.position() ), ")" );
    }
} // namespace

TEST( RuntimeLoadingState, TheRootIsFindable )
{
    ASSERT_FALSE( RepoRoot().empty() )
         << "the census could not find " << kGateHeader
         << " from the working directory, so every check below would pass on an empty string.";
}

TEST( RuntimeLoadingState, TheShippingHostConstructsItsGateShut )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    const std::string runtime = WithoutComments( ReadFile( root + kRuntimeHeader ) );
    ASSERT_FALSE( runtime.empty() ) << kRuntimeHeader << " could not be read";

    const std::regex shut( R"(ContentGate\s+m_Content\s*\{\s*Assets::ContentState::Loading\s*\})" );
    EXPECT_TRUE( std::regex_search( runtime, shut ) )
         << kRuntimeHeader
         << " does not construct its ContentGate in the Loading state. This process exists in order to "
            "read a world and a player is looking at its window from the first frame, so a gate that "
            "starts open presents exactly the frames it was added to cover -- and the window between "
            "construction and the first BeginWorld is not a window anybody would think to test.";
}

TEST( RuntimeLoadingState, TheSceneBlitSitsInsideTheBranchOnTheGate )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    const std::string runtime = WithoutComments( ReadFile( root + kRuntimeLayer ) );
    ASSERT_FALSE( runtime.empty() ) << kRuntimeLayer << " could not be read";

    // The one place the scene's final image reaches the swapchain.
    const std::regex blit( R"(SubmitFullscreenQuad\s*\()" );
    const auto       begin = std::sregex_iterator( runtime.begin(), runtime.end(), blit );
    const auto       count = std::distance( begin, std::sregex_iterator() );
    ASSERT_EQ( count, 1 ) << "there are " << count << " fullscreen blits in " << kRuntimeLayer
                          << "; this census pins ONE, so a second one would be an unguarded path to the "
                             "swapchain that it silently stopped covering.";

    ASSERT_NE( runtime.find( "const bool loading = m_Content.Loading();" ), std::string::npos )
         << kRuntimeLayer
         << " no longer asks the gate for the frame's verdict. The gate is then a state nothing reads, "
            "which is what the log marker it replaced already was.";

    const std::string guarded = BlockAfter( runtime, "if ( !loading )" );
    ASSERT_FALSE( guarded.empty() ) << kRuntimeLayer << " has no `if ( !loading )` block at all";
    EXPECT_NE( guarded.find( "SubmitFullscreenQuad" ), std::string::npos )
         << "the scene blit is no longer inside the `if ( !loading )` block of " << kRuntimeLayer
         << ". That single branch is the whole mechanism: with it removed, frame 1 of Clouds_HeroTrio is "
            "a cloudless procedural sky presented to the player, and nothing anywhere says so.";
}

TEST( RuntimeLoadingState, GameplayTimeIsFrozenWhileTheGateIsShut )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    const std::string runtime = WithoutComments( ReadFile( root + kRuntimeLayer ) );
    ASSERT_FALSE( runtime.empty() );

    const std::regex frozen( R"(m_Scene->OnUpdate\(\s*m_Content\.Loading\(\)\s*\?)" );
    EXPECT_TRUE( std::regex_search( runtime, frozen ) )
         << kRuntimeLayer
         << " advances the scene with the wall-clock timestep while the loading screen is up. The "
            "player's first visible frame is then already several frames into the game -- physics "
            "stepped, every script's OnUpdate called -- against a world they could not be seen reacting "
            "to. The render itself must still run: the render is what ASKS.";
}

TEST( RuntimeLoadingState, BothHostsTickTheGateWithBothCounters )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    // THE ARGUMENT LIST IS THE SECOND CONDITION. Asserted on the values passed and not on a name, for
    // the reason AsyncAssetPump records next door: the obvious spelling of a check like this reads a
    // COMMENT, which WithoutComments has already deleted.
    const std::regex tick( R"(m_Content\.Tick\(\s*loader\.Outstanding\(\)\s*,\s*loader\.StartedCount\(\)\s*\))" );

    for ( const char* layer : { kEditorLayer, kRuntimeLayer } )
    {
        const std::string source = WithoutComments( ReadFile( root + layer ) );
        ASSERT_FALSE( source.empty() ) << layer << " could not be read";
        EXPECT_TRUE( std::regex_search( source, tick ) )
             << layer
             << " does not tick its ContentGate with both of the loader's counters. Outstanding() alone "
                "is the one-condition gate: a queue can be empty in the GAP between two links of a chain "
                "-- a cloud type has landed and the frame that will ask for its volume has not run yet -- "
                "and a gate that opened there hands over a world one link short.";
    }
}

TEST( RuntimeLoadingState, NeitherHostKeepsItsOwnCopyOfTheRule )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    // The runtime's marker used to be a hand-copied half of the editor's rule, living inline in
    // OnUpdate. Both spellings of the first condition are forbidden in a host: the rule has one home.
    const std::regex reimplemented( R"(Outstanding\(\)\s*==\s*0)" );

    for ( const char* layer : { kEditorLayer, kRuntimeLayer } )
    {
        const std::string source = WithoutComments( ReadFile( root + layer ) );
        ASSERT_FALSE( source.empty() ) << layer << " could not be read";
        EXPECT_FALSE( std::regex_search( source, reimplemented ) )
             << layer
             << " tests the loader's queue itself. That is a second copy of a two-condition rule, and the "
                "way this defect was shipped the first time was one host holding a copy that had lost the "
                "second condition -- and said so only to the log.";
    }
}

TEST( RuntimeLoadingState, ALevelSwitchRearmsTheShippingHostsGate )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    const std::string runtime = WithoutComments( ReadFile( root + kRuntimeLayer ) );
    ASSERT_FALSE( runtime.empty() );

    const std::string body = FunctionBody( runtime, "LoadSceneInternal" );
    ASSERT_FALSE( body.empty() ) << "LoadSceneInternal was not found in " << kRuntimeLayer;
    EXPECT_NE( body.find( "m_Content.BeginWorld(" ), std::string::npos )
         << "a scene switch in " << kRuntimeLayer
         << " does not re-arm the loading gate. A door opened at run time hands over a second world whose "
            "clouds and layouts are read on demand exactly like the first one's, so a gate that could "
            "only close at boot ships the defect straight back into the game.";
}

// ===== The capture flags: the only instrument that can photograph the shipping host =====

TEST( RuntimeLoadingState, CaptureIsOffUnlessAskedFor )
{
    Desert::Player::RuntimeShot shot;
    EXPECT_FALSE( shot.Active() ) << "a game that was not asked for a screenshot would take one";
    ASSERT_TRUE( Desert::Player::ParseRuntimeShot( { "--project", "Game.deproj" }, shot ) );
    EXPECT_FALSE( shot.Active() );
}

TEST( RuntimeLoadingState, CaptureFlagsAreParsed )
{
    Desert::Player::RuntimeShot shot;
    ASSERT_TRUE( Desert::Player::ParseRuntimeShot(
         { "--project", "Game.deproj", "--shot", "out/frame.png", "--shot-frames", "5" }, shot ) );
    EXPECT_TRUE( shot.Active() );
    EXPECT_EQ( shot.Output, "out/frame.png" );
    EXPECT_EQ( shot.Frames, 5u );
}

TEST( RuntimeLoadingState, AFlagUsedWronglyIsRefusedByName )
{
    // REFUSED AND NOT IGNORED. An unattended capture that silently does not happen leaves a windowed
    // game running on a machine nobody is looking at, and the harness that started it waits for a file
    // that will never appear -- which reads as a hang in the thing being measured.
    Desert::Player::RuntimeShot shot;
    const auto                  noPath = Desert::Player::ParseRuntimeShot( { "--shot" }, shot );
    EXPECT_FALSE( noPath );

    const auto swallowedFlag = Desert::Player::ParseRuntimeShot( { "--shot", "--scene", "a.desce" }, shot );
    EXPECT_FALSE( swallowedFlag ) << "'--shot --scene a.desce' took '--scene' as the output path";

    const auto notANumber = Desert::Player::ParseRuntimeShot( { "--shot-frames", "many" }, shot );
    EXPECT_FALSE( notANumber );

    const auto zero = Desert::Player::ParseRuntimeShot( { "--shot-frames", "0" }, shot );
    EXPECT_FALSE( zero ) << "frames are counted from 1, so frame 0 can never be captured";
}

TEST( RuntimeLoadingState, TheCaptureReadsTheSwapchainAndNotTheScenesOwnImage )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    const std::string runtime = WithoutComments( ReadFile( root + kRuntimeLayer ) );
    ASSERT_FALSE( runtime.empty() );

    EXPECT_NE( runtime.find( "TakeCapturedFrameRGBA8" ), std::string::npos )
         << kRuntimeLayer
         << " does not collect its capture from the swapchain. The loading screen is drawn by Render2D "
            "straight into the swapchain pass; Scene::GetFinalImage() -- what the editor's --shot reads "
            "-- holds the WORLD and none of what is drawn over it, so a capture through it would "
            "photograph precisely the picture this host refuses to present and call it the frame.";

    EXPECT_EQ( runtime.find( "GetFinalImage" ), runtime.rfind( "GetFinalImage" ) )
         << kRuntimeLayer << " reads the scene's final image in more than one place; the blit is the one.";
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
