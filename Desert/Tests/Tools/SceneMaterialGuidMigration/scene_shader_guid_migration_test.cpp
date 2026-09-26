// SCNE 31 (T7i): a MaterialComponent stops naming its shader by file stem (`"ShaderName": "MatProbe"`) and states
// `"Shader": {"Guid": <the .shader header GUID>, "Path": "engine:Shaders/<relative>"}`, the form MATL 4 gave the
// `.demat` (MigrateShaderGuidsV30ToV31). An empty name becomes no key; a stem no `.shader` carries REFUSES the
// file, naming the entity; a second run changes nothing.

#include <SceneMigration.hpp>
#include <Engine/Core/Serialize/SceneFormat.hpp>

#include <rflcpp/rfl/json.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

namespace Migration = Desert::Migration;
namespace fs        = std::filesystem;

namespace
{
    // A real shader of the repository (run from the repository root, like every suite), copied into a
    // throwaway <root>/Resources/Shaders so the fixture states exactly one file with that stem.
    const char* kShaderSource = "Editor/Resources/Shaders/Programs/Graph/MatProbe.shader";
    const char* kShaderGuid   = "40293d62f22f46a49912f44c6c024d06";
    const char* kShaderKey    = "engine:Shaders/Programs/Graph/MatProbe.shader";

    struct Project
    {
        fs::path Root;
        fs::path AssetsRoot;

        explicit Project( const char* name )
        {
            Root       = fs::temp_directory_path() / ( std::string( "t7i_" ) + name );
            AssetsRoot = Root / "Resources" / "Assets";
            fs::remove_all( Root );
            fs::create_directories( AssetsRoot );
            const fs::path target = Root / "Resources" / "Shaders" / "Programs" / "Graph" / "MatProbe.shader";
            fs::create_directories( target.parent_path() );
            fs::copy_file( kShaderSource, target );
        }
        ~Project()
        {
            std::error_code ec;
            fs::remove_all( Root, ec );
        }
        Project( const Project& )            = delete;
        Project& operator=( const Project& ) = delete;
    };

    constexpr int kBeforeShaderGuids = Migration::kSceneVersionShaderGuids - 1;

    // Three materials: a named shader, an empty name, and one inside a prefab override.
    std::string V30Scene( const std::string& shaderName )
    {
        return std::string( R"({"Header":{"Kind":"Scene","Guid":"00000000000000000000000000000001",)" ) +
               R"("Versions":{"SCNE":)" + std::to_string( kBeforeShaderGuids ) +
               R"(,"UNIT":1},"Dependencies":[]},"SceneName":"S","Entities":[)" +
               R"({"id":1,"Tag":"Probe","Material":{"ShaderName":")" + shaderName +
               R"(","Params":[{"Name":"Tint","Value":[1.0,0.5,0.25,1.0]}]}},)" +
               R"({"id":2,"Tag":"Blank","Material":{"ShaderName":""}},)" +
               R"({"id":3,"Tag":"Inst","PrefabPath":"p.deprefab","PrefabOverrides":[{"Path":[],)" +
               R"("Material":{"ShaderName":")" + shaderName + R"("}}]}]})";
    }

    Migration::SceneSerialized Parse( const std::string& json )
    {
        auto parsed = rfl::json::read<Migration::SceneSerialized>( json );
        EXPECT_TRUE( parsed.has_value() ) << ( parsed.has_value() ? "" : parsed.error().what() );
        return parsed.has_value() ? parsed.value() : Migration::SceneSerialized{};
    }

    std::size_t Count( const std::string& text, const std::string& what )
    {
        std::size_t n = 0;
        for ( auto at = text.find( what ); at != std::string::npos; at = text.find( what, at + 1 ) )
            ++n;
        return n;
    }

    std::string ShaderRef()
    {
        return std::string( R"("Shader":{"Guid":")" ) + kShaderGuid + R"(","Path":")" + kShaderKey + R"("})";
    }
} // namespace

TEST( SceneShaderGuidMigration, AShaderNameBecomesTheHeaderGuidAndTheKeyAndAnEmptyNameNoKey )
{
    const Project project( "name" );
    auto          scene  = Parse( V30Scene( "MatProbe" ) );
    const auto    report = Migration::MigrateScene( scene, project.AssetsRoot, "", {} );

    ASSERT_TRUE( report.Refused.empty() ) << report.Refused;
    EXPECT_TRUE( report.ShaderGuidsRaised );
    EXPECT_FALSE( report.SpriteGuidsRaised ) << "the file was already past the sprite-GUID step";
    EXPECT_EQ( report.ShaderGuids.Rewritten, 2 );
    const std::string text = rfl::json::write( scene );
    EXPECT_EQ( Count( text, ShaderRef() ), 2u ) << text;
    EXPECT_EQ( Count( text, "ShaderName" ), 0u ) << "no by-name key survives, the empty one included: " << text;
    EXPECT_EQ( Count( text, R"("Tint")" ), 1u ) << "the rest of the block is kept: " << text;
}

TEST( SceneShaderGuidMigration, ASecondRunChangesNothing )
{
    const Project project( "twice" );
    auto          scene = Parse( V30Scene( "MatProbe" ) );
    ASSERT_TRUE( Migration::MigrateScene( scene, project.AssetsRoot, "", {} ).Refused.empty() );
    const std::string once  = rfl::json::write( scene );
    const auto        again = Migration::MigrateScene( scene, project.AssetsRoot, "", {} );
    EXPECT_TRUE( again.Refused.empty() ) << again.Refused;
    EXPECT_FALSE( again.Changed() ) << "a file at SCNE 31 is not raised again";
    EXPECT_EQ( rfl::json::write( scene ), once );
}

TEST( SceneShaderGuidMigration, ANameNoShaderCarriesRefusesTheFileNamingTheEntity )
{
    const Project project( "unknown" );
    auto          scene  = Parse( V30Scene( "NoSuchShader" ) );
    const auto    report = Migration::MigrateScene( scene, project.AssetsRoot, "", {} );

    ASSERT_FALSE( report.Refused.empty() );
    EXPECT_NE( report.Refused.find( "Probe > Material.ShaderName = 'NoSuchShader'" ), std::string::npos )
         << report.Refused;
    EXPECT_NE( report.Refused.find( "Inst > PrefabOverrides[0]" ), std::string::npos ) << report.Refused;
}

TEST( SceneShaderGuidMigration, TheEngineRequiresTheShaderGuidGeneration )
{
    EXPECT_EQ( Desert::Core::kSceneVersion, Migration::kSceneVersionShaderGuids );
}
