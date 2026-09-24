// THE PREFAB LOADER'S VERSION GATE: what this engine will read, what it refuses, and what it says.
//
// A .deprefab carries the scene's own EntityData, written by the same ComponentRegistry — yet for its
// whole life it carried NO version at all, while scenes moved through eleven schema generations behind a
// stamped gate. Every one of those moves broke saved prefabs silently, and the place it surfaced was the
// user's load (the crash a scene naming a deleted prefab used to cause — fixed by Ф1 — was this class of
// defect wearing a different symptom). Д28 gives prefabs exactly what scenes have: the stamp, the named
// refusal, the migrator, and this suite.
//
// THE RELATION UNDER TEST is the one the task names: the version IN THE FILE against the version THE
// ENGINE REQUIRES. It is held in both directions — the gate refuses every generation that is not the
// head, and the SAVER (WritePrefabJson, the one writer of .deprefab text) always produces a file the
// gate accepts, so the engine cannot write a prefab it would then refuse to read.
//
// PURE. It parses JSON and calls functions over the parsed tree. No GPU, no asset manager, no scene.

#include <Engine/Assets/Prefab/PrefabFormat.hpp>

#include <rflcpp/rfl/json.hpp>

#include <gtest/gtest.h>

#include <filesystem>
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

using Desert::Assets::ParseLoadablePrefab;
using Desert::Assets::PrefabData;
using Desert::Assets::PrefabIsAtCurrentVersion;
using Desert::Assets::RefusePrefabVersion;
using Desert::Assets::WritePrefabJson;
using Desert::Core::kSceneVersion;
using Desert::Core::kUnitVersion;

namespace
{
    /// A tree at the two versions asked for, carrying one recognizable entity so the round-trip tests can
    /// see content survive (the gate itself reads the two integers and nothing else).
    PrefabData At( int sceneVersion, int unitVersion )
    {
        PrefabData prefab;
        prefab.Name = "Fixture";
        Desert::Assets::EntityData entity;
        entity.Tag = "FixtureRoot";
        prefab.Entities.push_back( entity );
        prefab.Header = FixtureHeader( Common::Content::ContentKind::Prefab, sceneVersion, unitVersion );
        return prefab;
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
    // working directory is not fixed. Same shape as SceneVersionGate.
    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            std::ifstream probe( prefix + "Desert/Desert/Source/Engine/Assets/Prefab/PrefabFormat.hpp" );
            if ( probe )
                return prefix;
            prefix += "../";
        }
        return {};
    }

    // The PROJECT'S ASSET ROOTS, swept to their full depth — including files git does not track.
    //
    // TWO DECISIONS, AND THE SECOND IS NOT AN OVERSIGHT — do not "complete" this by walking the
    // repository root again:
    //
    // 1. DEPTH IS TOTAL, AND UNTRACKED FILES COUNT. A prefab is saved wherever the user typed a path
    //    under the assets tree, and an editor autosave or a file that never reached git is EXACTLY the
    //    one most likely to have been written by an older build. Listing git's index instead, or
    //    blessing one Prefabs/ subdirectory, is a corpus test reporting green about the files it
    //    happened to look at.
    //
    // 2. BREADTH STOPS AT THE PROJECT. This sweep began at the repository root and was narrowed on
    //    2026-09-06, because the root is not only this project: `.claude/worktrees/` holds a full
    //    checkout of ELEVEN other branches, so a prefab saved by another developer in unrelated work
    //    would turn this suite red and name THIS branch as the cause. A test that fails for a file
    //    outside what it is testing does not report on what it claims to; it reports plausibly on
    //    something else, which is worse than not running. SceneVersionGate scopes itself to the scene
    //    directory for the same reason, and this is the prefab twin of that decision.
    //
    // Adding a second project (a Runtime or sample assets tree) means adding its root to kAssetRoots,
    // not widening the walk back to the repository.
    std::vector<std::filesystem::path> RepositoryPrefabs()
    {
        // Relative to the repository root. Every place this engine writes a .deprefab lives under an
        // assets root — Common::Constants::Path::PREFAB_PATH is `<assets>/Prefabs/`, and the editor's
        // save dialog is rooted in the same tree.
        static constexpr const char* kAssetRoots[] = { "Editor/Resources/Assets" };

        std::vector<std::filesystem::path> prefabs;
        const std::filesystem::path        root = RepoRoot();

        for ( const char* assetRoot : kAssetRoots )
        {
            std::error_code ec;
            for ( auto it = std::filesystem::recursive_directory_iterator( root / assetRoot, ec );
                  it != std::filesystem::recursive_directory_iterator(); it.increment( ec ) )
            {
                if ( it->is_regular_file() && it->path().extension() == ".deprefab" )
                    prefabs.push_back( it->path() );
            }
        }
        return prefabs;
    }

    // The asset roots the sweep above claims to cover must actually be there. Without this, renaming or
    // moving the assets tree turns the corpus tests into a silent pass over zero files — the same
    // vacuous green SceneVersionGate guards with its `>= 40` scene count, which a repository shipping
    // zero prefabs cannot borrow.
    bool AssetRootsExist()
    {
        const std::filesystem::path root = RepoRoot();
        std::error_code             ec;
        return std::filesystem::is_directory( root / "Editor/Resources/Assets", ec );
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

TEST( PrefabVersionGate, ATreeAtBothHeadsIsCurrentAndOneOffEitherAxisIsNot )
{
    EXPECT_TRUE( PrefabIsAtCurrentVersion( At( kSceneVersion, kUnitVersion ) ) );

    EXPECT_FALSE( PrefabIsAtCurrentVersion( At( kSceneVersion - 1, kUnitVersion ) ) );
    EXPECT_FALSE( PrefabIsAtCurrentVersion( At( kSceneVersion, kUnitVersion - 1 ) ) );

    // AHEAD is refused too: a file stamped newer was written by a build that knows something this one
    // does not, and there is no migration DOWN for the refusal to point at.
    EXPECT_FALSE( PrefabIsAtCurrentVersion( At( kSceneVersion + 1, kUnitVersion ) ) );
}

// AN ABSENT INTEGER IS VERSION 0, NOT "CURRENT" — and for prefabs this is the load-bearing case rather
// than a corner: EVERY .deprefab written before Д28 states neither integer. Reading absent as current
// would wave the entire pre-gate population through, which is the exact silence this gate replaces.
TEST( PrefabVersionGate, AnAbsentVersionIntegerIsZeroRatherThanCurrent )
{
    PrefabData noScene;
    noScene.Header =
         FixtureHeader( Common::Content::ContentKind::Prefab, std::nullopt, kUnitVersion ); // scene version absent
    EXPECT_FALSE( PrefabIsAtCurrentVersion( noScene ) );

    PrefabData noUnit;
    noUnit.Header =
         FixtureHeader( Common::Content::ContentKind::Prefab, kSceneVersion, std::nullopt ); // unit version absent
    EXPECT_FALSE( PrefabIsAtCurrentVersion( noUnit ) );

    PrefabData neither;
    EXPECT_FALSE( PrefabIsAtCurrentVersion( neither ) );
}

// ---------------------------------------------------------------------------------------------------
// 2. WHAT THE REFUSAL SAYS — the four things a reader can act on without opening any source
// ---------------------------------------------------------------------------------------------------

TEST( PrefabVersionGate, TheRefusalNamesTheFileTheVersionTheTargetAndTheCommand )
{
    const std::string message = RefusePrefabVersion( "Assets/Prefabs/Old.deprefab", 0, 0 );

    // The file.
    EXPECT_TRUE( Mentions( message, "Assets/Prefabs/Old.deprefab" ) ) << message;
    // What it is: both found versions (the unversioned pre-Д28 file reads as v0/v0).
    EXPECT_TRUE( Mentions( message, "v0" ) ) << message;
    // What is needed: both required versions, taken from the constants so this cannot rot when the head
    // moves.
    EXPECT_TRUE( Mentions( message, "v" + std::to_string( kSceneVersion ) ) ) << message;
    EXPECT_TRUE( Mentions( message, "v" + std::to_string( kUnitVersion ) ) ) << message;
    // The command — the tool AND the argument, not just the tool's name.
    EXPECT_TRUE( Mentions( message, "SceneMigrator" ) ) << message;
    EXPECT_TRUE( Mentions( message, "SceneMigrator \"Assets/Prefabs/Old.deprefab\"" ) ) << message;
}

TEST( PrefabVersionGate, TheRefusalSaysNothingWasLoaded )
{
    const std::string message = RefusePrefabVersion( "Old.deprefab", 1, 0 );
    EXPECT_TRUE( Mentions( message, "NOTHING WAS LOADED" ) ) << message;
}

// The overload that reads the tree agrees with the one that takes the two integers, including on the
// absent-is-zero rule. Two spellings of one message is how the two drift.
TEST( PrefabVersionGate, TheTreeOverloadReportsTheSameVersionsTheTreeCarries )
{
    EXPECT_EQ( RefusePrefabVersion( "P.deprefab", At( 3, 0 ) ), RefusePrefabVersion( "P.deprefab", 3, 0 ) );

    PrefabData neither; // both absent
    EXPECT_EQ( RefusePrefabVersion( "P.deprefab", neither ), RefusePrefabVersion( "P.deprefab", 0, 0 ) );
}

// ---------------------------------------------------------------------------------------------------
// 3. WHAT PARSING PRODUCES — a tree, or an error, and never both
// ---------------------------------------------------------------------------------------------------

TEST( PrefabVersionGate, ACurrentFileParsesAndTheTreeComesBack )
{
    const auto loadable = ParseLoadablePrefab( "Current.deprefab", JsonAt( kSceneVersion, kUnitVersion ) );

    ASSERT_TRUE( static_cast<bool>( loadable ) ) << loadable.GetError();
    EXPECT_EQ( loadable.GetValue().Name, "Fixture" );
    ASSERT_EQ( loadable.GetValue().Entities.size(), 1u );
    EXPECT_EQ( loadable.GetValue().Entities.front().Tag.value_or( "" ), "FixtureRoot" );
}

// EVERY generation that is not the head is refused and named by its own number — a gate written as
// "< head" and one written as "!= head" disagree about a file from the future.
TEST( PrefabVersionGate, EveryOtherGenerationIsRefusedAndNamedByItsOwnNumber )
{
    for ( int version : { 0, 1, kSceneVersion - 1, kSceneVersion + 1 } )
    {
        const auto loadable = ParseLoadablePrefab( "Old.deprefab", JsonAt( version, kUnitVersion ) );

        ASSERT_FALSE( static_cast<bool>( loadable ) ) << "scene schema v" << version << " was accepted";
        EXPECT_TRUE( Mentions( loadable.GetError(), "v" + std::to_string( version ) ) )
             << "the refusal for v" << version << " does not say which version it found: " << loadable.GetError();
        EXPECT_TRUE( Mentions( loadable.GetError(), "SceneMigrator" ) ) << loadable.GetError();
    }
}

// THE PRE-Д28 FILE ITSELF: JSON with no version keys at all — the only .deprefab generation that ever
// actually existed. It parses (both integers are optional for exactly this reason) and is then refused
// by name rather than read into the current schema on faith.
TEST( PrefabVersionGate, AnUnversionedPrefabParsesAndIsRefusedAsVersionZero )
{
    PrefabData unstamped;
    unstamped.Name      = "Legacy";
    const auto loadable = ParseLoadablePrefab( "Legacy.deprefab", rfl::json::write( unstamped ) );

    ASSERT_FALSE( static_cast<bool>( loadable ) );
    EXPECT_TRUE( Mentions( loadable.GetError(), "v0" ) ) << loadable.GetError();
    EXPECT_TRUE( Mentions( loadable.GetError(), "SceneMigrator" ) ) << loadable.GetError();
}

// Text that is not a prefab at all fails as a READ rather than as a version. The two failures are worth
// telling apart: one is fixed by running a tool, the other is not — so the read failure must NOT name it.
TEST( PrefabVersionGate, TextThatIsNotAPrefabFailsAsAReadAndStillNamesTheFile )
{
    const auto loadable = ParseLoadablePrefab( "Broken.deprefab", "{ this is not json" );

    ASSERT_FALSE( static_cast<bool>( loadable ) );
    EXPECT_TRUE( Mentions( loadable.GetError(), "Broken.deprefab" ) ) << loadable.GetError();
    EXPECT_TRUE( Mentions( loadable.GetError(), "not a readable prefab file" ) ) << loadable.GetError();
    EXPECT_FALSE( Mentions( loadable.GetError(), "SceneMigrator" ) )
         << "a file that is not a prefab is not fixed by migrating it: " << loadable.GetError();
}

// ---------------------------------------------------------------------------------------------------
// 4. THE SAVER AND THE GATE — one relation, held from the writing side
// ---------------------------------------------------------------------------------------------------

// WritePrefabJson is the one writer of .deprefab text (PrefabAsset::Serialize and SceneMigrator both go
// through it), and everything it writes must satisfy the gate that will read it back. Delete the stamp
// inside WritePrefabJson and this is the test that goes red — verified by that exact mutation.
TEST( PrefabVersionGate, WhatTheSaverWritesTheGateAccepts )
{
    PrefabData authored; // as PrefabAsset::Serialize builds it: content, no versions yet
    authored.Name = "Saved";
    Desert::Assets::EntityData entity;
    entity.Tag = "Root";
    authored.Entities.push_back( entity );

    const auto written  = WritePrefabJson( authored );
    const auto loadable = ParseLoadablePrefab( "Saved.deprefab", written.GetValue() );

    ASSERT_TRUE( static_cast<bool>( loadable ) ) << loadable.GetError();
    // Explicitly stated, not defaulted: the file carries both keys at both heads.
    ASSERT_TRUE( loadable.GetValue().Header.has_value() );
    ASSERT_TRUE( loadable.GetValue().Header.has_value() );
    EXPECT_EQ( Desert::Assets::StatedVersion( loadable.GetValue().Header, Desert::Assets::kSceneSchemaTag ),
               kSceneVersion );
    EXPECT_EQ( Desert::Assets::StatedVersion( loadable.GetValue().Header, Desert::Assets::kUnitSchemaTag ),
               kUnitVersion );
    // And the content is the content — the stamp is additive.
    ASSERT_EQ( loadable.GetValue().Entities.size(), 1u );
    EXPECT_EQ( loadable.GetValue().Entities.front().Tag.value_or( "" ), "Root" );
}

// A stale stamp cannot ride through the saver: whatever generation the in-memory tree carried (a tree
// parsed years from now, a hand-built one), the file written states the CURRENT one. The saver is where
// "this build writes its own generation" is enforced, so it is asserted against the saver.
TEST( PrefabVersionGate, TheSaverOverwritesAStaleStampWithTheCurrentOne )
{
    const auto written = WritePrefabJson( At( 1, 0 ) );
    ASSERT_TRUE( static_cast<bool>( written ) ) << written.GetError();
    const auto loadable = ParseLoadablePrefab( "Restamped.deprefab", written.GetValue() );

    ASSERT_TRUE( static_cast<bool>( loadable ) ) << loadable.GetError();
    EXPECT_EQ( Desert::Assets::StatedVersion( loadable.GetValue().Header, Desert::Assets::kSceneSchemaTag ),
               kSceneVersion );
    EXPECT_EQ( Desert::Assets::StatedVersion( loadable.GetValue().Header, Desert::Assets::kUnitSchemaTag ),
               kUnitVersion );
}

// ---------------------------------------------------------------------------------------------------
// 5. THE CORPUS — every .deprefab in the project's assets, tracked or not, is one this engine will load
// ---------------------------------------------------------------------------------------------------

// 5a. The sweep is real: the repository root was found from wherever the runner started, AND the asset
// roots it walks are on disk. Either half missing turns 5b/5c into a vacuous pass over zero files, and a
// corpus test that passes because it looked at nothing is the failure mode this pair exists to catch.
//
// THERE IS DELIBERATELY NO MINIMUM COUNT, unlike the scene corpus's >= 40: the repository ships ZERO
// .deprefab files today (measured when this suite was written), and inventing a fixture prefab just to
// have a corpus would test the fixture. That is exactly why the two checks below carry the weight — with
// no count to fall back on, "the directory is there" is the only thing standing between a real sweep and
// a green report about nothing. The suite is armed for the first prefab that appears: the day one is
// saved under the assets tree (committed, autosaved, or never tracked at all), 5b and 5c hold it to the
// gate with no edit here.
TEST( PrefabVersionGateCorpus, TheSweepRunsFromTheRepositoryRootOverAssetRootsThatExist )
{
    ASSERT_FALSE( RepoRoot().empty() ) << "repository root not found - run from the workspace root or build/Bin";
    EXPECT_TRUE( AssetRootsExist() ) << "the assets tree this sweep walks is not there - 5b and 5c would "
                                        "pass over zero files while claiming to have checked the corpus";
}

// 5b. Every prefab on disk passes the exact gate the loader applies. A failure here is not a broken test:
// it means a real file in this tree will not load, and the fix is the one the message names.
TEST( PrefabVersionGateCorpus, EveryPrefabOnDiskIsOneThisEngineWillLoad )
{
    for ( const auto& path : RepositoryPrefabs() )
    {
        const auto loadable = ParseLoadablePrefab( path.string(), ReadAll( path ) );
        EXPECT_TRUE( static_cast<bool>( loadable ) ) << loadable.GetError();
    }
}

// 5c. And each one is at the head by STATING it, not by defaulting to it — the same distinction the
// scene corpus draws, for the same reason: 5b would also pass a file whose integers were right by luck.
TEST( PrefabVersionGateCorpus, EveryPrefabStatesBothVersionIntegersExplicitly )
{
    for ( const auto& path : RepositoryPrefabs() )
    {
        const auto parsed = rfl::json::read<PrefabData>( ReadAll( path ) );
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
