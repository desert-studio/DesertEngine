// SHADER-NAMING: MATL 5 -> 6. A material stored a DSL texture under its GLSL spelling (`u_NormalTexture`); it now
// stores the parameter name its shader declares (`NormalTexture`). Everything else and the GUID are kept, and what
// the step writes is what the engine reads.

#include <SceneMigration.hpp>

#include <Engine/Assets/MaterialFormat.hpp>
#include <Engine/Assets/TextAssetHeaderStamp.hpp>

#include <gtest/gtest.h>

#include <regex>
#include <string>

namespace
{
    constexpr const char* kGuid        = "f748ea218bfda9055083bc5a0cb94d64";
    constexpr const char* kTextureGuid = "ddcdc9483fa1c35b1e8d295f05ddb640";

    Desert::Assets::MaterialData SurfaceMaterial( const std::string& textureName )
    {
        Desert::Assets::MaterialData material;
        material.Header       = Common::Content::TextAssetHeaderSerialized{};
        material.Header->Guid = kGuid;
        material.Header->Dependencies.push_back( kTextureGuid );
        material.Params.push_back( { "NormalScale", glm::vec4( 0.5f, 0.0f, 0.0f, 0.0f ) } );
        material.Textures.push_back( { textureName, kTextureGuid, "assets:Textures/T_NormalWitness.detex", {} } );
        return material;
    }

    // A MATL 5 file: the current writer's text with the generation stated as 5.
    std::string AsV5( const Desert::Assets::MaterialData& material )
    {
        const auto text = Desert::Assets::WriteMaterialJson( material );
        EXPECT_TRUE( text ) << text.GetError();
        return std::regex_replace( text.GetValue(), std::regex( R"("MATL"\s*:\s*6)" ), "\"MATL\": 5" );
    }
} // namespace

// MUTATION: drop the rename in MigrateMaterialV5ToV6 and the texture keeps its GLSL spelling.
TEST( MaterialTextureNameMigration, AVersionFiveFileStoresTheParameterName )
{
    const std::string v5 = AsV5( SurfaceMaterial( "u_NormalTexture" ) );
    EXPECT_FALSE( Desert::Assets::ParseMaterialJson( "v5", v5 ) ) << "the engine still reads MATL 5";

    const auto raised = Desert::Migration::MigrateMaterialV5ToV6( v5 );
    ASSERT_TRUE( raised ) << raised.GetError();
    const auto read = Desert::Assets::ParseMaterialJson( "v6", raised.GetValue() );
    ASSERT_TRUE( read ) << read.GetError();

    EXPECT_EQ( read.GetValue().Header->Guid, kGuid ) << "the raise minted a new identity";
    ASSERT_EQ( read.GetValue().Textures.size(), 1u );
    EXPECT_EQ( read.GetValue().Textures[0].Name, "NormalTexture" );
    EXPECT_FLOAT_EQ( read.GetValue().Params[0].Value.x, 0.5f ) << "a parameter that did not change moved";

    // A second raise refuses: the step raises MATL 5 only.
    EXPECT_FALSE( Desert::Migration::MigrateMaterialV5ToV6( raised.GetValue() ) );
}

TEST( MaterialTextureNameMigration, AGlslNameNoSchemaDeclaredIsRefusedByName )
{
    const auto raised = Desert::Migration::MigrateMaterialV5ToV6( AsV5( SurfaceMaterial( "u_Mystery" ) ) );
    ASSERT_FALSE( raised );
    EXPECT_NE( raised.GetError().find( "u_Mystery" ), std::string::npos ) << raised.GetError();
}
