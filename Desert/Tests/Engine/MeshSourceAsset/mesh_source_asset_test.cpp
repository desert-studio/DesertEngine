// AF4b: the mesh asset envelope (META + IMPT + SRCE) round-trips byte for byte, states kind/GUID/dependencies
// from its header prefix alone, and refuses a file whose sections are reordered, missing or corrupted.
#include <Engine/Assets/MeshSourceAsset.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

namespace
{
    namespace CC = Common::Content;
    using namespace Desert::Assets;

    const CC::AssetGuid kMatA{ 0x1111222233334444ULL, 0x5555666677778888ULL };
    const CC::AssetGuid kMatB{ 0x9999AAAABBBBCCCCULL, 0xDDDDEEEEFFFF0001ULL };

    // Two triangles sharing an edge, two slots, every overlay kind, a hole in the colour layer.
    MeshSourceAsset MakeQuad( const bool skinned )
    {
        MeshSourceAsset a;
        a.Kind                         = skinned ? CC::ContentKind::SkinnedMesh : CC::ContentKind::StaticMesh;
        a.Guid                         = { 0xA5A5A5A5A5A5A5A5ULL, 0x0123456789ABCDEFULL };
        a.Name                         = "Quad";
        a.Import.SourceFile            = "assets:Meshes/Quad.fbx";
        a.Import.SourceHash            = 0xFEEDFACECAFEBEEFULL;
        a.Import.Settings.UniformScale = 2.54f;
        a.Import.Settings.UpAxis       = MeshSourceUpAxis::Z;
        a.Import.Settings.LodPolicy    = MeshLodPolicy::None;
        auto& m                        = a.Source.Mesh;
        m.Positions   = { -50.f, 0.f, -25.f, 50.f, 0.f, -25.f, 50.f, 10.f, 25.f, -50.f, 0.f, 25.f };
        m.Triangles   = { 0, 1, 2, 0, 2, 3 };
        m.PolyGroups  = { 7, 7 };
        m.MaterialIds = { 0, 1 };
        m.Normals     = Desert::Geometry::EditMeshOverlaySer{ { 0.f, 1.f, 0.f }, { 0, 0, 0, 0, 0, 0 } };
        m.Tangents    = Desert::Geometry::EditMeshOverlaySer{ { 1.f, 0.f, 0.f, -1.f }, { 0, 0, 0, 0, 0, 0 } };
        m.Colors      = Desert::Geometry::EditMeshOverlaySer{ { 1.f, 0.5f, 0.25f, 1.f }, { 0, 0, 0, -1, -1, -1 } };
        m.UVs         = { { { 0.f, 0.f, 1.f, 0.f, 1.f, 1.f, 0.f, 1.f }, { 0, 1, 2, 0, 2, 3 } } };
        // Slot 2 is unassigned and slot 1 repeats A: the header names A and B once each.
        a.Source.MaterialSlots = { { "Body", kMatA }, { "Trim", kMatB }, { "Spare", {} }, { "Again", kMatA } };
        if ( skinned )
            a.Source.Skin = MeshSkin{
                 0x5EE1E7011ULL, { "root", "arm" }, { { 0, 0, 1.f }, { 2, 1, 0.75f }, { 2, 0, 0.25f } } };
        return a;
    }

    std::vector<std::byte> Encode( const MeshSourceAsset& a )
    {
        auto bytes = EncodeMeshSourceAsset( a );
        if ( !bytes.IsSuccess() )
        {
            ADD_FAILURE() << bytes.GetError();
            return {};
        }
        return bytes.GetValue();
    }

    // Re-assemble a valid file's envelope with `edit` applied to its sections (a well-formed envelope
    // around a malformed mesh asset, so it is the MESH codec that must refuse it).
    template <typename Edit>
    std::vector<std::byte> Reassemble( const std::vector<std::byte>& file, Edit edit )
    {
        auto e = CC::ReadAssetEnvelope( file, MeshAssetHeaderReadContext() );
        if ( !e.IsSuccess() )
        {
            ADD_FAILURE() << e.GetError();
            return {};
        }
        CC::AssetEnvelope envelope = e.GetValue();
        edit( envelope );
        auto out = CC::WriteAssetEnvelope( envelope );
        if ( !out.IsSuccess() )
        {
            ADD_FAILURE() << out.GetError();
            return {};
        }
        return out.GetValue();
    }

    void ExpectRefused( const std::vector<std::byte>& file, const std::string_view needle )
    {
        const auto r = DecodeMeshSourceAsset( file );
        ASSERT_FALSE( r.IsSuccess() ) << "decoded a file that should be refused (" << needle << ")";
        EXPECT_NE( r.GetError().find( needle ), std::string::npos ) << r.GetError();
    }
} // namespace

TEST( MeshSourceAsset, StaticRoundTripIsByteIdentical )
{
    const MeshSourceAsset a     = MakeQuad( false );
    const auto            bytes = Encode( a );
    const auto            back  = DecodeMeshSourceAsset( bytes );
    if ( !back.IsSuccess() )
    {
        ADD_FAILURE() << back.GetError();
        return;
    }
    EXPECT_EQ( back.GetValue(), a );
    EXPECT_EQ( Encode( back.GetValue() ), bytes );
}

TEST( MeshSourceAsset, SkinnedRoundTripIsByteIdentical )
{
    const MeshSourceAsset a     = MakeQuad( true );
    const auto            bytes = Encode( a );
    const auto            back  = DecodeMeshSourceAsset( bytes );
    if ( !back.IsSuccess() )
    {
        ADD_FAILURE() << back.GetError();
        return;
    }
    EXPECT_EQ( back.GetValue(), a );
    EXPECT_EQ( Encode( back.GetValue() ), bytes );
}

TEST( MeshSourceAsset, RecoveredProvenanceRoundTrips )
{
    MeshSourceAsset a   = MakeQuad( false );
    a.Import.Provenance = MeshSourceProvenance::Recovered;
    a.Import.SourceFile.clear();
    a.Import.SourceHash = 0;
    const auto back     = DecodeMeshSourceAsset( Encode( a ) );
    if ( !back.IsSuccess() )
    {
        ADD_FAILURE() << back.GetError();
        return;
    }
    EXPECT_EQ( back.GetValue().Import, a.Import );
}

// The registry/loader question "what is this file" is answered from the header prefix: the file is cut right
// after the header, so no body byte exists to be read, and ReadAssetHeader still states kind, GUID and deps.
TEST( MeshSourceAsset, ReadAssetHeaderNeedsNoBody )
{
    const MeshSourceAsset a      = MakeQuad( true );
    const auto            bytes  = Encode( a );
    const auto            header = CC::ReadEnvelopeHeader( bytes, MeshAssetHeaderReadContext() );
    if ( !header.IsSuccess() )
    {
        ADD_FAILURE() << header.GetError();
        return;
    }
    ASSERT_LT( header.GetValue().HeaderSize, bytes.size() );
    const auto dir = std::filesystem::temp_directory_path() / "MeshSourceAssetTest";
    std::filesystem::create_directories( dir );
    const auto file = dir / "HeaderOnly.skmesh";
    {
        std::ofstream out( file, std::ios::binary | std::ios::trunc );
        for ( uint32_t i = 0; i < header.GetValue().HeaderSize; ++i )
            out.put( static_cast<char>( std::to_integer<uint8_t>( bytes[i] ) ) );
    }
    const auto stated = CC::ReadAssetHeader( file, MeshAssetHeaderReadContext() );
    if ( !stated.IsSuccess() )
    {
        ADD_FAILURE() << stated.GetError();
        return;
    }
    EXPECT_EQ( stated.GetValue().Kind, CC::ContentKind::SkinnedMesh );
    EXPECT_EQ( stated.GetValue().Guid, a.Guid );
    EXPECT_EQ( stated.GetValue().Dependencies, ( std::vector<CC::AssetGuid>{ kMatA, kMatB } ) );
    // ...and the body really is absent: the whole-asset read of the same file is refused.
    EXPECT_FALSE( ReadMeshSourceAssetFile( file ).IsSuccess() );
    std::filesystem::remove_all( dir );
}

TEST( MeshSourceAsset, WriteThenReadFile )
{
    const MeshSourceAsset a    = MakeQuad( false );
    const auto            dir  = std::filesystem::temp_directory_path() / "MeshSourceAssetFileTest";
    const auto            file = dir / "Quad.stmesh";
    std::filesystem::create_directories( dir );
    const auto written = WriteMeshSourceAssetFile( file, a );
    ASSERT_TRUE( written.IsSuccess() ) << written.GetError();
    const auto back = ReadMeshSourceAssetFile( file );
    if ( !back.IsSuccess() )
    {
        ADD_FAILURE() << back.GetError();
        return;
    }
    EXPECT_EQ( back.GetValue(), a );
    std::filesystem::remove_all( dir );
}

TEST( MeshSourceAsset, SwappedSectionOrderIsRefused )
{
    const auto file = Reassemble( Encode( MakeQuad( false ) ),
                                  []( CC::AssetEnvelope& e ) { std::swap( e.Sections[0], e.Sections[1] ); } );
    ExpectRefused( file, "in that order" );
}

TEST( MeshSourceAsset, MissingImportInfoIsRefused )
{
    const auto file = Reassemble( Encode( MakeQuad( false ) ),
                                  []( CC::AssetEnvelope& e ) { e.Sections.erase( e.Sections.begin() + 1 ); } );
    ExpectRefused( file, "META,IMPT,SRCE" );
}

TEST( MeshSourceAsset, CorruptHeaderCrcIsRefused )
{
    auto file = Encode( MakeQuad( false ) );
    file[20] ^= std::byte{ 0x01 }; // inside the GUID: the stored CRC no longer covers the header
    ExpectRefused( file, "CRC" );
}

TEST( MeshSourceAsset, CorruptSourceBodyIsRefused )
{
    auto file = Encode( MakeQuad( false ) );
    file[file.size() - 3] ^= std::byte{ 0x40 }; // the last section is SRCE; its TOC hash no longer matches
    const auto r = DecodeMeshSourceAsset( file );
    EXPECT_FALSE( r.IsSuccess() );
}

TEST( MeshSourceAsset, StaleMetaBoundsAreRefused )
{
    const auto file = Reassemble( Encode( MakeQuad( false ) ),
                                  []( CC::AssetEnvelope& e )
                                  {
                                      auto             decoded = CC::DecodeEnvelopeMeta( e.Sections[0].Bytes );
                                      CC::EnvelopeMeta meta    = decoded.GetValue();
                                      meta.Bounds->Hi[1] += 1.0f;
                                      e.Sections[0].Bytes = CC::EncodeEnvelopeMeta( meta );
                                  } );
    ExpectRefused( file, "bounds" );
}

TEST( MeshSourceAsset, HeaderDependenciesMustMatchSlots )
{
    const auto file = Reassemble( Encode( MakeQuad( false ) ),
                                  []( CC::AssetEnvelope& e ) { e.Asset.Dependencies.pop_back(); } );
    ExpectRefused( file, "dependencies" );
}

TEST( MeshSourceAsset, KindAndSkinMustAgree )
{
    MeshSourceAsset a = MakeQuad( false );
    a.Kind            = CC::ContentKind::SkinnedMesh;
    EXPECT_FALSE( EncodeMeshSourceAsset( a ).IsSuccess() );
    MeshSourceAsset b = MakeQuad( true );
    b.Kind            = CC::ContentKind::StaticMesh;
    EXPECT_FALSE( EncodeMeshSourceAsset( b ).IsSuccess() );
    MeshSourceAsset c = MakeQuad( false );
    c.Kind            = CC::ContentKind::Texture;
    EXPECT_FALSE( EncodeMeshSourceAsset( c ).IsSuccess() );
}

TEST( MeshSourceAsset, MalformedSourceIsRefusedOnEncode )
{
    MeshSourceAsset slot            = MakeQuad( false );
    slot.Source.Mesh.MaterialIds[1] = 4;
    EXPECT_FALSE( EncodeMeshSourceAsset( slot ).IsSuccess() );
    MeshSourceAsset torn = MakeQuad( false );
    if ( !torn.Source.Mesh.Colors.has_value() )
    {
        ADD_FAILURE() << "the quad fixture lost its colour overlay";
        return;
    }
    torn.Source.Mesh.Colors.value().Triangles[3] = 0; // half-unset triangle
    EXPECT_FALSE( EncodeMeshSourceAsset( torn ).IsSuccess() );
    MeshSourceAsset scale              = MakeQuad( false );
    scale.Import.Settings.UniformScale = 0.0f;
    EXPECT_FALSE( EncodeMeshSourceAsset( scale ).IsSuccess() );
    MeshSourceAsset anonymous = MakeQuad( false );
    anonymous.Import.SourceFile.clear();
    EXPECT_FALSE( EncodeMeshSourceAsset( anonymous ).IsSuccess() );
    MeshSourceAsset weight = MakeQuad( true );
    if ( !weight.Source.Skin.has_value() )
    {
        ADD_FAILURE() << "the skinned quad fixture lost its skin";
        return;
    }
    weight.Source.Skin.value().Influences[0].Bone = 2;
    EXPECT_FALSE( EncodeMeshSourceAsset( weight ).IsSuccess() );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
