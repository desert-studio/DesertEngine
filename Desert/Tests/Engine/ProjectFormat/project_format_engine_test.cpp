// The ENGINE's half of the shared-format contract. The conformance suite itself lives in the
// desert-shared submodule (Tests/project_format_test.cpp — compiled into this binary by the
// premake next door, and by the launcher's runner on its side); this file asserts the one relation
// the submodule cannot know about: its content-folder census against the engine's own
// Constants::Path globals. It also provides the gtest main() the shared file deliberately lacks.

#include <gtest/gtest.h>

#include <Common/Core/Constants.hpp>
#include <Common/Core/Version.hpp>
#include <DesertShared/EngineRegistry.hpp>
#include <DesertShared/ProjectFormat.hpp>
#include <Engine/Project/EngineRegistration.hpp>
#include <Engine/Project/ProjectContext.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

TEST( ProjectFormatEngine, EveryCensusRowIsAConstantTheEngineReads )
{
    // The census says "a project has <assetsRoot>/Meshes/"; the engine reads content through
    // Constants::Path::MESH_PATH. Two sides of one fact — assert the RELATION: after
    // SetProjectRoot, the constant each row is tied to must be exactly assetsRoot / row. The rows
    // here are a second list on purpose: it is the engine NAMING which of its constants answers
    // for each census entry, so a census edit without an engine-side answer fails right here.
    namespace P  = Common::Constants::Path;
    namespace fs = std::filesystem;

    struct Row
    {
        std::string_view Relative;
        const fs::path*  EnginePath;
    };
    const Row rows[] = {
         { "Meshes/", &P::MESH_PATH },
         { "Materials/", &P::MATERIAL_PATH },
         { "Textures/", &P::TEXTUREDIR_PATH },
         { "Scenes/", &P::SCENE_PATH },
         { "Prefabs/", &P::PREFAB_PATH },
         { "Scripts/", &P::SCRIPT_PATH },
         { "Collections/", &P::COLLECTIONS_PATH },
    };
    ASSERT_EQ( std::size( rows ), Common::Project::StandardContentFolders.size() )
         << "the shared census changed size and the engine has not answered for the new row";

    const fs::path                     projectDir = fs::path( "/tmp/desert-project-format-test" );
    const Common::Project::ProjectFile deproj;          // the default AssetsRoot every producer writes
    P::SetProjectRoot( projectDir, deproj.AssetsRoot ); // mutates process-wide globals — this suite only

    const fs::path assets = ( projectDir / deproj.AssetsRoot ).lexically_normal();
    for ( size_t i = 0; i < std::size( rows ); ++i )
    {
        EXPECT_EQ( rows[i].Relative, Common::Project::StandardContentFolders[i] )
             << "row " << i << ": engine mapping and shared census disagree on the folder itself";
        EXPECT_EQ( *rows[i].EnginePath, assets / rows[i].Relative )
             << "census row '" << rows[i].Relative << "' no longer matches the engine constant it is tied to";
    }
}

// ── the engine's half of engines.json: it is the WRITER, and the launcher only ever reads ───────

namespace
{
    std::filesystem::path TempConfigDirectory( const std::string& label )
    {
        const std::filesystem::path directory =
             std::filesystem::temp_directory_path() /
             ( "desert-engines-" + label + "-" +
               std::to_string( std::chrono::steady_clock::now().time_since_epoch().count() ) );
        std::error_code ec;
        std::filesystem::remove_all( directory, ec );
        std::filesystem::create_directories( directory, ec );
        return directory;
    }

    std::string ReadWhole( const std::filesystem::path& file )
    {
        std::ifstream in( file );
        return std::string( ( std::istreambuf_iterator<char>( in ) ), std::istreambuf_iterator<char>() );
    }
} // namespace

TEST( EngineRegistration, TheEngineWritesDownWhereItIsSoTheLauncherCanFindIt )
{
    // After L3 the launcher is a different repository with no DESERT_ROOT exported for it, so this
    // file is its ONLY route to an engine. What the engine writes has to come back out through the
    // shared reader the launcher calls — that relation is the whole point of the format.
    const std::filesystem::path config = TempConfigDirectory( "write" );
    const std::filesystem::path root   = TempConfigDirectory( "root" );

    const auto registered = Desert::Project::RegisterThisEngine( config.string(), root.string() );
    ASSERT_TRUE( registered.IsSuccess() ) << registered.GetError();

    auto read = Common::Engine::ReadEngineRegistry( ReadWhole( config / "engines.json" ) );
    ASSERT_TRUE( read.IsSuccess() ) << read.GetError();
    const Common::Engine::EngineInstall* install = Common::Engine::PreferredInstall( read.GetValue() );
    ASSERT_NE( install, nullptr );
    EXPECT_EQ( install->Root, root.string() );
    // The version is THIS build's, not a literal: a registry that reported someone else's version
    // would send the launcher's compatibility checks the wrong answer.
    EXPECT_EQ( install->VersionFull, Common::Version::Full() );
    // BOTH SIDES ARE OPTIONAL, AND THE ASSERTION IS THAT THEY AGREE ABOUT ABSENCE TOO. И7 made the
    // engine refuse to name a build it cannot trust (a shallow clone has no history to count), and the
    // registry carries that through rather than flattening it to 0 — a zero would lose every comparison
    // it was never entered into. Comparing the optionals directly says "known equals known, and unknown
    // equals unknown", which is the whole point of the change.
    EXPECT_EQ( install->CommitCount.has_value(), Common::Version::CommitCount().has_value() );
    if ( Common::Version::CommitCount() )
        EXPECT_EQ( *install->CommitCount, static_cast<int>( *Common::Version::CommitCount() ) );

    std::error_code ec;
    std::filesystem::remove_all( config, ec );
    std::filesystem::remove_all( root, ec );
}

TEST( EngineRegistration, StartingTheSameEngineAgainDoesNotGrowTheFile )
{
    // The Editor registers on EVERY start, and a developer's tree is one root opened a thousand
    // times. An append would hand the launcher a thousand identical sidebar entries.
    const std::filesystem::path config = TempConfigDirectory( "twice" );
    const std::filesystem::path root   = TempConfigDirectory( "root2" );

    ASSERT_TRUE( Desert::Project::RegisterThisEngine( config.string(), root.string() ).IsSuccess() );
    ASSERT_TRUE( Desert::Project::RegisterThisEngine( config.string(), root.string() ).IsSuccess() );

    auto read = Common::Engine::ReadEngineRegistry( ReadWhole( config / "engines.json" ) );
    ASSERT_TRUE( read.IsSuccess() ) << read.GetError();
    EXPECT_EQ( read.GetValue().Engines.size(), 1u ) << "the same root was registered twice";

    std::error_code ec;
    std::filesystem::remove_all( config, ec );
    std::filesystem::remove_all( root, ec );
}

TEST( EngineRegistration, ARegistryThisBuildCannotParseIsLeftUntouchedRatherThanOverwritten )
{
    // The file belongs to the USER: it may hold an engine this build knows nothing about, written
    // by a newer one. Rewriting it from scratch would silently delete that engine from the
    // launcher's sidebar every time this one started.
    const std::filesystem::path config   = TempConfigDirectory( "corrupt" );
    const std::filesystem::path root     = TempConfigDirectory( "root3" );
    const std::string           original = R"({"Engines": this is not json)";
    {
        std::ofstream out( config / "engines.json" );
        out << original;
    }

    const auto registered = Desert::Project::RegisterThisEngine( config.string(), root.string() );
    EXPECT_FALSE( registered.IsSuccess() ) << "a file that could not be parsed was reported as written";
    EXPECT_NE( registered.GetError().find( "engines.json" ), std::string::npos ) << registered.GetError();
    EXPECT_EQ( ReadWhole( config / "engines.json" ), original )
         << "the user's registry was overwritten by a build that could not read it";

    std::error_code ec;
    std::filesystem::remove_all( config, ec );
    std::filesystem::remove_all( root, ec );
}

TEST( EngineRegistration, WithNoEngineRootThereIsNothingToRegisterAndItSaysWhy )
{
    const std::filesystem::path config     = TempConfigDirectory( "noroot" );
    const auto                  registered = Desert::Project::RegisterThisEngine( config.string(), "" );
    ASSERT_FALSE( registered.IsSuccess() );
    // NOT "DESERT_ROOT". The variable stopped being the only way an engine can be located — the
    // Editor derives the tree from its own executable when nothing is set — so a refusal naming the
    // variable would send a reader of a DOWNLOADED build after something that is neither the cause
    // nor the cure. What has to be in it is the consequence: the launcher will not list this copy.
    EXPECT_NE( registered.GetError().find( "engines.json" ), std::string::npos ) << registered.GetError();
    EXPECT_EQ( registered.GetError().find( "RunEditor" ), std::string::npos )
         << "the refusal names a run script a packaged build does not carry: " << registered.GetError();
    EXPECT_FALSE( std::filesystem::exists( config / "engines.json" ) )
         << "an empty registry was written for an engine that was never located";

    std::error_code ec;
    std::filesystem::remove_all( config, ec );
}

// ── K11: the .deproj is tracked by git, so a key this build drops travels to everybody ───────────

TEST( ProjectContextDescriptor, SavingCarriesAcrossAKeyWrittenAfterTheProjectWasOpened )
{
    // The half a ForeignKeys member alone cannot do. ProjectContext parses the descriptor ONCE, in
    // Open(), and holds it for the whole session — so an editor that started before a key existed
    // carries leftovers that have never heard of it. The launcher's settings screen writes this
    // same file from another process while the editor is up, which is exactly when that happens.
    // Save() therefore re-reads the file's leftovers at the moment of writing (K9's shape, applied
    // one file over).
    const std::filesystem::path project = TempConfigDirectory( "deproj-adopt" );
    const std::filesystem::path deproj  = project / "Game.deproj";
    {
        std::ofstream out( deproj );
        out << R"({"FileVersion":1,"Name":"Game","AssetsRoot":"Assets","DefaultScene":"",)"
               R"("Description":"","EngineVersion":""})";
    }

    // RecordInRecent::No — a test must not file itself in the developer's own recent list.
    ASSERT_TRUE( Desert::Project::ProjectContext::Open( deproj.string(),
                                                        Desert::Project::ProjectContext::RecordInRecent::No ) );

    // ... and NOW another program adds a key this build has never heard of.
    {
        std::ofstream out( deproj );
        out << R"({"FileVersion":1,"Name":"Game","AssetsRoot":"Assets","DefaultScene":"",)"
               R"("Description":"","EngineVersion":"","PrimaryPlatform":"Switch"})";
    }

    ASSERT_TRUE( Desert::Project::ProjectContext::SetDefaultScene( "Assets/Scenes/Main.desce" ) );

    const std::string written = ReadWhole( deproj );
    EXPECT_NE( written.find( R"("DefaultScene":"Assets/Scenes/Main.desce")" ), std::string::npos ) << written;
    EXPECT_NE( written.find( R"("PrimaryPlatform":"Switch")" ), std::string::npos )
         << "a key another program wrote after this project was opened was deleted by saving: " << written;

    std::error_code ec;
    std::filesystem::remove_all( project, ec );
}

// ── K11: the engine's half of projects.json, a file two programs write ───────────────────────────
//
// engines.json has had "a registry this build cannot parse is left untouched" since И7 (three tests
// up). projects.json — the file with TWO writers rather than one — did not: an unreadable registry
// was read as EMPTY and the promotion was then written over the top, so one bad byte forgot every
// project the user had. These assert the same protection and the read-modify-write beside it.

TEST( ProjectContextRecent, ARegistryThisBuildCannotParseIsLeftUntouchedRatherThanOverwritten )
{
    const std::filesystem::path config   = TempConfigDirectory( "recent-corrupt" );
    const std::string           original = R"({"Projects": [ this is not json)";
    {
        std::ofstream out( config / "projects.json" );
        out << original;
    }

    // The refusal is visible through the reader as well: "empty" and "unreadable" are different
    // answers, and this used to return the same value for both.
    const auto read = Desert::Project::ProjectContext::RecentProjects( config.string() );
    EXPECT_FALSE( read.IsSuccess() ) << "a corrupt registry was reported as an empty one";

    Desert::Project::ProjectContext::RegisterRecent( config.string(), "/p/New.deproj" );
    EXPECT_EQ( ReadWhole( config / "projects.json" ), original )
         << "the user's whole project list was replaced by a registry built on a failed read";

    std::error_code ec;
    std::filesystem::remove_all( config, ec );
}

TEST( ProjectContextRecent, AProjectTheLauncherFiledSurvivesTheEnginesNextWrite )
{
    // The other half of ProjectHubTwoWriters, from this side: whatever is in the file when the
    // engine writes must still be in the file afterwards. The engine re-reads immediately before
    // writing, so "whatever is in the file" is not a copy from process start.
    const std::filesystem::path config = TempConfigDirectory( "recent-merge" );

    Common::Project::ProjectsRegistry fromLauncher;
    Common::Project::PromoteRecent( fromLauncher, "/p/FiledByTheLauncher.deproj", 500 );
    {
        std::ofstream out( config / "projects.json" );
        out << Common::Project::WriteProjectsRegistry( fromLauncher );
    }

    Desert::Project::ProjectContext::RegisterRecent( config.string(), "/p/OpenedByTheEditor.deproj" );

    const auto after = Desert::Project::ProjectContext::RecentProjects( config.string() );
    ASSERT_TRUE( after.IsSuccess() ) << after.GetError();
    ASSERT_EQ( after.GetValue().Projects.size(), 2u ) << "the engine wrote over the launcher's entry";
    EXPECT_EQ( after.GetValue().Projects[0].Path, "/p/OpenedByTheEditor.deproj" ) << "most recent first";
    EXPECT_EQ( after.GetValue().Projects[1].Path, "/p/FiledByTheLauncher.deproj" );

    std::error_code ec;
    std::filesystem::remove_all( config, ec );
}

TEST( ProjectContextRecent, AMachineWithNoRegistryYetIsNotAFailure )
{
    // The one case that answers "empty" successfully — and it has to, or a fresh machine would
    // refuse to record the first project ever opened on it.
    const std::filesystem::path config = TempConfigDirectory( "recent-fresh" );
    const auto                  fresh  = Desert::Project::ProjectContext::RecentProjects( config.string() );
    ASSERT_TRUE( fresh.IsSuccess() ) << fresh.GetError();
    EXPECT_TRUE( fresh.GetValue().Projects.empty() );

    Desert::Project::ProjectContext::RegisterRecent( config.string(), "/p/First.deproj" );
    const auto after = Desert::Project::ProjectContext::RecentProjects( config.string() );
    ASSERT_TRUE( after.IsSuccess() );
    ASSERT_EQ( after.GetValue().Projects.size(), 1u );
    EXPECT_EQ( after.GetValue().Projects[0].Path, "/p/First.deproj" );

    std::error_code ec;
    std::filesystem::remove_all( config, ec );
}

TEST( ProjectContextRecent, ARegistryReadAndWrittenBackUnchangedIsByteIdentical )
{
    // The relation K11 states for every one of these files, on the engine's side of this one: a
    // read followed by a write with no change in between must leave the bytes alone. Anything else
    // means the two writers churn each other's file on every open.
    //
    // True for a registry THIS BUILD wrote. A registry carrying a key this build does not declare
    // is not byte-stable — the writer emits ProjectsRegistry's members and nothing else — and that
    // is the half that needs an rfl::ExtraFields carrier on the shared struct.
    const std::filesystem::path config = TempConfigDirectory( "recent-identity" );
    Desert::Project::ProjectContext::RegisterRecent( config.string(), "/p/A.deproj" );
    Desert::Project::ProjectContext::RegisterRecent( config.string(), "/p/B.deproj" );
    const std::string before = ReadWhole( config / "projects.json" );
    ASSERT_FALSE( before.empty() );

    // Read it and write it straight back, through the same two shared functions both hosts use.
    const auto parsed = Common::Project::ReadProjectsRegistry( before );
    ASSERT_TRUE( parsed.IsSuccess() ) << parsed.GetError();
    EXPECT_EQ( Common::Project::WriteProjectsRegistry( parsed.GetValue() ), before )
         << "reading and writing back with no change altered the file";

    std::error_code ec;
    std::filesystem::remove_all( config, ec );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
