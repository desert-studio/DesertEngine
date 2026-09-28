// GR-2. The meadow's grass blade and its root-to-tip gradient are AUTHORED here, as recipes, and the checked-in
// assets are what the recipes produce: the mesh through the modeling path (ShapeMesh -> ShapeToEditMesh ->
// MeshSourceAsset, what a Create-shape "Output: Static Mesh" writes) and the gradient through the texture asset
// (PNG source bytes inside a `.detex`). A recipe edit without the asset (or the reverse) is red; the failure
// writes the asset the recipe produces, with the checked-in GUID, to the temp directory and names the copy
// command, so the asset is never edited by hand.
//
// UE pattern: a Landscape Grass Type's mesh is an ordinary static mesh with an ordinary two-sided foliage
// material; nothing about it is special to grass. Two-sidedness is authored as geometry here (a back sheet
// wound the other way) because this engine's PBR pipelines have no per-material cull mode, and a back sheet
// lights its own side with its own normal instead of a flipped one.

#include "../../TestSupport/scratch_dir.hpp"

#include <Common/Content/TextAssetHeader.hpp>
#include <Engine/Assets/MeshSourceAsset.hpp>
#include <Engine/Assets/TextureSourceAsset.hpp>
#include <Engine/Geometry/EditMeshAsset.hpp>
#include <Engine/Geometry/EditMeshSerialization.hpp>
#include <Engine/Geometry/ShapeGenerators.hpp>

// The writer's implementation stays private to this file: the engine library compiles its own copy.
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image/stb_image_write.h>

#include <gtest/gtest.h>
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace Desert;

namespace
{
    constexpr const char* kBladePath    = "Editor/Resources/Assets/Landscape/Grass/GrassBlade.stmesh";
    constexpr const char* kGradientPath = "Editor/Resources/Assets/Textures/T_GrassBlade.detex";
    constexpr const char* kMaterialPath = "Editor/Resources/Assets/Materials/M_GrassBlade.demat";

    // ── The clump ────────────────────────────────────────────────────────────────────────────────────────
    // Five blades around the pivot, each a tapered strip bent away from the centre. cm, +Y up, root at y = 0.
    struct BladeSpec
    {
        float YawDegrees; // the direction the blade leans towards, about +Y from +Z
        float Height;     // cm
        float Lean;       // cm the tip is displaced along the yaw, growing with height squared
        float Offset;     // cm from the pivot to the root
    };
    constexpr std::array<BladeSpec, 5> kBlades    = { { { 0.0f, 42.0f, 10.0f, 1.2f },
                                                        { 73.0f, 34.0f, 7.0f, 1.6f },
                                                        { 151.0f, 46.0f, 12.0f, 1.0f },
                                                        { 214.0f, 30.0f, 6.0f, 1.8f },
                                                        { 289.0f, 38.0f, 9.0f, 1.4f } } };
    constexpr int                      kSegments  = 4;    // rows of quads; the last row closes on the tip
    constexpr float                    kRootWidth = 1.6f; // cm across the root
    constexpr float kSheetGap = 0.04f; // cm between the front and back sheets, so the weld keeps them apart

    // The engine's own winding convention, read from its own generator rather than assumed: +1 when a
    // triangle's (b - a) x (c - a) points along its outward normal.
    float FrontWindingSign()
    {
        const Geometry::ShapeMesh rect = Geometry::MakeRectangle( {} );
        const Desert::Index&      t    = rect.Indices.front();
        const Desert::Vertex&     a    = rect.Vertices[t.V1];
        const glm::vec3           n =
             glm::cross( rect.Vertices[t.V2].Position - a.Position, rect.Vertices[t.V3].Position - a.Position );
        return glm::dot( n, a.Normal ) >= 0.0f ? 1.0f : -1.0f;
    }

    Geometry::ShapeMesh GrassClump()
    {
        const float         winding = FrontWindingSign();
        Geometry::ShapeMesh shape;
        int                 group = 0;
        for ( const BladeSpec& blade : kBlades )
        {
            const float     yaw = glm::radians( blade.YawDegrees );
            const glm::vec3 lean( std::sin( yaw ), 0.0f, std::cos( yaw ) );
            const glm::vec3 side( std::cos( yaw ), 0.0f, -std::sin( yaw ) );
            const glm::vec3 root = lean * blade.Offset;

            for ( const float sheet : { 1.0f, -1.0f } )
            {
                const auto first = static_cast<uint32_t>( shape.Vertices.size() );
                for ( int row = 0; row <= kSegments; ++row )
                {
                    const float     t = static_cast<float>( row ) / static_cast<float>( kSegments );
                    const glm::vec3 centre =
                         root + lean * ( blade.Lean * t * t ) + glm::vec3( 0.0f, blade.Height * t, 0.0f );
                    const glm::vec3 up     = glm::normalize( lean * ( 2.0f * blade.Lean * t ) +
                                                             glm::vec3( 0.0f, blade.Height, 0.0f ) );
                    const glm::vec3 normal = sheet * glm::normalize( glm::cross( side, up ) );
                    const glm::vec3 at     = centre + normal * ( 0.5f * kSheetGap );
                    const float     half   = 0.5f * kRootWidth * std::pow( 1.0f - t, 0.7f );
                    if ( row == kSegments )
                    {
                        shape.Vertices.push_back( { at, normal, side, up, glm::vec2( 0.5f, t ) } );
                        break;
                    }
                    shape.Vertices.push_back( { at - side * half, normal, side, up, glm::vec2( 0.0f, t ) } );
                    shape.Vertices.push_back( { at + side * half, normal, side, up, glm::vec2( 1.0f, t ) } );
                }
                const auto emit = [&]( uint32_t a, uint32_t b, uint32_t c )
                {
                    const glm::vec3 n = glm::cross( shape.Vertices[b].Position - shape.Vertices[a].Position,
                                                    shape.Vertices[c].Position - shape.Vertices[a].Position );
                    if ( winding * glm::dot( n, shape.Vertices[a].Normal ) < 0.0f )
                        std::swap( b, c );
                    shape.Indices.push_back( { a, b, c } );
                    shape.Groups.push_back( group );
                };
                for ( int row = 0; row < kSegments; ++row )
                {
                    const uint32_t l0 = first + static_cast<uint32_t>( 2 * row );
                    const uint32_t r0 = l0 + 1;
                    if ( row == kSegments - 1 )
                    {
                        emit( l0, r0, l0 + 2 ); // the tip
                        continue;
                    }
                    emit( l0, r0, l0 + 2 );
                    emit( r0, r0 + 2, l0 + 2 );
                }
                ++group;
            }
        }
        return shape;
    }

    // ── The gradient ─────────────────────────────────────────────────────────────────────────────────────
    // 4 x 64 sRGB, image row 0 = the TIP (the mesh shaders sample v flipped: row 0 is v = 1).
    constexpr int kGradientWidth  = 4;
    constexpr int kGradientHeight = 64;

    std::vector<std::byte> GradientPng()
    {
        struct Stop
        {
            float     At; // 0 = root, 1 = tip
            glm::vec3 Srgb;
        };
        constexpr std::array<Stop, 3> stops = { { { 0.0f, { 34.0f, 58.0f, 16.0f } },
                                                  { 0.45f, { 84.0f, 128.0f, 36.0f } },
                                                  { 1.0f, { 178.0f, 188.0f, 96.0f } } } };
        std::vector<unsigned char>    rgb( static_cast<size_t>( kGradientWidth * kGradientHeight * 3 ) );
        for ( int y = 0; y < kGradientHeight; ++y )
        {
            const float tip = 1.0f - static_cast<float>( y ) / static_cast<float>( kGradientHeight - 1 );
            size_t      s   = 0;
            while ( s + 2 < stops.size() && tip > stops[s + 1].At )
                ++s;
            const float f = glm::clamp( ( tip - stops[s].At ) / ( stops[s + 1].At - stops[s].At ), 0.0f, 1.0f );
            const glm::vec3 c = glm::mix( stops[s].Srgb, stops[s + 1].Srgb, f );
            for ( int x = 0; x < kGradientWidth; ++x )
                for ( int k = 0; k < 3; ++k )
                    rgb[static_cast<size_t>( ( y * kGradientWidth + x ) * 3 + k )] =
                         static_cast<unsigned char>( std::lround( c[k] ) );
        }
        std::vector<std::byte> png;
        stbi_write_png_to_func(
             []( void* context, void* data, int size )
             {
                 auto*       out   = static_cast<std::vector<std::byte>*>( context );
                 const auto* bytes = static_cast<const std::byte*>( data );
                 out->insert( out->end(), bytes, bytes + size );
             },
             &png, kGradientWidth, kGradientHeight, 3, rgb.data(), kGradientWidth * 3 );
        return png;
    }

    std::string ReadText( const fs::path& path )
    {
        std::ifstream in( path, std::ios::binary );
        return { std::istreambuf_iterator<char>( in ), std::istreambuf_iterator<char>() };
    }

    // The header GUID of a text asset: the first "Guid" in the file is the header's.
    Common::Content::AssetGuid HeaderGuidOf( const std::string& text )
    {
        const std::string key = "\"Guid\": \"";
        const size_t      at  = text.find( key );
        EXPECT_NE( at, std::string::npos );
        if ( at == std::string::npos )
            return {};
        auto guid = Common::Content::AssetGuidFromText( std::string_view( text ).substr( at + key.size(), 32 ) );
        EXPECT_TRUE( guid.IsSuccess() ) << guid.GetError();
        return guid.IsSuccess() ? guid.GetValue() : Common::Content::AssetGuid{};
    }

    fs::path AdoptionPath( const char* name )
    {
        const fs::path dir = fs::temp_directory_path() / "GrassBladeAsset";
        fs::create_directories( dir );
        return dir / name;
    }

    fs::path Root()
    {
        const fs::path root = TestSupport::RepositoryRoot();
        EXPECT_FALSE( root.empty() ) << "could not locate the repository root from the working directory";
        return root;
    }
} // namespace

TEST( GrassBladeAsset, TheClumpWeldsIntoAnEditMeshTheStaticMeshPathAccepts )
{
    auto mesh = Geometry::ShapeToEditMesh( GrassClump() );
    ASSERT_TRUE( mesh.IsSuccess() ) << mesh.GetError();
    const std::array<Common::Content::AssetGuid, 1> slot   = { Common::Content::AssetGuid{ 1, 2 } };
    auto                                            render = Geometry::ToMeshAssetData( mesh.GetValue(), slot );
    ASSERT_TRUE( render.IsSuccess() ) << render.GetError();
    // 5 blades x 2 sheets x (2 x 3 quads' triangles + the tip).
    EXPECT_EQ( GrassClump().Indices.size(), kBlades.size() * 2u * ( 2u * ( kSegments - 1 ) + 1u ) );
}

TEST( GrassBladeAsset, EveryBladeIsTwoSidedAndStandsOnItsRoot )
{
    const Geometry::ShapeMesh clump   = GrassClump();
    const float               winding = FrontWindingSign();
    float                     lowest = 1.0e9f, highest = -1.0e9f;
    for ( const Desert::Vertex& v : clump.Vertices )
    {
        lowest  = std::min( lowest, v.Position.y );
        highest = std::max( highest, v.Position.y );
    }
    EXPECT_FLOAT_EQ( lowest, 0.0f ) << "the root is the pivot: the wind's height mask is zero there";
    EXPECT_LE( highest, 50.0f );

    // Each triangle faces along its own vertices' normal, and the two sheets of a blade face opposite ways:
    // from any side one of them is front-facing.
    size_t facingUp = 0, facingDown = 0;
    for ( const Desert::Index& t : clump.Indices )
    {
        const auto&     a = clump.Vertices[t.V1];
        const glm::vec3 n =
             glm::cross( clump.Vertices[t.V2].Position - a.Position, clump.Vertices[t.V3].Position - a.Position );
        EXPECT_GT( winding * glm::dot( n, a.Normal ), 0.0f );
        ( glm::dot( a.Normal, glm::vec3( 0.0f, 0.0f, 1.0f ) ) >= 0.0f ? facingUp : facingDown )++;
    }
    EXPECT_GT( facingUp, 0u );
    EXPECT_GT( facingDown, 0u );
}

TEST( GrassBladeAsset, TheCheckedInGradientIsItsRecipe )
{
    const fs::path root = Root();
    ASSERT_FALSE( root.empty() );
    const std::vector<std::byte> expected = GradientPng();

    auto read = Assets::ReadTextureSourceAssetFile( root / kGradientPath );
    if ( read.IsSuccess() && read.GetValue().Source == expected &&
         read.GetValue().Import.Settings.Intent == Core::Formats::TextureIntent::Colour )
        return;

    Assets::TextureSourceAsset adopted =
         Assets::MakeTextureSourceAsset( Common::Content::ContentKind::Texture, "assets:Textures/T_GrassBlade.png",
                                         expected, { Core::Formats::TextureIntent::Colour } );
    if ( read.IsSuccess() )
        adopted.Guid = read.GetValue().Guid; // the identity the material names survives the re-author
    const fs::path out = AdoptionPath( "T_GrassBlade.detex" );
    ASSERT_TRUE( Assets::WriteTextureSourceAssetFile( out, adopted ).IsSuccess() );
    FAIL() << "T_GrassBlade.detex is not its recipe (" << ( read.IsSuccess() ? "differs" : read.GetError() )
           << "); the recipe's asset is at " << out << " - cp it to " << ( root / kGradientPath );
}

TEST( GrassBladeAsset, TheMaterialDrawsTheGradient )
{
    const fs::path root = Root();
    ASSERT_FALSE( root.empty() );
    auto texture = Assets::ReadTextureSourceAssetFile( root / kGradientPath );
    ASSERT_TRUE( texture.IsSuccess() ) << texture.GetError();
    const std::string material = ReadText( root / kMaterialPath );
    ASSERT_FALSE( material.empty() ) << kMaterialPath << " is missing";
    EXPECT_NE( material.find( "\"u_AlbedoTexture\"" ), std::string::npos );
    EXPECT_NE( material.find( Common::Content::AssetGuidToText( texture.GetValue().Guid ) ), std::string::npos )
         << "the material must name the gradient by its GUID "
         << Common::Content::AssetGuidToText( texture.GetValue().Guid );
    EXPECT_NE( material.find( "assets:Textures/T_GrassBlade.detex" ), std::string::npos );
}

TEST( GrassBladeAsset, TheCheckedInBladeIsItsRecipe )
{
    const fs::path root = Root();
    ASSERT_FALSE( root.empty() );
    auto mesh = Geometry::ShapeToEditMesh( GrassClump() );
    ASSERT_TRUE( mesh.IsSuccess() ) << mesh.GetError();
    const Common::Content::AssetGuid material = HeaderGuidOf( ReadText( root / kMaterialPath ) );

    Assets::MeshSourceData expected;
    expected.Models.push_back( { Geometry::ToSerialized( mesh.GetValue() ) } );
    expected.MaterialSlots.push_back( { "Slot0", material } );

    auto read = Assets::ReadMeshSourceAssetFile( root / kBladePath );
    if ( read.IsSuccess() && read.GetValue().Source == expected && read.GetValue().Name == "GrassBlade" )
        return;

    Assets::MeshSourceAsset adopted;
    adopted.Kind              = Common::Content::ContentKind::StaticMesh;
    adopted.Guid              = read.IsSuccess() ? read.GetValue().Guid : Common::Content::AssetGuid::Generate();
    adopted.Name              = "GrassBlade";
    adopted.Import.Provenance = Assets::MeshSourceProvenance::Recovered;
    adopted.Source            = expected;
    const fs::path out        = AdoptionPath( "GrassBlade.stmesh" );
    ASSERT_TRUE( Assets::WriteMeshSourceAssetFile( out, adopted ).IsSuccess() );
    FAIL() << "GrassBlade.stmesh is not its recipe (" << ( read.IsSuccess() ? "differs" : read.GetError() )
           << "); the recipe's asset is at " << out << " - cp it to " << ( root / kBladePath );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
