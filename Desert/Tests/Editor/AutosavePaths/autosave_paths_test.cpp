// AN AUTOSAVE IS NOT PROJECT CONTENT (AUTO1, UE's <Project>/Saved/Autosaves).
//
// While the copies lived in <Assets>/Scenes/Autosave/ every content scan saw them: the cooked-registry
// gate folded a copy's GUID into Starter's and the scene-version gate read an old-schema copy as corpus.
// These tests hold the relation that ends it — every recovery copy is OUTSIDE the assets root, under the
// project's Saved/ — and the round trip recovery depends on: copy -> the scene it stands for.
#include <Editor/Core/AutosavePaths.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <fstream>
#include <optional>
#include <vector>
#include <string>
#include <iterator>
#include <algorithm>

namespace fs   = std::filesystem;
namespace Path = Common::Constants::Path;
namespace AS   = Desert::Editor::Autosave;

namespace
{
    struct ProjectRootGuard
    {
        ProjectRootGuard( const fs::path& projectDir, const fs::path& assetsRoot )
        {
            Path::SetProjectRoot( projectDir, assetsRoot );
        }
        ~ProjectRootGuard()
        {
            Path::ResetToSandbox();
        }
        ProjectRootGuard( const ProjectRootGuard& )            = delete;
        ProjectRootGuard& operator=( const ProjectRootGuard& ) = delete;
    };

    bool IsUnder( const fs::path& root, const fs::path& p )
    {
        const fs::path rel =
             fs::absolute( p ).lexically_normal().lexically_relative( fs::absolute( root ).lexically_normal() );
        return !rel.empty() && *rel.begin() != "..";
    }

    fs::path TempProject( const char* name )
    {
        const fs::path dir = fs::temp_directory_path() / "DesertAutosavePaths" / name;
        fs::remove_all( dir );
        fs::create_directories( dir );
        return dir;
    }
} // namespace

// THE INVARIANT: no recovery copy lands under the assets root, in a project or in the sandbox, for a
// saved scene, a nested one, a never-saved one, or the device-lost save.
TEST( AutosavePaths, NoRecoveryCopyIsEverUnderTheAssetsRoot )
{
    const fs::path project = TempProject( "Invariant" );
    for ( const bool withProject : { false, true } )
    {
        std::optional<ProjectRootGuard> guard;
        if ( withProject )
            guard.emplace( project, "Content" );

        const fs::path scenes[] = {
             Path::SCENE_PATH / "Starter.desce", Path::SCENE_PATH / "Levels" / "A.desce", {} };
        for ( const fs::path& scene : scenes )
        {
            for ( const auto suffix : { AS::kPeriodicSuffix, AS::kDeviceLostSuffix } )
            {
                const fs::path copy = AS::PathFor( scene, "My Scene", suffix );
                EXPECT_FALSE( IsUnder( Path::ASSETS_PATH, copy ) ) << copy << " is under " << Path::ASSETS_PATH;
                EXPECT_TRUE( IsUnder( Path::CurrentProjectRoot().ProjectDir / "Saved" / "Autosaves", copy ) )
                     << copy;
            }
        }
    }
}

// The copy MIRRORS the scene tree, so two scenes with one name in two folders never share a copy.
TEST( AutosavePaths, CopiesMirrorTheSceneTree )
{
    const ProjectRootGuard guard( TempProject( "Mirror" ), "Content" );
    const fs::path         a = AS::PathFor( Path::SCENE_PATH / "Levels" / "A.desce", "A", AS::kPeriodicSuffix );
    const fs::path         b = AS::PathFor( Path::SCENE_PATH / "Other" / "A.desce", "A", AS::kPeriodicSuffix );
    EXPECT_NE( a, b );
    EXPECT_EQ( a, AS::Dir() / "Scenes" / "Levels" / "A_autosave.desce" );
}

// THE RECOVERY ROUND TRIP: the copy of a scene opens as that scene (so Ctrl+S writes the user's file),
// the copy of a never-saved scene opens untitled (empty: Save As), and a file that is not a recovery copy
// is not claimed.
TEST( AutosavePaths, RecoveryRoundTripNamesTheOriginalScene )
{
    const ProjectRootGuard guard( TempProject( "RoundTrip" ), "Content" );
    for ( const fs::path& scene : { Path::SCENE_PATH / "Starter.desce", Path::SCENE_PATH / "Levels" / "A.desce" } )
    {
        for ( const auto suffix : { AS::kPeriodicSuffix, AS::kDeviceLostSuffix } )
        {
            const auto original = AS::SceneFor( AS::PathFor( scene, "ignored", suffix ) );
            ASSERT_TRUE( original.has_value() ) << scene;
            EXPECT_EQ( fs::absolute( *original ).lexically_normal(), fs::absolute( scene ).lexically_normal() );
        }
    }

    const auto untitled = AS::SceneFor( AS::PathFor( {}, "New Scene", AS::kPeriodicSuffix ) );
    ASSERT_TRUE( untitled.has_value() );
    EXPECT_TRUE( untitled->empty() );

    EXPECT_FALSE( AS::SceneFor( Path::SCENE_PATH / "Starter.desce" ).has_value() );
    EXPECT_FALSE( AS::SceneFor( AS::Dir() / "Scenes" / "Starter.desce" ).has_value() );
}


namespace
{
    constexpr uint32_t kScene = Common::Content::FourCC( "SCNE" );
    constexpr uint32_t kUnit  = Common::Content::FourCC( "UNIT" );
    // The generation this "build" opens; the suite does not link the engine, so the numbers are its own.
    constexpr AS::StatedGenerations kCurrent{ 39, 1 };

    // A recovery copy stating (scene, unit) in its header, the way SceneSerializer writes one.
    void WriteCopy( const fs::path& p, int scene, int unit )
    {
        fs::create_directories( p.parent_path() );
        std::ofstream( p ) << R"({"Header":{"Kind":"Scene","Guid":"0123456789abcdef0123456789abcdef","Versions":{"SCNE":)"
                           << scene << R"(,"UNIT":)" << unit << R"(},"Dependencies":[]},"SceneName":"S"})";
    }

    std::string BytesOf( const fs::path& p )
    {
        std::ifstream in( p, std::ios::binary );
        return { std::istreambuf_iterator<char>( in ), std::istreambuf_iterator<char>() };
    }

    std::vector<fs::path> FilesUnder( const fs::path& root )
    {
        std::vector<fs::path> files;
        for ( const auto& e : fs::recursive_directory_iterator( root ) )
            files.push_back( e.path() );
        std::sort( files.begin(), files.end() );
        return files;
    }
} // namespace

// The header reader is the instrument the choice below rests on: if it read 0 for everything, every copy
// would be "not offered" and the refusal side of the tests below would pass on a broken reader.
TEST( AutosavePaths, StatedInReadsBothGenerationsFromTheHeader )
{
    const fs::path copy = TempProject( "Stated" ) / "A_autosave.desce";
    WriteCopy( copy, 38, 1 );
    EXPECT_EQ( AS::StatedIn( copy, kScene, kUnit ), ( AS::StatedGenerations{ 38, 1 } ) );
    std::ofstream( copy ) << "{}";
    EXPECT_EQ( AS::StatedIn( copy, kScene, kUnit ), ( AS::StatedGenerations{ 0, 0 } ) ) << "absent is 0";
}

TEST( AutosavePaths, RecoveryOffersTheNewestCopyAtTheCurrentVersion )
{
    const ProjectRootGuard guard( TempProject( "Latest" ), "Content" );
    const fs::path older = AS::PathFor( Path::SCENE_PATH / "Starter.desce", "Starter", AS::kPeriodicSuffix );
    const fs::path newer = AS::PathFor( Path::SCENE_PATH / "Levels" / "A.desce", "A", AS::kDeviceLostSuffix );
    WriteCopy( older, kCurrent.Scene, kCurrent.Unit );
    WriteCopy( newer, kCurrent.Scene, kCurrent.Unit );
    fs::last_write_time( older, fs::file_time_type::clock::now() - std::chrono::hours( 1 ) );
    WriteCopy( AS::Dir() / "Scenes" / "NotACopy.desce", kCurrent.Scene, kCurrent.Unit );

    const AS::RecoveryChoice choice = AS::ChooseRecovery( AS::Dir(), kCurrent, kScene, kUnit );
    EXPECT_EQ( choice.Offered, newer );
    EXPECT_TRUE( choice.NotOffered.empty() );
    EXPECT_FALSE( IsUnder( Path::ASSETS_PATH, choice.Offered ) );
}

// THE LEAD'S DECISION (AUTO1), HELD: an older copy -- even the NEWEST file on disk -- is not offered, is
// reported by path and stated version, and is NOT migrated: the project tree has the same files afterwards,
// the copy's bytes are unchanged, and no material (or anything else) appears anywhere.
TEST( AutosavePaths, AnOlderCopyIsReportedNotOfferedAndNotMigrated )
{
    const fs::path         project = TempProject( "Older" );
    const ProjectRootGuard guard( project, "Content" );
    const fs::path current = AS::PathFor( Path::SCENE_PATH / "Starter.desce", "Starter", AS::kPeriodicSuffix );
    const fs::path stale   = AS::PathFor( Path::SCENE_PATH / "Levels" / "A.desce", "A", AS::kPeriodicSuffix );
    const fs::path wrongUnit = AS::PathFor( {}, "Untitled Scene", AS::kDeviceLostSuffix );
    WriteCopy( current, kCurrent.Scene, kCurrent.Unit );
    WriteCopy( stale, kCurrent.Scene - 1, kCurrent.Unit );
    WriteCopy( wrongUnit, kCurrent.Scene, kCurrent.Unit - 1 );
    fs::last_write_time( current, fs::file_time_type::clock::now() - std::chrono::hours( 1 ) );

    const auto        before     = FilesUnder( project );
    const std::string staleBytes = BytesOf( stale );

    const AS::RecoveryChoice choice = AS::ChooseRecovery( AS::Dir(), kCurrent, kScene, kUnit );
    EXPECT_EQ( choice.Offered, current ) << "the newest file on disk is at an older version and must not be offered";
    ASSERT_EQ( choice.NotOffered.size(), 2u );
    for ( const AS::NotOfferedCopy& copy : choice.NotOffered )
    {
        if ( copy.Path == stale )
        {
            EXPECT_EQ( copy.Stated, ( AS::StatedGenerations{ kCurrent.Scene - 1, kCurrent.Unit } ) );
        }
        else
        {
            EXPECT_EQ( copy.Path, wrongUnit );
            EXPECT_EQ( copy.Stated, ( AS::StatedGenerations{ kCurrent.Scene, kCurrent.Unit - 1 } ) );
        }
    }

    EXPECT_EQ( FilesUnder( project ), before ) << "choosing created or removed a file";
    EXPECT_EQ( BytesOf( stale ), staleBytes ) << "the older copy was rewritten";

    // Only older copies left: nothing is offered at all.
    fs::remove( current );
    EXPECT_TRUE( AS::ChooseRecovery( AS::Dir(), kCurrent, kScene, kUnit ).Offered.empty() );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
