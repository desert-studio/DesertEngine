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

#include <Common/Core/Constants.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

using Desert::Project::DeriveEngineRoot;
using Desert::Project::ProjectBesideExecutable;
using Desert::Project::ResolveEngineDir;

namespace
{
    fs::path FreshDirectory( const std::string& name )
    {
        const fs::path  directory = fs::temp_directory_path() / ( "desert_startup_" + name );
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
        const fs::path  root = FreshDirectory( name );
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
    EXPECT_NE( lookup.Explanation.find( "build/Bin" ), std::string::npos )
         << "the message does not say what shape was expected: " << lookup.Explanation;
    EXPECT_NE( lookup.Explanation.find( "engines.json" ), std::string::npos )
         << "the message does not say what the consequence is: " << lookup.Explanation;
}

TEST( StartupLayout, HalfAnEngineRootIsNotAnEngineRoot )
{
    // BOTH MARKERS, NOT EITHER. `Templates/` alone is what the shared format's comment names, and a
    // rule built on it alone would call any directory with a folder of that name an engine — the
    // launcher would then be handed a root it can create projects from and cannot start.
    const fs::path  onlyTemplates = FreshDirectory( "half_templates" );
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

TEST( StartupLayout, ADropUNZIPPEDINSIDEACheckoutStillDoesNotRegisterThatCheckout )
{
    // MEASURED, NOT IMAGINED, and it is why this function checks a shape instead of walking up.
    // The first end-to-end run put the drop at `<worktree>/dist/DesertEngine-Release` — three
    // directories under a real checkout — and the ancestor walk found the checkout and filed it in
    // engines.json. The launcher would then have offered to start `<worktree>/build/Bin/...`,
    // which is a DIFFERENT editor from the one the person had just double-clicked.
    const fs::path root = MakeCheckout( "drop_inside_checkout" );
    const fs::path drop = root / "dist" / "DesertEngine-Release";
    Touch( drop / "Editor" );
    Touch( drop / "Desert.deproj" );

    const auto lookup = DeriveEngineRoot( drop / "Editor" );
    EXPECT_TRUE( lookup.Root.empty() )
         << "a drop sitting inside a checkout registered that checkout as the engine to start: " << lookup.Root;
    EXPECT_FALSE( lookup.Explanation.empty() );
}

TEST( StartupLayout, ABinaryThreeDirectoriesUnderACheckoutIsNotAutomaticallyThatCheckoutsEditor )
{
    // THE SCENARIO IS BUILT SO THAT ONLY THE SHAPE CAN ANSWER IT. Dropping the `build/Bin` test and
    // keeping "three directories up holds Templates/ and scripts/" was measured to leave the two
    // tests above green — in both of them nothing three levels up is a checkout at all, so they
    // never reach the question. Here it IS one, and the only thing separating this binary from the
    // one the launcher starts is the two directory names on the way down.
    //
    // `<checkout>/dist/DesertEngine-Release/bin/Editor` is a drop unzipped one folder deeper than
    // the one measured in the wild, which is not a stretch: a browser unpacking into a subfolder
    // produces exactly this.
    const fs::path root = MakeCheckout( "three_deep_not_build_bin" );
    const fs::path exe  = root / "dist" / "DesertEngine-Release" / "bin" / "Editor";
    Touch( exe );

    const auto lookup = DeriveEngineRoot( exe );
    EXPECT_TRUE( lookup.Root.empty() )
         << "a binary that merely sits three directories under a checkout was filed as that "
            "checkout's editor: "
         << lookup.Root;
    EXPECT_FALSE( lookup.Explanation.empty() );

    // ...AND BOTH NAMES ON THE WAY DOWN, not just the last one. Dropping the `build` half was
    // measured to leave the case above green, because its `Bin` half already answered — so the
    // second half needs a scenario of its own: a `Bin/<config>/` under some OTHER directory.
    const fs::path notUnderBuild = root / "dist" / "Bin" / "Release" / "Editor";
    Touch( notUnderBuild );
    EXPECT_TRUE( DeriveEngineRoot( notUnderBuild ).Root.empty() )
         << "a Bin/<config>/ directory outside build/ was taken for the checkout's own build output";
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

TEST( StartupLayout, ADescriptorSpelledInAnotherCaseIsStillADescriptor )
{
    // `path::extension() != ".deproj"` is a case-SENSITIVE compare on every platform, including the
    // one whose filesystem is not: a descriptor saved as `.DEPROJ` on Windows opens fine by name and
    // would have been invisible to the search — "no project beside this executable" for a project
    // sitting right there. Asserted on both platforms because the RULE is the same on both; only the
    // chance of meeting it differs.
    const fs::path drop = FreshDirectory( "upper_case_deproj" );
    Touch( drop / "Editor" );
    Touch( drop / "Shouty.DEPROJ" );

    const auto found = ProjectBesideExecutable( drop );
    ASSERT_TRUE( found.IsSuccess() ) << found.GetError();
    EXPECT_EQ( fs::path( found.GetValue() ).stem().string(), "Shouty" );
}

TEST( StartupLayout, ADirectoryNamedLikeADescriptorIsNotADescriptor )
{
    // A negative control for the extension test: `Cooked.deproj/` as a FOLDER would satisfy a
    // suffix compare on the name and then fail to parse, which is a message about JSON for a
    // problem about layout.
    const fs::path  drop = MakeDrop( "dir_named_deproj" );
    std::error_code ec;
    fs::create_directories( drop / "Decoy.deproj", ec );

    const auto found = ProjectBesideExecutable( drop );
    ASSERT_TRUE( found.IsSuccess() ) << found.GetError();
    EXPECT_EQ( fs::path( found.GetValue() ).filename().string(), "Desert.deproj" );
}

// ── 3. WHERE `Resources/` IS ────────────────────────────────────────────────────────────────────

// ── 3. THE ENGINE DIRECTORY ─────────────────────────────────────────────────────────────────────
//
// No test here takes a working directory: ResolveEngineDir has no such parameter, which is the point —
// the answer is the same wherever the process was started (ENG-ROOT-1).

namespace
{
    fs::path Canonical( const fs::path& p )
    {
        std::error_code ec;
        return fs::weakly_canonical( p, ec );
    }
} // namespace

TEST( StartupLayout, ADevelopmentBinaryFindsItsCheckoutsEditorFromItsOwnPosition )
{
    const fs::path root = MakeCheckout( "engine_dir_build_layout" );
    fs::create_directories( root / "Editor" / "Resources" / "Shaders" );
    const fs::path bin = root / "build" / "Bin" / "Release";

    const auto lookup = ResolveEngineDir( bin, {} );
    ASSERT_TRUE( lookup.Explanation.empty() ) << lookup.Explanation;
    EXPECT_TRUE( lookup.FromCheckout );
    EXPECT_TRUE( lookup.Dir.is_absolute() ) << lookup.Dir;
    EXPECT_EQ( Canonical( lookup.Dir ), Canonical( root / "Editor" ) );

    // ...and only by that shape: Bin/<config> outside build/ is not the checkout's build output.
    const fs::path notBuild = root / "dist" / "Bin" / "Debug";
    fs::create_directories( notBuild );
    fs::create_directories( root / "dist" / "Editor" / "Resources" / "Shaders" );
    EXPECT_FALSE( ResolveEngineDir( notBuild, {} ).Explanation.empty() )
         << "a Bin/<config> outside build/ was taken for a checkout's build output";
}

TEST( StartupLayout, APackagedDropIsItsOwnEngineDirectory )
{
    const fs::path drop   = MakeDrop( "engine_dir_drop" );
    const auto     lookup = ResolveEngineDir( drop, {} );
    ASSERT_TRUE( lookup.Explanation.empty() ) << lookup.Explanation;
    EXPECT_FALSE( lookup.FromCheckout );
    EXPECT_TRUE( lookup.Dir.is_absolute() ) << lookup.Dir;
    EXPECT_EQ( Canonical( lookup.Dir ), Canonical( drop ) );
}

TEST( StartupLayout, EngineDirFlagWinsOverTheExecutablesOwnLayout )
{
    // BOTH candidates are valid engine directories, so the answer can only come from the flag being
    // asked FIRST — with one valid candidate the order would be unobservable.
    const fs::path drop  = MakeDrop( "engine_dir_flag_exe" );
    const fs::path other = MakeDrop( "engine_dir_flag_named" );

    const auto lookup = ResolveEngineDir( drop, other );
    ASSERT_TRUE( lookup.Explanation.empty() ) << lookup.Explanation;
    EXPECT_EQ( Canonical( lookup.Dir ), Canonical( other ) );
}

TEST( StartupLayout, AWrongEngineDirFlagIsRefusedByNameNotReplacedByTheDerivation )
{
    // The executable's own directory is a perfectly good engine; the flag names one that is not. Falling
    // back would run a different engine than the one asked for, with nothing said.
    const fs::path drop  = MakeDrop( "engine_dir_bad_flag_exe" );
    const fs::path wrong = FreshDirectory( "engine_dir_bad_flag" );

    const auto lookup = ResolveEngineDir( drop, wrong );
    EXPECT_TRUE( lookup.Dir.empty() ) << lookup.Dir;
    ASSERT_FALSE( lookup.Explanation.empty() );
    EXPECT_NE( lookup.Explanation.find( wrong.string() ), std::string::npos ) << lookup.Explanation;
    EXPECT_NE( lookup.Explanation.find( "Resources/Shaders" ), std::string::npos ) << lookup.Explanation;
}

TEST( StartupLayout, AnEmptyResourcesFolderIsNotTheEngineResources )
{
    // The marker is `Resources/Shaders`, not `Resources`. A drop that lost its shader tree would
    // satisfy the weaker test, start, and fail 43 shaders later with a message about one shader.
    const fs::path  hollow = FreshDirectory( "hollow_resources" );
    std::error_code ec;
    fs::create_directories( hollow / "Resources", ec );

    const auto lookup = ResolveEngineDir( hollow, {} );
    EXPECT_TRUE( lookup.Dir.empty() );
    ASSERT_FALSE( lookup.Explanation.empty() ) << "an empty Resources/ folder was accepted";
    EXPECT_NE( lookup.Explanation.find( "Resources/Shaders" ), std::string::npos ) << lookup.Explanation;
}

TEST( StartupLayout, WithNoMarkerAnywhereTheRefusalNamesEveryPlaceItLooked )
{
    const fs::path root = MakeCheckout( "engine_dir_nothing" ); // a checkout whose Editor/ has no shaders
    const fs::path bin  = root / "build" / "Bin" / "Release";

    const auto lookup = ResolveEngineDir( bin, {} );
    EXPECT_TRUE( lookup.Dir.empty() );
    ASSERT_FALSE( lookup.Explanation.empty() );
    EXPECT_NE( lookup.Explanation.find( bin.string() ), std::string::npos ) << lookup.Explanation;
    EXPECT_NE( lookup.Explanation.find( ( root / "Editor" ).string() ), std::string::npos ) << lookup.Explanation;
}

// THE SERVICE HALF: what ResolveEngineDir answers is handed to Constants::Path::SetEngineDir, and every
// engine resource spelling must follow it — absolute, under that directory, at the SAME address (tables
// hold `&SHADERDIR_PATH` and must follow for free), with the no-project sandbox following too.
TEST( StartupLayout, SettingTheEngineDirMakesEveryEngineResourcePathAbsoluteAtStableAddresses )
{
    namespace P = Common::Constants::Path;
    P::ResetToSandbox();
    const fs::path* shadersAddress = &P::SHADERDIR_PATH;
    ASSERT_EQ( P::SHADERDIR_PATH, fs::path( "Resources/Shaders/" ) ) << "the unset state changed spelling";

    const fs::path engine = FreshDirectory( "engine_dir_service" ) / "Editor";
    P::SetEngineDir( engine );

    EXPECT_EQ( &P::SHADERDIR_PATH, shadersAddress );
    EXPECT_EQ( P::EngineDir(), engine.lexically_normal() );
    EXPECT_EQ( P::SHADERDIR_PATH, engine / "Resources" / "Shaders" / "" );
    EXPECT_EQ( P::ShaderDir(), P::SHADERDIR_PATH );
    EXPECT_EQ( P::RESOURCE_PATH, engine / "Resources" / "" );
    EXPECT_EQ( P::FONTS_PATH, engine / "Resources" / "Fonts" / "" );
    EXPECT_EQ( P::ICONS_PATH, engine / "Resources" / "Icons" / "" );
    EXPECT_EQ( P::ENGINE_CONTENT_PATH, engine / "Resources" / "Engine" / "" );
    EXPECT_EQ( P::EngineContentDir(), P::ENGINE_CONTENT_PATH );
    EXPECT_TRUE( P::SHADERDIR_PATH.is_absolute() );

    // The sandbox (no project) is engine content, so it follows the engine directory...
    EXPECT_EQ( P::ProjectDir(), P::EngineDir() );
    EXPECT_EQ( fs::path( P::ASSETS_PATH ).lexically_normal(),
               ( engine / P::SANDBOX_ASSETS_ROOT / "" ).lexically_normal() );

    // ...and an opened project does not move the engine resources.
    const fs::path project = FreshDirectory( "engine_dir_service_project" );
    P::SetProjectRoot( project, "Content" );
    EXPECT_EQ( P::ProjectDir(), project );
    EXPECT_EQ( P::SHADERDIR_PATH, engine / "Resources" / "Shaders" / "" );

    P::SetEngineDir( {} );
    P::ResetToSandbox();
    EXPECT_EQ( P::SHADERDIR_PATH, fs::path( "Resources/Shaders/" ) );
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

        // The resource trees, under `Resources/` in the output root — ResolveEngineDir() looks
        // for `Resources/Shaders` beside the executable.
        EXPECT_NE( text.find( "Shaders Fonts Icons" ), std::string::npos )
             << what << " no longer copies the three engine resource trees the drop needs";

        // And NOT Templates/: a drop must not become something the launcher will pick and fail to
        // start. This is the census half of the refusal asserted above.
        EXPECT_EQ( text.find( "Templates" ), std::string::npos )
             << what
             << " now ships Templates/, which would make a drop look like a checkout to the "
                "launcher while still carrying no way for it to start this editor";

        // NO REGISTRY TRAVELS (AF9). Until AF7 the packager cooked the drop's own registry, because a
        // drop with none preloaded zero shaders and died at the first material. Since AF9 the packaged
        // editor GATHERS its registry on first start from the headers of the files the drop carries
        // (Common::Content::GatherContentRegistry) into Intermediate/AssetRegistry.cache. A registry
        // cooked here would be a second answer to the same question, and one that goes stale the
        // moment the drop's content differs from what was cooked — so the census now asserts its absence.
        EXPECT_EQ( text.find( "AssetRegistryTool" ), std::string::npos )
             << what
             << " cooks an asset registry again; the packaged editor gathers its own at first start "
                "(AF9), and a cooked one would be a second, stale-able source for the same rows";
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
