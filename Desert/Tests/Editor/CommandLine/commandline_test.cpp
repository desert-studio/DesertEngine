#include <sstream>

#include <gtest/gtest.h>

#include <DesertShared/LaunchProtocol.hpp>
#include <Editor/Core/CommandLine.hpp>

#include <string>
#include <vector>

using namespace Desert::Editor;

namespace
{
    // The command line as the shell hands it over, minus argv[0].
    using Args = std::vector<std::string>;

    CommandLineOptions ParseOk( const Args& args )
    {
        auto result = ParseCommandLine( args );
        EXPECT_TRUE( result.IsSuccess() ) << "expected a parse, got: " << result.GetError();
        return result.GetValue();
    }

    std::string ParseError( const Args& args )
    {
        auto result = ParseCommandLine( args );
        EXPECT_FALSE( result.IsSuccess() ) << "expected a NAMED failure, but the arguments parsed";
        return result.GetError();
    }
} // namespace

// ── Defect 1: an argument that is taken and dropped ──────────────────────────────────────────────
//
// This is the whole reason the parser is a function. `--sceen` is one transposed letter, and under the
// old loop it fell off the end of an if/else chain without a word: the editor loaded the PROJECT'S
// DEFAULT scene, rendered it, wrote a plausible PNG under the name the caller chose, and exited 0.
// Measured on the unmodified binary before this change — the log said "Loading scene: Desert Sandbox"
// and the flag was never mentioned once.
//
// The assertion is not merely "it fails". It is that the message NAMES THE TOKEN, because an error that
// says only "bad arguments" sends the reader back to the same guessing the silence did.
TEST( CommandLine, AMistypedFlagIsRejectedAndNamed )
{
    const std::string error = ParseError( { "--sceen", "Clouds_Demo.desce" } );
    EXPECT_NE( error.find( "--sceen" ), std::string::npos ) << error;
}

TEST( CommandLine, AnUnknownFlagListsTheOnesThatExist )
{
    const std::string error = ParseError( { "--not-a-flag" } );
    // The list is generated from the flag table, so this also pins that the table reaches the message.
    EXPECT_NE( error.find( "--scene" ), std::string::npos ) << error;
    EXPECT_NE( error.find( "--shot-frames" ), std::string::npos ) << error;
}

// A bare word is as silent a loss as a mistyped flag: `--shot out.png Clouds_Demo.desce` reads like it
// names a scene and never did.
TEST( CommandLine, APositionalTokenIsRejected )
{
    const std::string error = ParseError( { "Clouds_Demo.desce" } );
    EXPECT_NE( error.find( "Clouds_Demo.desce" ), std::string::npos ) << error;
}

// ── The value that never arrived ─────────────────────────────────────────────────────────────────
//
// The old loop guarded every value-taking flag with `i + 1 < argc` and, when that was false, simply let
// the token fall through every branch. A command line ending in `--scene` therefore captured the default
// scene, and one ending in `--shot-frames` captured at the default 90 — both in silence.
TEST( CommandLine, AFlagWrittenLastWithNoValueIsRejected )
{
    for ( const CommandLineFlag& flag : kCommandLineFlags )
    {
        if ( !flag.TakesValue )
            continue;

        const std::string error = ParseError( { flag.Name } );
        EXPECT_NE( error.find( flag.Name ), std::string::npos ) << error;
    }
}

// ── Malformed values ─────────────────────────────────────────────────────────────────────────────
//
// `sscanf("%f,%f,%f")` returns 3 for "0,200,0,7" and stops caring about the rest, and it returns 2 for
// "0,200" — which the old code turned into `HasCamera = false || HasCamera`, i.e. nothing at all. The
// camera then sat at the scene's own pose and the frame was of somewhere else entirely.
TEST( CommandLine, AVectorThatIsNotThreeNumbersIsRejected )
{
    const char* bad[] = {
         "0,200",       // too few — the old parser's silent no-op
         "0,200,0,7",   // too many
         "0,200,x",     // not a number
         "0,200,0.5abc" // parses in PART, which is the one strtof alone would accept
    };

    for ( const char* value : bad )
    {
        const std::string error = ParseError( { "--camera", value } );
        EXPECT_NE( error.find( value ), std::string::npos ) << error;
    }
}

// A camera at infinity is not a camera. These parse cleanly as floats and produce a frame of one flat
// colour, which reads as a broken renderer rather than as the bad argument it is.
TEST( CommandLine, ANonFiniteVectorComponentIsRejected )
{
    EXPECT_FALSE( ParseCommandLine( { "--camera", "0,inf,0" } ).IsSuccess() );
    EXPECT_FALSE( ParseCommandLine( { "--look", "0,nan,-1" } ).IsSuccess() );
}

// `atoi` answers 0 for "abc" and for "90x" alike, so a mistyped frame count silently became "capture on
// the first frame" — a picture of the dither, which for a volumetric scene looks like a real defect.
TEST( CommandLine, AFrameCountThatIsNotAWholeNumberIsRejected )
{
    const char* bad[] = { "abc", "90x", "0", "-5", "9.5", "" };
    for ( const char* value : bad )
    {
        const std::string error = ParseError( { "--shot-frames", value } );
        EXPECT_FALSE( error.empty() ) << "value: " << value;
    }

    EXPECT_EQ( ParseOk( { "--shot-frames", "90" } ).Shot.Frames, 90 );
    EXPECT_EQ( ParseOk( { "--shot-frames", "1" } ).Shot.Frames, 1 );
}

TEST( CommandLine, AShotIntervalBelowOneIsRejected )
{
    EXPECT_FALSE( ParseCommandLine( { "--shot-every", "0" } ).IsSuccess() );
    EXPECT_FALSE( ParseCommandLine( { "--shot-every", "-1" } ).IsSuccess() );
    EXPECT_EQ( ParseOk( { "--shot-every", "3" } ).Shot.SequenceEvery, 3 );
}

// ── The relation between the flag table and the parser ───────────────────────────────────────────
//
// The table drives the "known flags" message and the missing-value check; the parser's if/else chain
// stores the values. Two lists that must agree is exactly the shape this project keeps getting wrong, so
// the agreement is asserted rather than maintained by hand: a flag added to the table but never stored
// would still be reported as known, and a caller would be told it works.
TEST( CommandLine, EveryFlagInTheTableIsAcceptedByTheParser )
{
    for ( const CommandLineFlag& flag : kCommandLineFlags )
    {
        Args args{ flag.Name };
        if ( flag.TakesValue )
        {
            ASSERT_NE( flag.ExampleValue, nullptr ) << flag.Name << " takes a value but offers no example";
            args.emplace_back( flag.ExampleValue );
        }
        // A flag that cannot stand alone says so in the table, and the words are a command line. Without
        // this the test would be red for the one flag behaving correctly — see CommandLineFlag::AlsoNeeds.
        if ( flag.AlsoNeeds != nullptr )
        {
            std::istringstream extra( flag.AlsoNeeds );
            for ( std::string word; extra >> word; )
                args.push_back( word );
        }

        const auto result = ParseCommandLine( args );
        EXPECT_TRUE( result.IsSuccess() ) << flag.Name << ": " << result.GetError();
    }
}

// ── What must NOT have changed ───────────────────────────────────────────────────────────────────
//
// Every capture in the repository was taken through the old loop. The values this parser resolves have
// to be the same values, on the exact float — a hundredth of a degree of pan is a different picture, and
// a different picture invalidates the comparisons already recorded against it. EQ, not NEAR.
TEST( CommandLine, TheDocumentedCaptureCommandResolvesToExactlyItsOldValues )
{
    const CommandLineOptions options =
         ParseOk( { "--project", "Desert.deproj", "--scene", "Resources/Assets/Scenes/Clouds_Demo.desce", "--shot",
                    "/tmp/out.png", "--shot-frames", "90", "--camera", "0,200,0", "--look", "0,0.9,-1" } );

    EXPECT_EQ( options.Project, "Desert.deproj" );
    EXPECT_EQ( options.Shot.Scene, "Resources/Assets/Scenes/Clouds_Demo.desce" );
    EXPECT_EQ( options.Shot.Output, "/tmp/out.png" );
    EXPECT_EQ( options.Shot.Frames, 90 );
    EXPECT_TRUE( options.Shot.HasCamera );
    EXPECT_EQ( options.Shot.Position, glm::vec3( 0.0f, 200.0f, 0.0f ) );
    EXPECT_EQ( options.Shot.Forward, glm::vec3( 0.0f, 0.9f, -1.0f ) );
    EXPECT_FALSE( options.Shot.HasMotion() );
    EXPECT_TRUE( options.Shot.Active() );
}

// The Editor's half of the shared launch protocol (desert-shared LaunchProtocol.hpp): the launcher
// composes its command from these constants, so the parser accepting them is a RELATION between two
// repositories, not a spelling — and after the launcher moves out (L3), this test is the only thing
// on this side holding it.
TEST( CommandLine, TheLaunchProtocolFlagIsAccepted )
{
    const CommandLineOptions options = ParseOk( { Common::Launch::kProjectFlag, "MyGame.deproj" } );
    EXPECT_EQ( options.Project, "MyGame.deproj" );
}

// An empty command line is a plain interactive launch, and it must resolve to the built-in defaults with
// no complaint — the editor is started this way by hand every day.
TEST( CommandLine, NoArgumentsIsNotAnError )
{
    const CommandLineOptions options = ParseOk( {} );

    EXPECT_TRUE( options.Project.empty() );
    EXPECT_FALSE( options.Shot.Active() );
    EXPECT_FALSE( options.Shot.HasCamera );
    EXPECT_EQ( options.Shot.Frames, 90 );
    EXPECT_TRUE( options.ControlSocket.empty() ) << "an editor nobody asked to listen must not listen";
}

// The path flags imply --camera, because a path that nothing places is a path the scene's own camera
// ignores. Carried over from the old loop deliberately, so it is pinned here.
TEST( CommandLine, APathEndpointImpliesAPlacedCamera )
{
    const CommandLineOptions toPosition = ParseOk( { "--camera-to", "0,200,-100" } );
    EXPECT_TRUE( toPosition.Shot.HasPositionTo );
    EXPECT_TRUE( toPosition.Shot.HasCamera );
    EXPECT_TRUE( toPosition.Shot.HasMotion() );

    const CommandLineOptions toForward = ParseOk( { "--look-to", "0,0.5,-1" } );
    EXPECT_TRUE( toForward.Shot.HasForwardTo );
    EXPECT_TRUE( toForward.Shot.HasCamera );
}

// Order independence, which the endpoint flags depend on: `--camera-to` before `--camera` must still end
// at the position `--camera` names, not at the default it was holding when parsed.
TEST( CommandLine, TheEndpointFlagsDoNotDependOnTheOrderTheyAreWrittenIn )
{
    const CommandLineOptions before =
         ParseOk( { "--camera-to", "10,20,30", "--camera", "1,2,3", "--shot-frames", "2" } );
    const CommandLineOptions after =
         ParseOk( { "--camera", "1,2,3", "--camera-to", "10,20,30", "--shot-frames", "2" } );

    EXPECT_EQ( before.Shot.CameraAt( 0.0f ).Position, after.Shot.CameraAt( 0.0f ).Position );
    EXPECT_EQ( before.Shot.CameraAt( 1.0f ).Position, after.Shot.CameraAt( 1.0f ).Position );
    EXPECT_EQ( before.Shot.CameraAt( 1.0f ).Position, glm::vec3( 10.0f, 20.0f, 30.0f ) );
}

// THE FOUR CONTROL FLAGS ARE GONE, AND THIS IS WHERE THEIR ABSENCE IS ASSERTED.
//
// `--open-panel`, `--select`, `--open-menu` and `--preview-orbit` each existed for one reason: macOS
// refuses this machine synthetic input, so a panel, a selection, a menu or a preview angle could not be
// reached by a click. Each could be spent exactly ONCE, at boot, because that is all a flag can do — and
// every task that needed a different window added another flag beside it.
//
// The control channel replaces the whole family with a session (`--control-socket`), running the command
// palette's own entries at any moment and as many times as asked. Keeping the flags beside it would have
// been two ways to open a panel: the defect shape this project spends its days removing, whose losing side
// is always the one nobody remembers to update.
//
// They must therefore be REJECTED, not ignored. A retired flag that parsed to nothing would be the silent
// no-op this entire file exists to forbid — and worse than the general case, because a script that still
// passed `--select Sun` would boot, render, and write a plausible PNG with nothing selected.
TEST( CommandLine, TheRetiredControlFlagsAreRejectedRatherThanIgnored )
{
    for ( const char* retired : { "--open-panel", "--open-menu", "--select", "--preview-orbit" } )
    {
        const auto result = ParseCommandLine( { retired, "Details" } );
        ASSERT_FALSE( result.IsSuccess() ) << retired << " still parses";
        EXPECT_NE( result.GetError().find( retired ), std::string::npos )
             << "the refusal must name the flag that was passed";
        EXPECT_NE( result.GetError().find( "--control-socket" ), std::string::npos )
             << "and point at what replaced it, since the list of known flags is what it prints";
    }
}

// `--control-socket <path>` is the one flag that arrived with the channel. Empty by default, and empty is
// load-bearing: an editor that listened without being asked would be a hole, because the channel can run
// every entry the palette offers — including saving over the user's scene.
TEST( CommandLine, TheControlSocketIsOffUnlessNamed )
{
    EXPECT_TRUE( ParseOk( {} ).ControlSocket.empty() );
    EXPECT_EQ( ParseOk( { "--control-socket", "/tmp/desert-a6.sock" } ).ControlSocket, "/tmp/desert-a6.sock" );

    // Written last with its value forgotten — the shape that used to drop a flag entirely.
    EXPECT_FALSE( ParseCommandLine( { "--control-socket" } ).IsSuccess() );
}

TEST( CommandLine, TheValuelessFlagsSetTheirOwnField )
{
    EXPECT_TRUE( ParseOk( { "--play" } ).Shot.Play );
    EXPECT_TRUE( ParseOk( { "--gpu-profile" } ).Shot.GpuProfile );
    EXPECT_TRUE( ParseOk( { "--gpu-profile-frame-only" } ).Shot.GpuFrameOnly );
    EXPECT_FALSE( ParseOk( { "--no-gpu-timing" } ).Shot.GpuTiming );

    // ...and are not confused for a flag that consumes the next token: `--play --scene X` must still
    // load X, which is how every --play capture in the programme is written.
    const CommandLineOptions options = ParseOk( { "--play", "--scene", "X.desce" } );
    EXPECT_TRUE( options.Shot.Play );
    EXPECT_EQ( options.Shot.Scene, "X.desce" );
}

// ── The capture precondition: "capture is active" against "the scene is there" ───────────────────
//
// The relation that IS defect 1. The scene loader logs a missing file and leaves the current scene
// standing — right for an editor, wrong for a capture, where the run goes on to write a PNG named after
// the scene that was asked for holding the picture of a different one.
TEST( CommandLine, AMissingSceneIsFatalToACaptureAndNamesThePath )
{
    const CommandLineOptions options = ParseOk( { "--scene", "Gone.desce", "--shot", "/tmp/out.png" } );
    ASSERT_TRUE( options.Shot.Active() );

    const auto verdict = ValidateSceneForCapture( options.Shot, /*sceneExists=*/false );
    EXPECT_FALSE( verdict.IsSuccess() );
    EXPECT_NE( verdict.GetError().find( "Gone.desce" ), std::string::npos ) << verdict.GetError();
}

TEST( CommandLine, ASceneThatIsThereLetsTheCaptureProceed )
{
    const CommandLineOptions options = ParseOk( { "--scene", "Here.desce", "--shot", "/tmp/out.png" } );
    EXPECT_TRUE( ValidateSceneForCapture( options.Shot, /*sceneExists=*/true ).IsSuccess() );
}

// A sequence-only run is a capture too — a motion study wants the frames and has no use for a designated
// last one — so it must be held to the same rule. `Active()` is what says so, and this pins it.
TEST( CommandLine, ASequenceOnlyRunIsACaptureAndIsHeldToTheSameRule )
{
    const CommandLineOptions options = ParseOk( { "--scene", "Gone.desce", "--shot-sequence", "/tmp/seq" } );
    ASSERT_TRUE( options.Shot.Active() );
    EXPECT_FALSE( ValidateSceneForCapture( options.Shot, /*sceneExists=*/false ).IsSuccess() );
}

// Interactive `--scene` keeps the editor's own behaviour: there is a person here and the loader's message
// reaches them. Pinned so that tightening the capture rule never quietly kills the interactive one.
TEST( CommandLine, AMissingSceneWithoutACaptureIsNotFatal )
{
    const CommandLineOptions options = ParseOk( { "--scene", "Gone.desce" } );
    ASSERT_FALSE( options.Shot.Active() );
    EXPECT_TRUE( ValidateSceneForCapture( options.Shot, /*sceneExists=*/false ).IsSuccess() );
}

TEST( CommandLine, WithNoSceneFlagTheRuleSaysNothing )
{
    const CommandLineOptions options = ParseOk( { "--shot", "/tmp/out.png" } );
    EXPECT_TRUE( ValidateSceneForCapture( options.Shot, /*sceneExists=*/false ).IsSuccess() );
}

// ---------------------------------------------------------------------------------------------------
// IS ANYBODY SITTING AT THIS EDITOR?
//
// Three consequences used to ask `shot.Active()` and mean this: the recent-projects registry, the
// engine-install registry, and the project tile written on exit. Their own comments are all about
// throwaway worktrees and none about capture — so a CONTROL-CHANNEL run, which is the unattended path
// that no longer needs `--shot` at all, escaped every one of them and filed a worktree that would be
// reclaimed within the hour in the developer's registries. A capture flag must not be the switch for
// things that are not capture (A6-1 point 5).
// ---------------------------------------------------------------------------------------------------

TEST( CommandLine, AControlChannelRunIsUnattendedEvenWithNoCaptureFlagAtAll )
{
    const CommandLineOptions options = ParseOk( { "--control-socket", "/tmp/desert.sock" } );

    ASSERT_FALSE( options.Shot.Active() ) << "this run takes no capture; that is the whole point of it";
    EXPECT_TRUE( IsUnattendedSession( options.Shot, !options.ControlSocket.empty() ) );
}

// The two originals still count, including `--shot-sequence` alone — a motion study is as unattended as
// a single capture, and it was already the case the capture flag covered.
TEST( CommandLine, EitherFlavourOfCaptureIsStillUnattended )
{
    for ( const Args& argv : { Args{ "--shot", "/tmp/out.png" }, Args{ "--shot-sequence", "/tmp/seq" } } )
    {
        const CommandLineOptions options = ParseOk( argv );
        EXPECT_TRUE( IsUnattendedSession( options.Shot, !options.ControlSocket.empty() ) );
    }
}

// AND A PERSON'S SESSION IS STILL A PERSON'S SESSION. This is the direction that would go wrong quietly:
// an over-eager predicate would stop filing real projects in the recent list and stop registering the
// engine the launcher needs, and nothing would say so until somebody opened the launcher.
TEST( CommandLine, AnOrdinaryInteractiveRunIsNotUnattended )
{
    for ( const Args& argv :
          { Args{}, Args{ "--project", "Desert.deproj" },
            Args{ "--scene", "Resources/Assets/Scenes/Starter.desce" },
            // `--camera`/`--look` without a capture: a pose asked for by a person, which is exactly the
            // case that used to be unreachable and is now the control channel's `set Camera.Position`.
            Args{ "--camera", "0,200,0", "--look", "0,0,-1" } } )
    {
        const CommandLineOptions options = ParseOk( argv );
        EXPECT_FALSE( IsUnattendedSession( options.Shot, !options.ControlSocket.empty() ) );
    }
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}

TEST( CommandLine, ViewBudgetMiBIsReadAndANonNumberIsRefused )
{
    auto given = ParseCommandLine( { "--view-budget-mib", "768" } );
    ASSERT_TRUE( given.IsSuccess() ) << given.GetError();
    EXPECT_EQ( given.GetValue().ViewBudgetMiB, 768u ) << "--view-budget-mib was parsed but not recorded";

    auto none = ParseCommandLine( {} );
    ASSERT_TRUE( none.IsSuccess() ) << none.GetError();
    EXPECT_EQ( none.GetValue().ViewBudgetMiB, 0u )
         << "without the flag the driver's budget must stand; a non-zero default is a silent ceiling";

    for ( const char* bad : { "0", "-5", "lots", "1.5" } )
        EXPECT_FALSE( ParseCommandLine( { "--view-budget-mib", bad } ).IsSuccess() )
             << "--view-budget-mib " << bad << " was accepted";
}
