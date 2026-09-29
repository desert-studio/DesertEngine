// MAT1b-2: a glTF written here goes through the source adapter and the shipped StaticMeshPBR template's Import
// rows; every key must reach its Property or slot of the .demat, and a material no template takes is refused.
#include <gtest/gtest.h>

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <stb_image/stb_image.h>
#include <stb_image/stb_image_write.h>

#include <Editor/Import/Assimp/EmbeddedSourceTexture.hpp>
#include <Editor/Import/Assimp/SourceMaterialAdapter.hpp>
#include <Editor/Import/Assimp/SourceTexturePath.hpp>
#include <Editor/Import/MaterialImportContract.hpp>
#include <Editor/Import/TextureChannelPack.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <iterator>
#include <map>
#include <cstring>
#include <format>
#include <fstream>
#include <optional>
#include <vector>
#include <sstream>

using namespace Desert::Editor;
namespace fs = std::filesystem;

namespace
{
    // 2x2 RGBA PNG with alpha (0, 255 / 255, 0); its colour channels are read back, not assumed.
    constexpr std::array<uint8_t, 76> kPng = {
         0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
         0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x08, 0x06, 0x00, 0x00, 0x00, 0x72, 0xb6, 0x0d,
         0x24, 0x00, 0x00, 0x00, 0x13, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0xd0, 0x38, 0xa1, 0xc1,
         0x00, 0xc4, 0xff, 0xc1, 0x04, 0x88, 0x03, 0x00, 0x38, 0x93, 0x06, 0x5f, 0x42, 0x8d, 0x50, 0x75,
         0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82 };

    std::string RepoFile( const std::string& relative )
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up, prefix += "../" )
            if ( const std::ifstream probe( prefix + relative ); probe )
            {
                std::stringstream text;
                text << probe.rdbuf();
                return text.str();
            }
        return {};
    }

    ImportTemplate Template( const std::string& file )
    {
        const auto read =
             ReadImportTemplate( RepoFile( std::format( "Editor/Resources/Shaders/Programs/{}", file ) ), file );
        EXPECT_TRUE( read.IsSuccess() ) << ( read.IsSuccess() ? "" : read.GetError() );
        return read.IsSuccess() ? read.GetValue() : ImportTemplate{};
    }

    // Images: base.png (albedo + mask), mr.png, occ.png (a DIFFERENT file: the ORM slot must be packed), nrm, emi.
    // @p samplers: the body of one glTF sampler object; when given, texture 0 (base.png) samples through it.
    fs::path WriteGltf( const std::string& caseName, const std::string& materialBody,
                        const std::string& extensionsUsed, const std::string& samplers = {} )
    {
        const fs::path  dir = fs::temp_directory_path() / "DesertMaterialImportAdapter" / caseName;
        std::error_code ec;
        fs::remove_all( dir, ec );
        fs::create_directories( dir );
        for ( const char* name : { "base.png", "mr.png", "occ.png", "nrm.png", "emi.png" } )
        {
            std::ofstream png( dir / name, std::ios::binary );
            for ( const uint8_t byte : kPng )
                png.put( static_cast<char>( byte ) );
        }
        const float       pos[12] = { -0.5f, 0, 0, 0.5f, 0, 0, 0.5f, 1, 0, -0.5f, 1, 0 };
        const float       uv[8]   = { 0, 1, 1, 1, 1, 0, 0, 0 };
        const uint16_t    idx[6]  = { 0, 1, 2, 0, 2, 3 };
        std::vector<char> bin( 92 );
        std::memcpy( bin.data(), pos, 48 );
        std::memcpy( bin.data() + 48, uv, 32 );
        std::memcpy( bin.data() + 80, idx, 12 );
        std::ofstream( dir / "m.bin", std::ios::binary ).write( bin.data(), 92 );
        std::ofstream( dir / "m.gltf" )
             << R"({ "asset": { "version": "2.0" }, "extensionsUsed": [ )" << extensionsUsed << R"( ],
  "scene": 0, "scenes": [ { "nodes": [ 0 ] } ], "nodes": [ { "mesh": 0 } ],
  "meshes": [ { "primitives": [ { "attributes": { "POSITION": 0, "TEXCOORD_0": 1 }, "indices": 2, "material": 0 } ] } ],
  "materials": [ { "name": "M", )"
             << materialBody << R"( } ],
  "samplers": [ )"
             << ( samplers.empty() ? std::string( "{}" ) : samplers ) << R"( ],
  "textures": [ { "source": 0)"
             << ( samplers.empty() ? "" : R"(, "sampler": 0)" )
             << R"( }, { "source": 1 }, { "source": 2 }, { "source": 3 }, { "source": 4 } ],
  "images": [ { "uri": "base.png" }, { "uri": "mr.png" }, { "uri": "occ.png" }, { "uri": "nrm.png" }, { "uri": "emi.png" } ],
  "buffers": [ { "uri": "m.bin", "byteLength": 92 } ],
  "bufferViews": [ { "buffer": 0, "byteOffset": 0, "byteLength": 48 }, { "buffer": 0, "byteOffset": 48, "byteLength": 32 },
                   { "buffer": 0, "byteOffset": 80, "byteLength": 12 } ],
  "accessors": [
    { "bufferView": 0, "componentType": 5126, "count": 4, "type": "VEC3", "min": [ -0.5, 0, 0 ], "max": [ 0.5, 1, 0 ] },
    { "bufferView": 1, "componentType": 5126, "count": 4, "type": "VEC2" },
    { "bufferView": 2, "componentType": 5123, "count": 6, "type": "SCALAR" } ] })";
        return dir / "m.gltf";
    }

    SourceMaterial Read( const fs::path& file, Assimp::Importer& importer )
    {
        const aiScene* scene = importer.ReadFile( file.string(), aiProcess_Triangulate );
        EXPECT_NE( scene, nullptr ) << importer.GetErrorString();
        if ( scene == nullptr )
            return {};
        const aiMaterial& mat = *scene->mMaterials[scene->mMeshes[0]->mMaterialIndex];
        return ReadSourceMaterial( mat, SourceFormatOf( file ), "M", [&]( const std::string& ref )
                                   { return FindSourceTexture( file.parent_path(), ref ); } )
             .Material;
    }

    const ImportedParam* Param( const TemplateFill& fill, const std::string& name )
    {
        for ( const auto& p : fill.Params )
            if ( p.Name == name )
                return &p;
        return nullptr;
    }
    const ImportedTextureSlot* Slot( const TemplateFill& fill, const std::string& name )
    {
        for ( const auto& s : fill.Textures )
            if ( s.Slot == name )
                return &s;
        return nullptr;
    }

    const std::string kFullMaterial = R"(
      "alphaMode": "MASK", "alphaCutoff": 0.3, "doubleSided": true,
      "pbrMetallicRoughness": { "baseColorFactor": [ 0.5, 0.25, 0.125, 1.0 ], "metallicFactor": 0.75, "roughnessFactor": 0.4,
        "baseColorTexture": { "index": 0, "extensions": { "KHR_texture_transform": { "offset": [ 0.25, 0.5 ], "scale": [ 2, 3 ], "rotation": 0.5 } } },
        "metallicRoughnessTexture": { "index": 1 } },
      "occlusionTexture": { "index": 2, "strength": 0.6 }, "normalTexture": { "index": 3, "scale": 1.5 },
      "emissiveTexture": { "index": 4 }, "emissiveFactor": [ 1.0, 0.5, 0.0 ],
      "extensions": { "KHR_materials_emissive_strength": { "emissiveStrength": 4.0 },
                      "KHR_materials_clearcoat": { "clearcoatFactor": 0.8 } })";
    const std::string kFullExtensions =
         R"("KHR_texture_transform", "KHR_materials_emissive_strength", "KHR_materials_clearcoat")";
} // namespace

TEST( MaterialImportAdapter, EveryGltfKeyReachesItsPropertyOrSlot )
{
    const fs::path       file = WriteGltf( "full", kFullMaterial, kFullExtensions );
    Assimp::Importer     importer;
    const SourceMaterial source = Read( file, importer );

    const std::vector<ImportTemplate> templates = { Template( "PBR/StandardSurface.shader" ),
                                                    Template( "Unlit/Unlit.shader" ) };
    const auto                        choice    = ChooseImportTemplate( source, templates, file.generic_string() );
    ASSERT_TRUE( choice.IsSuccess() ) << choice.GetError();
    ASSERT_EQ( templates[choice.GetValue()].ShaderName, "StandardSurface" );
    const TemplateFill fill = FillFromTemplate( source, templates[choice.GetValue()] );

    const auto expectParam = [&]( const char* name, glm::vec4 value, int components )
    {
        const ImportedParam* p = Param( fill, name );
        ASSERT_NE( p, nullptr ) << name << " was not filled";
        for ( int c = 0; c < components; ++c )
            EXPECT_NEAR( p->Value[c], value[c], 1e-4f ) << name << "[" << c << "]";
    };
    expectParam( "AlbedoColor", { 0.5f, 0.25f, 0.125f, 1.0f }, 4 );
    expectParam( "MetallicFactor", { 0.75f, 0, 0, 0 }, 1 );
    expectParam( "RoughnessFactor", { 0.4f, 0, 0, 0 }, 1 );
    expectParam( "OcclusionStrength", { 0.6f, 0, 0, 0 }, 1 );
    expectParam( "NormalScale", { 1.5f, 0, 0, 0 }, 1 );
    expectParam( "EmissiveColor", { 1.0f, 0.5f, 0.0f, 1.0f }, 4 );
    expectParam( "EmissiveIntensity", { 4.0f, 0, 0, 0 }, 1 );
    expectParam( "AlphaCutoff", { 0.3f, 0, 0, 0 }, 1 );
    expectParam( "OpacityChannel", { 3.0f, 0, 0, 0 }, 1 );
    expectParam( "UVTiling", { 2.0f, 3.0f, 0, 0 }, 2 );
    ASSERT_NE( Param( fill, "UVOffset" ), nullptr );
    ASSERT_NE( Param( fill, "UVRotation" ), nullptr );

    const auto expectSlot = [&]( const char* name, const char* image )
    {
        const ImportedTextureSlot* s = Slot( fill, name );
        ASSERT_NE( s, nullptr ) << name << " was not bound";
        EXPECT_EQ( s->Parts.front().Source.filename().string(), image ) << name;
    };
    expectSlot( "u_AlbedoTexture", "base.png" );
    expectSlot( "u_OpacityTexture", "base.png" ); // the mask is the albedo's alpha, OpacityChannel 3
    expectSlot( "u_NormalTexture", "nrm.png" );
    expectSlot( "u_EmissiveTexture", "emi.png" );

    // ORM: metallic-roughness (.gb) and occlusion (.r) are different images -> one packed image.
    const ImportedTextureSlot* orm = Slot( fill, "u_ORMTexture" );
    ASSERT_NE( orm, nullptr );
    ASSERT_EQ( orm->Parts.size(), 2u );
    EXPECT_TRUE( orm->NeedsPacking() );
    const fs::path packed = PackedTexturePath( *orm );
    EXPECT_EQ( packed.filename().string(), "mr+occ_ORMTexture.png" );
    const auto wrote = PackTextureChannels( *orm, packed );
    ASSERT_TRUE( wrote.IsSuccess() ) << wrote.GetError();

    // What the template does not read is named, not dropped silently.
    EXPECT_NE( std::ranges::find( fill.UnreadKeys, "gltf.KHR_materials_clearcoat" ), fill.UnreadKeys.end() );
    EXPECT_NE( std::ranges::find( fill.UnreadKeys, "gltf.doubleSided" ), fill.UnreadKeys.end() );
}

TEST( MaterialImportAdapter, AMetallicRoughnessImageAloneIsPackedWithWhiteOcclusion )
{
    const fs::path file =
         WriteGltf( "mr-only", R"("pbrMetallicRoughness": { "metallicRoughnessTexture": { "index": 1 } })", "" );
    Assimp::Importer   importer;
    const TemplateFill fill = FillFromTemplate( Read( file, importer ), Template( "PBR/StandardSurface.shader" ) );
    const ImportedTextureSlot* orm = Slot( fill, "u_ORMTexture" );
    ASSERT_NE( orm, nullptr );
    ASSERT_TRUE( orm->NeedsPacking() ) << "glTF's R of a metallic-roughness image is not occlusion";
    const fs::path packed = PackedTexturePath( *orm );
    ASSERT_TRUE( PackTextureChannels( *orm, packed ).IsSuccess() );
    int      w  = 0;
    int      h  = 0;
    int      n  = 0;
    uint8_t* px = stbi_load( packed.string().c_str(), &w, &h, &n, 4 );
    ASSERT_NE( px, nullptr );
    uint8_t* src = stbi_load( ( file.parent_path() / "mr.png" ).string().c_str(), &w, &h, &n, 4 );
    ASSERT_NE( src, nullptr );
    EXPECT_EQ( px[0], 255 );    // R: no occlusion source -> white, not the source's red
    EXPECT_NE( src[0], 255 );   // (which would have read as occlusion)
    EXPECT_EQ( px[1], src[1] ); // G: the source's green (roughness)
    EXPECT_EQ( px[2], src[2] ); // B: the source's blue (metal)
    stbi_image_free( px );
    stbi_image_free( src );
}

// THE PACKED ORM IS AN IMPORTED TEXTURE ASSET (lead decision, MAT1b-3): its name is a function of the sources,
// so the texture importer keeps its GUID (kept by path); two imports in a row leave the file untouched (bytes and
// mtime); a changed input rebuilds it.
TEST( MaterialImportAdapter, APackedImageIsRebuiltOnlyWhenAnInputChanges )
{
    const fs::path     file = WriteGltf( "pack-stable", kFullMaterial, kFullExtensions );
    Assimp::Importer   importer;
    const TemplateFill fill = FillFromTemplate( Read( file, importer ), Template( "PBR/StandardSurface.shader" ) );
    const ImportedTextureSlot* orm = Slot( fill, "u_ORMTexture" );
    ASSERT_NE( orm, nullptr );
    ASSERT_TRUE( orm->NeedsPacking() );
    const fs::path packed = PackedTexturePath( *orm );
    const auto     bytes  = [&]
    {
        std::ifstream in( packed, std::ios::binary );
        return std::string( std::istreambuf_iterator<char>( in ), std::istreambuf_iterator<char>() );
    };

    const auto first = PackTextureChannels( *orm, packed );
    ASSERT_TRUE( first.IsSuccess() ) << first.GetError();
    EXPECT_EQ( first.GetValue(), PackOutcome::Written );
    const std::string firstBytes = bytes();

    // Stamp the file into the past: a rewrite would move the stamp back to now.
    const auto stamp = fs::last_write_time( packed ) - std::chrono::hours( 1 );
    fs::last_write_time( packed, stamp );
    Assimp::Importer   again;
    const TemplateFill refill = FillFromTemplate( Read( file, again ), Template( "PBR/StandardSurface.shader" ) );
    ASSERT_NE( Slot( refill, "u_ORMTexture" ), nullptr );
    EXPECT_EQ( PackedTexturePath( *Slot( refill, "u_ORMTexture" ) ), packed ) << "same sources, same asset path";
    const auto second = PackTextureChannels( *orm, packed );
    ASSERT_TRUE( second.IsSuccess() ) << second.GetError();
    EXPECT_EQ( second.GetValue(), PackOutcome::Unchanged );
    EXPECT_EQ( fs::last_write_time( packed ), stamp ) << "unchanged inputs rewrote the packed image";
    EXPECT_EQ( bytes(), firstBytes );

    // One input changes (the occlusion image's red): the pack is rebuilt and differs.
    int      w   = 0;
    int      h   = 0;
    int      n   = 0;
    uint8_t* occ = stbi_load( ( file.parent_path() / "occ.png" ).string().c_str(), &w, &h, &n, 4 );
    ASSERT_NE( occ, nullptr );
    std::vector<uint8_t> changed( occ, occ + static_cast<std::size_t>( w * h * 4 ) );
    stbi_image_free( occ );
    for ( std::size_t i = 0; i < changed.size(); i += 4 )
        changed[i] = static_cast<uint8_t>( changed[i] + 1 );
    ASSERT_NE(
         stbi_write_png( ( file.parent_path() / "occ.png" ).string().c_str(), w, h, 4, changed.data(), w * 4 ),
         0 );
    const auto third = PackTextureChannels( *orm, packed );
    ASSERT_TRUE( third.IsSuccess() ) << third.GetError();
    EXPECT_EQ( third.GetValue(), PackOutcome::Written );
    EXPECT_NE( bytes(), firstBytes );
}

TEST( MaterialImportAdapter, AnUnlitMaterialTakesTheUnlitTemplate )
{
    const fs::path   file = WriteGltf( "unlit", R"("pbrMetallicRoughness": { "baseColorTexture": { "index": 0 } },
      "extensions": { "KHR_materials_unlit": {} })",
                                       R"("KHR_materials_unlit")" );
    Assimp::Importer importer;
    const std::vector<ImportTemplate> templates = { Template( "PBR/StandardSurface.shader" ),
                                                    Template( "Unlit/Unlit.shader" ) };
    const auto choice = ChooseImportTemplate( Read( file, importer ), templates, file.generic_string() );
    ASSERT_TRUE( choice.IsSuccess() ) << choice.GetError();
    EXPECT_EQ( templates[choice.GetValue()].ShaderName, "Unlit" );
}

TEST( MaterialImportAdapter, AMaterialNoTemplateTakesIsRefused )
{
    const fs::path file =
         WriteGltf( "clearcoat-only", R"("extensions": { "KHR_materials_clearcoat": { "clearcoatFactor": 1.0 } })",
                    R"("KHR_materials_clearcoat")" );
    Assimp::Importer importer;
    SourceMaterial   source = Read( file, importer );
    // assimp states PBR defaults for every glTF material; keep only what this file wrote.
    std::erase_if( source.Entries, []( const auto& e ) { return e.first != "gltf.KHR_materials_clearcoat"; } );
    ASSERT_EQ( source.Entries.size(), 1u );
    const std::vector<ImportTemplate> templates = { Template( "PBR/StandardSurface.shader" ),
                                                    Template( "Unlit/Unlit.shader" ) };
    const auto                        choice    = ChooseImportTemplate( source, templates, file.generic_string() );
    ASSERT_FALSE( choice.IsSuccess() );
    EXPECT_NE( choice.GetError().find( "'M'" ), std::string::npos ) << choice.GetError();
}

// MAT1b-4: an FBX PBR material as assimp's FBX converter states it (Maya Stingray PBS / 3ds Max Physical: maps in
// METALNESS, DIFFUSE_ROUGHNESS, AMBIENT_OCCLUSION, factors in METALLIC_FACTOR / ROUGHNESS_FACTOR). assimp cannot
// write those properties into an .fbx, so the aiMaterial is built here with the converter's own keys.
TEST( MaterialImportAdapter, EveryFbxPbrKeyReachesTheOrmTextureAndItsFactors )
{
    aiMaterial mat;
    const auto map = [&]( aiTextureType type, const char* file )
    {
        const aiString path( file );
        mat.AddProperty( &path, AI_MATKEY_TEXTURE( type, 0 ) );
    };
    const ai_real   metallic  = 0.7f;
    const ai_real   roughness = 0.35f;
    const aiColor4D diffuse( 0.5f, 0.25f, 0.125f, 1.0f );
    mat.AddProperty( &diffuse, 1, AI_MATKEY_COLOR_DIFFUSE );
    mat.AddProperty( &metallic, 1, AI_MATKEY_METALLIC_FACTOR );
    mat.AddProperty( &roughness, 1, AI_MATKEY_ROUGHNESS_FACTOR );
    map( aiTextureType_DIFFUSE, "albedo.png" );
    map( aiTextureType_METALNESS, "metal.png" );
    map( aiTextureType_DIFFUSE_ROUGHNESS, "rough.png" );
    map( aiTextureType_AMBIENT_OCCLUSION, "ao.png" );
    map( aiTextureType_SHININESS, "gloss.png" );

    const SourceMaterial source = ReadSourceMaterial( mat, SourceFormatOf( "helmet.fbx" ), "M",
                                                      []( const std::string& ref ) { return fs::path( ref ); } )
                                       .Material;
    const std::vector<ImportTemplate> templates = { Template( "PBR/StandardSurface.shader" ),
                                                    Template( "Unlit/Unlit.shader" ) };
    const auto                        choice    = ChooseImportTemplate( source, templates, "helmet.fbx" );
    ASSERT_TRUE( choice.IsSuccess() ) << choice.GetError();
    ASSERT_EQ( templates[choice.GetValue()].ShaderName, "StandardSurface" );
    const TemplateFill fill = FillFromTemplate( source, templates[choice.GetValue()] );

    const ImportedParam* metal = Param( fill, "MetallicFactor" );
    ASSERT_NE( metal, nullptr ) << "fbx metallic factor was not read";
    EXPECT_NEAR( metal->Value.x, 0.7f, 1e-5f );
    const ImportedParam* rough = Param( fill, "RoughnessFactor" );
    ASSERT_NE( rough, nullptr ) << "fbx roughness factor was not read";
    EXPECT_NEAR( rough->Value.x, 0.35f, 1e-5f );

    // Three grey maps from three images -> one packed ORM: AO in R, roughness in G, metal in B (the glTF layout).
    const ImportedTextureSlot* orm = Slot( fill, "u_ORMTexture" );
    ASSERT_NE( orm, nullptr );
    ASSERT_EQ( orm->Parts.size(), 3u );
    std::map<std::string, std::string> channelOf;
    for ( const auto& part : orm->Parts )
        channelOf[part.Source.filename().string()] = part.Channels;
    EXPECT_EQ( channelOf["ao.png"], "r" );
    EXPECT_EQ( channelOf["rough.png"], "g" );
    EXPECT_EQ( channelOf["metal.png"], "b" );
    EXPECT_TRUE( orm->NeedsPacking() );

    // A glossiness map has no taker: named as unread, not dropped.
    EXPECT_NE( std::ranges::find( fill.UnreadKeys, "fbx.GlossinessMap" ), fill.UnreadKeys.end() );
    for ( const char* read : { "fbx.Metalness", "fbx.Roughness", "fbx.AmbientOcclusion" } )
        EXPECT_EQ( std::ranges::find( fill.UnreadKeys, read ), fill.UnreadKeys.end() ) << read;
}

namespace
{
    // A .glb whose one image lives in its BIN chunk (bufferView 3, image/png), as DamagedHelmet.glb ships its
    // five.
    fs::path WriteGlbWithEmbeddedPng( const std::string& caseName )
    {
        const fs::path  dir = fs::temp_directory_path() / "DesertMaterialImportAdapter" / caseName;
        std::error_code ec;
        fs::remove_all( dir, ec );
        fs::create_directories( dir );
        const float       pos[12] = { -0.5f, 0, 0, 0.5f, 0, 0, 0.5f, 1, 0, -0.5f, 1, 0 };
        const float       uv[8]   = { 0, 1, 1, 1, 1, 0, 0, 0 };
        const uint16_t    idx[6]  = { 0, 1, 2, 0, 2, 3 };
        std::vector<char> bin( 92 );
        std::memcpy( bin.data(), pos, 48 );
        std::memcpy( bin.data() + 48, uv, 32 );
        std::memcpy( bin.data() + 80, idx, 12 );
        bin.insert( bin.end(), kPng.begin(), kPng.end() );
        while ( bin.size() % 4 != 0 )
            bin.push_back( 0 );
        std::string json = std::format( R"({{ "asset": {{ "version": "2.0" }},
  "scene": 0, "scenes": [ {{ "nodes": [ 0 ] }} ], "nodes": [ {{ "mesh": 0 }} ],
  "meshes": [ {{ "primitives": [ {{ "attributes": {{ "POSITION": 0, "TEXCOORD_0": 1 }}, "indices": 2, "material": 0 }} ] }} ],
  "materials": [ {{ "name": "M", "pbrMetallicRoughness": {{ "baseColorTexture": {{ "index": 0 }} }} }} ],
  "textures": [ {{ "source": 0 }} ], "images": [ {{ "bufferView": 3, "mimeType": "image/png" }} ],
  "buffers": [ {{ "byteLength": {} }} ],
  "bufferViews": [ {{ "buffer": 0, "byteOffset": 0, "byteLength": 48 }}, {{ "buffer": 0, "byteOffset": 48, "byteLength": 32 }},
                   {{ "buffer": 0, "byteOffset": 80, "byteLength": 12 }}, {{ "buffer": 0, "byteOffset": 92, "byteLength": {} }} ],
  "accessors": [
    {{ "bufferView": 0, "componentType": 5126, "count": 4, "type": "VEC3", "min": [ -0.5, 0, 0 ], "max": [ 0.5, 1, 0 ] }},
    {{ "bufferView": 1, "componentType": 5126, "count": 4, "type": "VEC2" }},
    {{ "bufferView": 2, "componentType": 5123, "count": 6, "type": "SCALAR" }} ] }})",
                                        bin.size(), kPng.size() );
        while ( json.size() % 4 != 0 )
            json.push_back( ' ' );
        const auto u32 = []( std::ofstream& out, uint32_t v )
        {
            const auto bytes = std::bit_cast<std::array<char, 4>>( v );
            out.write( bytes.data(), bytes.size() );
        };
        const fs::path file = dir / "helmet.glb";
        std::ofstream  out( file, std::ios::binary );
        out.write( "glTF", 4 );
        u32( out, 2 );
        u32( out, static_cast<uint32_t>( 12 + 8 + json.size() + 8 + bin.size() ) );
        u32( out, static_cast<uint32_t>( json.size() ) );
        u32( out, 0x4E4F534Au ); // JSON
        out.write( json.data(), static_cast<std::streamsize>( json.size() ) );
        u32( out, static_cast<uint32_t>( bin.size() ) );
        u32( out, 0x004E4942u ); // BIN
        out.write( bin.data(), static_cast<std::streamsize>( bin.size() ) );
        return file;
    }

    // Reads the .glb as AssimpImporter does: every reference resolved by ResolveSourceTexture.
    SourceMaterial ReadResolving( const fs::path& file, Assimp::Importer& importer,
                                  std::vector<std::optional<PackOutcome>>& extracted )
    {
        const aiScene* scene = importer.ReadFile( file.string(), aiProcess_Triangulate );
        EXPECT_NE( scene, nullptr ) << importer.GetErrorString();
        if ( scene == nullptr )
            return {};
        const aiMaterial& mat = *scene->mMaterials[scene->mMeshes[0]->mMaterialIndex];
        return ReadSourceMaterial( mat, SourceFormatOf( file ), "M",
                                   [&]( const std::string& ref )
                                   {
                                       const auto resolved = ResolveSourceTexture( *scene, file, ref );
                                       EXPECT_TRUE( resolved.IsSuccess() )
                                            << ( resolved.IsSuccess() ? "" : resolved.GetError() );
                                       if ( !resolved.IsSuccess() )
                                           return fs::path{};
                                       extracted.push_back( resolved.GetValue().Extracted );
                                       return resolved.GetValue().Path;
                                   } )
             .Material;
    }
} // namespace

TEST( MaterialImportAdapter, AnEmbeddedTextureIsDerivedBesideTheSourceAndRewrittenOnlyWhenItChanges )
{
    const fs::path                          file    = WriteGlbWithEmbeddedPng( "embedded" );
    const fs::path                          derived = file.parent_path() / "helmet_0.png";
    std::vector<std::optional<PackOutcome>> extracted;
    Assimp::Importer                        importer;
    const TemplateFill                      fill =
         FillFromTemplate( ReadResolving( file, importer, extracted ), Template( "PBR/StaticMeshPBR.shader" ) );

    const ImportedTextureSlot* albedo = Slot( fill, "u_AlbedoTexture" );
    ASSERT_NE( albedo, nullptr ) << "the embedded base colour did not reach its slot";
    ASSERT_FALSE( albedo->Parts.empty() );
    EXPECT_EQ( albedo->Parts.front().Source, derived );
    ASSERT_FALSE( extracted.empty() );
    EXPECT_EQ( extracted.front(), PackOutcome::Written );
    const auto bytes = [&]
    {
        std::ifstream in( derived, std::ios::binary );
        return std::string( std::istreambuf_iterator<char>( in ), std::istreambuf_iterator<char>() );
    };
    EXPECT_EQ( bytes(), std::string( kPng.begin(), kPng.end() ) )
         << "a compressed embedded image is kept byte for byte";

    // A second import of the same source names the same file and leaves it alone.
    const auto stamp = fs::last_write_time( derived ) - std::chrono::hours( 1 );
    fs::last_write_time( derived, stamp );
    extracted.clear();
    Assimp::Importer   again;
    const TemplateFill refill =
         FillFromTemplate( ReadResolving( file, again, extracted ), Template( "PBR/StaticMeshPBR.shader" ) );
    ASSERT_NE( Slot( refill, "u_AlbedoTexture" ), nullptr );
    EXPECT_EQ( Slot( refill, "u_AlbedoTexture" )->Parts.front().Source, derived );
    ASSERT_FALSE( extracted.empty() );
    EXPECT_EQ( extracted.front(), PackOutcome::Unchanged );
    EXPECT_EQ( fs::last_write_time( derived ), stamp ) << "an unchanged embedded texture was rewritten";
}

TEST( MaterialImportAdapter, AnUncompressedEmbeddedTextureIsEncodedToPng )
{
    aiTexel   texels[2] = { { 10, 20, 30, 255 }, { 40, 50, 60, 128 } }; // b, g, r, a
    aiTexture texture;
    texture.mWidth    = 2;
    texture.mHeight   = 1;
    texture.pcData    = texels;
    aiTexture* list[] = { &texture };
    aiScene    scene;
    scene.mNumTextures = 1;
    scene.mTextures    = list;

    const fs::path  dir = fs::temp_directory_path() / "DesertMaterialImportAdapter" / "embedded-raw";
    std::error_code ec;
    fs::remove_all( dir, ec );
    fs::create_directories( dir );
    const auto resolved = ResolveSourceTexture( scene, dir / "chair.fbx", "*0" );
    scene.mTextures     = nullptr; // the scene does not own them
    scene.mNumTextures  = 0;
    texture.pcData      = nullptr;
    ASSERT_TRUE( resolved.IsSuccess() ) << resolved.GetError();
    EXPECT_EQ( resolved.GetValue().Path, dir / "chair_0.png" );
    int      w    = 0;
    int      h    = 0;
    int      n    = 0;
    uint8_t* rgba = stbi_load( ( dir / "chair_0.png" ).string().c_str(), &w, &h, &n, 4 );
    ASSERT_NE( rgba, nullptr );
    EXPECT_EQ( w, 2 );
    EXPECT_EQ( h, 1 );
    const std::vector<uint8_t> got( rgba, rgba + 8 );
    stbi_image_free( rgba );
    EXPECT_EQ( got, ( std::vector<uint8_t>{ 30, 20, 10, 255, 60, 50, 40, 128 } ) );
}

TEST( MaterialImportAdapter, AnFbxBaseColorMapIsTheAlbedoWhenNoDiffuseIsStated )
{
    const auto read = []( bool withDiffuse )
    {
        aiMaterial     mat;
        const aiString base( "base.png" );
        const aiString diffuse( "diffuse.png" );
        mat.AddProperty( &base, AI_MATKEY_TEXTURE( aiTextureType_BASE_COLOR, 0 ) );
        if ( withDiffuse )
            mat.AddProperty( &diffuse, AI_MATKEY_TEXTURE( aiTextureType_DIFFUSE, 0 ) );
        return FillFromTemplate( ReadSourceMaterial( mat, SourceFormatOf( "chair.fbx" ), "M",
                                                     []( const std::string& ref ) { return fs::path( ref ); } )
                                      .Material,
                                 Template( "PBR/StaticMeshPBR.shader" ) );
    };
    const TemplateFill onlyBase = read( false );
    ASSERT_NE( Slot( onlyBase, "u_AlbedoTexture" ), nullptr ) << "an FBX base_color_map was dropped";
    EXPECT_EQ( Slot( onlyBase, "u_AlbedoTexture" )->Parts.front().Source, fs::path( "base.png" ) );
    const TemplateFill both = read( true );
    ASSERT_NE( Slot( both, "u_AlbedoTexture" ), nullptr );
    ASSERT_EQ( Slot( both, "u_AlbedoTexture" )->Parts.size(), 1u );
    EXPECT_EQ( Slot( both, "u_AlbedoTexture" )->Parts.front().Source, fs::path( "diffuse.png" ) );
}

// MAT1s: the glTF sampler of a texture (wrapS/wrapT/magFilter) rides on its key to the template's slot, and a
// texture with no sampler states none (the engine default is not written into every imported .demat).
TEST( MaterialImportAdapter, AGltfSamplerReachesItsSlotAndADefaultOneStatesNothing )
{
    const fs::path       file = WriteGltf( "sampler", kFullMaterial, kFullExtensions,
                                           R"({ "wrapS": 33071, "wrapT": 33648, "magFilter": 9728 })" );
    Assimp::Importer     importer;
    const SourceMaterial source = Read( file, importer );

    using Desert::Core::Formats::SamplerFilter;
    using Desert::Core::Formats::SamplerState;
    using Desert::Core::Formats::SamplerWrap;
    const SamplerState expected{ SamplerWrap::Clamp, SamplerWrap::Mirror, SamplerFilter::Nearest };

    const auto base = source.Entries.find( "gltf.baseColorTexture" );
    ASSERT_NE( base, source.Entries.end() );
    ASSERT_TRUE( base->second.Sampler.has_value() ) << "the source sampler was not read";
    EXPECT_EQ( base->second.Sampler, std::optional<SamplerState>( expected ) );

    const std::vector<ImportTemplate> templates = { Template( "PBR/StaticMeshPBR.shader" ) };
    const TemplateFill                fill      = FillFromTemplate( source, templates[0] );
    const ImportedTextureSlot*        albedo    = Slot( fill, "u_AlbedoTexture" );
    ASSERT_NE( albedo, nullptr );
    ASSERT_TRUE( albedo->Sampler.has_value() ) << "the sampler did not reach the slot";
    EXPECT_EQ( albedo->Sampler, std::optional<SamplerState>( expected ) );

    const ImportedTextureSlot* normal = Slot( fill, "u_NormalTexture" );
    ASSERT_NE( normal, nullptr );
    EXPECT_FALSE( normal->Sampler.has_value() ) << "a texture with no glTF sampler states the default: nothing";
}
