// MigratePBRSurfaceShaderV40ToV41 (MAT1g): a Material block whose Shader names the `Role PBRSurface` template
// (by the GUID the shader file states) overrode nothing, and the key goes; every other key of the block, an
// override template's reference, and the rest of the record stay. A reference that cannot be judged - no such
// file, a GUID the file does not state - refuses the file by name.

#include <SceneMigration.hpp>
#include <Engine/Core/Serialize/SceneFormat.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace Migration = Desert::Migration;
namespace fs        = std::filesystem;
using Desert::Assets::EntityData;
using Desert::Assets::PrefabOverrideData;

// NOLINTBEGIN(bugprone-unchecked-optional-access)
namespace
{
    constexpr const char* kPBRGuid      = "1111aaaa1111aaaa1111aaaa1111aaaa";
    constexpr const char* kOverrideGuid = "2222bbbb2222bbbb2222bbbb2222bbbb";

    struct ShaderTree
    {
        fs::path Root;
        ShaderTree()
        {
            Root = fs::temp_directory_path() /
                   ( "pbr_surface_shader_migration_" + std::to_string( ::testing::UnitTest::GetInstance()->random_seed() ) +
                     "_" + ::testing::UnitTest::GetInstance()->current_test_info()->name() );
            fs::remove_all( Root );
            fs::create_directories( Root / "Shaders" );
            Write( "StaticMeshPBR", kPBRGuid, "    Role PBRSurface\n    Default Surface\n" );
            Write( "Unlit", kOverrideGuid, "    Role DebugColor\n" );
        }
        ~ShaderTree()
        {
            std::error_code ec;
            fs::remove_all( Root, ec );
        }
        void Write( const std::string& name, const std::string& guid, const std::string& manifest ) const
        {
            std::ofstream( Root / "Shaders" / ( name + ".shader" ) )
                 << "// DesertAsset {\"Kind\":\"Shader\",\"Guid\":\"" << guid
                 << "\",\"Versions\":{\"SHDR\":1},\"Dependencies\":[]}\nShader \"" << name
                 << "\"\n{\n    Domain Surface\n"
                 << manifest << "}\n";
        }
    };

    rfl::Generic::Object MaterialBlock( const std::string& guid, const std::string& path )
    {
        rfl::Generic::Object ref;
        ref["Guid"] = rfl::Generic( guid );
        ref["Path"] = rfl::Generic( path );
        rfl::Generic::Object block;
        block["Shader"] = rfl::Generic( std::move( ref ) );
        block["Params"] = rfl::Generic( rfl::Generic::Array{} );
        return block;
    }

    EntityData Entity( const char* tag, rfl::Generic::Object material )
    {
        EntityData entity;
        entity.Tag                    = tag;
        entity.Components["Material"] = rfl::Generic( std::move( material ) );
        entity.Components["Tag"]      = rfl::Generic( rfl::Generic::Object{} );
        return entity;
    }

    rfl::Generic::Object MaterialOf( const rfl::ExtraFields<rfl::Generic>& components )
    {
        return components.get( "Material" ).value().to_object().value();
    }
} // namespace

TEST( ScenePBRSurfaceShaderMigration, VersionIsTheHeadGeneration )
{
    EXPECT_EQ( Migration::kSceneVersionNoPBRSurfaceShader, 41 );
    EXPECT_EQ( Desert::Core::kSceneVersion, Migration::kSceneVersionNoPBRSurfaceShader );
}

TEST( ScenePBRSurfaceShaderMigration, ThePBRSurfaceReferenceGoesTheRestStays )
{
    const ShaderTree        tree;
    std::vector<EntityData> entities{ Entity( "Rock", MaterialBlock( kPBRGuid, "Shaders/StaticMeshPBR.shader" ) ),
                                      Entity( "Marker", MaterialBlock( kOverrideGuid, "Shaders/Unlit.shader" ) ) };
    const auto              report = Migration::MigratePBRSurfaceShaderV40ToV41( entities, tree.Root );
    EXPECT_TRUE( report.Refused.empty() ) << report.Refused.front();
    EXPECT_EQ( report.Dropped, 1u );

    const auto rock = MaterialOf( entities[0].Components );
    EXPECT_FALSE( rock.get( "Shader" ).has_value() );
    EXPECT_TRUE( rock.get( "Params" ).has_value() ) << "the block's other keys stay";
    EXPECT_TRUE( entities[0].Components.get( "Tag" ).has_value() ) << "the record's other blocks stay";
    EXPECT_TRUE( MaterialOf( entities[1].Components ).get( "Shader" ).has_value() ) << "an override stays";

    // Again over the raised records: nothing left to change (the --check after --write).
    const auto again = Migration::MigratePBRSurfaceShaderV40ToV41( entities, tree.Root );
    EXPECT_EQ( again.Dropped, 0u );
    EXPECT_TRUE( again.Refused.empty() );
}

TEST( ScenePBRSurfaceShaderMigration, APrefabOverrideLosesItToo )
{
    const ShaderTree   tree;
    EntityData         instance;
    PrefabOverrideData override_;
    override_.Components["Material"] = rfl::Generic( MaterialBlock( kPBRGuid, "Shaders/StaticMeshPBR.shader" ) );
    instance.Tag                     = "Instance";
    instance.PrefabOverrides         = std::vector<PrefabOverrideData>{ override_ };
    std::vector<EntityData> entities{ instance };
    const auto              report = Migration::MigratePBRSurfaceShaderV40ToV41( entities, tree.Root );
    EXPECT_EQ( report.Dropped, 1u );
    EXPECT_FALSE( MaterialOf( ( *entities[0].PrefabOverrides )[0].Components ).get( "Shader" ).has_value() );
}

TEST( ScenePBRSurfaceShaderMigration, AnUnjudgeableReferenceRefusesByName )
{
    const ShaderTree        tree;
    std::vector<EntityData> entities{ Entity( "Missing", MaterialBlock( kPBRGuid, "Shaders/Gone.shader" ) ),
                                      Entity( "Stale", MaterialBlock( kOverrideGuid, "Shaders/StaticMeshPBR.shader" ) ) };
    const auto              report = Migration::MigratePBRSurfaceShaderV40ToV41( entities, tree.Root );
    ASSERT_EQ( report.Refused.size(), 2u );
    EXPECT_NE( report.Refused[0].find( "Missing > Material.Shader" ), std::string::npos ) << report.Refused[0];
    EXPECT_NE( report.Refused[1].find( kOverrideGuid ), std::string::npos ) << report.Refused[1];
    EXPECT_EQ( report.Dropped, 0u );
    EXPECT_TRUE( MaterialOf( entities[1].Components ).get( "Shader" ).has_value() ) << "a refusal changes nothing";
}
// NOLINTEND(bugprone-unchecked-optional-access)
