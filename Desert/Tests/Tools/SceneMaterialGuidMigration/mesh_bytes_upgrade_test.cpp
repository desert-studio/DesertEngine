// MESH v1/v2 -> v3 (AF7q): SceneMigrator's mesh pass raises a pre-GUID cooked mesh by bytes, giving it a
// GUID and translating each submesh's old material NUMBER through the legacy material register. The raise is
// checked against the ENCODER: raising the v1/v2 spelling of a mesh must give exactly the bytes
// EncodeMeshBinary writes for that mesh with the new GUIDs, and the decoder must read them back.

#include <LegacyMaterialIds.hpp>

#include <Engine/Assets/Serialization/MeshBinary.hpp>

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

namespace Migration = Desert::Migration;
namespace Ser       = Desert::Assets::Serialization;
using Common::Content::AssetGuid;

namespace
{
    constexpr uint64_t kNumberA = 0x44d056a9359b4d1cull;
    const AssetGuid    kGuidA{ 0x1111222233334444ull, 0x5555666677778888ull };
    const AssetGuid    kMeshGuid{ 0x0123456789abcdefull, 0xfedcba9876543210ull };

    Ser::MeshAssetData TwoSubmeshMesh()
    {
        Ser::MeshAssetData data;
        for ( uint32_t i = 0; i < 4; ++i )
        {
            const float f = static_cast<float>( i ) + 0.5f;
            data.StaticVertices.push_back( Ser::StaticVertexData{
                 glm::vec3( f, -f, 2.0f * f ), glm::vec3( 0.0f, 1.0f, 0.0f ), glm::vec3( 1.0f, 0.0f, 0.0f ),
                 glm::vec3( 0.0f, 0.0f, 1.0f ), glm::vec2( f, 1.0f - f ) } );
        }
        data.Indices = { Ser::IndexData{ 0, 1, 2 }, Ser::IndexData{ 1, 2, 3 } };
        Ser::SubmeshData a{};
        a.Name             = "numbered";
        a.VertexCount      = 4;
        a.IndexCount       = 3;
        a.Transform        = glm::mat4( 1.0f );
        a.BoundingBox      = Common::Math::AABB{ glm::vec3( -1.0f ), glm::vec3( 1.0f ) };
        a.LODs             = { { Ser::IndexData{ 0, 1, 2 } } };
        Ser::SubmeshData b = a;
        b.Name             = "no material";
        b.IndexOffset      = 3;
        b.LODs.clear();
        data.Submeshes = { a, b };
        return data;
    }

    // The pre-v3 spelling of `v3`: no GUID after the 64-byte header, each 136-byte submesh row cut to its
    // first 120 bytes plus the 8-byte `numbers[i]`, the sections laid out again; `version` 1 also drops the
    // (empty) PolyGroups row.
    std::string Downgrade( const std::string& v3, const std::vector<uint64_t>& numbers, uint32_t version )
    {
        constexpr size_t kHeader = 64, kRow = 24;
        uint32_t         sections = 0;
        std::memcpy( &sections, v3.data() + 24, 4 );
        const uint32_t kept = version == 1 ? sections - 1 : sections;
        std::string    table, body;
        uint64_t       at = kHeader + kept * kRow;
        for ( uint32_t row = 0; row < kept; ++row )
        {
            const char* in = v3.data() + Common::Content::kMeshBinaryPrefixV3 + row * kRow;
            uint32_t    id = 0, elementSize = 0;
            uint64_t    offset = 0, count = 0;
            std::memcpy( &id, in, 4 );
            std::memcpy( &elementSize, in + 4, 4 );
            std::memcpy( &offset, in + 8, 8 );
            std::memcpy( &count, in + 16, 8 );
            std::string bytes = v3.substr( offset, count * elementSize );
            if ( id == 4 )
            {
                std::string rows;
                for ( uint64_t i = 0; i < count; ++i )
                    rows += bytes.substr( i * 136, 120 ) +
                            std::string( reinterpret_cast<const char*>( &numbers[i] ), 8 );
                bytes       = rows;
                elementSize = 128;
            }
            table.append( reinterpret_cast<const char*>( &id ), 4 );
            table.append( reinterpret_cast<const char*>( &elementSize ), 4 );
            table.append( reinterpret_cast<const char*>( &at ), 8 );
            table.append( reinterpret_cast<const char*>( &count ), 8 );
            body += bytes;
            at += bytes.size();
            while ( at % 8 != 0 )
            {
                body.push_back( '\0' );
                ++at;
            }
        }
        std::string old = v3.substr( 0, kHeader ) + table + body;
        std::memcpy( old.data() + 12, &version, 4 );
        std::memcpy( old.data() + 16, &at, 8 );
        std::memcpy( old.data() + 24, &kept, 4 );
        return old;
    }
} // namespace

TEST( MeshBytesUpgrade, VersionsOneAndTwoRaiseToTheEncodersV3BytesOfTheSameMesh )
{
    Ser::MeshAssetData expected             = TwoSubmeshMesh();
    expected.Guid                           = kMeshGuid;
    expected.Submeshes[0].MaterialGuid      = kGuidA;
    const std::string                    v3 = Ser::EncodeMeshBinary( expected );
    const Migration::LegacyMaterialIdMap map{ { kNumberA, kGuidA } };

    for ( const uint32_t version : { 1u, 2u } )
    {
        const std::string old = Downgrade( v3, { kNumberA, 0 }, version );
        ASSERT_NE( old, v3 );
        const auto raised = Migration::UpgradeMeshBytesToV3( "probe.stmesh", old, kMeshGuid, map );
        ASSERT_TRUE( raised.IsSuccess() ) << "v" << version << ": " << raised.GetError();
        EXPECT_EQ( raised.GetValue(), v3 ) << "v" << version << " did not raise to the encoder's bytes";

        const auto read = Ser::DecodeMeshBinary( raised.GetValue(), "raised.stmesh" );
        ASSERT_TRUE( read.IsSuccess() ) << read.GetError();
        EXPECT_EQ( read.GetValue().Guid, kMeshGuid );
        EXPECT_EQ( read.GetValue().Submeshes[0].MaterialGuid, kGuidA );
        EXPECT_TRUE( read.GetValue().Submeshes[1].MaterialGuid.IsNull() ) << "number 0 is no material";
    }
}

TEST( MeshBytesUpgrade, ANumberTheRegisterDoesNotKnowIsRefusedByName )
{
    Ser::MeshAssetData mesh  = TwoSubmeshMesh();
    mesh.Guid                = kMeshGuid;
    const std::string old    = Downgrade( Ser::EncodeMeshBinary( mesh ), { 0, 0xd83c0882e3dc15c3ull }, 2 );
    const auto        raised = Migration::UpgradeMeshBytesToV3( "stranger.skmesh", old, kMeshGuid, {} );
    ASSERT_FALSE( raised.IsSuccess() );
    EXPECT_NE( raised.GetError().find( "stranger.skmesh" ), std::string::npos ) << raised.GetError();
    EXPECT_NE( raised.GetError().find( std::to_string( 0xd83c0882e3dc15c3ull ) ), std::string::npos )
         << raised.GetError();
}

// The pass never hands a v3 file in (it skips it, which is what keeps a second run a no-op), and the raise
// itself refuses one rather than re-GUIDing it; so does it refuse a null GUID.
TEST( MeshBytesUpgrade, AVersionThreeFileAndANullGuidAreRefused )
{
    Ser::MeshAssetData mesh = TwoSubmeshMesh();
    mesh.Guid               = kMeshGuid;
    const std::string v3    = Ser::EncodeMeshBinary( mesh );
    EXPECT_FALSE( Migration::UpgradeMeshBytesToV3( "v3.stmesh", v3, AssetGuid::Generate(), {} ).IsSuccess() );
    EXPECT_FALSE(
         Migration::UpgradeMeshBytesToV3( "v2.stmesh", Downgrade( v3, { 0, 0 }, 2 ), {}, {} ).IsSuccess() );
}

// THE PASS'S FIRST QUESTION IS "IS THIS A COOKED MESH AT ALL", NOT "WHICH VERSION". Four of the owner's
// meshes were JSON-era files (`{"IsSkinned":false,"Stat...`); read as a header, bytes 12..15 of that text
// (":fal") are version 1818322490, which is >= 3, so the pass reported them "ok - already at mesh
// v1818322490" and left them. The signature now gates the version, and a version past this build's is a
// refusal too - never "ok".
TEST( MeshBytesUpgrade, APreBinaryJsonMeshIsRefusedByNameAndNotReadAsAVersion )
{
    std::string json = R"({"IsSkinned":false,"StaticVertices":[{"Position":[0.5,-0.5,1.0]}],"Submeshes":[)";
    json.resize( 1024, ' ' );
    const auto version = Migration::CookedMeshVersion( "base.stmesh", json );
    ASSERT_FALSE( version.IsSuccess() ) << "read as mesh v" << version.GetValue();
    EXPECT_NE( version.GetError().find( "base.stmesh" ), std::string::npos ) << version.GetError();
    EXPECT_NE( version.GetError().find( "JSON mesh (pre-binary format) — re-import from source" ),
               std::string::npos )
         << version.GetError();
}

TEST( MeshBytesUpgrade, BytesWithNoMeshSignatureAreAnUnknownMeshFormat )
{
    std::string junk( 1024, '\0' );
    for ( size_t i = 0; i < junk.size(); ++i )
        junk[i] = static_cast<char>( ( i * 37 + 11 ) & 0xff );
    const uint32_t looksCurrent = Common::Content::kMeshBinaryVersion;
    std::memcpy( junk.data() + 12, &looksCurrent, 4 ); // a plausible version where a header keeps one
    const auto version = Migration::CookedMeshVersion( "junk.skmesh", junk );
    ASSERT_FALSE( version.IsSuccess() ) << "read as mesh v" << version.GetValue();
    EXPECT_NE( version.GetError().find( "junk.skmesh" ), std::string::npos ) << version.GetError();
    EXPECT_NE( version.GetError().find( "unknown mesh format" ), std::string::npos ) << version.GetError();

    const auto tiny = Migration::CookedMeshVersion( "tiny.stmesh", "DESTM" );
    ASSERT_FALSE( tiny.IsSuccess() );
    EXPECT_NE( tiny.GetError().find( "tiny.stmesh" ), std::string::npos ) << tiny.GetError();
}

TEST( MeshBytesUpgrade, AVersionPastThisBuildsIsRefusedNotOk )
{
    Ser::MeshAssetData mesh = TwoSubmeshMesh();
    mesh.Guid               = kMeshGuid;
    std::string    future   = Ser::EncodeMeshBinary( mesh );
    const uint32_t v99      = 99;
    std::memcpy( future.data() + 12, &v99, 4 );
    const auto version = Migration::CookedMeshVersion( "future.stmesh", future );
    ASSERT_FALSE( version.IsSuccess() ) << "read as mesh v" << version.GetValue();
    EXPECT_NE( version.GetError().find( "future.stmesh" ), std::string::npos ) << version.GetError();
    EXPECT_NE( version.GetError().find( "version 99" ), std::string::npos ) << version.GetError();
}

TEST( MeshBytesUpgrade, ARealCookedMeshStatesItsVersion )
{
    Ser::MeshAssetData mesh = TwoSubmeshMesh();
    mesh.Guid               = kMeshGuid;
    const std::string v3    = Ser::EncodeMeshBinary( mesh );
    const auto        now   = Migration::CookedMeshVersion( "v3.stmesh", v3 );
    ASSERT_TRUE( now.IsSuccess() ) << now.GetError();
    EXPECT_EQ( now.GetValue(), Common::Content::kMeshBinaryVersion ) << "the pass reports this one ok";

    const auto old = Migration::CookedMeshVersion( "v2.stmesh", Downgrade( v3, { 0, 0 }, 2 ) );
    ASSERT_TRUE( old.IsSuccess() ) << old.GetError();
    EXPECT_EQ( old.GetValue(), 2u ) << "the pass raises this one";
}
