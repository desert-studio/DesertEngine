// AF3 — the texture asset (`.detex` envelope with the source inside) and its DDC key.
//
// Relations, not values: the key moves with a setting and with the deriver GUID and does NOT move with the
// asset's path; an asset round-trips byte for byte; and every texture asset committed to the project
// carries the handle its source's path derived before the migration, so the `.demat` files that name those
// handles still resolve.
#include <Engine/Assets/TextureSourceAsset.hpp>

#include <Common/Core/AssetHandle.hpp>
#include <Common/Utilities/PakFile.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>

namespace fs = std::filesystem;
using namespace Desert::Assets;
namespace Fmt = Desert::Core::Formats;

namespace
{
    std::vector<std::byte> Bytes( const std::string& s )
    {
        return { reinterpret_cast<const std::byte*>( s.data() ),
                 reinterpret_cast<const std::byte*>( s.data() ) + s.size() };
    }

    fs::path RepoRoot()
    {
        for ( fs::path dir = fs::current_path(); !dir.empty(); dir = dir.parent_path() )
        {
            std::error_code ec;
            if ( fs::exists( dir / ".gitignore", ec ) && fs::is_directory( dir / "Editor" / "Resources", ec ) )
                return dir;
            if ( dir == dir.parent_path() )
                break;
        }
        return {};
    }
} // namespace

TEST( TextureAsset, KeyMovesWithEverySettingAndTheDeriverVersion )
{
    const TextureBuildSettings base{ { Fmt::TextureIntent::Unspecified }, 1 };
    const uint64_t             k0 = TextureDerivedDataKey( 42, base );
    EXPECT_NE( k0, TextureDerivedDataKey( 43, base ) ) << "the source id is an input";
    EXPECT_NE( k0, TextureDerivedDataKey( 42, { { Fmt::TextureIntent::Data }, 1 } ) ) << "the intent is an input";
    EXPECT_NE( k0, TextureDerivedDataKey( 42, { { Fmt::TextureIntent::Unspecified }, 2 } ) )
         << "the encoder version is an input";
    constexpr Common::DDC::Deriver bumped{
         "Texture", ".tex", { kTextureDeriver.Version.Hi, kTextureDeriver.Version.Lo + 1 } };
    EXPECT_NE( k0, TextureDerivedDataKey( 42, base, bumped ) ) << "TEXTURE_DERIVEDDATA_VER is an input";
    EXPECT_EQ( k0, TextureDerivedDataKey( 42, base ) ) << "the key is a pure function";
}

TEST( TextureAsset, RenamingTheAssetKeepsItsKeyAndItsHandle )
{
    const fs::path dir = fs::temp_directory_path() / "af3_texture_asset_rename";
    fs::remove_all( dir );
    fs::create_directories( dir );
    const auto asset = MakeTextureSourceAsset( Common::Content::ContentKind::Texture, Common::UUID( 777 ),
                                               "assets:Textures/T.png", Bytes( "not really a png" ),
                                               { Fmt::TextureIntent::Colour } );
    ASSERT_TRUE( WriteTextureSourceAssetFile( dir / "A.detex", asset ).IsSuccess() );
    fs::rename( dir / "A.detex", dir / "Renamed.detex" );
    const auto back = ReadTextureSourceAssetFile( dir / "Renamed.detex" );
    ASSERT_TRUE( back.IsSuccess() ) << back.GetError();
    EXPECT_EQ( static_cast<uint64_t>( back.GetValue().Handle() ), 777u );
    const TextureBuildSettings s{ back.GetValue().Import.Settings, 1 };
    EXPECT_EQ( TextureDerivedDataKey( back.GetValue().Import.SourceHash, s ),
               TextureDerivedDataKey( asset.Import.SourceHash, { asset.Import.Settings, 1 } ) );
    fs::remove_all( dir );
}

TEST( TextureAsset, RoundTripIsByteIdenticalAndCarriesTheSource )
{
    const auto asset = MakeTextureSourceAsset( Common::Content::ContentKind::Skybox, Common::UUID( 0x1234 ),
                                               "assets:Textures/HDR/Sky.hdr", Bytes( "#?RADIANCE fake" ),
                                               { Fmt::TextureIntent::Unspecified } );
    const auto first = EncodeTextureSourceAsset( asset );
    ASSERT_TRUE( first.IsSuccess() ) << first.GetError();
    const auto decoded = DecodeTextureSourceAsset( first.GetValue() );
    ASSERT_TRUE( decoded.IsSuccess() ) << decoded.GetError();
    EXPECT_EQ( decoded.GetValue(), asset );
    const auto second = EncodeTextureSourceAsset( decoded.GetValue() );
    ASSERT_TRUE( second.IsSuccess() );
    EXPECT_EQ( first.GetValue(), second.GetValue() );
    EXPECT_EQ( decoded.GetValue().Import.SourceHash,
               Common::Utils::PakContentHash( asset.Source.data(), asset.Source.size() ) );
}

TEST( TextureAsset, ASourceThatDoesNotMatchItsImportHashIsRefused )
{
    auto asset = MakeTextureSourceAsset( Common::Content::ContentKind::Texture, Common::UUID( 5 ), "assets:x.png",
                                         Bytes( "abc" ), {} );
    asset.Source = Bytes( "abd" );
    EXPECT_FALSE( EncodeTextureSourceAsset( asset ).IsSuccess() );
}

// THE MIGRATION'S TWO CHECKS, on the committed tree: (1) no texture source is left outside an asset in
// the loose roots — one asset per former source; (2) each asset's handle is the number its original
// source path derived, i.e. the handle before the migration equals the handle after it, for every texture.
TEST( TextureAsset, EveryProjectTextureIsAnAssetAndKeepsItsHandle )
{
    const fs::path repo = RepoRoot();
    ASSERT_FALSE( repo.empty() );
    const fs::path resources = repo / "Editor" / "Resources";
    Common::Constants::Path::SetProjectRoot( resources.parent_path(), "Resources/Assets" );
    size_t assets = 0;
    for ( const char* sub : { "Assets/Textures", "Assets/Meshes" } )
    {
        for ( const auto& e : fs::recursive_directory_iterator( resources / sub ) )
        {
            std::string ext = e.path().extension().string();
            for ( const char* raw : { ".png", ".jpg", ".jpeg", ".tga", ".bmp", ".hdr", ".exr" } )
                EXPECT_NE( ext, raw ) << e.path() << " is a texture source outside an asset";
            if ( ext != ".detex" )
                continue;
            ++assets;
            const auto a = ReadTextureSourceAssetFile( e.path() );
            ASSERT_TRUE( a.IsSuccess() ) << a.GetError();
            const uint64_t before =
                 static_cast<uint64_t>( Common::AssetHandle::FromKey( a.GetValue().Import.SourceFile ) );
            EXPECT_EQ( static_cast<uint64_t>( a.GetValue().Handle() ), before ) << e.path();
        }
    }
    EXPECT_EQ( assets, 11u ) << "9 images + 2 HDR panoramas were migrated";
}
