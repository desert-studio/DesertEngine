// THE LOADER'S VERSION GATE: what this engine will read, what it refuses, and what it says when it does.
//
// Eight schema migrations used to run inside SceneSerializer::DeserializeFromJson, on every load of every
// scene, forever. Each of them was written with an expiry — "deleted once no v<n> file remains" — and not
// one was ever collected, because a migration that runs at LOAD never writes its result back and so can
// never observe that the last old file is gone. DEV_CONTRACT §4.3 ends "the runtime knows nothing about the
// old format"; it knew eight. They moved, whole, to Tools/SceneMigrator, and what replaced them here is a
// REFUSAL: the engine reads one generation and names the command that converts anything else.
//
// A refusal is only honest if three things are true, and this suite is those three:
//
//   1. It fires on exactly the files it should — and NOT on any file we ship. The corpus block at the
//      bottom is the second half of that and is the load-bearing one: if a single scene in the repository
//      would trip the gate, this suite goes red with the scene's name in it.
//   2. It says enough to act on. Not "unsupported version" — the file, what it is, what is needed, and the
//      exact command. That is asserted here rather than eyeballed, because an error message nobody can act
//      on is the same dead end as no error at all (§1.4).
//   3. It produces NOTHING. ParseLoadableScene either hands back a tree or hands back an error; there is no
//      third outcome in which a partially-read scene escapes. The whole-loader half of that claim — that no
//      entity, setting or scene name is created either — is not assertable without a renderer, and was
//      measured instead: the editor launched on a v1 file renders the empty grid and its log carries the
//      refusal with no "Loading scene:" line at all.
//
// PURE. It parses JSON and calls two functions over the parsed tree. No GPU, no scene graph, no asset
// manager — which is the same property that let the migrations move out of the engine in the first place.

#include <Engine/Core/Serialize/SceneFormat.hpp>

#include <Common/Content/AssetRedirector.hpp>
#include <Common/Content/TextAssetHeader.hpp>

#include <rflcpp/rfl/json.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <algorithm>
#include <array>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <optional>

namespace
{
    // A fixture's text header stating the given generations; an absent one is not stated at all, which is
    // how a file that predates one of the two numbers reads since v26 moved them into the header. One
    // fixed GUID: fixtures built twice must be the same bytes, as two saves of one asset are.
    Common::Content::TextAssetHeaderSerialized FixtureHeader( Common::Content::ContentKind kind,
                                                              std::optional<int>           sceneVersion,
                                                              std::optional<int>           unitVersion )
    {
        std::vector<Common::Content::SubsystemVersion> versions;
        if ( sceneVersion )
            versions.push_back( { Desert::Assets::kSceneSchemaTag, static_cast<uint32_t>( *sceneVersion ) } );
        if ( unitVersion )
            versions.push_back( { Desert::Assets::kUnitSchemaTag, static_cast<uint32_t>( *unitVersion ) } );
        const auto guid = Common::Content::AssetGuidFromText( "0f1e2d3c4b5a69788796a5b4c3d2e1f0" );
        return Common::Content::MakeTextHeader( kind, guid.GetValue(), versions );
    }
} // namespace

using Desert::Core::kSceneVersion;
using Desert::Core::kUnitVersion;
using Desert::Core::ParseLoadableScene;
using Desert::Core::RefuseSceneVersion;
using Desert::Core::SceneIsAtCurrentVersion;
using Desert::Core::SceneSerialized;

namespace
{
    /// A tree at the two versions asked for. Everything else is empty on purpose: the gate reads the two
    /// integers and nothing else, so a fixture that carried entities would only invite the reader to wonder
    /// whether they mattered.
    SceneSerialized At( int sceneVersion, int unitVersion )
    {
        SceneSerialized scene;
        scene.SceneName    = "Fixture";
        scene.Header       = FixtureHeader( Common::Content::ContentKind::Scene, sceneVersion, unitVersion );
        return scene;
    }

    std::string JsonAt( int sceneVersion, int unitVersion )
    {
        return rfl::json::write( At( sceneVersion, unitVersion ) );
    }

    bool Mentions( const std::string& haystack, const std::string& needle )
    {
        return haystack.find( needle ) != std::string::npos;
    }

    // Walks up from the working directory looking for a file only the repository has — the test runner's
    // working directory is not fixed. Same shape as Desert/Tests/Engine/SceneStitch.
    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            std::ifstream probe( prefix + "Desert/Desert/Source/Engine/Core/SceneSettings.hpp" );
            if ( probe )
                return prefix;
            prefix += "../";
        }
        return {};
    }

    // EVERY PLACE A `.desce` CAN LIVE IN THIS REPOSITORY, and the second row is why this is a list.
    //
    // `Templates/<Id>/Payload/` is copied BYTE FOR BYTE into a new project by the launcher and then
    // opened by the Editor, so a scene in a payload is engine content that has to travel with the
    // engine's migrations — it just does not live under the assets root. Nothing pointed a migration
    // or a gate at it: `SceneMigrator` walks only the roots it is handed, and this gate walked one
    // directory. Today no payload contains a `.desce` (the only payload file in the tree is a `.lua`),
    // so the hole is not yet a defect; the moment the FirstPerson template its README plans is
    // authored, a stale scene would ship to whoever creates a project from it, and the first report
    // would come from a user rather than from here.
    //
    // MustContainScenes is the difference between "this root is allowed to be empty" and "this root
    // being empty means the walk is broken". Without it a renamed assets directory would leave both
    // corpus tests below iterating an empty list and passing in silence.
    struct SceneRoot
    {
        const char* Relative;
        bool        MustContainScenes;
    };

    constexpr std::array<SceneRoot, 2> kSceneRoots = { {
         { "Editor/Resources/Assets/Scenes", true },
         // May be empty: templates are authored content and there is no rule that one must exist.
         // The DIRECTORY still has to, so a rename or a move reddens here rather than going unseen.
         { "Templates", false },
    } };

    std::vector<std::filesystem::path> ScenesUnder( const std::filesystem::path& root )
    {
        std::vector<std::filesystem::path> scenes;
        std::error_code                    ec;
        for ( const auto& entry : std::filesystem::recursive_directory_iterator( root, ec ) )
        {
            if ( entry.is_regular_file() && entry.path().extension() == ".desce" )
                scenes.push_back( entry.path() );
        }
        return scenes;
    }

    std::vector<std::filesystem::path> RepositoryScenes()
    {
        std::vector<std::filesystem::path> scenes;
        for ( const SceneRoot& root : kSceneRoots )
        {
            const auto found = ScenesUnder( RepoRoot() + root.Relative );
            scenes.insert( scenes.end(), found.begin(), found.end() );
        }
        return scenes;
    }

    std::string ReadAll( const std::filesystem::path& path )
    {
        std::ifstream      in( path, std::ios::binary );
        std::ostringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }
} // namespace

// ---------------------------------------------------------------------------------------------------
// 1. WHICH TREES ARE CURRENT
// ---------------------------------------------------------------------------------------------------

TEST( SceneVersionGate, ATreeAtBothHeadsIsCurrentAndOneOffEitherAxisIsNot )
{
    EXPECT_TRUE( SceneIsAtCurrentVersion( At( kSceneVersion, kUnitVersion ) ) );

    EXPECT_FALSE( SceneIsAtCurrentVersion( At( kSceneVersion - 1, kUnitVersion ) ) );
    EXPECT_FALSE( SceneIsAtCurrentVersion( At( kSceneVersion, kUnitVersion - 1 ) ) );

    // AHEAD is refused too, and deliberately. A file stamped v9 was written by a build that knows something
    // this one does not; loading it on the assumption that "newer is compatible" is the silent substitution
    // §1.4 forbids, and there is no migration DOWN for the refusal to point at either.
    EXPECT_FALSE( SceneIsAtCurrentVersion( At( kSceneVersion + 1, kUnitVersion ) ) );
}

// AN ABSENT INTEGER IS VERSION 0, NOT "CURRENT". Every stamp this engine writes states both numbers, so a
// file missing one was written by something older. Reading it as current would load a metres-era scene as
// though it were centimetres — the world a hundred times too small, and nothing said about it.
TEST( SceneVersionGate, AnAbsentVersionIntegerIsZeroRatherThanCurrent )
{
    SceneSerialized noScene;
    noScene.Header =
         FixtureHeader( Common::Content::ContentKind::Scene, std::nullopt, kUnitVersion ); // scene version absent
    EXPECT_FALSE( SceneIsAtCurrentVersion( noScene ) );

    SceneSerialized noUnit;
    noUnit.Header =
         FixtureHeader( Common::Content::ContentKind::Scene, kSceneVersion, std::nullopt ); // unit version absent
    EXPECT_FALSE( SceneIsAtCurrentVersion( noUnit ) );

    SceneSerialized neither;
    EXPECT_FALSE( SceneIsAtCurrentVersion( neither ) );
}

// ---------------------------------------------------------------------------------------------------
// 2. WHAT THE REFUSAL SAYS
// ---------------------------------------------------------------------------------------------------

// THE FOUR THINGS, ASSERTED ONE BY ONE. A reader who hits this message has to be able to act on it without
// reading any source: which file, what it is, what is needed, and the command. Dropping any one of them
// turns the message into "it did not work", which is where a user's afternoon goes.
TEST( SceneVersionGate, TheRefusalNamesTheFileTheVersionTheTargetAndTheCommand )
{
    const std::string message = RefuseSceneVersion( "Resources/Assets/Scenes/Old.desce", 1, 0 );

    // The file.
    EXPECT_TRUE( Mentions( message, "Resources/Assets/Scenes/Old.desce" ) ) << message;
    // What it is: both found versions.
    EXPECT_TRUE( Mentions( message, "v1" ) ) << message;
    EXPECT_TRUE( Mentions( message, "v0" ) ) << message;
    // What is needed: both required versions, taken from the constants rather than spelled out, so this
    // assertion cannot rot the day the head moves.
    EXPECT_TRUE( Mentions( message, "v" + std::to_string( kSceneVersion ) ) ) << message;
    EXPECT_TRUE( Mentions( message, "v" + std::to_string( kUnitVersion ) ) ) << message;
    // The command — the tool AND the argument, not just the tool's name.
    EXPECT_TRUE( Mentions( message, "SceneMigrator" ) ) << message;
    EXPECT_TRUE( Mentions( message, "SceneMigrator \"Resources/Assets/Scenes/Old.desce\"" ) ) << message;
}

// AND IT SAYS THAT NOTHING HAPPENED. The difference between "this file was refused" and "this file was
// refused and your scene is untouched" is the difference between a user re-opening their work and a user
// assuming it is gone.
TEST( SceneVersionGate, TheRefusalSaysNothingWasLoaded )
{
    const std::string message = RefuseSceneVersion( "Old.desce", 1, 0 );
    EXPECT_TRUE( Mentions( message, "NOTHING WAS LOADED" ) ) << message;
}

// The overload that reads the tree agrees with the one that takes the two integers, including on the
// absent-is-zero rule. Two spellings of one message is how the two drift.
TEST( SceneVersionGate, TheTreeOverloadReportsTheSameVersionsTheTreeCarries )
{
    EXPECT_EQ( RefuseSceneVersion( "S.desce", At( 3, 0 ) ), RefuseSceneVersion( "S.desce", 3, 0 ) );

    SceneSerialized neither; // both absent
    EXPECT_EQ( RefuseSceneVersion( "S.desce", neither ), RefuseSceneVersion( "S.desce", 0, 0 ) );
}

// ---------------------------------------------------------------------------------------------------
// 3. WHAT PARSING PRODUCES — A TREE, OR AN ERROR, AND NEVER BOTH
// ---------------------------------------------------------------------------------------------------

TEST( SceneVersionGate, ACurrentFileParsesAndTheTreeComesBack )
{
    const auto loadable = ParseLoadableScene( "Current.desce", JsonAt( kSceneVersion, kUnitVersion ) );

    ASSERT_TRUE( static_cast<bool>( loadable ) ) << loadable.GetError();
    EXPECT_EQ( loadable.GetValue().Scene.SceneName, "Fixture" );
    EXPECT_EQ( Desert::Assets::StatedVersion( loadable.GetValue().Scene.Header, Desert::Assets::kSceneSchemaTag ),
               kSceneVersion );
    EXPECT_EQ( Desert::Assets::StatedVersion( loadable.GetValue().Scene.Header, Desert::Assets::kUnitSchemaTag ),
               kUnitVersion );
}

// EVERY GENERATION THAT EVER SHIPPED IS REFUSED, not just the one before this. A gate written as
// "< head" and a gate written as "== head - 1" pass the same single-version test and disagree about v1.
TEST( SceneVersionGate, EveryOlderGenerationIsRefusedAndNamedByItsOwnNumber )
{
    for ( int version = 0; version < kSceneVersion; ++version )
    {
        const auto loadable = ParseLoadableScene( "Old.desce", JsonAt( version, kUnitVersion ) );

        ASSERT_FALSE( static_cast<bool>( loadable ) ) << "scene schema v" << version << " was accepted";
        EXPECT_TRUE( Mentions( loadable.GetError(), "v" + std::to_string( version ) ) )
             << "the refusal for v" << version << " does not say which version it found: " << loadable.GetError();
        EXPECT_TRUE( Mentions( loadable.GetError(), "SceneMigrator" ) ) << loadable.GetError();
    }
}

// The metres-era file is refused on the OTHER axis, and the two axes are independent: a scene at the
// current schema whose units were never stamped is exactly the file the x100 migration existed for.
TEST( SceneVersionGate, AMetresEraFileIsRefusedEvenAtTheCurrentSchema )
{
    const auto loadable = ParseLoadableScene( "Metres.desce", JsonAt( kSceneVersion, 0 ) );

    ASSERT_FALSE( static_cast<bool>( loadable ) );
    EXPECT_TRUE( Mentions( loadable.GetError(), "world units v0" ) ) << loadable.GetError();
}

// Text that is not a scene at all fails as a READ rather than as a version, and still names the file. The
// two failures are worth telling apart: one is fixed by running a tool, the other is not.
TEST( SceneVersionGate, TextThatIsNotASceneFailsAsAReadAndStillNamesTheFile )
{
    const auto loadable = ParseLoadableScene( "Broken.desce", "{ this is not json" );

    ASSERT_FALSE( static_cast<bool>( loadable ) );
    EXPECT_TRUE( Mentions( loadable.GetError(), "Broken.desce" ) ) << loadable.GetError();
    EXPECT_TRUE( Mentions( loadable.GetError(), "not a readable scene file" ) ) << loadable.GetError();
    EXPECT_FALSE( Mentions( loadable.GetError(), "SceneMigrator" ) )
         << "a file that is not a scene is not fixed by migrating it, and saying so sends the reader to a "
            "tool that will also fail: "
         << loadable.GetError();
}

// The editor opens a scene BY FILE. After a move (AF10b) the old path holds a binary redirector; the gate says
// so and names where the scene went (here by GUID: this suite has no registry), instead of "not JSON".
TEST( SceneVersionGate, ARedirectorAtTheOldPathIsRefusedAsARedirectorNotAsBrokenText )
{
    const Common::Content::AssetGuid target{ 0xAF10B2ull, 7 };
    const auto                       encoded = Common::Content::EncodeRedirector(
         Common::Content::AssetRedirector{ { 0xAF10B2ull, 8 }, target, "Scene/Before.desce" } );
    ASSERT_TRUE( static_cast<bool>( encoded ) ) << encoded.GetError();
    std::string bytes( encoded.GetValue().size(), '\0' );
    std::ranges::transform( encoded.GetValue(), bytes.begin(),
                            []( std::byte b ) { return static_cast<char>( b ); } );

    const auto loadable = ParseLoadableScene( "Before.desce", bytes );

    ASSERT_FALSE( static_cast<bool>( loadable ) );
    EXPECT_TRUE( Mentions( loadable.GetError(), "Before.desce" ) ) << loadable.GetError();
    EXPECT_TRUE( Mentions( loadable.GetError(), "is a redirector" ) ) << loadable.GetError();
    EXPECT_TRUE( Mentions( loadable.GetError(), Common::Content::AssetGuidToText( target ) ) )
         << loadable.GetError();
    EXPECT_FALSE( Mentions( loadable.GetError(), "not a readable scene file" ) ) << loadable.GetError();
}

// ---------------------------------------------------------------------------------------------------
// 4. THE CORPUS — the refusal is unreachable for everything this repository ships
// ---------------------------------------------------------------------------------------------------

// 4a. The corpus exists and this suite found it. Without this, a wrong working directory turns every
// assertion below into a vacuous pass over zero files — a corpus test reporting green about nothing.
TEST( SceneVersionGateCorpus, TheScenesAreWhereThisSuiteThinksTheyAre )
{
    EXPECT_GE( RepositoryScenes().size(), 40u );
}

// 4b. THE CLAIM THIS WHOLE CHANGE RESTS ON, MEASURED RATHER THAN ASSERTED. Removing the migrations from the
// runtime is only safe if nothing we ship needs them, and the honest way to know that is to put every
// single scene through the gate the loader applies — not to trust that they were all converted.
//
// A FAILURE HERE IS NOT A BROKEN TEST. It means a real file in this tree will not open in the editor or the
// runtime, and the fix is the one the message names: run Tools/SceneMigrator over it. That includes files
// this repository does not track — the sweep is recursive and Scenes/Autosave holds gitignored editor
// crash-recovery files, which are exactly the ones most likely to have been written by an older build.
// 4a. THE POSITIVE CONTROL FOR 4b AND 4c, AND IT WAS MISSING. Both tests below are `for` loops over
// `RepositoryScenes()`, so an empty list passes them — a renamed assets directory would have turned this
// gate off without turning it red. That is this repository's most frequent defect shape: the instrument
// answering a different question with nothing in its output to say so.
//
// Each root is checked SEPARATELY rather than by one total, because a total is satisfied by the other
// root's files: `Templates/` may legitimately hold no scene, but if it stops EXISTING the row is stale
// and has to be read again by a person.
TEST( SceneVersionGateCorpus, EveryDeclaredSceneRootExistsAndTheCorpusIsNotEmpty )
{
    for ( const SceneRoot& root : kSceneRoots )
    {
        const std::filesystem::path path = RepoRoot() + root.Relative;
        EXPECT_TRUE( std::filesystem::is_directory( path ) )
             << root.Relative
             << " is declared as a place scenes can live and is not a directory. Either it moved — in "
                "which case this row has to move with it — or the walk below is examining nothing.";

        if ( root.MustContainScenes )
        {
            EXPECT_FALSE( ScenesUnder( path ).empty() )
                 << root.Relative
                 << " holds no .desce at all, so the corpus tests below iterate nothing "
                    "and pass vacuously.";
        }
    }
}

TEST( SceneVersionGateCorpus, EverySceneOnDiskIsOneThisEngineWillLoad )
{
    for ( const auto& path : RepositoryScenes() )
    {
        const auto loadable = ParseLoadableScene( path.string(), ReadAll( path ) );
        EXPECT_TRUE( static_cast<bool>( loadable ) ) << loadable.GetError();
    }
}

// 4c. And they are at the head by STATING it, not by defaulting to it. 4b would also pass on a file whose
// two integers happened to be right for the wrong reason; this says the file carries both keys, which is
// what the saver writes and what SceneMigrator stamps.
TEST( SceneVersionGateCorpus, EverySceneStatesBothVersionIntegersExplicitly )
{
    for ( const auto& path : RepositoryScenes() )
    {
        const auto parsed = rfl::json::read<SceneSerialized>( ReadAll( path ) );
        ASSERT_TRUE( parsed.has_value() ) << path.string();

        ASSERT_TRUE( parsed->Header.has_value() ) << path.string() << " states no header";
        ASSERT_TRUE( parsed->Header.has_value() ) << path.string() << " states no header";
        EXPECT_EQ( Desert::Assets::StatedVersion( parsed->Header, Desert::Assets::kSceneSchemaTag ),
                   kSceneVersion )
             << path.string();
        EXPECT_EQ( Desert::Assets::StatedVersion( parsed->Header, Desert::Assets::kUnitSchemaTag ), kUnitVersion )
             << path.string();
    }
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
