// SCNE 29 (T6d): a skybox reference and a material component's texture reference stop being a bare string
// (skybox: a stable key the writer could render as an ABSOLUTE path) or a bare runtime handle (material
// texture) and become `{"Guid": <the .detex header GUID>, "Path": "assets:<path under the assets root>"}`
// - in entity records and in prefab-override records (MigrateTextureGuidsV28ToV29). A reference whose file
// is missing, lies outside the assets root, or states no texture GUID REFUSES the file, naming it.
//
// The second half is the writer: a SkyboxComponent serializes to that same object, resolves back through
// ResolveGuidRef, and a bare string is no longer read.

#include <SceneMigration.hpp>
#include <Engine/Core/Serialize/SceneFormat.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/Reflection/ReflectionRegistry.hpp>
#include <Engine/Reflection/ReflectionSerializer.hpp>

#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Content/TextAssetHeader.hpp>

#include <rflcpp/rfl/json.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <Common/Json/Document.hpp>

namespace
{
    // DeserializeReflected reads a Json::Node and collects wrong-typed values as Issues; every fixture this
    // file feeds it is well-typed, so an Issue is a failure here.
    void ReadReflectedValue( const Desert::Reflection::TypeInfo& type, void* obj, const Common::Json::Value& src,
                             const Desert::Reflection::AssetResolver* resolver = nullptr )
    {
        Common::Json::Issues issues;
        Desert::Reflection::DeserializeReflected( type, obj, Common::Json::Root( src ), issues, resolver );
        for ( const auto& issue : issues )
            ADD_FAILURE() << Common::Json::Describe( issue );
    }
    void ReadReflectedValue( const Desert::Reflection::TypeInfo& type, void* obj, const Common::Json::Object& src,
                             const Desert::Reflection::AssetResolver* resolver = nullptr )
    {
        ReadReflectedValue( type, obj, Common::Json::Value( src ), resolver );
    }
} // namespace

namespace Migration = Desert::Migration;
namespace fs        = std::filesystem;
using Common::Content::AssetGuid;

namespace
{
    const AssetGuid kSkyGuid{ 0x1111222233334444ULL, 0x5555666677778888ULL };
    const char*     kSkyGuidText = "11112222333344445555666677778888";
    const char*     kSkyKey      = "assets:Textures/HDR/Sky.detex";

    uint64_t SkyHandle()
    {
        return static_cast<uint64_t>( Common::Content::HandleForGuid( kSkyGuid ) );
    }

    void WriteDetex( const fs::path& file, const AssetGuid& guid,
                     Common::Content::ContentKind kind = Common::Content::ContentKind::Texture )
    {
        fs::create_directories( file.parent_path() );
        Common::Content::AssetEnvelope envelope;
        envelope.Asset.Kind = kind;
        envelope.Asset.Guid = guid;
        envelope.Sections.push_back( { Common::Content::EnvelopeSection::ImportInfo,
                                       Common::Content::EnvelopeCodec::Stored,
                                       std::vector<std::byte>( 16, std::byte{ 7 } ) } );
        const auto written = Common::Content::WriteAssetEnvelopeFile( file, envelope );
        ASSERT_TRUE( written ) << written.GetError();
    }

    // <root>/Resources/Assets/Textures/HDR/Sky.detex stating kSkyGuid, and a second .detex OUTSIDE the
    // assets root at <root>/Elsewhere/Stray.detex.
    struct Project
    {
        fs::path Root;
        fs::path AssetsRoot;

        explicit Project( const char* name )
        {
            Root       = fs::temp_directory_path() / ( std::string( "t6d_" ) + name );
            AssetsRoot = Root / "Resources" / "Assets";
            fs::remove_all( Root );
            WriteDetex( AssetsRoot / "Textures" / "HDR" / "Sky.detex", kSkyGuid );
            WriteDetex( Root / "Elsewhere" / "Stray.detex", AssetGuid{ 9, 9 } );
        }
        ~Project()
        {
            std::error_code ec;
            fs::remove_all( Root, ec );
        }
        Project( const Project& )            = delete;
        Project& operator=( const Project& ) = delete;
    };

    std::string Quoted( const std::string& s )
    {
        return rfl::json::write( rfl::Generic( s ) );
    }

    std::string V28Scene( const std::string& skyboxValue, const std::string& textureHandle = "0" )
    {
        return std::string( R"({"Header":{"Kind":"Scene","Guid":"00000000000000000000000000000001",)" ) +
               R"("Versions":{"SCNE":28,"UNIT":1},"Dependencies":[]},"SceneName":"S","Entities":[
        {"id":1,"Tag":"Sky","Skybox":{"SkyboxHandle":)" +
               skyboxValue + R"(,"Intensity":1.0}},
        {"id":2,"Tag":"Inst","PrefabPath":"p.deprefab","PrefabOverrides":[{"Path":[],
          "Skybox":{"SkyboxHandle":)" +
               skyboxValue + R"(}}]},
        {"id":3,"Tag":"Rock","Material":{"ShaderName":"PBR","Textures":[{"Name":"Albedo","TextureHandle":)" +
               textureHandle + R"(}]}}]})";
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
} // namespace

TEST( SceneTextureGuidMigration, ASkyboxKeyBecomesTheHeaderGuidAndTheKeyInRecordsAndOverrides )
{
    const Project project( "key" );
    auto          scene  = Parse( V28Scene( Quoted( kSkyKey ) ) );
    const auto    report = Migration::MigrateScene( scene, project.AssetsRoot, "", {} );

    ASSERT_TRUE( report.Refused.empty() ) << report.Refused;
    EXPECT_TRUE( report.TextureGuidsRaised );
    EXPECT_EQ( report.TextureGuids.Rewritten, 2 );
    const std::string text = rfl::json::write( scene );
    const std::string ref  = std::string( R"({"Guid":")" ) + kSkyGuidText + R"(","Path":")" + kSkyKey + R"("})";
    EXPECT_EQ( Count( text, ref ), 2u ) << text;
    EXPECT_EQ( Count( text, R"({"Name":"Albedo","Guid":"","Path":""})" ), 1u ) << text;
    EXPECT_EQ( Desert::Assets::StatedVersion( scene.Header, Desert::Assets::kSceneSchemaTag ),
               Desert::Core::kSceneVersion );
}

TEST( SceneTextureGuidMigration, AnAbsolutePathUnderTheAssetsRootBecomesTheGuidAndARelativeKey )
{
    const Project     project( "absolute" );
    const std::string absolute = ( project.AssetsRoot / "Textures" / "HDR" / "Sky.detex" ).generic_string();
    auto              scene    = Parse( V28Scene( Quoted( absolute ) ) );
    const auto        report   = Migration::MigrateScene( scene, project.AssetsRoot, "", {} );

    ASSERT_TRUE( report.Refused.empty() ) << report.Refused;
    const std::string text = rfl::json::write( scene );
    EXPECT_EQ( text.find( project.Root.generic_string() ), std::string::npos )
         << "the absolute path survived the migration: " << text;
    EXPECT_EQ( Count( text, std::string( R"("Path":")" ) + kSkyKey + '"' ), 2u ) << text;
    EXPECT_EQ( Count( text, kSkyGuidText ), 2u ) << text;
}

TEST( SceneTextureGuidMigration, AnEmptySkyboxBecomesAnEmptyReference )
{
    const Project project( "empty" );
    auto          scene  = Parse( V28Scene( R"("")" ) );
    const auto    report = Migration::MigrateScene( scene, project.AssetsRoot, "", {} );

    ASSERT_TRUE( report.Refused.empty() ) << report.Refused;
    EXPECT_EQ( report.TextureGuids.Rewritten, 0 );
    EXPECT_EQ( Count( rfl::json::write( scene ), R"("SkyboxHandle":{"Guid":"","Path":""})" ), 2u );
}

TEST( SceneTextureGuidMigration, ANonexistentFileRefusesNamingTheSiteAndLeavesTheSceneUnstamped )
{
    const Project project( "missing" );
    auto          scene  = Parse( V28Scene( Quoted( "assets:Textures/HDR/Gone.detex" ) ) );
    const auto    report = Migration::MigrateScene( scene, project.AssetsRoot, "", {} );

    ASSERT_FALSE( report.Refused.empty() );
    EXPECT_NE( report.Refused.find( "Sky > Skybox.SkyboxHandle" ), std::string::npos ) << report.Refused;
    EXPECT_NE( report.Refused.find( "PrefabOverrides[0] > Skybox.SkyboxHandle" ), std::string::npos )
         << report.Refused;
    EXPECT_NE( report.Refused.find( "Gone.detex" ), std::string::npos ) << report.Refused;
    EXPECT_EQ( Desert::Assets::StatedVersion( scene.Header, Desert::Assets::kSceneSchemaTag ), 28 );
}

TEST( SceneTextureGuidMigration, AnAbsolutePathOutsideTheAssetsRootRefusesByName )
{
    const Project     project( "outside" );
    const std::string stray  = ( project.Root / "Elsewhere" / "Stray.detex" ).generic_string();
    auto              scene  = Parse( V28Scene( Quoted( stray ) ) );
    const auto        report = Migration::MigrateScene( scene, project.AssetsRoot, "", {} );

    ASSERT_FALSE( report.Refused.empty() );
    EXPECT_NE( report.Refused.find( "outside the assets root" ), std::string::npos ) << report.Refused;
    EXPECT_NE( report.Refused.find( "Stray.detex" ), std::string::npos ) << report.Refused;
}

TEST( SceneTextureGuidMigration, AFileThatIsNotATextureRefuses )
{
    const Project project( "kind" );
    WriteDetex( project.AssetsRoot / "Textures" / "HDR" / "Mesh.detex", AssetGuid{ 3, 4 },
                Common::Content::ContentKind::StaticMesh );
    auto       scene  = Parse( V28Scene( Quoted( "assets:Textures/HDR/Mesh.detex" ) ) );
    const auto report = Migration::MigrateScene( scene, project.AssetsRoot, "", {} );

    ASSERT_FALSE( report.Refused.empty() );
    EXPECT_NE( report.Refused.find( "Mesh.detex" ), std::string::npos ) << report.Refused;
}

TEST( SceneTextureGuidMigration, AMaterialTextureHandleBecomesTheGuidOfTheTextureWhoseHandleItIs )
{
    const Project project( "material" );
    auto          scene  = Parse( V28Scene( R"("")", std::to_string( static_cast<int64_t>( SkyHandle() ) ) ) );
    const auto    report = Migration::MigrateScene( scene, project.AssetsRoot, "", {} );

    ASSERT_TRUE( report.Refused.empty() ) << report.Refused;
    EXPECT_EQ( report.TextureGuids.Rewritten, 1 );
    const std::string text = rfl::json::write( scene );
    EXPECT_EQ( Count( text, std::string( R"({"Name":"Albedo","Guid":")" ) + kSkyGuidText + R"(","Path":")" +
                                 kSkyKey + R"("})" ),
               1u )
         << text;
}

TEST( SceneTextureGuidMigration, AMaterialTextureHandleNoTextureStatesRefuses )
{
    const Project project( "material_unknown" );
    auto          scene  = Parse( V28Scene( R"("")", "12345" ) );
    const auto    report = Migration::MigrateScene( scene, project.AssetsRoot, "", {} );

    ASSERT_FALSE( report.Refused.empty() );
    EXPECT_NE( report.Refused.find( "Rock > Material.Textures[0] (Albedo) = 12345" ), std::string::npos )
         << report.Refused;
}

TEST( SceneTextureGuidMigration, ASecondRunOfTheStepChangesNothing )
{
    const Project project( "twice" );
    auto scene = Parse( V28Scene( Quoted( kSkyKey ), std::to_string( static_cast<int64_t>( SkyHandle() ) ) ) );
    ASSERT_TRUE( Migration::MigrateScene( scene, project.AssetsRoot, "", {} ).Refused.empty() );
    const std::string first = rfl::json::write( scene );

    auto       entities = scene.Entities;
    const auto again    = Migration::MigrateTextureGuidsV28ToV29( entities, project.AssetsRoot );
    EXPECT_EQ( again.Rewritten, 0 );
    EXPECT_TRUE( again.UnknownNames.empty() );
    scene.Entities = entities;
    EXPECT_EQ( rfl::json::write( scene ), first );

    const auto rerun = Migration::MigrateScene( scene, project.AssetsRoot, "", {} );
    EXPECT_FALSE( rerun.TextureGuidsRaised );
    EXPECT_EQ( rfl::json::write( scene ), first );
}

// ---- The writer ------------------------------------------------------------------------------------------

namespace
{
    // The resolver MakeAssetResolver builds, reduced to one skybox: its GUID, its key, its handle.
    Desert::Reflection::AssetResolver SkyResolver( int* fromGuidAsked = nullptr )
    {
        Desert::Reflection::AssetResolver r;
        r.ToPath = []( uint64_t handle, const std::string& type ) -> std::string
        { return type == "SkyboxAsset" && handle == SkyHandle() ? kSkyKey : ""; };
        r.ToGuid = []( uint64_t handle, const std::string& type ) -> std::string
        { return type == "SkyboxAsset" && handle == SkyHandle() ? kSkyGuidText : ""; };
        r.FromPath = []( const std::string& path, const std::string& type ) -> uint64_t
        { return type == "SkyboxAsset" && path == kSkyKey ? SkyHandle() : 0; };
        r.FromGuid = [fromGuidAsked]( uint64_t guid, const std::string& type ) -> uint64_t
        {
            if ( fromGuidAsked != nullptr )
                ++*fromGuidAsked;
            return type == "SkyboxAsset" && guid == SkyHandle() ? guid : 0;
        };
        return r;
    }

    const Desert::Reflection::TypeInfo& SkyboxType()
    {
        const auto* type = Desert::Reflection::ReflectionRegistry::Get().Find( "SkyboxComponent" );
        EXPECT_NE( type, nullptr );
        return *type;
    }
} // namespace

TEST( SkyboxReferenceWriter, AnAssignedSkyboxIsWrittenAsItsGuidAndItsProjectKey )
{
    Desert::ECS::SkyboxComponent sky;
    sky.SkyboxHandle    = Desert::Assets::AssetHandle( SkyHandle() );
    const auto resolver = SkyResolver();

    const rfl::Generic::Object out  = Desert::Reflection::SerializeReflected( SkyboxType(), &sky, &resolver );
    const std::string          text = rfl::json::write( out );
    EXPECT_NE( text.find( std::string( R"("SkyboxHandle":{"Guid":")" ) + kSkyGuidText + R"(","Path":")" + kSkyKey +
                          R"("})" ),
               std::string::npos )
         << text;

    Desert::ECS::SkyboxComponent back;
    int                          asked = 0;
    const auto                   again = SkyResolver( &asked );
    ReadReflectedValue( SkyboxType(), &back, out, &again );
    EXPECT_EQ( static_cast<uint64_t>( back.SkyboxHandle ), SkyHandle() );
    EXPECT_EQ( asked, 1 ) << "the GUID is the identity and is asked first";
}

TEST( SkyboxReferenceWriter, AnUnsetSkyboxIsWrittenAsAnEmptyReference )
{
    Desert::ECS::SkyboxComponent sky;
    const auto                   resolver = SkyResolver();
    const std::string            text =
         rfl::json::write( Desert::Reflection::SerializeReflected( SkyboxType(), &sky, &resolver ) );
    EXPECT_NE( text.find( R"("SkyboxHandle":{"Guid":"","Path":""})" ), std::string::npos ) << text;
}

TEST( SkyboxReferenceWriter, ABareStringIsNoLongerReadAsASkybox )
{
    rfl::Generic::Object in;
    in["SkyboxHandle"] = std::string( kSkyKey );
    Desert::ECS::SkyboxComponent sky;
    const auto                   resolver = SkyResolver();
    ReadReflectedValue( SkyboxType(), &sky, in, &resolver );
    EXPECT_EQ( static_cast<uint64_t>( sky.SkyboxHandle ), 0u );
}

TEST( SkyboxReferenceWriter, AGuidWhosePathHoldsAnotherAssetStaysEmpty )
{
    rfl::Generic::Object ref;
    ref["Guid"] = std::string( "0000000000000000000000000000abcd" );
    ref["Path"] = std::string( kSkyKey );
    rfl::Generic::Object in;
    in["SkyboxHandle"] = ref;
    Desert::ECS::SkyboxComponent sky;
    const auto                   resolver = SkyResolver();
    ReadReflectedValue( SkyboxType(), &sky, in, &resolver );
    EXPECT_EQ( static_cast<uint64_t>( sky.SkyboxHandle ), 0u );
}
