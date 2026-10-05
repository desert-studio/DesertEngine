// SCENE-DEPS: Header.Dependencies is gathered by ONE rule (Core::GatherSceneDependencies) on the save and in the
// v42 -> v43 step. The step's list is the rule's list; a path-only slot and a UI sequence's sounds count, a
// hosted header's own Guid and a prefab instance's overrides do not; and every corpus scene states exactly what
// the rule gathers from its own records - the list a save of that scene writes.

#include <SceneMigration.hpp>
#include <Common/Core/Constants.hpp>
#include <Engine/Core/Serialize/SceneDependencies.hpp>

#include <gtest/gtest.h>

#include <TestSupport/scratch_dir.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace Migration = Desert::Migration;

// NOLINTBEGIN(bugprone-unchecked-optional-access)
namespace
{
    void WriteText( const std::filesystem::path& file, const std::string& text )
    {
        std::filesystem::create_directories( file.parent_path() );
        std::ofstream( file, std::ios::binary ) << text;
    }

    std::string HeaderOnly( const std::string& kind, const std::string& guid, const std::string& tag )
    {
        return R"({"Header":{"Kind":")" + kind + R"(","Guid":")" + guid + R"(","Versions":{")" + tag +
               R"(":1},"Dependencies":[]}})";
    }

    // SceneMigrator's root for a scene (MigratorMain.cpp SceneOutputRoot): its census root, else its folder.
    std::filesystem::path SceneRoot( const std::filesystem::path& scene )
    {
        namespace Path = Common::Constants::Path;
        if ( const auto root = Path::RootForContentPath( Path::ContentDir::Scene, scene ) )
            return *root;
        return scene.parent_path();
    }

    Migration::SceneSerialized Parse( const std::string& text )
    {
        auto scene = rfl::json::read<Migration::SceneSerialized>( text );
        EXPECT_TRUE( scene ) << ( scene ? "" : scene.error().what() );
        return scene.value();
    }
} // namespace

TEST( SceneDependenciesMigration, TheStepStatesEveryReferenceOnceSortedAndNothingElse )
{
    const Desert::TestSupport::ScratchDir scratch( "scene-deps" );
    const auto                            assets = scratch.Path() / "Assets";
    WriteText( assets / "Materials" / "M_Cloud.demat",
               HeaderOnly( "Material", "0000000000000000000000000000000c", "MATL" ) );
    WriteText( assets / "Prefabs" / "P.deprefab",
               HeaderOnly( "Prefab", "0000000000000000000000000000000d", "SCNE" ) );

    auto scene = Parse( std::string(
         R"({"Header":{"Kind":"Scene","Guid":"00000000000000000000000000000001","Versions":{"SCNE":42,"UNIT":1}},)"
         R"("SceneName":"Deps","Entities":[)"
         R"({"id":1,"StaticMesh":{"MeshGuid":"0000000000000000000000000000000a","MaterialGuids":["0000000000000000000000000000000b"]},)"
         R"("AudioSource":{"Sound":{"Guid":"00000000000000000000000000000005","Path":"Sounds/S.desound"}},)"
         R"("VolumetricCloud":{"Material":"Materials/M_Cloud.demat"},)"
         R"("UIAnimation":{"Sequences":[{"Header":{"Kind":"Sequence","Guid":"000000000000000000000000000000ee",)"
         R"("Dependencies":["00000000000000000000000000000006"]}}]},)"
         R"("UIText":{"Text":"Hello.txt"}},)"
         R"({"id":2,"PrefabPath":"Prefabs/P.deprefab","PrefabOverrides":[{"Path":[3],)"
         R"("AudioSource":{"Sound":{"Guid":"000000000000000000000000000000ff","Path":"x"}}}]}]})" ) );

    const auto report = Migration::MigrateSceneDependenciesV42ToV43( scene, assets );
    ASSERT_TRUE( report.Refused.empty() ) << report.Refused.front();
    ASSERT_TRUE( scene.Header.has_value() );
    const std::vector<std::string> expected = {
         "00000000000000000000000000000005", "00000000000000000000000000000006",
         "0000000000000000000000000000000a", "0000000000000000000000000000000b",
         "0000000000000000000000000000000c", "0000000000000000000000000000000d" };
    EXPECT_EQ( scene.Header->Dependencies, expected );
    EXPECT_EQ( report.Stated, expected.size() );
}

TEST( SceneDependenciesMigration, APrefabWhoseFileStatesNoGuidRefusesAndStatesNothing )
{
    const Desert::TestSupport::ScratchDir scratch( "scene-deps-refuse" );
    auto       scene  = Parse( R"({"Header":{"Kind":"Scene","Guid":"00000000000000000000000000000001",)"
                                      R"("Versions":{"SCNE":42,"UNIT":1},"Dependencies":["keep"]},"SceneName":"R",)"
                                      R"("Entities":[{"id":2,"PrefabPath":"Prefabs/Missing.deprefab"}]})" );
    const auto report = Migration::MigrateSceneDependenciesV42ToV43( scene, scratch.Path() );
    ASSERT_EQ( report.Refused.size(), 1u );
    EXPECT_NE( report.Refused.front().find( "Missing.deprefab" ), std::string::npos );
    EXPECT_EQ( scene.Header->Dependencies, std::vector<std::string>{ "keep" } );
}

// The corpus as migrated == the corpus as a save would write it: the save gathers by the same rule from the
// records it writes, so a scene whose stated list differs from the rule's answer on its own records changes on
// its first save.
TEST( SceneDependenciesMigration, EveryCorpusSceneStatesWhatTheSaveWouldGather )
{
    const auto root  = Desert::TestSupport::RepositoryRoot();
    size_t     count = 0;
    for ( const char* dir : { "Editor/Resources/Assets/Scenes", "Editor/Resources/Engine/Maps/Templates",
                              "Games/Mirage/Content/Scenes", "Templates/Starter/Payload/Assets/Scenes",
                              "Desert/Tests/Data/Resources/Assets/Scenes" } )
    {
        if ( !std::filesystem::is_directory( root / dir ) )
            continue;
        for ( const auto& entry : std::filesystem::recursive_directory_iterator( root / dir ) )
        {
            if ( entry.path().extension() != ".desce" ||
                 entry.path().string().find( "Autosave" ) != std::string::npos )
                continue;
            std::ifstream     in( entry.path(), std::ios::binary );
            std::stringstream text;
            text << in.rdbuf();
            const auto scene = rfl::json::read<Migration::SceneSerialized>( text.str() );
            if ( !scene || !scene.value().Header.has_value() )
                continue;
            const auto& s      = scene.value();
            const auto  gather = Desert::Core::GatherSceneDependencies(
                 s.Entities, s.Settings.has_value() ? &*s.Settings : nullptr, SceneRoot( entry.path() ) );
            EXPECT_TRUE( gather.Refused.empty() ) << entry.path();
            EXPECT_EQ( s.Header->Dependencies, gather.Guids ) << entry.path();
            ++count;
        }
    }
    EXPECT_GT( count, 50u );
}
// NOLINTEND(bugprone-unchecked-optional-access)
