// THM1a: a glTF `alphaMode: MASK` keeps its mask in the base colour's ALPHA and its threshold in the
// material. The importer used to switch the cut-out on only for a separate opacity map (FBX), so a glTF
// foliage card imported with AlphaCutoff 0 and drew as a solid square. The fixture is written by the test
// itself into a temporary folder - one quad, a 2x2 RGBA texture whose alpha is 0/255 - and parsed by the
// linked assimp exactly as the importer parses it.
#include <gtest/gtest.h>

#include <assimp/Importer.hpp>
#include <assimp/material.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <Editor/Import/Assimp/SourceAlphaMode.hpp>
#include <Editor/Import/Assimp/SourceTexturePath.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using Desert::Editor::ResolveSourceAlpha;
using Desert::Editor::SourceAlpha;
using Desert::Editor::SourceAlphaKind;

namespace
{
    // 2x2 RGBA PNG, green; row 0 alpha (0, 255), row 1 alpha (255, 0).
    constexpr std::array<uint8_t, 76> kCardPng = {
         0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
         0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x08, 0x06, 0x00, 0x00, 0x00, 0x72, 0xb6, 0x0d,
         0x24, 0x00, 0x00, 0x00, 0x13, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0xd0, 0x38, 0xa1, 0xc1,
         0x00, 0xc4, 0xff, 0xc1, 0x04, 0x88, 0x03, 0x00, 0x38, 0x93, 0x06, 0x5f, 0x42, 0x8d, 0x50, 0x75,
         0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82 };

    // 2x2 RGB PNG (colour type 2, no alpha channel): what a JPG base colour is to the mask - nothing.
    constexpr std::array<uint8_t, 73> kRgbCardPng = {
         0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44,
         0x52, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x08, 0x02, 0x00, 0x00, 0x00, 0xfd,
         0xd4, 0x9a, 0x73, 0x00, 0x00, 0x00, 0x10, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0xd0,
         0x58, 0xa0, 0x01, 0x44, 0x0c, 0x10, 0x0a, 0x00, 0x1a, 0x4e, 0x03, 0xc1, 0x04, 0xba, 0xcb,
         0x0c, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82 };

    std::filesystem::path WriteCard( const std::string& caseName, const std::string& alphaStatement )
    {
        namespace fs        = std::filesystem;
        const fs::path  dir = fs::temp_directory_path() / "DesertGltfAlphaMask" / caseName;
        std::error_code ec;
        fs::remove_all( dir, ec );
        fs::create_directories( dir );

        // One 100 x 100 cm card: 4 positions, 4 UVs, 6 indices = 48 + 32 + 12 = 92 bytes.
        const float       pos[12] = { -0.5f, 0, 0, 0.5f, 0, 0, 0.5f, 1, 0, -0.5f, 1, 0 };
        const float       uv[8]   = { 0, 1, 1, 1, 1, 0, 0, 0 };
        const uint16_t    idx[6]  = { 0, 1, 2, 0, 2, 3 };
        std::vector<char> bin( 92 );
        std::memcpy( bin.data(), pos, 48 );
        std::memcpy( bin.data() + 48, uv, 32 );
        std::memcpy( bin.data() + 80, idx, 12 );
        std::ofstream( dir / "card.bin", std::ios::binary )
             .write( bin.data(), static_cast<std::streamsize>( bin.size() ) );
        std::ofstream( dir / "card.png", std::ios::binary )
             .write( reinterpret_cast<const char*>( kCardPng.data() ),
                     static_cast<std::streamsize>( kCardPng.size() ) );

        const std::string gltf = R"({
  "asset": { "version": "2.0" },
  "scene": 0, "scenes": [ { "nodes": [ 0 ] } ], "nodes": [ { "mesh": 0 } ],
  "meshes": [ { "primitives": [ { "attributes": { "POSITION": 0, "TEXCOORD_0": 1 }, "indices": 2, "material": 0 } ] } ],
  "materials": [ { "name": "Card", )" +
                                 alphaStatement +
                                 R"( "pbrMetallicRoughness": { "baseColorTexture": { "index": 0 } } } ],
  "textures": [ { "source": 0 } ], "images": [ { "uri": "card.png" } ],
  "buffers": [ { "uri": "card.bin", "byteLength": 92 } ],
  "bufferViews": [ { "buffer": 0, "byteOffset": 0, "byteLength": 48 }, { "buffer": 0, "byteOffset": 48, "byteLength": 32 },
                   { "buffer": 0, "byteOffset": 80, "byteLength": 12 } ],
  "accessors": [
    { "bufferView": 0, "componentType": 5126, "count": 4, "type": "VEC3", "min": [ -0.5, 0, 0 ], "max": [ 0.5, 1, 0 ] },
    { "bufferView": 1, "componentType": 5126, "count": 4, "type": "VEC2" },
    { "bufferView": 2, "componentType": 5123, "count": 6, "type": "SCALAR" } ]
})";
        std::ofstream( dir / "card.gltf" ) << gltf;
        return dir / "card.gltf";
    }

    struct Imported
    {
        Assimp::Importer Importer;
        const aiScene*   Scene = nullptr;
        // assimp's glTF reader appends a default material of its own; the card's is the one its mesh names.
        const aiMaterial& Material() const
        {
            return *Scene->mMaterials[Scene->mMeshes[0]->mMaterialIndex];
        }
    };

    void Import( Imported& into, const std::filesystem::path& file )
    {
        into.Scene = into.Importer.ReadFile( file.string(), aiProcess_Triangulate );
        ASSERT_NE( into.Scene, nullptr ) << into.Importer.GetErrorString();
        ASSERT_EQ( into.Scene->mNumMeshes, 1u );
        ASSERT_LT( into.Scene->mMeshes[0]->mMaterialIndex, into.Scene->mNumMaterials );
    }
} // namespace

TEST( GltfAlphaMask, AMaskCardTakesTheCutoffTheFileStates )
{
    Imported card;
    Import( card, WriteCard( "mask", R"("alphaMode": "MASK", "alphaCutoff": 0.3,)" ) );
    if ( HasFatalFailure() )
        return;

    const SourceAlpha alpha = ResolveSourceAlpha( card.Material() );
    EXPECT_EQ( alpha.Kind, SourceAlphaKind::Mask );
    EXPECT_FLOAT_EQ( alpha.AlphaCutoff, 0.3f ) << "AlphaCutoff 0 is the solid-square defect";

    // The mask is the base colour's alpha: the card names no opacity map, only the albedo the importer
    // loads into u_AlbedoTexture (PBRSurfaceParams::MaskTexture then picks the albedo).
    EXPECT_EQ( card.Material().GetTextureCount( aiTextureType_OPACITY ), 0u );
    EXPECT_EQ( card.Material().GetTextureCount( aiTextureType_DIFFUSE ), 1u );
}

TEST( GltfAlphaMask, AMaskWithoutACutoffTakesTheGltfDefault )
{
    Imported card;
    Import( card, WriteCard( "mask-default", R"("alphaMode": "MASK",)" ) );
    if ( HasFatalFailure() )
        return;

    const SourceAlpha alpha = ResolveSourceAlpha( card.Material() );
    EXPECT_EQ( alpha.Kind, SourceAlphaKind::Mask );
    EXPECT_FLOAT_EQ( alpha.AlphaCutoff, Desert::Editor::kGltfDefaultAlphaCutoff );
}

TEST( GltfAlphaMask, AnOpaqueCardIsNotCut )
{
    for ( const char* statement : { R"("alphaMode": "OPAQUE",)", "" } )
    {
        Imported card;
        Import( card, WriteCard( "opaque", statement ) );
        if ( HasFatalFailure() )
            return;

        const SourceAlpha alpha = ResolveSourceAlpha( card.Material() );
        EXPECT_EQ( alpha.Kind, SourceAlphaKind::Opaque ) << statement;
        EXPECT_EQ( alpha.AlphaCutoff, 0.0f ) << statement;
    }
}

TEST( GltfAlphaMask, ABlendCardIsDrawnMaskedAndSaysSo )
{
    Imported card;
    Import( card, WriteCard( "blend", R"("alphaMode": "BLEND",)" ) );
    if ( HasFatalFailure() )
        return;

    const SourceAlpha alpha = ResolveSourceAlpha( card.Material() );
    EXPECT_EQ( alpha.Kind, SourceAlphaKind::BlendAsMask );
    EXPECT_EQ( alpha.AlphaMode, "BLEND" );
    EXPECT_FLOAT_EQ( alpha.AlphaCutoff, Desert::Editor::kGltfDefaultAlphaCutoff );
}

TEST( GltfAlphaMask, AMaskOverAnRgbaBaseColourStaysMasked )
{
    Imported                    card;
    const std::filesystem::path file = WriteCard( "mask-rgba", R"("alphaMode": "MASK",)" );
    Import( card, file );
    if ( HasFatalFailure() )
        return;

    const SourceAlpha alpha = ResolveSourceAlpha( card.Material(), file.parent_path() / "card.png" );
    EXPECT_EQ( alpha.Kind, SourceAlphaKind::Mask );
    EXPECT_TRUE( alpha.Warning.empty() ) << alpha.Warning;
}

// Poly Haven's glTF grass: alphaMode BLEND over a JPG base colour. The file states a cut-out it cannot carry.
TEST( GltfAlphaMask, ACutOutOverABaseColourWithoutAlphaIsImportedOpaqueAndSaysSo )
{
    for ( const char* mode : { "MASK", "BLEND" } )
    {
        SCOPED_TRACE( mode );
        Imported                    card;
        const std::filesystem::path file =
             WriteCard( std::string( "rgb-" ) + mode, std::string( R"("alphaMode": ")" ) + mode + R"(",)" );
        const std::filesystem::path png = file.parent_path() / "card.png";
        std::ofstream( png, std::ios::binary | std::ios::trunc )
             .write( reinterpret_cast<const char*>( kRgbCardPng.data() ),
                     static_cast<std::streamsize>( kRgbCardPng.size() ) );
        Import( card, file );
        if ( HasFatalFailure() )
            return;

        const SourceAlpha alpha = ResolveSourceAlpha( card.Material(), png );
        EXPECT_EQ( alpha.Kind, SourceAlphaKind::Opaque );
        EXPECT_FLOAT_EQ( alpha.AlphaCutoff, 0.0f );
        EXPECT_EQ( alpha.AlphaMode, mode );
        EXPECT_NE( alpha.Warning.find( "'Card'" ), std::string::npos ) << alpha.Warning;
        EXPECT_NE( alpha.Warning.find( "'card.png'" ), std::string::npos ) << alpha.Warning;
        EXPECT_NE( alpha.Warning.find( "no alpha channel" ), std::string::npos ) << alpha.Warning;
    }
}

// Poly Haven's FBX states its textures Windows-style ("..\\..\\textures\\x.jpg"); on POSIX that was ONE
// filename, so all three textures of the grass went NOT FOUND and the card imported without albedo or mask.
TEST( GltfAlphaMask, AWindowsSeparatedTextureReferenceFindsTheFile )
{
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "GltfAlphaMask_TexRef";
    fs::remove_all( root );
    fs::create_directories( root / "a" / "b" / "mesh" );
    fs::create_directories( root / "a" / "textures" );
    std::ofstream( root / "a" / "textures" / "x.jpg" ) << "x";

    const fs::path base  = root / "a" / "b" / "mesh";
    const fs::path found = Desert::Editor::FindSourceTexture( base, "..\\..\\textures\\x.jpg" );
    EXPECT_EQ( found.lexically_normal(), ( root / "a" / "textures" / "x.jpg" ).lexically_normal() );
    fs::remove_all( root );
}

TEST( GltfAlphaMask, AGenericTextureReferenceIsNotChanged )
{
    EXPECT_EQ( Desert::Editor::NormalizeTextureReference( "a/b.png" ), std::filesystem::path( "a/b.png" ) );
    EXPECT_EQ( Desert::Editor::NormalizeTextureReference( "..\\t\\x.jpg" ), std::filesystem::path( "../t/x.jpg" ) );
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
