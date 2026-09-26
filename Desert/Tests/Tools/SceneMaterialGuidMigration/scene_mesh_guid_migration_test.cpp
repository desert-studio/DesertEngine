// SCNE 28 (AF7o): a scene's `MeshGuid` stops being the mesh's PATH-derived u64 handle and becomes the GUID
// text the mesh file's v3 header states - in entity records and in prefab-override records. A number that
// is not the handle of the file beside it, or a file stating no GUID, REFUSES the file and leaves it
// unstamped (MigrateMeshGuidsV27ToV28).

#include <SceneMigration.hpp>
#include <Engine/Core/Serialize/SceneFormat.hpp>

#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Content/MeshBinaryHeader.hpp>
#include <Common/Content/TextAssetHeader.hpp>
#include <Common/Core/AssetHandle.hpp>

#include <rflcpp/rfl/json.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

namespace Migration = Desert::Migration;
namespace fs        = std::filesystem;
using Common::Content::AssetGuid;

namespace
{
    const AssetGuid kMeshGuid{ 0x0123456789abcdefULL, 0xfedcba9876543210ULL };
    const char*     kMeshGuidText = "0123456789abcdeffedcba9876543210";

    // A project on disk: <root>/Cooked/Meshes/Probe.skmesh stating kMeshGuid (or a v2 prefix stating
    // none), and the assets root <root>/Resources/Assets the migrator is handed.
    struct Project
    {
        fs::path Root;
        fs::path AssetsRoot;

        explicit Project( const char* name, uint32_t version = 3 )
        {
            Root       = fs::temp_directory_path() / ( std::string( "af7o_" ) + name );
            AssetsRoot = Root / "Resources" / "Assets";
            fs::remove_all( Root );
            fs::create_directories( AssetsRoot );
            fs::create_directories( Root / "Cooked" / "Meshes" );
            Common::Content::MeshBinaryFileHeader header{};
            std::memcpy( header.Magic, Common::Content::kMeshBinaryMagic, 8 );
            header.ByteOrder = Common::Content::kMeshBinaryByteOrderTag;
            header.Version   = version;
            std::string bytes( Common::Content::kMeshBinaryPrefixV3, '\0' );
            std::memcpy( bytes.data(), &header, sizeof( header ) );
            std::memcpy( bytes.data() + Common::Content::kMeshBinaryGuidOffset, &kMeshGuid.Hi, 8 );
            std::memcpy( bytes.data() + Common::Content::kMeshBinaryGuidOffset + 8, &kMeshGuid.Lo, 8 );
            std::ofstream( Root / "Cooked" / "Meshes" / "Probe.skmesh", std::ios::binary ) << bytes;
        }
        ~Project()
        {
            std::error_code ec;
            fs::remove_all( Root, ec );
        }
        Project( const Project& )            = delete;
        Project& operator=( const Project& ) = delete;
    };

    // The handle a v27 writer stored for Cooked/Meshes/Probe.skmesh, as the signed JSON integer it wrote.
    std::string OldHandleText()
    {
        const auto handle = static_cast<uint64_t>( Common::AssetHandle::FromKey( "cooked:Meshes/Probe.skmesh" ) );
        return std::to_string( static_cast<int64_t>( handle ) );
    }

    std::string V27Scene( const std::string& handle )
    {
        return std::string( R"({"Header":{"Kind":"Scene","Guid":"00000000000000000000000000000001",)" ) +
               R"("Versions":{"SCNE":27,"UNIT":1},"Dependencies":[]},"SceneName":"S","Entities":[
        {"id":1,"Tag":"Rig","SkinnedMesh":{"MeshPath":"Cooked/Meshes/Probe.skmesh","MeshGuid":)" +
               handle + R"(}},
        {"id":2,"Tag":"Inst","PrefabPath":"p.deprefab","PrefabOverrides":[{"Path":[],
          "StaticMesh":{"MeshPath":"Cooked/Meshes/Probe.skmesh","MeshGuid":)" +
               handle + R"(}}]}]})";
    }

    Migration::SceneSerialized Parse( const std::string& json )
    {
        auto parsed = rfl::json::read<Migration::SceneSerialized>( json );
        EXPECT_TRUE( parsed.has_value() ) << ( parsed.has_value() ? "" : parsed.error().what() );
        return parsed.has_value() ? parsed.value() : Migration::SceneSerialized{};
    }
} // namespace

TEST( SceneMeshGuidMigration, RecordsAndOverridesRaiseThePathHandleToTheHeaderGuid )
{
    const Project project( "raise" );
    auto          scene  = Parse( V27Scene( OldHandleText() ) );
    const auto    report = Migration::MigrateScene( scene, project.AssetsRoot, "", {} );

    ASSERT_TRUE( report.Refused.empty() ) << report.Refused;
    EXPECT_TRUE( report.MeshGuidsRaised );
    EXPECT_EQ( report.MeshGuids.Rewritten, 2 );
    const std::string text = rfl::json::write( scene );
    EXPECT_EQ( text.find( OldHandleText() ), std::string::npos ) << text;
    std::size_t count = 0;
    for ( std::size_t at = text.find( kMeshGuidText ); at != std::string::npos;
          at             = text.find( kMeshGuidText, at + 1 ) )
        ++count;
    EXPECT_EQ( count, 2u ) << "one in the record, one in its prefab override: " << text;
    ASSERT_TRUE( scene.Header.has_value() );
    EXPECT_EQ( Desert::Assets::StatedVersion( scene.Header, Desert::Assets::kSceneSchemaTag ),
               Desert::Core::kSceneVersion );
}

TEST( SceneMeshGuidMigration, AHandleThatIsNotThePathsRefusesAndLeavesTheSceneUnstamped )
{
    const Project project( "mismatch" );
    auto          scene  = Parse( V27Scene( "12345" ) );
    const auto    report = Migration::MigrateScene( scene, project.AssetsRoot, "", {} );

    ASSERT_FALSE( report.Refused.empty() );
    EXPECT_NE( report.Refused.find( "Rig > SkinnedMesh.MeshGuid = 12345" ), std::string::npos ) << report.Refused;
    EXPECT_NE( report.Refused.find( "PrefabOverrides[0] > StaticMesh.MeshGuid" ), std::string::npos )
         << report.Refused;
    EXPECT_EQ( Desert::Assets::StatedVersion( scene.Header, Desert::Assets::kSceneSchemaTag ), 27 );
}

TEST( SceneMeshGuidMigration, AMeshStatingNoGuidRefuses )
{
    const Project project( "noguid", 2 );
    auto          scene  = Parse( V27Scene( OldHandleText() ) );
    const auto    report = Migration::MigrateScene( scene, project.AssetsRoot, "", {} );

    ASSERT_FALSE( report.Refused.empty() );
    EXPECT_NE( report.Refused.find( "states no mesh GUID" ), std::string::npos ) << report.Refused;
}

TEST( SceneMeshGuidMigration, AMissingMeshFileRefuses )
{
    const Project project( "missing" );
    fs::remove( project.Root / "Cooked" / "Meshes" / "Probe.skmesh" );
    auto       scene  = Parse( V27Scene( OldHandleText() ) );
    const auto report = Migration::MigrateScene( scene, project.AssetsRoot, "", {} );

    ASSERT_FALSE( report.Refused.empty() );
    EXPECT_NE( report.Refused.find( "no file 'Cooked/Meshes/Probe.skmesh'" ), std::string::npos )
         << report.Refused;
}

// SCNE 32 (MSH1): a mesh block that names a MeshPath and states NO MeshGuid - the key missing or "" - gains
// the GUID the file's v3 header states. The v28 step above rewrote MeshGuid VALUES only, so a block with no
// key crossed v28..v31 as it was and the loader left the slot empty (M10_MeshSlot).
namespace
{
    // Three blocks at v31: no MeshGuid key, an empty MeshGuid inside a prefab override, and a block that
    // already states a GUID (which must be left exactly as it is).
    std::string V31PathOnlyScene( const std::string& meshPath )
    {
        return std::string( R"({"Header":{"Kind":"Scene","Guid":"00000000000000000000000000000003",)" ) +
               R"("Versions":{"SCNE":31,"UNIT":1},"Dependencies":[]},"SceneName":"S","Entities":[
        {"id":1,"Tag":"Probe","StaticMesh":{"MeshPath":")" +
               meshPath + R"("}},
        {"id":2,"Tag":"Inst","PrefabPath":"p.deprefab","PrefabOverrides":[{"Path":[],
          "SkinnedMesh":{"MeshPath":")" +
               meshPath + R"(","MeshGuid":""}}]},
        {"id":3,"Tag":"Named","InstancedStaticMesh":{"MeshPath":")" +
               meshPath + R"(","MeshGuid":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}}]})";
    }

    std::size_t Occurrences( const std::string& text, const std::string& what )
    {
        std::size_t count = 0;
        for ( std::size_t at = text.find( what ); at != std::string::npos; at = text.find( what, at + 1 ) )
            ++count;
        return count;
    }
} // namespace

TEST( ScenePathOnlyMeshGuidMigration, APathOnlyBlockInARecordAndAnOverrideGainsTheHeaderGuid )
{
    const Project project( "pathonly" );
    auto          scene  = Parse( V31PathOnlyScene( "Cooked/Meshes/Probe.skmesh" ) );
    const auto    report = Migration::MigrateScene( scene, project.AssetsRoot, "", {} );

    ASSERT_TRUE( report.Refused.empty() ) << report.Refused;
    EXPECT_TRUE( report.PathOnlyMeshGuidsRaised );
    EXPECT_FALSE( report.MeshGuidsRaised ) << "the v28 step must not run on a v31 file";
    EXPECT_EQ( report.PathOnlyMeshGuids.Rewritten, 2 );
    const std::string text = rfl::json::write( scene );
    EXPECT_EQ( Occurrences( text, kMeshGuidText ), 2u ) << "the record and the override: " << text;
    EXPECT_EQ( Occurrences( text, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" ), 1u ) << "a stated GUID is kept: " << text;
    EXPECT_EQ( Occurrences( text, R"("MeshGuid":"")" ), 0u ) << text;
    EXPECT_EQ( Desert::Assets::StatedVersion( scene.Header, Desert::Assets::kSceneSchemaTag ),
               Desert::Core::kSceneVersion );
}

TEST( ScenePathOnlyMeshGuidMigration, ASecondRunOfTheStepChangesNothing )
{
    const Project project( "pathonly_twice" );
    auto          scene = Parse( V31PathOnlyScene( "Cooked/Meshes/Probe.skmesh" ) );
    ASSERT_TRUE( Migration::MigrateScene( scene, project.AssetsRoot, "", {} ).Refused.empty() );
    const std::string once   = rfl::json::write( scene );
    const auto        report = Migration::MigratePathOnlyMeshGuidsV31ToV32( scene.Entities, project.AssetsRoot );
    EXPECT_EQ( report.Rewritten, 0 );
    EXPECT_TRUE( report.UnknownNames.empty() );
    EXPECT_EQ( rfl::json::write( scene ), once );
}

TEST( ScenePathOnlyMeshGuidMigration, AMissingFileRefusesNamingBothBlocksAndLeavesTheSceneUnstamped )
{
    const Project project( "pathonly_missing" );
    auto          scene  = Parse( V31PathOnlyScene( "Cooked/Meshes/Gone.skmesh" ) );
    const auto    report = Migration::MigrateScene( scene, project.AssetsRoot, "", {} );

    ASSERT_FALSE( report.Refused.empty() );
    EXPECT_NE( report.Refused.find( "Probe > StaticMesh.MeshPath = 'Cooked/Meshes/Gone.skmesh'" ),
               std::string::npos )
         << report.Refused;
    EXPECT_NE( report.Refused.find( "PrefabOverrides[0] > SkinnedMesh.MeshPath" ), std::string::npos )
         << report.Refused;
    EXPECT_EQ( Desert::Assets::StatedVersion( scene.Header, Desert::Assets::kSceneSchemaTag ), 31 );
}

TEST( ScenePathOnlyMeshGuidMigration, AMeshStatingNoGuidRefuses )
{
    const Project project( "pathonly_noguid", 2 );
    auto          scene  = Parse( V31PathOnlyScene( "Cooked/Meshes/Probe.skmesh" ) );
    const auto    report = Migration::MigrateScene( scene, project.AssetsRoot, "", {} );

    ASSERT_FALSE( report.Refused.empty() );
    EXPECT_NE( report.Refused.find( "states no mesh GUID" ), std::string::npos ) << report.Refused;
}

TEST( ScenePathOnlyMeshGuidMigration, TheEngineRequiresThePathOnlyMeshGeneration )
{
    EXPECT_EQ( Desert::Core::kSceneVersion, Migration::kSceneVersionPathOnlyMeshGuids );
}

namespace
{
    // A source mesh asset as the editor writes it: a DAST envelope of kind `kind` stating kMeshGuid,
    // at <assets root>/Meshes/Probe.stmesh. No v3 mesh-binary header anywhere in the file.
    void WriteEnvelopeMesh( const Project& project, Common::Content::ContentKind kind )
    {
        fs::create_directories( project.AssetsRoot / "Meshes" );
        Common::Content::AssetEnvelope envelope;
        envelope.Asset.Kind = kind;
        envelope.Asset.Guid = kMeshGuid;
        envelope.Sections.push_back( { Common::Content::EnvelopeSection::Payload,
                                       Common::Content::EnvelopeCodec::Stored,
                                       { std::byte{ 1 }, std::byte{ 2 } } } );
        const auto written =
             Common::Content::WriteAssetEnvelopeFile( project.AssetsRoot / "Meshes" / "Probe.stmesh", envelope );
        ASSERT_TRUE( written ) << written.GetError();
    }
} // namespace

TEST( ScenePathOnlyMeshGuidMigration, AnEnvelopedSourceMeshGivesItsHeaderGuid )
{
    const Project project( "pathonly_envelope" );
    WriteEnvelopeMesh( project, Common::Content::ContentKind::StaticMesh );
    auto       scene  = Parse( V31PathOnlyScene( "Meshes/Probe.stmesh" ) );
    const auto report = Migration::MigrateScene( scene, project.AssetsRoot, "", {} );

    ASSERT_TRUE( report.Refused.empty() ) << report.Refused;
    EXPECT_EQ( report.PathOnlyMeshGuids.Rewritten, 2 );
    EXPECT_EQ( Occurrences( rfl::json::write( scene ), kMeshGuidText ), 2u );
}

TEST( ScenePathOnlyMeshGuidMigration, AnEnvelopeOfAnotherKindRefuses )
{
    const Project project( "pathonly_envelope_kind" );
    WriteEnvelopeMesh( project, Common::Content::ContentKind::Texture );
    auto       scene  = Parse( V31PathOnlyScene( "Meshes/Probe.stmesh" ) );
    const auto report = Migration::MigrateScene( scene, project.AssetsRoot, "", {} );

    ASSERT_FALSE( report.Refused.empty() );
    EXPECT_NE( report.Refused.find( "is not a mesh" ), std::string::npos ) << report.Refused;
}
