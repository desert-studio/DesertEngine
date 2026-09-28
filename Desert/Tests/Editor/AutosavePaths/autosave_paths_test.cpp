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

// Recovery offers the NEWEST copy anywhere in the mirrored tree, on disk.
TEST( AutosavePaths, LatestFindsTheNewestCopyInTheMirroredTree )
{
    const ProjectRootGuard guard( TempProject( "Latest" ), "Content" );
    const fs::path older = AS::PathFor( Path::SCENE_PATH / "Starter.desce", "Starter", AS::kPeriodicSuffix );
    const fs::path newer = AS::PathFor( Path::SCENE_PATH / "Levels" / "A.desce", "A", AS::kDeviceLostSuffix );
    for ( const fs::path& p : { older, newer } )
    {
        fs::create_directories( p.parent_path() );
        std::ofstream( p ) << "{}";
    }
    fs::last_write_time( older, fs::file_time_type::clock::now() - std::chrono::hours( 1 ) );
    std::ofstream( AS::Dir() / "Scenes" / "NotACopy.desce" ) << "{}";

    EXPECT_EQ( AS::LatestIn( AS::Dir() ), newer );
    EXPECT_FALSE( IsUnder( Path::ASSETS_PATH, AS::LatestIn( AS::Dir() ) ) );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
