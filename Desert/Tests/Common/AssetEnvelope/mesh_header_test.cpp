// The cooked mesh's header as the content scan reads it (MeshBinaryHeaderFormat): a skinned mesh states its
// skeleton among its dependencies, as a clip's text header does, and an older generation is refused by its
// VERSION with the migrator named - never as "a null GUID", which is a defect the file does not have.

#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Content/MeshBinaryHeader.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <format>
#include <string>

namespace
{
    namespace fs = std::filesystem;
    using namespace Common::Content;

    const AssetGuid kMesh{ 0x1111111111111111ULL, 0x2222222222222222ULL };
    const AssetGuid kMaterial{ 0x3333333333333333ULL, 0x4444444444444444ULL };
    const AssetGuid kSkeleton{ 0x5555555555555555ULL, 0x6666666666666666ULL };

    // A current-version mesh prefix: header, GUID, one section row naming one submesh record whose material
    // is kMaterial. Only what the header reader reads is written - no vertex or index byte.
    std::string CurrentMesh( const AssetGuid& skeleton, uint32_t flags )
    {
        const std::size_t rows   = kMeshBinaryPrefixSize;
        const std::size_t submsh = rows + kMeshBinarySectionRowSize;
        std::string       bytes( submsh + kMeshBinarySubmeshSizeV3, '\0' );

        MeshBinaryFileHeader header{};
        std::memcpy( header.Magic, kMeshBinaryMagic, 8 );
        header.ByteOrder    = kMeshBinaryByteOrderTag;
        header.Version      = kMeshBinaryVersion;
        header.FileSize     = bytes.size();
        header.SectionCount = 1;
        header.Flags        = flags;
        header.SkeletonGuid = skeleton;
        std::memcpy( bytes.data(), &header, sizeof( header ) );
        std::memcpy( bytes.data() + kMeshBinaryGuidOffset, &kMesh.Hi, 8 );
        std::memcpy( bytes.data() + kMeshBinaryGuidOffset + 8, &kMesh.Lo, 8 );

        const uint32_t id     = kMeshBinarySubmeshSectionId;
        const uint32_t size   = kMeshBinarySubmeshSizeV3;
        const uint64_t offset = submsh;
        const uint64_t count  = 1;
        std::memcpy( bytes.data() + rows, &id, 4 );
        std::memcpy( bytes.data() + rows + 4, &size, 4 );
        std::memcpy( bytes.data() + rows + 8, &offset, 8 );
        std::memcpy( bytes.data() + rows + 16, &count, 8 );
        std::memcpy( bytes.data() + submsh + kMeshBinarySubmeshMaterialGuidOffset, &kMaterial.Hi, 8 );
        std::memcpy( bytes.data() + submsh + kMeshBinarySubmeshMaterialGuidOffset + 8, &kMaterial.Lo, 8 );
        return bytes;
    }

    fs::path WriteFile( const std::string& name, const std::string& bytes )
    {
        const fs::path file = fs::temp_directory_path() / name;
        std::ofstream( file, std::ios::binary )
             .write( bytes.data(), static_cast<std::streamsize>( bytes.size() ) );
        return file;
    }

    bool Lists( const AssetHeader& header, const AssetGuid& guid )
    {
        return std::find( header.Dependencies.begin(), header.Dependencies.end(), guid ) !=
               header.Dependencies.end();
    }
} // namespace

TEST( MeshBinaryHeader, ASkinnedMeshListsItsSkeletonBesideItsMaterials )
{
    const fs::path file = WriteFile( "mesh_header_skinned.skmesh", CurrentMesh( kSkeleton, kMeshFlagIsSkinned ) );
    const auto     header = ReadAssetHeader( file, AssetHeaderReadContext{} );
    ASSERT_TRUE( header ) << header.GetError();
    EXPECT_EQ( header.GetValue().Kind, ContentKind::SkinnedMesh );
    EXPECT_EQ( header.GetValue().Guid, kMesh );
    EXPECT_TRUE( Lists( header.GetValue(), kMaterial ) );
    EXPECT_TRUE( Lists( header.GetValue(), kSkeleton ) ) << "the skeleton the header names is not an edge";
    EXPECT_EQ( header.GetValue().Dependencies.size(), 2u );
    fs::remove( file );
}

TEST( MeshBinaryHeader, AMeshNamingNoSkeletonHasNoSkeletonEdge )
{
    const fs::path file   = WriteFile( "mesh_header_static.dmesh", CurrentMesh( AssetGuid{}, 0 ) );
    const auto     header = ReadAssetHeader( file, AssetHeaderReadContext{} );
    ASSERT_TRUE( header ) << header.GetError();
    EXPECT_EQ( header.GetValue().Kind, ContentKind::StaticMesh );
    EXPECT_EQ( header.GetValue().Dependencies.size(), 1u );
    EXPECT_FALSE( Lists( header.GetValue(), AssetGuid{} ) );
    fs::remove( file );
}

TEST( MeshBinaryHeader, AnOlderGenerationIsRefusedByVersionNamingTheMigrator )
{
    for ( const uint32_t version : { 3u, 4u } )
    {
        std::string bytes = CurrentMesh( kSkeleton, kMeshFlagIsSkinned );
        std::memcpy( bytes.data() + offsetof( MeshBinaryFileHeader, Version ), &version, 4 );
        const fs::path file   = WriteFile( "mesh_header_old.skmesh", bytes );
        const auto     header = ReadAssetHeader( file, AssetHeaderReadContext{} );
        ASSERT_FALSE( header ) << "a version " << version << " mesh was read";
        const std::string& error = header.GetError();
        EXPECT_NE( error.find( std::format( "version {}", version ) ), std::string::npos ) << error;
        EXPECT_NE( error.find( "SceneMigrator" ), std::string::npos ) << error;
        EXPECT_EQ( error.find( "null GUID" ), std::string::npos ) << error;
        fs::remove( file );
    }
}
