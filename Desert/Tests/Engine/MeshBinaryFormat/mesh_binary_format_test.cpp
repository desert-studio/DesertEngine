/**
 * B11 — THE COOKED MESH CONTAINER: a round trip that is an IDENTITY, and a truncation that is not an
 * empty mesh.
 *
 * WHAT WAS MEASURED BEFORE THIS EXISTED, on this tree, on `Cooked/Meshes/base.stmesh` (105 317
 * vertices, 120 000 triangles, one submesh):
 *
 *      phase                     JSON            container       ratio
 *      size on disk              40 185 992 B    7 338 168 B      5.48x
 *      read (page-cached)            10.42 ms        1.85 ms      5.62x
 *      parse                         63.36 ms        0.23 ms    271.12x
 *      build the asset's vectors      0.59 ms        0.59 ms         1x   (identical work)
 *      = first touch, total          75.69 ms        2.93 ms     25.88x
 *
 * (Release, medians of 11 interleaved reps, instrument floor 0.000 ms; Debug is 634.00 ms -> 11.43 ms.)
 * The interesting row is NOT the one the programme document leads with. `Docs/World/PROGRAMME.md` §5
 * names the disk ratio, and the disk ratio buys 8.6 of the 72.8 ms saved — 12 %. The other 88 % is the
 * PARSE, and it is 271x rather than 5x because a container is `memcpy` where JSON is a state machine
 * over 40 MB of decimal text.
 *
 * ── WHAT THESE TESTS ARE ABOUT ────────────────────────────────────────────────────────────────────
 *
 * 1. THE ROUND TRIP IS AN IDENTITY, BY VALUE. Not "looks the same": every field of `MeshAssetData` is
 *    compared, and the vertex and index arrays are compared with `memcmp` so a float that survives
 *    printing but not its last mantissa bit is caught. A format that loses a field silently is the
 *    middle-link defect this tree has closed seven times in one day.
 *
 * 2. A TRUNCATED FILE IS NOT AN EMPTY MESH. This is the property the container was given a declared
 *    `FileSize` for. Both files decode "successfully" under a naive reader: the empty one holds zero
 *    of everything, and the truncated one holds whatever fitted. Here the empty one is a success and
 *    the truncated one is a refusal that names both numbers — and the test asserts the DIFFERENCE, not
 *    each half alone, because each half alone passes on a reader that cannot tell them apart.
 *
 * 3. OLD FILES STILL OPEN. `ReadMeshAssetData` is the migration: version 0 is the retired JSON form,
 *    recognised by the absence of the magic. The JSON fixture here is DERIVED from the same
 *    `MeshAssetData` the binary one is built from, rather than being a file on disk — A27's lesson,
 *    where two fixtures were swallowed by an ignore rule in silence and the suite was green only in
 *    the tree where they happened to sit.
 *
 * 4. THE COMMITTED CORPUS IS CONVERTED, and the register of what the corpus IS comes from
 *    `.gitignore`'s own `!` lines rather than from a list typed here. A list typed here is a list one
 *    can fall out of; the `!` lines are what git actually admits, so they cannot disagree with the
 *    repository.
 *
 * 5. THE WRITER WRITES THE CONTAINER. A census over source text, because `ImportManager.cpp` is
 *    compiled by the Editor alone and no suite can link it — the same instrument, and the same reason,
 *    as StaticMeshCooked's census over `MeshService::BuildAndCache`.
 */

#include <gtest/gtest.h>

#include "../../TestSupport/cooked_static_mesh.hpp"

#include <Common/Core/Serialization/GlmReflection.hpp>

#include <glm/gtc/type_ptr.hpp>
#include <Common/Content/ContentScan.hpp>
#include <Common/Content/MeshBinaryHeader.hpp>
#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/Constants.hpp>
#include <Common/Project/ProjectFormat.hpp>
#include <Common/Utilities/AssetRegistry.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <Engine/Assets/Mesh/StaticMeshAsset.hpp>
#include <Engine/Assets/Serialization/MeshBinary.hpp>

#include <rflcpp/rfl.hpp>
#include <rflcpp/rfl/json.hpp>

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace Ser = Desert::Assets::Serialization;

namespace
{
    std::filesystem::path RepoRoot()
    {
        std::filesystem::path here = std::filesystem::current_path();
        for ( int up = 0; up < 6; ++up )
        {
            if ( std::filesystem::exists( here / ".gitignore" ) && std::filesystem::exists( here / "Desert" ) )
                return here;
            here = here.parent_path();
        }
        return {};
    }

    std::string ReadFile( const std::filesystem::path& path )
    {
        const std::ifstream in( path, std::ios::binary );
        if ( !in )
            return {};
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    /// A mesh that exercises EVERY field of `MeshAssetData` at once, including the three that are
    /// jagged (submesh names, LOD chains, blendshape deltas) and the one that is optional. A fixture
    /// that leaves a field empty cannot tell "written and read back" from "never written".
    Ser::MeshAssetData FullyPopulated()
    {
        Ser::MeshAssetData data;
        data.IsSkinned = false;
        data.Guid      = { 0x0123456789abcdefull, 0xfedcba9876543210ull };

        for ( uint32_t i = 0; i < 7; ++i )
        {
            const float f = static_cast<float>( i ) + 0.125f;
            data.StaticVertices.push_back( Ser::StaticVertexData{
                 glm::vec3( f, -f, f * 3.5f ), glm::vec3( 0.0f, 1.0f, 0.0f ), glm::vec3( 1.0f, 0.0f, 0.0f ),
                 glm::vec3( 0.0f, 0.0f, 1.0f ), glm::vec2( f * 0.25f, 1.0f - f ) } );
        }
        // Skinned vertices in the same file as static ones: the container has a section for each and
        // filling both is the only way to catch a reader that wires one section to the other's offset.
        for ( uint32_t i = 0; i < 4; ++i )
        {
            Ser::SkinnedVertexData v{};
            const auto             fi = static_cast<float>( i );
            v.Position                = glm::vec3( -1.0f * fi, 2.0f * fi, 0.5f );
            v.Normal                  = glm::vec3( 0.0f, 0.0f, -1.0f );
            v.Tangent                 = glm::vec3( 0.0f, 1.0f, 0.0f );
            v.Bitangent               = glm::vec3( 1.0f, 0.0f, 0.0f );
            v.TexCoord                = glm::vec2( 0.5f, 0.25f * fi );
            v.BoneIDs                 = { i, i + 1u, i + 2u, i + 3u };
            v.BoneWeights             = { 0.5f, 0.25f, 0.125f, 0.125f };
            data.SkinnedVertices.push_back( v );
        }
        for ( uint32_t i = 0; i < 5; ++i )
            data.Indices.push_back( Ser::IndexData{ i, i + 1u, i + 2u } );

        Ser::SubmeshData a{};
        a.Name           = "first";
        a.VertexOffset   = 0;
        a.VertexCount    = 4;
        a.IndexOffset    = 0;
        a.IndexCount     = 6;
        a.Transform      = glm::mat4( 2.0f );
        a.BoundingBox    = Common::Math::AABB{ glm::vec3( -1.0f, -2.0f, -3.0f ), glm::vec3( 4.0f, 5.0f, 6.0f ) };
        a.MaterialGuid   = Common::Content::AssetGuid{ 0xABCDEF0123456789ull, 0x0123456789ABCDEFull };
        a.LODs = { { Ser::IndexData{ 0, 1, 2 }, Ser::IndexData{ 1, 2, 3 } }, { Ser::IndexData{ 0, 2, 3 } } };

        // A SECOND SUBMESH AT A NON-ZERO OFFSET, for StaticMeshCooked's reason: the first submesh of
        // any mesh starts at 0 and looks correct however the offsets are computed.
        Ser::SubmeshData b{};
        b.Name           = "second submesh with a much longer name";
        b.VertexOffset   = 4;
        b.VertexCount    = 3;
        b.IndexOffset    = 6;
        b.IndexCount     = 9;
        b.Transform      = glm::mat4( 1.0f );
        b.BoundingBox    = Common::Math::AABB{ glm::vec3( 0.0f ), glm::vec3( 1.0f ) };
        b.MaterialGuid   = Common::Content::AssetGuid{ 7ull, 11ull };
        // Deliberately NO LOD chain: a submesh with LODs beside one without is what catches a reader
        // that computes the LOD range from the submesh index instead of from the record.
        data.Submeshes = { a, b };

        Ser::MorphTargetData m0;
        m0.Name           = "smile";
        m0.DeltaPositions = { glm::vec3( 0.1f, 0.2f, 0.3f ), glm::vec3( -0.1f, 0.0f, 0.5f ) };
        m0.DeltaNormals   = { glm::vec3( 0.0f, 1.0f, 0.0f ) };
        Ser::MorphTargetData m1;
        m1.Name           = "blink";
        m1.DeltaPositions = { glm::vec3( 9.0f, 8.0f, 7.0f ) };
        // Position-only, which the schema allows: an empty second array must come back empty and not
        // borrow the previous target's.
        data.MorphTargets = { m0, m1 };

        data.SkeletonSignature = 0x0123456789ABCDEFull;

        // Version 2: one polygroup per face, not numbered like the faces, so a reader that invented them
        // from the face index would fail.
        for ( size_t f = 0; f < data.Indices.size(); ++f )
            data.PolyGroups.push_back( static_cast<int32_t>( 40 - ( f * 3 ) % 7 ) );
        return data;
    }

    /// BY VALUE, AND BIT FOR BIT ON THE ARRAYS. `EXPECT_EQ` on a float compares the values, which is
    /// what we want for the scalars; the vertex and index arrays go through `memcmp` so that a lost
    /// mantissa bit — the one failure a printed comparison cannot see — is a red test.
    void ExpectSameMesh( const Ser::MeshAssetData& expected, const Ser::MeshAssetData& actual )
    {
        EXPECT_EQ( expected.IsSkinned, actual.IsSkinned );
        EXPECT_EQ( expected.Guid, actual.Guid );
        EXPECT_EQ( expected.SkeletonSignature.has_value(), actual.SkeletonSignature.has_value() );
        if ( expected.SkeletonSignature.has_value() && actual.SkeletonSignature.has_value() )
            EXPECT_EQ( expected.SkeletonSignature.value(), actual.SkeletonSignature.value() );

        ASSERT_EQ( expected.StaticVertices.size(), actual.StaticVertices.size() );
        if ( !expected.StaticVertices.empty() )
        {
            EXPECT_EQ( 0, std::memcmp( expected.StaticVertices.data(), actual.StaticVertices.data(),
                                       expected.StaticVertices.size() * sizeof( Ser::StaticVertexData ) ) );
        }

        ASSERT_EQ( expected.SkinnedVertices.size(), actual.SkinnedVertices.size() );
        if ( !expected.SkinnedVertices.empty() )
        {
            EXPECT_EQ( 0, std::memcmp( expected.SkinnedVertices.data(), actual.SkinnedVertices.data(),
                                       expected.SkinnedVertices.size() * sizeof( Ser::SkinnedVertexData ) ) );
        }

        ASSERT_EQ( expected.Indices.size(), actual.Indices.size() );
        if ( !expected.Indices.empty() )
        {
            EXPECT_EQ( 0, std::memcmp( expected.Indices.data(), actual.Indices.data(),
                                       expected.Indices.size() * sizeof( Ser::IndexData ) ) );
        }

        ASSERT_EQ( expected.Submeshes.size(), actual.Submeshes.size() );
        for ( size_t i = 0; i < expected.Submeshes.size(); ++i )
        {
            const Ser::SubmeshData& e = expected.Submeshes[i];
            const Ser::SubmeshData& g = actual.Submeshes[i];
            EXPECT_EQ( e.Name, g.Name ) << "submesh " << i;
            EXPECT_EQ( e.VertexOffset, g.VertexOffset ) << "submesh " << i;
            EXPECT_EQ( e.VertexCount, g.VertexCount ) << "submesh " << i;
            EXPECT_EQ( e.IndexOffset, g.IndexOffset ) << "submesh " << i;
            EXPECT_EQ( e.IndexCount, g.IndexCount ) << "submesh " << i;
            // Component by component rather than by object representation: a float type has no unique
            // object representation, so a byte comparison over one is a question with no single right
            // answer (clang-tidy's bugprone-suspicious-memory-comparison says so). Equality of every
            // component is the property that matters and it is exact for these values.
            for ( int c = 0; c < 16; ++c )
                EXPECT_EQ( glm::value_ptr( e.Transform )[c], glm::value_ptr( g.Transform )[c] )
                     << "submesh " << i << " transform component " << c;
            for ( int c = 0; c < 3; ++c )
            {
                EXPECT_EQ( glm::value_ptr( e.BoundingBox.Min )[c], glm::value_ptr( g.BoundingBox.Min )[c] )
                     << "submesh " << i << " bounds min " << c;
                EXPECT_EQ( glm::value_ptr( e.BoundingBox.Max )[c], glm::value_ptr( g.BoundingBox.Max )[c] )
                     << "submesh " << i << " bounds max " << c;
            }
            EXPECT_EQ( e.MaterialGuid, g.MaterialGuid ) << "submesh " << i;
            ASSERT_EQ( e.LODs.size(), g.LODs.size() ) << "submesh " << i;
            for ( size_t l = 0; l < e.LODs.size(); ++l )
            {
                ASSERT_EQ( e.LODs[l].size(), g.LODs[l].size() ) << "submesh " << i << " lod " << l;
                if ( !e.LODs[l].empty() )
                {
                    EXPECT_EQ( 0, std::memcmp( e.LODs[l].data(), g.LODs[l].data(),
                                               e.LODs[l].size() * sizeof( Ser::IndexData ) ) );
                }
            }
        }

        EXPECT_EQ( expected.PolyGroups, actual.PolyGroups );

        ASSERT_EQ( expected.MorphTargets.size(), actual.MorphTargets.size() );
        for ( size_t i = 0; i < expected.MorphTargets.size(); ++i )
        {
            const Ser::MorphTargetData& e = expected.MorphTargets[i];
            const Ser::MorphTargetData& g = actual.MorphTargets[i];
            EXPECT_EQ( e.Name, g.Name ) << "blendshape " << i;
            ASSERT_EQ( e.DeltaPositions.size(), g.DeltaPositions.size() ) << "blendshape " << i;
            ASSERT_EQ( e.DeltaNormals.size(), g.DeltaNormals.size() ) << "blendshape " << i;
            if ( !e.DeltaPositions.empty() )
            {
                EXPECT_EQ( 0, std::memcmp( e.DeltaPositions.data(), g.DeltaPositions.data(),
                                           e.DeltaPositions.size() * sizeof( glm::vec3 ) ) );
            }
            if ( !e.DeltaNormals.empty() )
            {
                EXPECT_EQ( 0, std::memcmp( e.DeltaNormals.data(), g.DeltaNormals.data(),
                                           e.DeltaNormals.size() * sizeof( glm::vec3 ) ) );
            }
        }
    }
} // namespace

TEST( MeshBinaryFormat, EveryFieldSurvivesTheRoundTripByValue )
{
    const Ser::MeshAssetData source  = FullyPopulated();
    const std::string        encoded = Ser::EncodeMeshBinary( source );

    ASSERT_TRUE( Ser::LooksLikeMeshBinary( encoded ) );
    const auto decoded = Ser::DecodeMeshBinary( encoded, "round-trip" );
    ASSERT_TRUE( decoded.IsSuccess() ) << decoded.GetError();
    ExpectSameMesh( source, decoded.GetValue() );
}

TEST( MeshBinaryFormat, EncodingIsDeterministic )
{
    // The cook's freshness check and every content-addressed scheme after it rest on this: the same
    // mesh must produce the same bytes, or a re-cook that changed nothing looks like a change.
    const Ser::MeshAssetData source = FullyPopulated();
    EXPECT_EQ( Ser::EncodeMeshBinary( source ), Ser::EncodeMeshBinary( source ) );
}

TEST( MeshBinaryFormat, ARoundTrippedMeshReEncodesToTheSameBytes )
{
    // The stronger form of the identity: not only do the VALUES survive, the file does. A field that
    // decoded into the right value but re-encoded differently would mean the two halves disagree about
    // the layout — which is exactly the state in which a later version bump reads the old file wrong.
    const std::string once = Ser::EncodeMeshBinary( FullyPopulated() );
    const auto        back = Ser::DecodeMeshBinary( once, "re-encode" );
    ASSERT_TRUE( back.IsSuccess() ) << back.GetError();
    EXPECT_EQ( once, Ser::EncodeMeshBinary( back.GetValue() ) );
}

TEST( MeshBinaryFormat, ATruncatedFileIsRefusedAndAnEmptyOneIsNot )
{
    // THE PROPERTY THE DECLARED SIZE EXISTS FOR, and it is stated as a DIFFERENCE between two files
    // rather than as two separate expectations: a reader that cannot tell them apart passes each half.
    const Ser::MeshAssetData empty; // zero of everything — a legal, complete container
    const std::string        emptyBytes = Ser::EncodeMeshBinary( empty );

    const auto emptyRead = Ser::DecodeMeshBinary( emptyBytes, "empty.stmesh" );
    ASSERT_TRUE( emptyRead.IsSuccess() ) << emptyRead.GetError();
    EXPECT_TRUE( emptyRead.GetValue().StaticVertices.empty() );
    EXPECT_TRUE( emptyRead.GetValue().Submeshes.empty() );

    const std::string full = Ser::EncodeMeshBinary( FullyPopulated() );
    ASSERT_GT( full.size(), emptyBytes.size() );

    // Cut at the length of the empty file, and at every other interesting boundary. The first of these
    // is the case that motivated the field: the bytes that remain ARE a complete-looking header.
    for ( const size_t cut : { emptyBytes.size(), full.size() - 1, full.size() / 2, static_cast<size_t>( 64 ),
                               static_cast<size_t>( 63 ), static_cast<size_t>( 0 ) } )
    {
        const auto cutRead =
             Ser::DecodeMeshBinary( std::string_view( full ).substr( 0, cut ), "truncated.stmesh" );
        EXPECT_FALSE( cutRead.IsSuccess() )
             << "a file cut to " << cut << " of " << full.size() << " bytes was accepted";
    }
}

// VERSION 1 IS STILL READ, AND IT IS READ AS "NO POLYGROUPS". The v1 bytes are MADE here from a v2 encoding
// rather than taken from the committed probes, so the test keeps proving the v1 path after those probes are
// re-cooked: a v1 file is exactly a v2 file without its last table row (24 bytes), every offset 24 lower,
// the size 24 smaller, Version 1 and SectionCount 9 - which is also the precise statement of what v2 added.
namespace
{
    // v3 minus its identity: the 16 GUID bytes after the 64-byte header go, and every submesh row ends in
    // the 8-byte pre-GUID material number (`materialNumber` in each) where v3 has the material's 16-byte
    // GUID. The sections are laid out again, each at the next 8-byte boundary, as the reader derives them.
    std::string AsVersionTwo( const std::string& v3, uint64_t materialNumber = 0 )
    {
        constexpr size_t kHeader = 64, kRow = 24, kSubmeshRow = 3, kRowV3 = 136, kRowV2 = 128, kShared = 120;
        const size_t     prefix  = Common::Content::kMeshBinaryPrefixV3;
        uint32_t         version = 2, sections = 0;
        std::memcpy( &sections, v3.data() + 24, 4 );
        std::string table = v3.substr( prefix, sections * kRow );
        std::string body;
        uint64_t    at = kHeader + sections * kRow;
        for ( uint32_t row = 0; row < sections; ++row )
        {
            uint32_t elementSize = 0;
            uint64_t offset = 0, count = 0;
            std::memcpy( &elementSize, table.data() + row * kRow + 4, 4 );
            std::memcpy( &offset, table.data() + row * kRow + 8, 8 );
            std::memcpy( &count, table.data() + row * kRow + 16, 8 );
            std::string bytes = v3.substr( offset, count * elementSize );
            if ( row == kSubmeshRow )
            {
                EXPECT_EQ( elementSize, kRowV3 );
                std::string rows;
                for ( uint64_t i = 0; i < count; ++i )
                    rows += bytes.substr( i * kRowV3, kShared ) +
                            std::string( reinterpret_cast<const char*>( &materialNumber ), 8 );
                bytes       = rows;
                elementSize = kRowV2;
            }
            while ( at % 8 != 0 )
            {
                body.push_back( '\0' );
                ++at;
            }
            std::memcpy( table.data() + row * kRow + 4, &elementSize, 4 );
            std::memcpy( table.data() + row * kRow + 8, &at, 8 );
            body += bytes;
            at += bytes.size();
        }
        // The file ends at the next 8-byte boundary after its last section, as the encoder writes it.
        while ( body.size() % 8 != 0 )
            body.push_back( '\0' );
        std::string    v2       = v3.substr( 0, kHeader ) + table + body;
        const uint64_t fileSize = v2.size();
        std::memcpy( v2.data() + 12, &version, 4 );
        std::memcpy( v2.data() + 16, &fileSize, 8 );
        return v2;
    }

    std::string AsVersionOne( const std::string& v2 )
    {
        constexpr size_t kHeader  = 64;
        constexpr size_t kRow     = 24;
        constexpr size_t kRowsV2  = 10;
        uint32_t         version  = 0;
        uint32_t         sections = 0;
        uint64_t         fileSize = 0;
        std::memcpy( &version, v2.data() + 12, 4 );
        std::memcpy( &fileSize, v2.data() + 16, 8 );
        std::memcpy( &sections, v2.data() + 24, 4 );
        EXPECT_EQ( version, 2u );
        EXPECT_EQ( sections, kRowsV2 );

        uint64_t lastCount = 0;
        std::memcpy( &lastCount, v2.data() + kHeader + ( kRowsV2 - 1 ) * kRow + 16, 8 );
        EXPECT_EQ( lastCount, 0u ) << "only a mesh with no polygroups has a v1 spelling";

        std::string v1 = v2.substr( 0, kHeader + ( kRowsV2 - 1 ) * kRow ) + v2.substr( kHeader + kRowsV2 * kRow );
        version        = 1;
        sections       = kRowsV2 - 1;
        fileSize -= kRow;
        std::memcpy( v1.data() + 12, &version, 4 );
        std::memcpy( v1.data() + 16, &fileSize, 8 );
        std::memcpy( v1.data() + 24, &sections, 4 );
        for ( size_t row = 0; row < kRowsV2 - 1; ++row )
        {
            uint64_t offset = 0;
            std::memcpy( &offset, v1.data() + kHeader + row * kRow + 8, 8 );
            offset -= kRow;
            std::memcpy( v1.data() + kHeader + row * kRow + 8, &offset, 8 );
        }
        EXPECT_EQ( v1.size(), fileSize );
        return v1;
    }
} // namespace

TEST( MeshBinaryFormat, AVersionTwoFileIsReadWithANullGuid )
{
    Ser::MeshAssetData source = FullyPopulated();
    const auto read = Ser::DecodeMeshBinary( AsVersionTwo( Ser::EncodeMeshBinary( source ) ), "v2.stmesh" );
    ASSERT_TRUE( read.IsSuccess() ) << read.GetError();
    EXPECT_TRUE( read.GetValue().Guid.IsNull() );
    source.Guid = {};
    for ( Ser::SubmeshData& submesh : source.Submeshes )
        submesh.MaterialGuid = {}; // a v2 row with material number 0 names no material
    ExpectSameMesh( source, read.GetValue() );
}

// A v2 SUBMESH THAT NAMES ITS MATERIAL BY THE PRE-GUID NUMBER IS REFUSED BY NAME, never read as "no
// material": the number cannot become a GUID in this build, and the message names the tool that maps it.
TEST( MeshBinaryFormat, AVersionTwoMaterialNumberIsRefusedAndNamesTheMigrator )
{
    const auto read = Ser::DecodeMeshBinary(
         AsVersionTwo( Ser::EncodeMeshBinary( FullyPopulated() ), 0x44d056a9359b4d1cull ), "numbered.stmesh" );
    ASSERT_FALSE( read.IsSuccess() );
    EXPECT_NE( read.GetError().find( "SceneMigrator" ), std::string::npos ) << read.GetError();
    EXPECT_NE( read.GetError().find( std::to_string( 0x44d056a9359b4d1cull ) ), std::string::npos )
         << read.GetError();
}

// THE GATHER LEARNS THE MESH'S IDENTITY FROM ITS PREFIX: the one header entry point states the kind (from
// the skinned flag) and the GUID the file was written with, and a v2 file states no header at all.
TEST( MeshBinaryFormat, TheHeaderEntryPointStatesKindAndGuid )
{
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "af7l_mesh_guid";
    std::filesystem::create_directories( dir );
    Ser::MeshAssetData mesh = FullyPopulated();
    for ( const bool skinned : { false, true } )
    {
        mesh.IsSkinned                  = skinned;
        const std::filesystem::path out = dir / ( skinned ? "m.skmesh" : "m.stmesh" );
        std::ofstream( out, std::ios::binary ) << Ser::EncodeMeshBinary( mesh );
        const auto stated = Common::Content::ReadAssetHeaderIfStated( out, { {}, true } );
        ASSERT_TRUE( stated.IsSuccess() ) << stated.GetError();
        ASSERT_TRUE( stated.GetValue().has_value() );
        EXPECT_EQ( stated.GetValue()->Guid, mesh.Guid );
        EXPECT_EQ( stated.GetValue()->Kind, skinned ? Common::Content::ContentKind::SkinnedMesh
                                                    : Common::Content::ContentKind::StaticMesh );
    }
    const std::filesystem::path v2 = dir / "v2.stmesh";
    std::ofstream( v2, std::ios::binary ) << AsVersionTwo( Ser::EncodeMeshBinary( mesh ) );
    const auto none = Common::Content::ReadAssetHeaderIfStated( v2, { {}, true } );
    ASSERT_TRUE( none.IsSuccess() ) << none.GetError();
    EXPECT_FALSE( none.GetValue().has_value() );

    mesh.Guid                            = {};
    const std::filesystem::path nullGuid = dir / "null.stmesh";
    std::ofstream( nullGuid, std::ios::binary ) << Ser::EncodeMeshBinary( mesh );
    EXPECT_FALSE( Common::Content::ReadAssetHeaderIfStated( nullGuid, { {}, true } ).IsSuccess() );
    std::filesystem::remove_all( dir );
}

// THE MESH'S EDGES ARE STATED BY ITS HEADER READER (T6e2): the registry builds a row's dependencies from the
// header alone, so the mesh -> material edge exists only if this reader hands the submesh materials over
// without the body being decoded. Distinct, in submesh order, a null slot dropped; a submesh table that
// does not fit the declared file is refused rather than read as edges.
TEST( MeshBinaryFormat, TheHeaderEntryPointStatesTheSubmeshMaterialsAsDependencies )
{
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "t6e2_mesh_edges";
    std::filesystem::create_directories( dir );
    Ser::MeshAssetData mesh   = FullyPopulated();
    Ser::SubmeshData   repeat = mesh.Submeshes[0]; // the first material again: one edge, not two
    Ser::SubmeshData   empty  = mesh.Submeshes[1];
    empty.MaterialGuid        = {}; // no material assigned: no edge
    mesh.Submeshes.push_back( empty );
    mesh.Submeshes.push_back( repeat );
    const std::vector<Common::Content::AssetGuid> expected = { mesh.Submeshes[0].MaterialGuid,
                                                               mesh.Submeshes[1].MaterialGuid };
    for ( const bool skinned : { false, true } )
    {
        mesh.IsSkinned                  = skinned;
        const std::filesystem::path out = dir / ( skinned ? "m.skmesh" : "m.stmesh" );
        std::ofstream( out, std::ios::binary ) << Ser::EncodeMeshBinary( mesh );
        const auto stated = Common::Content::ReadAssetHeaderIfStated( out, { {}, true } );
        ASSERT_TRUE( stated.IsSuccess() ) << stated.GetError();
        ASSERT_TRUE( stated.GetValue().has_value() );
        EXPECT_EQ( stated.GetValue()->Dependencies, expected ) << ( skinned ? "skinned" : "static" );
    }

    // The submesh section's count pushed past the declared size: refused by name, not read off the end.
    std::string           bytes = Ser::EncodeMeshBinary( mesh );
    const uint64_t        huge  = 1ull << 40;
    constexpr std::size_t kSubmeshRow =
         Common::Content::kMeshBinaryPrefixV3 +
         ( Common::Content::kMeshBinarySubmeshSectionId - 1 ) * Common::Content::kMeshBinarySectionRowSize;
    std::memcpy( bytes.data() + kSubmeshRow + 16, &huge, 8 );
    const std::filesystem::path bad = dir / "bad.stmesh";
    std::ofstream( bad, std::ios::binary ) << bytes;
    const auto refused = Common::Content::ReadAssetHeaderIfStated( bad, { {}, true } );
    std::filesystem::remove_all( dir );
    ASSERT_FALSE( refused.IsSuccess() ) << "a submesh table past the file's end was read as edges";
    EXPECT_NE( refused.GetError().find( "submesh records" ), std::string::npos ) << refused.GetError();
}

// A VERSION PAST THIS BUILD'S IS REFUSED BY NAME, not read as a v3 prefix: nothing says a later layout keeps
// the GUID at byte 64, so claiming one from there would hand the registry an identity the file never stated.
TEST( MeshBinaryFormat, TheHeaderEntryPointRefusesAVersionPastThisBuilds )
{
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "msh1_mesh_future";
    std::filesystem::create_directories( dir );
    std::string    future = Ser::EncodeMeshBinary( FullyPopulated() );
    const uint32_t v99    = 99;
    std::memcpy( future.data() + 12, &v99, 4 );
    const std::filesystem::path out = dir / "future.stmesh";
    std::ofstream( out, std::ios::binary ) << future;
    const auto stated = Common::Content::ReadAssetHeaderIfStated( out, { {}, true } );
    std::filesystem::remove_all( dir );
    ASSERT_FALSE( stated.IsSuccess() ) << "a v99 mesh was read as a v3 prefix";
    EXPECT_NE( stated.GetError().find( "version 99" ), std::string::npos ) << stated.GetError();
    EXPECT_NE( stated.GetError().find( "future.stmesh" ), std::string::npos ) << stated.GetError();
}

TEST( MeshBinaryFormat, AVersionOneFileIsReadWithNoPolyGroups )
{
    Ser::MeshAssetData source = FullyPopulated();
    source.PolyGroups.clear();
    const std::string v1 = AsVersionOne( AsVersionTwo( Ser::EncodeMeshBinary( source ) ) );
    source.Guid          = {}; // v1 states no identity
    for ( Ser::SubmeshData& submesh : source.Submeshes )
        submesh.MaterialGuid = {};

    const auto read = Ser::ReadMeshAssetData( v1, "v1.stmesh" );
    ASSERT_TRUE( read.IsSuccess() ) << read.GetError();
    EXPECT_TRUE( read.GetValue().PolyGroups.empty() );
    ExpectSameMesh( source, read.GetValue() );

    // And v1 is exactly nine sections: a v1 header over a ten-row table is refused, not half-read.
    std::string lying = Ser::EncodeMeshBinary( source );
    lying[12]         = '\x01';
    EXPECT_FALSE( Ser::DecodeMeshBinary( lying, "v1-with-ten-rows" ).IsSuccess() );
}

TEST( MeshBinaryFormat, PolyGroupsThatDoNotCoverEveryFaceAreRefused )
{
    Ser::MeshAssetData source = FullyPopulated();
    source.PolyGroups.pop_back();
    const auto read = Ser::DecodeMeshBinary( Ser::EncodeMeshBinary( source ), "short-groups.stmesh" );
    ASSERT_FALSE( read.IsSuccess() );
    EXPECT_NE( read.GetError().find( "polygroups for" ), std::string::npos ) << read.GetError();
}

TEST( MeshBinaryFormat, ACorruptHeaderIsRefusedByName )
{
    const std::string good = Ser::EncodeMeshBinary( FullyPopulated() );

    const auto Mutate = []( const std::string& bytes, const size_t at, const char value )
    {
        std::string copy = bytes;
        copy[at]         = value;
        return copy;
    };

    // Magic, byte-order tag, version, declared size, section count, and one section's element size —
    // each is a separate refusal, and each must be a refusal rather than a wrong mesh.
    EXPECT_FALSE( Ser::DecodeMeshBinary( Mutate( good, 3, 'X' ), "bad-magic" ).IsSuccess() );
    EXPECT_FALSE( Ser::DecodeMeshBinary( Mutate( good, 8, '\x09' ), "bad-endian" ).IsSuccess() );
    EXPECT_FALSE( Ser::DecodeMeshBinary( Mutate( good, 12, '\x63' ), "bad-version" ).IsSuccess() );
    EXPECT_FALSE( Ser::DecodeMeshBinary( Mutate( good, 16, '\x00' ), "bad-size" ).IsSuccess() );
    EXPECT_FALSE( Ser::DecodeMeshBinary( Mutate( good, 24, '\x02' ), "bad-section-count" ).IsSuccess() );
    // Prefix + 4 is the first section row's ElementSize.
    EXPECT_FALSE( Ser::DecodeMeshBinary( Mutate( good, Common::Content::kMeshBinaryPrefixV3 + 4, '\x37' ),
                                         "bad-element-size" )
                       .IsSuccess() );
    // Prefix + 8 is the first section row's Offset: pushing it past the end must not be followed.
    EXPECT_FALSE(
         Ser::DecodeMeshBinary( Mutate( good, Common::Content::kMeshBinaryPrefixV3 + 8, '\x78' ), "bad-offset" )
              .IsSuccess() );

    // The control: the unmutated bytes still load, so the expectations above are about the mutation
    // and not about the fixture.
    EXPECT_TRUE( Ser::DecodeMeshBinary( good, "control" ).IsSuccess() );
}

TEST( MeshBinaryFormat, ARecordPointingOutsideItsSectionIsRefusedRatherThanFollowed )
{
    // A submesh whose LOD range names a level that does not exist, and a name that runs off the end of
    // the string section. Both are reads of whatever is next in memory if the reader trusts the file.
    const std::string good = Ser::EncodeMeshBinary( FullyPopulated() );

    // The Submeshes section is row 4 of the table (1-based), so its Offset is at 64 + 3*24 + 8.
    uint64_t submeshOffset = 0;
    // The v3 prefix (header + GUID), then three 24-byte rows, then the row's Id+ElementSize: the Submeshes
    // section's Offset field. Named rather than multiplied inline so the widening is explicit.
    const std::ptrdiff_t submeshRowOffsetField =
         static_cast<std::ptrdiff_t>( Common::Content::kMeshBinaryPrefixV3 ) +
         3 * static_cast<std::ptrdiff_t>( 24 ) + 8;
    std::memcpy( &submeshOffset, good.data() + submeshRowOffsetField, sizeof( submeshOffset ) );

    {
        std::string    bad  = good;
        const uint32_t huge = 4000u;
        std::memcpy( &bad[submeshOffset + 24], &huge, sizeof( huge ) ); // BinSubmesh::LODFirst
        EXPECT_FALSE( Ser::DecodeMeshBinary( bad, "lod-out-of-range" ).IsSuccess() );
    }
    {
        std::string    bad  = good;
        const uint32_t huge = 4000u;
        std::memcpy( &bad[submeshOffset + 4], &huge, sizeof( huge ) ); // BinSubmesh::NameLength
        EXPECT_FALSE( Ser::DecodeMeshBinary( bad, "name-out-of-range" ).IsSuccess() );
    }
}

TEST( MeshBinaryFormat, TheRetiredJsonFormIsRefusedByNameAndTheMessageNamesTheRemedy )
{
    // THIS TEST USED TO ASSERT THE OPPOSITE, and the change of direction is the point rather than an
    // edit. The JSON arm was removed by owner decision — cooked content is DERIVED, so a stale cook is
    // deleted and cooked again rather than migrated — and a test asserting a retired guarantee is a
    // test that fails for being right about yesterday.
    //
    // WHAT REPLACES IT IS NOT "it fails". A stale cook is not a corrupt file and must not read like
    // one: the reader has to say WHICH file, WHY, and WHAT TO RUN. That is the difference between a
    // user re-cooking in ten seconds and a user filing a corruption report.
    const Ser::MeshAssetData source = FullyPopulated();
    const std::string        asJson = rfl::json::write( source );

    ASSERT_FALSE( Ser::LooksLikeMeshBinary( asJson ) );

    const auto refused = Ser::ReadMeshAssetData( asJson, "legacy.stmesh" );
    ASSERT_FALSE( refused.IsSuccess() ) << "the JSON form is readable again — if that is deliberate, "
                                           "this test is what has to change with it";

    const std::string& why = refused.GetError();
    EXPECT_NE( why.find( "legacy.stmesh" ), std::string::npos ) << why; // which file
    EXPECT_NE( why.find( "magic" ), std::string::npos ) << why;         // why
    EXPECT_NE( why.find( "cook" ), std::string::npos ) << why;          // what to run

    // AND THE CONTAINER IS STILL READ, because "everything is refused" would satisfy every assertion
    // above and be a dead reader. The negative control belongs in the same test as the positive one.
    const auto accepted = Ser::ReadMeshAssetData( Ser::EncodeMeshBinary( source ), "current.stmesh" );
    ASSERT_TRUE( accepted.IsSuccess() ) << accepted.GetError();
    ExpectSameMesh( source, accepted.GetValue() );
}

TEST( MeshBinaryFormat, BytesThatAreNeitherFormAreRefusedRatherThanReadAsEmpty )
{
    for ( const std::string_view junk :
          { std::string_view( "" ), std::string_view( "not a mesh" ), std::string_view( "{\"IsSkinned\":" ) } )
    {
        const auto read = Ser::ReadMeshAssetData( junk, "junk.stmesh" );
        EXPECT_FALSE( read.IsSuccess() ) << "accepted " << junk.size() << " bytes of junk";
    }
}

TEST( MeshBinaryFormat, TheAssetLoaderReadsAContainerOffDisk )
{
    // End to end through the shipped loader, because the two tests above prove the codec and say
    // nothing about the class that calls it.
    const std::filesystem::path path =
         std::filesystem::temp_directory_path() / "desert_b11_asset_roundtrip.stmesh";

    Ser::MeshAssetData source = FullyPopulated();
    source.SkeletonSignature.reset(); // a static mesh has none

    // A static mesh on disk is its source asset (AF4d); the container is its render form in the DDC, where the
    // cook leaves it for a game.
    const std::filesystem::path derived =
         Desert::TestSupport::WriteCookedStaticMesh( path, Ser::EncodeMeshBinary( source ) );
    ASSERT_FALSE( derived.empty() );

    Desert::Assets::StaticMeshAsset asset( Desert::Assets::AssetPriority::Medium, path );
    const auto                      loaded = asset.LoadFromFile();
    ASSERT_TRUE( loaded.IsSuccess() ) << loaded.GetError();

    ASSERT_EQ( source.StaticVertices.size(), asset.GetVertices().size() );
    ASSERT_EQ( source.Indices.size(), asset.GetIndices().size() );
    ASSERT_EQ( source.Submeshes.size(), asset.GetSubmeshes().size() );
    EXPECT_EQ( source.Submeshes[1].Name, asset.GetSubmeshes()[1].Name );
    EXPECT_EQ( source.Submeshes[0].LODs.size(), asset.GetSubmeshes()[0].BakedLODs.size() );
    EXPECT_EQ( 0u, asset.GetSubmeshes()[1].BakedLODs.size() );
    ASSERT_EQ( source.MorphTargets.size(), asset.GetMorphTargets().size() );
    EXPECT_EQ( source.MorphTargets[1].Name, asset.GetMorphTargets()[1].Name );
    EXPECT_TRUE( asset.GetMorphTargets()[1].DeltaNormals.empty() );

    // And a file whose bytes stop early does not become a mesh, on the path a real load takes.
    const std::string           full = Ser::EncodeMeshBinary( source );
    const std::filesystem::path cut  = std::filesystem::temp_directory_path() / "desert_b11_cut.stmesh";
    const std::filesystem::path cutDerived =
         Desert::TestSupport::WriteCookedStaticMesh( cut, std::string( full.substr( 0, full.size() - 16 ) ) );
    ASSERT_FALSE( cutDerived.empty() );
    Desert::Assets::StaticMeshAsset truncated( Desert::Assets::AssetPriority::Medium, cut );
    EXPECT_FALSE( truncated.LoadFromFile().IsSuccess() );

    std::error_code ec;
    std::filesystem::remove( path, ec );
    std::filesystem::remove( cut, ec );
    std::filesystem::remove( derived, ec );
    std::filesystem::remove( cutDerived, ec );
}

TEST( MeshBinaryFormat, EveryCommittedCookedMeshIsTheContainer )
{
    // THE CORPUS, AND THE REGISTER OF IT IS `.gitignore` ITSELF. `Editor/Cooked/` is a blanket ignore
    // with one `!` line per committed fixture, so those lines are exactly the cooked meshes this
    // repository ships — a list typed here would be one a new fixture can fall out of in silence,
    // which is the A27 defect one level up.
    //
    // Enumerating the DIRECTORY would be wrong for the opposite reason: a developer's own cooks live
    // there too (`base.stmesh` is 40 MB of them), are machine-local, and are none of this suite's
    // business.
    const std::filesystem::path root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "could not find the repository root from the working directory";

    const std::string ignore = ReadFile( root / ".gitignore" );
    ASSERT_FALSE( ignore.empty() );

    std::vector<std::string> corpus;
    std::istringstream       in( ignore );
    std::string              line;
    while ( std::getline( in, line ) )
    {
        while ( !line.empty() && ( line.back() == '\r' || line.back() == ' ' ) )
            line.pop_back();
        if ( line.empty() || line[0] == '#' || line[0] != '!' )
            continue; // comments are dropped before parsing: §8.3
        const std::string path = line.substr( 1 );
        if ( path.size() > 7 && ( path.compare( path.size() - 7, 7, ".stmesh" ) == 0 ||
                                  path.compare( path.size() - 7, 7, ".skmesh" ) == 0 ) )
            corpus.push_back( path );
    }

    ASSERT_FALSE( corpus.empty() ) << ".gitignore admits no cooked mesh at all; either the rules moved "
                                      "or this test is reading the wrong file";

    for ( const std::string& relative : corpus )
    {
        const std::string bytes = ReadFile( root / relative );
        ASSERT_FALSE( bytes.empty() ) << relative << " is admitted by .gitignore and is empty or absent";
        EXPECT_TRUE( Desert::Assets::Serialization::LooksLikeMeshBinary( bytes ) )
             << relative << " is still the retired JSON form. The corpus is converted BY THE COMMIT "
             << "that changed the format — a migration that only runs on someone's machine is not one.";

        const auto read = Ser::ReadMeshAssetData( bytes, relative );
        ASSERT_TRUE( read.IsSuccess() ) << relative << ": " << read.GetError();
        EXPECT_FALSE( read.GetValue().Submeshes.empty() ) << relative << " carries no submesh";
    }
}

namespace
{
    // The editor's project, opened the way the editor opens it: cwd = Editor/ (engine resource roots resolve
    // against it) and the project root set from Desert.deproj. Restored on scope exit.
    class EditorProject
    {
    public:
        explicit EditorProject( const std::filesystem::path& repoRoot )
             : m_SavedRoot( Common::Constants::Path::CurrentProjectRoot() ),
               m_SavedCwd( std::filesystem::current_path() )
        {
            const std::filesystem::path editorDir = std::filesystem::absolute( repoRoot / "Editor" );
            std::filesystem::current_path( editorDir );
            const auto project = Common::Project::ReadProjectFile( ReadFile( editorDir / "Desert.deproj" ) );
            if ( !project )
                return;
            Common::Constants::Path::SetProjectRoot( editorDir, project.GetValue().AssetsRoot );
            m_Opened = true;
        }
        ~EditorProject()
        {
            Common::Constants::Path::SetProjectRoot( m_SavedRoot.ProjectDir, m_SavedRoot.AssetsRoot );
            std::error_code ec;
            std::filesystem::current_path( m_SavedCwd, ec );
        }
        EditorProject( const EditorProject& )            = delete;
        EditorProject& operator=( const EditorProject& ) = delete;
        bool           Opened() const
        {
            return m_Opened;
        }

    private:
        Common::Constants::Path::ProjectRootState m_SavedRoot;
        std::filesystem::path                     m_SavedCwd;
        bool                                      m_Opened = false;
    };
} // namespace

// THE GATHERED REGISTRY'S BOUNDS COLUMN AGREES WITH THE MESH IT DESCRIBES, for every mesh the repository
// tracks. The column exists so the world partitioner can place a mesh WITHOUT reading it (WP15), and the
// gather learns it from the mesh's 64-byte header alone (MeshBinaryHeader.hpp) — so nothing downstream ever
// reads the body to notice a header box that disagrees with it. The relation is held here, bit for bit.
//
// The cook states the box with Serialization::MeshDataBounds and the loaded asset unions the same stored
// submesh boxes (Geometry::LocalBounds), so one function is the right side of the comparison.
TEST( MeshBinaryFormat, EveryGatheredMeshRowStatesTheBoundsOfItsFile )
{
    const std::filesystem::path root = RepoRoot();
    ASSERT_FALSE( root.empty() );
    const EditorProject project( root );
    ASSERT_TRUE( project.Opened() );
    const Common::Content::GatheredRegistry gathered = Common::Content::GatherContentRegistry( {} );
    ASSERT_TRUE( gathered.Refused.empty() ) << gathered.Refused.front();
    EXPECT_TRUE( gathered.MeshesWithoutHeaderBounds.empty() )
         << gathered.MeshesWithoutHeaderBounds.front()
         << " states no box in its header; the tracked meshes are re-cooked by the commit that added it";

    std::size_t meshes = 0;
    for ( const Common::Utils::AssetRegistryEntry& row : gathered.Registry.Entries() )
    {
        if ( row.Kind != "StaticMesh" && row.Kind != "SkinnedMesh" )
        {
            EXPECT_FALSE( row.Bounds.has_value() )
                 << row.Key << " is a " << row.Kind << " and states bounds nothing computes for it";
            continue;
        }
        ++meshes;
        const std::filesystem::path       file = Common::AssetHandle::PathForStableKey( row.Key );
        std::optional<Common::Math::AABB> box;
        if ( row.Kind == "StaticMesh" )
        {
            // A static mesh file is the source asset: its header states LOD0's source box.
            const auto source = Desert::Assets::ReadMeshSourceAssetFile( file );
            ASSERT_TRUE( source.IsSuccess() ) << row.Key << ": " << source.GetError();
            ASSERT_FALSE( source.GetValue().Source.Models.empty() ) << row.Key << " has no source model";
            const auto b = Desert::Assets::MeshSourceBounds( source.GetValue().Source.Models[0].Mesh );
            if ( b.has_value() )
                box = Common::Math::AABB{ { b->Lo[0], b->Lo[1], b->Lo[2] }, { b->Hi[0], b->Hi[1], b->Hi[2] } };
        }
        else
        {
            const std::string bytes = ReadFile( file );
            ASSERT_FALSE( bytes.empty() ) << row.Key << " -> " << file.string();
            const auto read = Ser::ReadMeshAssetData( bytes, file.string() );
            ASSERT_TRUE( read.IsSuccess() ) << row.Key << ": " << read.GetError();
            box = Ser::MeshDataBounds( read.GetValue() );
        }
        ASSERT_TRUE( box.has_value() ) << row.Key << " has no submesh";
        EXPECT_TRUE( Common::Utils::SameBounds( row.Bounds, box ) )
             << row.Key << " states " << ( row.Bounds.has_value() ? "a different box" : "no box" )
             << " in its header than its body holds. Re-cook the mesh.";
    }
    EXPECT_GT( meshes, 0u ) << "the gather found no mesh; this test compares nothing";
}

// THE HEADER STATES THE BOX THE BODY HOLDS, and says "no extent" and "states nothing" apart: an empty mesh
// states an inverted box under the flag, a file written before the flag reads as unstated (never as a box
// at the origin), and neither is the other.
TEST( MeshBinaryFormat, TheHeaderStatesTheBodysBoxAndTellsNoExtentFromNoStatement )
{
    Ser::MeshAssetData data;
    Ser::SubmeshData   first;
    first.BoundingBox.Min = { -1.5f, 0.0f, 2.0f };
    first.BoundingBox.Max = { 1.0f, 3.25f, 4.0f };
    Ser::SubmeshData second;
    second.BoundingBox.Min = { -0.5f, -2.0f, 3.0f };
    second.BoundingBox.Max = { 7.0f, 1.0f, 3.5f };
    data.Submeshes         = { first, second };

    std::string bytes  = Ser::EncodeMeshBinary( data );
    const auto  stated = Common::Content::ReadMeshHeaderBounds( bytes );
    ASSERT_TRUE( stated.has_value() );
    EXPECT_TRUE( stated->Stated );
    EXPECT_TRUE( Common::Utils::SameBounds( stated->Bounds, Ser::MeshDataBounds( data ) ) );

    const auto empty = Common::Content::ReadMeshHeaderBounds( Ser::EncodeMeshBinary( Ser::MeshAssetData{} ) );
    ASSERT_TRUE( empty.has_value() );
    EXPECT_TRUE( empty->Stated );
    EXPECT_FALSE( empty->Bounds.has_value() );

    // A file from before the flag: the bit clear and the 24 bytes zero, as the writer left them then.
    Common::Content::MeshBinaryFileHeader header{};
    std::memcpy( &header, bytes.data(), sizeof( header ) );
    header.Flags &= ~Common::Content::kMeshFlagHasBounds;
    std::memset( header.BoundsMin, 0, sizeof( header.BoundsMin ) );
    std::memset( header.BoundsMax, 0, sizeof( header.BoundsMax ) );
    std::memcpy( bytes.data(), &header, sizeof( header ) );
    const auto old = Common::Content::ReadMeshHeaderBounds( bytes );
    ASSERT_TRUE( old.has_value() );
    EXPECT_FALSE( old->Stated );
    EXPECT_FALSE( old->Bounds.has_value() );
}

TEST( MeshBinaryFormat, TheImporterWritesTheContainerAndNotJson )
{
    // A CENSUS OVER SOURCE TEXT, for StaticMeshCooked's reason: `ImportManager.cpp` is compiled by the
    // Editor alone, no suite links it, and a property no runtime test on this machine can observe is
    // gated over the text instead. What it forbids: the one function that writes a cooked mesh going
    // back to `WriteCookedJson`, which would leave the readers accepting both forms for ever while
    // nothing ever produced the fast one.
    const std::filesystem::path root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    const std::string source = ReadFile( root / "Editor" / "Source" / "Editor" / "Import" / "ImportManager.cpp" );
    ASSERT_FALSE( source.empty() ) << "ImportManager.cpp not found — did it move?";

    const size_t serialize = source.find( "ImportManager::SerializeMeshAsset" );
    ASSERT_NE( serialize, std::string::npos ) << "SerializeMeshAsset is gone or renamed";

    // The body ends at the next function definition; looking only inside it keeps the sibling cooked
    // kinds (.skeleton, .anim, .demat), which legitimately stay JSON, out of the question.
    const size_t nextFunction = source.find( "ImportManager::SerializeSkeletonAsset", serialize );
    ASSERT_NE( nextFunction, std::string::npos );
    const std::string body = source.substr( serialize, nextFunction - serialize );

    EXPECT_NE( body.find( "EncodeMeshBinary" ), std::string::npos )
         << "SerializeMeshAsset no longer encodes the container";
    EXPECT_EQ( body.find( "WriteCookedJson" ), std::string::npos )
         << "SerializeMeshAsset writes a cooked mesh as JSON again";
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
