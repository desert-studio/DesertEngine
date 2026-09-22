// ── WHAT A DOWNLOADED BUILD KNOWS ABOUT ITSELF ──────────────────────────────────────────────────
//
// THE REPORT THIS SUITE COMES FROM. The owner downloaded `DesertEngine-Windows-Release` from CI,
// unzipped it, double-clicked `Editor.exe`, and got two messages — one demanding an environment
// variable and naming `scripts\Windows\RunEditor.bat`, a file that IS NOT IN THE DROP, and one
// demanding `--project` for a descriptor sitting in the same directory as the executable.
//
// Everything both messages asked for was derivable from `GetModuleFileNameW`. These tests drive the
// three derivations (Engine/Project/StartupLayout.hpp) over directories shaped like the two layouts
// this repository actually produces — a checkout and a drop — plus the shapes in between that a
// weaker rule would get wrong.
//
// NO DEVICE, NO WINDOW, NO EDITOR. Paths and a filesystem, which is the whole point of the
// derivations being functions instead of statements inside CreateApplication().

#include <Engine/Project/StartupLayout.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

using Desert::Project::DeriveEngineRoot;
using Desert::Project::ProjectBesideExecutable;
using Desert::Project::ResolveResourceRoot;

namespace
{
    fs::path FreshDirectory( const std::string& name )
    {
        const fs::path directory = fs::temp_directory_path() / ( "desert_startup_" + name );
        std::error_code ec;
        fs::remove_all( directory, ec );
        fs::create_directories( directory, ec );
        return directory;
    }

    void Touch( const fs::path& file )
    {
        std::error_code ec;
        fs::create_directories( file.parent_path(), ec );
        std::ofstream out( file );
        out << "x";
    }

    std::string ReadAll( const fs::path& path )
    {
        std::ifstream in( path, std::ios::binary );
        return std::string( ( std::istreambuf_iterator<char>( in ) ), std::istreambuf_iterator<char>() );
    }

    fs::path RepoRoot()
    {
        fs::path prefix = ".";
        for ( int up = 0; up < 8; ++up )
        {
            if ( fs::exists( prefix / "Desert/Common/Source/Common/Core/Constants.hpp" ) )
                return prefix;
            prefix /= "..";
        }
        return {};
    }

    // A CHECKOUT, as the launcher understands one: Templates/ to create projects from and scripts/
    // to start the editor with. The executable sits where the build puts it.
    fs::path MakeCheckout( const std::string& name )
    {
        const fs::path root = FreshDirectory( name );
        std::error_code ec;
        fs::create_directories( root / "Templates" / "Blank", ec );
        fs::create_directories( root / "scripts" / "MacOS", ec );
        Touch( root / "build" / "Bin" / "Release" / "Editor" );
        return root;
    }

    // A DROP, as scripts/*/Package.sh|bat produces one: binaries, Resources/{Shaders,Fonts,Icons}
    // and the project descriptor, all in ONE directory, and no repository anywhere above it.
    fs::path MakeDrop( const std::string& name )
    {
        const fs::path drop = FreshDirectory( name );
        Touch( drop / "Editor" );
        Touch( drop / "Runtime" );
        Touch( drop / "Desert.deproj" );
        std::error_code ec;
        fs::create_directories( drop / "Resources" / "Shaders", ec );
        fs::create_directories( drop / "Resources" / "Fonts", ec );
        fs::create_directories( drop / "Resources" / "Icons", ec );
        return drop;
    }
} // namespace

// ── 1. WHERE THIS ENGINE IS ─────────────────────────────────────────────────────────────────────

TEST( StartupLayout, ADevelopmentBinaryFindsItsOwnCheckoutWithNoEnvironmentVariable )
{
    const fs::path root   = MakeCheckout( "checkout" );
    const auto     lookup = DeriveEngineRoot( root / "build" / "Bin" / "Release" / "Editor" );

    ASSERT_FALSE( lookup.Root.empty() ) << lookup.Explanation;
    // Compared canonically: the derivation resolves symlinks (on this machine /tmp is one), and a
    // string compare against the unresolved temp path would fail for a correct answer.
    std::error_code ec;
    EXPECT_EQ( fs::weakly_canonical( lookup.Root, ec ), fs::weakly_canonical( root, ec ) );
    EXPECT_TRUE( lookup.Explanation.empty() ) << "an answer came with a reason for not having one";
}

TEST( StartupLayout, ADropIsNotAnEngineRootAndSaysSoWithoutNamingAScriptItDoesNotCarry )
{
    // THE REFUSAL IS THE CORRECT ANSWER HERE, not a shortfall, and this test exists to keep anybody
    // from "fixing" it into a registration. The launcher starts an engine by running
    // `<root>/scripts/MacOS/RunEditor.sh` or `<root>/build/Bin/<config>/Editor.exe`
    // (desert-launcher Source/Launch.cpp), and it PREFERS THE NEWEST ENTRY in engines.json. A drop
    // that registered itself would therefore displace the developer's real checkout in the sidebar
    // and then fail to start anything at all.
    const auto lookup = DeriveEngineRoot( MakeDrop( "drop_root" ) / "Editor" );

    EXPECT_TRUE( lookup.Root.empty() ) << "a drop registered itself as an engine the launcher can start";
    ASSERT_FALSE( lookup.Explanation.empty() ) << "a drop was refused in silence";

    // The defect verbatim: the old message told the reader to start the editor through
    // `scripts/Windows/RunEditor.bat`, which a drop does not contain. An instruction that cannot be
    // followed reads as a broken build.
    EXPECT_EQ( lookup.Explanation.find( "RunEditor" ), std::string::npos )
         << "the message tells the reader to run a script the drop does not carry: " << lookup.Explanation;
    EXPECT_NE( lookup.Explanation.find( "Templates" ), std::string::npos )
         << "the message does not say what was looked for: " << lookup.Explanation;
    EXPECT_NE( lookup.Explanation.find( "engines.json" ), std::string::npos )
         << "the message does not say what the consequence is: " << lookup.Explanation;
}

TEST( StartupLayout, HalfAnEngineRootIsNotAnEngineRoot )
{
    // BOTH MARKERS, NOT EITHER. `Templates/` alone is what the shared format's comment names, and a
    // rule built on it alone would call any directory with a folder of that name an engine — the
    // launcher would then be handed a root it can create projects from and cannot start.
    const fs::path onlyTemplates = FreshDirectory( "half_templates" );
    std::error_code ec;
    fs::create_directories( onlyTemplates / "Templates", ec );
    Touch( onlyTemplates / "build" / "Bin" / "Release" / "Editor" );
    EXPECT_TRUE( DeriveEngineRoot( onlyTemplates / "build" / "Bin" / "Release" / "Editor" ).Root.empty() )
         << "a directory with Templates/ and no scripts/ was accepted as an engine the launcher can start";

    const fs::path onlyScripts = FreshDirectory( "half_scripts" );
    fs::create_directories( onlyScripts / "scripts", ec );
    Touch( onlyScripts / "build" / "Bin" / "Release" / "Editor" );
    EXPECT_TRUE( DeriveEngineRoot( onlyScripts / "build" / "Bin" / "Release" / "Editor" ).Root.empty() )
         << "a directory with scripts/ and no Templates/ was accepted as an engine root";
}

TEST( StartupLayout, TheWalkUpwardsIsBoundedSoAStrayAncestorCannotBecomeTheEngine )
{
    // A walk to `/` makes the answer depend on directories that have nothing to do with this
    // program. Deep enough to be past the bound, and the marker is real — so a failure here means
    // the bound is gone, not that the markers stopped being recognised.
    const fs::path root = MakeCheckout( "deep" );
    fs::path       deep = root;
    for ( int level = 0; level < 10; ++level )
        deep /= "d";
    Touch( deep / "Editor" );

    EXPECT_TRUE( DeriveEngineRoot( deep / "Editor" ).Root.empty() )
         << "the walk reached a root ten directories above the executable";
}

TEST( StartupLayout, AnExecutableThatCouldNotBeLocatedSaysThatRatherThanBlamingTheDirectories )
{
    // ExecutablePath() answers with an empty path when the platform call fails. Reporting that as
    // "no engine tree above ''" would send the reader looking at folders when the fault is here.
    const auto lookup = DeriveEngineRoot( {} );
    EXPECT_TRUE( lookup.Root.empty() );
    EXPECT_NE( lookup.Explanation.find( "executable" ), std::string::npos ) << lookup.Explanation;
}

// ── 2. WHICH PROJECT TO OPEN ────────────────────────────────────────────────────────────────────

TEST( StartupLayout, TheDropsOwnDescriptorIsFoundBesideTheExecutable )
{
    const fs::path drop  = MakeDrop( "drop_project" );
    const auto     found = ProjectBesideExecutable( drop );

    ASSERT_TRUE( found.IsSuccess() ) << found.GetError();
    EXPECT_EQ( fs::path( found.GetValue() ).filename().string(), "Desert.deproj" );
}

TEST( StartupLayout, ADevelopmentBinaryFindsNoProjectBesideItselfAndTheMessageNamesWhereItLooked )
{
    // `build/Bin/<Config>/` holds executables and no descriptor, which is why the run scripts pass
    // `--project` and always did. Nothing about that launch changes; what changes is that the
    // refusal now says which directory it searched instead of only naming the flag.
    const fs::path root  = MakeCheckout( "checkout_project" );
    const auto     found = ProjectBesideExecutable( root / "build" / "Bin" / "Release" );

    ASSERT_FALSE( found.IsSuccess() ) << "a project was invented for a directory holding none";
    EXPECT_NE( found.GetError().find( "Release" ), std::string::npos ) << found.GetError();
    EXPECT_NE( found.GetError().find( "--project" ), std::string::npos ) << found.GetError();
}

TEST( StartupLayout, TwoDescriptorsAreARefusalThatNamesBothRatherThanAChoiceTakenSilently )
{
    // Picking the first would open A while the person is looking at B, and every downstream
    // display — the title bar, the recent list, the scene — would agree with each other and with
    // nobody. The count and both names are in the message so the reader can act on it.
    const fs::path drop = MakeDrop( "two_projects" );
    Touch( drop / "Another.deproj" );

    const auto found = ProjectBesideExecutable( drop );
    ASSERT_FALSE( found.IsSuccess() ) << "one of two descriptors was chosen without saying so";
    EXPECT_NE( found.GetError().find( "Desert.deproj" ), std::string::npos ) << found.GetError();
    EXPECT_NE( found.GetError().find( "Another.deproj" ), std::string::npos ) << found.GetError();
    EXPECT_NE( found.GetError().find( "--project" ), std::string::npos ) << found.GetError();
}

TEST( StartupLayout, ADirectoryNamedLikeADescriptorIsNotADescriptor )
{
    // A negative control for the extension test: `Cooked.deproj/` as a FOLDER would satisfy a
    // suffix compare on the name and then fail to parse, which is a message about JSON for a
    // problem about layout.
    const fs::path drop = MakeDrop( "dir_named_deproj" );
    std::error_code ec;
    fs::create_directories( drop / "Decoy.deproj", ec );

    const auto found = ProjectBesideExecutable( drop );
    ASSERT_TRUE( found.IsSuccess() ) << found.GetError();
    EXPECT_EQ( fs::path( found.GetValue() ).filename().string(), "Desert.deproj" );
}

// ── 3. WHERE `Resources/` IS ────────────────────────────────────────────────────────────────────

TEST( StartupLayout, AWorkingDirectoryThatAlreadyHoldsTheResourcesIsLeftAlone )
{
    // THE NEGATIVE CONTROL FOR EVERY EXISTING LAUNCH. `scripts/*/RunEditor.*` change into
    // `Editor/`, which holds `Resources/Shaders`; this must answer "do not move", or the change
    // would silently rebase every relative path a developer passes on the command line.
    //
    // THE SCENARIO IS DELIBERATELY THE HARD ONE. With resources in only one of the two places, the
    // ORDER of the two checks cannot be observed at all: swapping them was measured to leave this
    // test green, which makes it a test of nothing. So BOTH candidates hold `Resources/Shaders`
    // here, which is a real layout — a drop's Editor run from inside a checkout — and the answer
    // "do not move" can then only come from the working directory being asked FIRST.
    const fs::path root = MakeCheckout( "cwd_has_resources" );
    std::error_code ec;
    fs::create_directories( root / "Editor" / "Resources" / "Shaders", ec );
    fs::create_directories( root / "build" / "Bin" / "Release" / "Resources" / "Shaders", ec );

    const auto lookup = ResolveResourceRoot( root / "Editor", root / "build" / "Bin" / "Release" );
    EXPECT_TRUE( lookup.WorkingDirectory.empty() )
         << "a launch that already had its resources was moved to " << lookup.WorkingDirectory;
    EXPECT_TRUE( lookup.Explanation.empty() ) << lookup.Explanation;
}

TEST( StartupLayout, ADropStartedFromSomewhereElseWorksFromBesideItsOwnBinaries )
{
    const fs::path drop      = MakeDrop( "drop_resources" );
    const fs::path elsewhere = FreshDirectory( "elsewhere" );

    const auto lookup = ResolveResourceRoot( elsewhere, drop );
    ASSERT_TRUE( lookup.Explanation.empty() ) << lookup.Explanation;
    std::error_code ec;
    EXPECT_EQ( fs::weakly_canonical( lookup.WorkingDirectory, ec ), fs::weakly_canonical( drop, ec ) );
}

TEST( StartupLayout, AnEmptyResourcesFolderIsNotTheEngineResources )
{
    // The marker is `Resources/Shaders`, not `Resources`. A drop that lost its shader tree would
    // satisfy the weaker test, start, and fail 43 shaders later with a message about one shader.
    const fs::path hollow = FreshDirectory( "hollow_resources" );
    std::error_code ec;
    fs::create_directories( hollow / "Resources", ec );

    const auto lookup = ResolveResourceRoot( hollow, hollow );
    EXPECT_TRUE( lookup.WorkingDirectory.empty() );
    ASSERT_FALSE( lookup.Explanation.empty() ) << "an empty Resources/ folder was accepted";
    EXPECT_NE( lookup.Explanation.find( "Resources/Shaders" ), std::string::npos ) << lookup.Explanation;
}

TEST( StartupLayout, WithNoResourcesAnywhereTheRefusalNamesBothPlacesItLooked )
{
    const fs::path nothing   = FreshDirectory( "no_resources_cwd" );
    const fs::path alsoEmpty = FreshDirectory( "no_resources_exe" );

    const auto lookup = ResolveResourceRoot( nothing, alsoEmpty );
    EXPECT_TRUE( lookup.WorkingDirectory.empty() );
    ASSERT_FALSE( lookup.Explanation.empty() );
    EXPECT_NE( lookup.Explanation.find( nothing.filename().string() ), std::string::npos )
         << lookup.Explanation;
    EXPECT_NE( lookup.Explanation.find( alsoEmpty.filename().string() ), std::string::npos )
         << lookup.Explanation;
}

// ── 4. THE RELATION BETWEEN THE PACKAGER AND THE DERIVATIONS ────────────────────────────────────
//
// Both ends above are individually correct and that is not enough: the derivations look beside the
// executable, and it is the PACKAGING SCRIPTS that decide what ends up there. If a script stopped
// copying the descriptor, or moved the resource trees under a subdirectory, every test above would
// stay green and the drop would stop starting — the middle link dropping a property. So the scripts
// are read as text and asserted to put the two things where the derivations look.

namespace
{
    void AssertPackagerPutsTheDropTogether( const fs::path& script, const std::string& what )
    {
        const std::string text = ReadAll( script );
        ASSERT_FALSE( text.empty() ) << "could not read " << script.string();

        // The descriptor, copied into the output root — ProjectBesideExecutable() looks for exactly
        // one `.deproj` in the directory holding the binaries.
        EXPECT_NE( text.find( "Desert.deproj" ), std::string::npos )
             << what << " no longer names the project descriptor, so a drop has nothing to open";

        // The resource trees, under `Resources/` in the output root — ResolveResourceRoot() looks
        // for `Resources/Shaders` beside the executable.
        EXPECT_NE( text.find( "Shaders Fonts Icons" ), std::string::npos )
             << what << " no longer copies the three engine resource trees the drop needs";

        // And NOT Templates/: a drop must not become something the launcher will pick and fail to
        // start. This is the census half of the refusal asserted above.
        EXPECT_EQ( text.find( "Templates" ), std::string::npos )
             << what << " now ships Templates/, which would make a drop look like a checkout to the "
                        "launcher while still carrying no way for it to start this editor";
    }
} // namespace

TEST( StartupLayout, BothPackagersBuildTheLayoutTheDerivationsLookFor )
{
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "the repository root was not found from the test's working directory";

    AssertPackagerPutsTheDropTogether( root / "scripts" / "MacOS" / "Package.sh", "scripts/MacOS/Package.sh" );
    AssertPackagerPutsTheDropTogether( root / "scripts" / "Windows" / "Package.bat",
                                       "scripts\\Windows\\Package.bat" );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
