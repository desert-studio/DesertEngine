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

#include <Common/Core/Serialization/GlmReflection.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <Engine/Assets/Mesh/StaticMeshAsset.hpp>
#include <Engine/Assets/Serialization/MeshBinary.hpp>

#include <rflcpp/rfl.hpp>
#include <rflcpp/rfl/json.hpp>

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
            v.Position    = glm::vec3( -1.0f * i, 2.0f * i, 0.5f );
            v.Normal      = glm::vec3( 0.0f, 0.0f, -1.0f );
            v.Tangent     = glm::vec3( 0.0f, 1.0f, 0.0f );
            v.Bitangent   = glm::vec3( 1.0f, 0.0f, 0.0f );
            v.TexCoord    = glm::vec2( 0.5f, 0.25f * i );
            v.BoneIDs     = { i, i + 1u, i + 2u, i + 3u };
            v.BoneWeights = { 0.5f, 0.25f, 0.125f, 0.125f };
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
        a.MaterialHandle = Common::UUID( 0xABCDEF0123456789ull );
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
        b.MaterialHandle = Common::UUID( 7ull );
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
        return data;
    }

    /// BY VALUE, AND BIT FOR BIT ON THE ARRAYS. `EXPECT_EQ` on a float compares the values, which is
    /// what we want for the scalars; the vertex and index arrays go through `memcmp` so that a lost
    /// mantissa bit — the one failure a printed comparison cannot see — is a red test.
    void ExpectSameMesh( const Ser::MeshAssetData& expected, const Ser::MeshAssetData& actual )
    {
        EXPECT_EQ( expected.IsSkinned, actual.IsSkinned );
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
            EXPECT_EQ( 0, std::memcmp( &e.Transform, &g.Transform, sizeof( glm::mat4 ) ) ) << "submesh " << i;
            EXPECT_EQ( 0, std::memcmp( &e.BoundingBox, &g.BoundingBox, sizeof( Common::Math::AABB ) ) )
                 << "submesh " << i;
            EXPECT_EQ( static_cast<uint64_t>( e.MaterialHandle ), static_cast<uint64_t>( g.MaterialHandle ) )
                 << "submesh " << i;
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
    for ( const size_t cut :
          { emptyBytes.size(), full.size() - 1, full.size() / 2, size_t( 64 ), size_t( 63 ), size_t( 0 ) } )
    {
        const auto cutRead =
             Ser::DecodeMeshBinary( std::string_view( full ).substr( 0, cut ), "truncated.stmesh" );
        EXPECT_FALSE( cutRead.IsSuccess() )
             << "a file cut to " << cut << " of " << full.size() << " bytes was accepted";
    }
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
    // Byte 68 is the first section row's ElementSize.
    EXPECT_FALSE( Ser::DecodeMeshBinary( Mutate( good, 68, '\x37' ), "bad-element-size" ).IsSuccess() );
    // Byte 72 is the first section row's Offset: pushing it past the end must not be followed.
    EXPECT_FALSE( Ser::DecodeMeshBinary( Mutate( good, 72, '\x78' ), "bad-offset" ).IsSuccess() );

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
    std::memcpy( &submeshOffset, good.data() + 64 + 3 * 24 + 8, sizeof( submeshOffset ) );

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

TEST( MeshBinaryFormat, TheRetiredJsonFormStillOpensAndAgreesWithTheContainer )
{
    // MIGRATION, AND IT IS THE WHOLE REASON THE JSON READER IS STILL HERE. The fixture is DERIVED from
    // the same source mesh as the binary one — no file on disk, so no ignore rule can swallow it and
    // no clone can be green only because the bytes happen to be there.
    const Ser::MeshAssetData source   = FullyPopulated();
    const std::string        asJson   = rfl::json::write( source );
    const std::string        asBinary = Ser::EncodeMeshBinary( source );

    ASSERT_FALSE( Ser::LooksLikeMeshBinary( asJson ) );

    const auto fromJson = Ser::ReadMeshAssetData( asJson, "legacy.stmesh" );
    ASSERT_TRUE( fromJson.IsSuccess() ) << fromJson.GetError();
    const auto fromBinary = Ser::ReadMeshAssetData( asBinary, "current.stmesh" );
    ASSERT_TRUE( fromBinary.IsSuccess() ) << fromBinary.GetError();

    // The relation, not each half: the two forms of one mesh must arrive as the same mesh.
    ExpectSameMesh( fromJson.GetValue(), fromBinary.GetValue() );
    ExpectSameMesh( source, fromJson.GetValue() );
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

    ASSERT_TRUE( Common::Utils::FileSystem::WriteContentToFileAtomic( path, Ser::EncodeMeshBinary( source ) ) );

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
    ASSERT_TRUE( Common::Utils::FileSystem::WriteContentToFileAtomic(
         cut, std::string( full.substr( 0, full.size() - 16 ) ) ) );
    Desert::Assets::StaticMeshAsset truncated( Desert::Assets::AssetPriority::Medium, cut );
    EXPECT_FALSE( truncated.LoadFromFile().IsSuccess() );

    std::error_code ec;
    std::filesystem::remove( path, ec );
    std::filesystem::remove( cut, ec );
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
