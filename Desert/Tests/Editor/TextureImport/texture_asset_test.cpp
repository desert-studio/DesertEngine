// AF3 — the texture asset (`.detex` envelope with the source inside) and its DDC key.
//
// Relations, not values: the key moves with a setting and with the deriver GUID and does NOT move with the
// asset's path; an asset round-trips byte for byte; and every texture asset committed to the project
// carries the handle its source's path derived before the migration, so the `.demat` files that name those
// handles still resolve.
#include <Engine/Assets/TextureSourceAsset.hpp>

#include <Common/Core/AssetHandle.hpp>
#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Utilities/PakFile.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <unordered_set>
#include "../../TestSupport/project_scope.hpp"
#include "../../TestSupport/scratch_dir.hpp"
#include <Common/Core/Constants.hpp>

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
        return Desert::TestSupport::RepositoryRoot();
    }

    constexpr Common::Content::AssetGuid kOldGuid{ 0x1234abcd5678ef01ull, 0x0fedcba987654321ull };

    void PutLE( std::vector<std::byte>& out, uint64_t v, int width )
    {
        for ( int i = 0; i < width; ++i )
            out.push_back( static_cast<std::byte>( ( v >> ( 8 * i ) ) & 0xffu ) );
    }

    void PutLEString( std::vector<std::byte>& out, const std::string& s )
    {
        PutLE( out, s.size(), 4 );
        for ( const char c : s )
            out.push_back( static_cast<std::byte>( c ) );
    }

    // A `.detex` exactly as TXAS version 1 wrote it: the envelope stamped TXAS 1 and an IMPT of version 1 --
    // SourceFile, SourceHash, Intent by name, and no colour space. Spelled out byte by byte here rather than
    // produced by the current encoder, which can only write version 2.
    std::vector<std::byte> VersionOneAsset( const std::string& intentName, const std::vector<std::byte>& source )
    {
        namespace CC = Common::Content;
        std::vector<std::byte> impt;
        PutLE( impt, 1, 4 );
        PutLEString( impt, "assets:Textures/T_Old.png" );
        PutLE( impt, Common::Utils::PakContentHash( source.data(), source.size() ), 8 );
        PutLEString( impt, intentName );

        CC::AssetEnvelope envelope;
        envelope.Asset.Kind       = CC::ContentKind::Texture;
        envelope.Asset.Guid       = kOldGuid;
        envelope.Asset.Subsystems = { { kTextureAssetSubsystemTag, 1 } };
        CC::EnvelopeMeta meta;
        meta.Name = "T_Old";
        envelope.Sections.push_back(
             { CC::EnvelopeSection::Meta, CC::EnvelopeCodec::Stored, CC::EncodeEnvelopeMeta( meta ) } );
        envelope.Sections.push_back( { CC::EnvelopeSection::ImportInfo, CC::EnvelopeCodec::Stored, impt } );
        envelope.Sections.push_back( { CC::EnvelopeSection::Source, CC::EnvelopeCodec::Stored, source } );
        auto written = CC::WriteAssetEnvelope( envelope );
        EXPECT_TRUE( written.IsSuccess() ) << written.GetError();
        return written.IsSuccess() ? written.ExtractValue() : std::vector<std::byte>{};
    }
} // namespace

// TEX-SRGB: a TXAS 1 asset is REFUSED by the reader (never read leniently) and raised by SceneMigrator's step,
// which keeps identity, name, provenance, intent and source, and states the colour space DefaultTextureColorSpace
// gives: Colour/Unspecified sRGB, NormalMap/Mask/Data Linear, a float source (HDR/EXR) Linear whatever its intent.
TEST( TextureAsset, AVersionOneAssetIsRefusedAndRaisedWithTheDefaultColourSpace )
{
    struct Case
    {
        const char*            Intent;
        std::vector<std::byte> Source;
        Fmt::TextureColorSpace Expected;
    };
    const std::string exrMagic( "v/1\x01 fake", 9 );
    const Case        cases[] = {
         { "Colour", Bytes( "\x89PNG fake" ), Fmt::TextureColorSpace::SRGB },
         { "Unspecified", Bytes( "\x89PNG fake" ), Fmt::TextureColorSpace::SRGB },
         { "NormalMap", Bytes( "\x89PNG fake" ), Fmt::TextureColorSpace::Linear },
         { "Mask", Bytes( "\x89PNG fake" ), Fmt::TextureColorSpace::Linear },
         { "Data", Bytes( "\x89PNG fake" ), Fmt::TextureColorSpace::Linear },
         { "Colour", Bytes( "#?RADIANCE fake" ), Fmt::TextureColorSpace::Linear },
         { "Colour", Bytes( exrMagic ), Fmt::TextureColorSpace::Linear },
    };
    for ( const Case& c : cases )
    {
        const std::vector<std::byte> old = VersionOneAsset( c.Intent, c.Source );
        ASSERT_FALSE( old.empty() );
        EXPECT_FALSE( DecodeTextureSourceAsset( old ).IsSuccess() ) << c.Intent << ": a TXAS 1 asset was read";

        const auto raised = UpgradeTextureSourceAsset( old );
        ASSERT_TRUE( raised.IsSuccess() ) << c.Intent << ": " << raised.GetError();
        ASSERT_TRUE( raised.GetValue().has_value() ) << c.Intent << ": a TXAS 1 asset was called current";
        const auto decoded = DecodeTextureSourceAsset( *raised.GetValue() );
        ASSERT_TRUE( decoded.IsSuccess() ) << c.Intent << ": " << decoded.GetError();
        const TextureSourceAsset& asset = decoded.GetValue();
        EXPECT_EQ( asset.Import.Settings.ColorSpace, c.Expected ) << c.Intent;
        EXPECT_EQ( Fmt::TextureIntentName( asset.Import.Settings.Intent ), std::string_view( c.Intent ) );
        EXPECT_EQ( asset.Guid, kOldGuid );
        EXPECT_EQ( asset.Name, "T_Old" );
        EXPECT_EQ( asset.Import.SourceFile, "assets:Textures/T_Old.png" );
        EXPECT_EQ( asset.Source, c.Source );

        const auto again = UpgradeTextureSourceAsset( *raised.GetValue() );
        ASSERT_TRUE( again.IsSuccess() ) << again.GetError();
        EXPECT_FALSE( again.GetValue().has_value() ) << c.Intent << ": a current asset was raised again";
    }
}

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
    const auto asset = MakeTextureSourceAsset( Common::Content::ContentKind::Texture, "assets:Textures/T.png",
                                               Bytes( "not really a png" ), { Fmt::TextureIntent::Colour } );
    ASSERT_FALSE( asset.Guid.IsNull() ) << "a new texture asset was minted without an identity";
    ASSERT_TRUE( WriteTextureSourceAssetFile( dir / "A.detex", asset ).IsSuccess() );
    fs::rename( dir / "A.detex", dir / "Renamed.detex" );
    const auto back = ReadTextureSourceAssetFile( dir / "Renamed.detex" );
    ASSERT_TRUE( back.IsSuccess() ) << back.GetError();
    EXPECT_EQ( back.GetValue().Guid, asset.Guid );
    EXPECT_EQ( static_cast<uint64_t>( back.GetValue().Handle() ),
               static_cast<uint64_t>( Common::Content::HandleForGuid( asset.Guid ) ) )
         << "a texture's handle is the one fold of its header GUID, not a number of its own";
    const TextureBuildSettings s{ back.GetValue().Import.Settings, 1 };
    EXPECT_EQ( TextureDerivedDataKey( back.GetValue().Import.SourceHash, s ),
               TextureDerivedDataKey( asset.Import.SourceHash, { asset.Import.Settings, 1 } ) );
    fs::remove_all( dir );
}

TEST( TextureAsset, RoundTripIsByteIdenticalAndCarriesTheSource )
{
    const auto asset = MakeTextureSourceAsset( Common::Content::ContentKind::Skybox, "assets:Textures/HDR/Sky.hdr",
                                               Bytes( "#?RADIANCE fake" ), { Fmt::TextureIntent::Unspecified } );
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
    auto asset =
         MakeTextureSourceAsset( Common::Content::ContentKind::Texture, "assets:x.png", Bytes( "abc" ), {} );
    asset.Source = Bytes( "abd" );
    EXPECT_FALSE( EncodeTextureSourceAsset( asset ).IsSuccess() );
}

// THE MIGRATION'S CHECKS, on the committed tree: (1) no texture source is left outside an asset in the
// loose roots — one asset per former source; (2) each MIGRATED asset (the register below, one row each, so
// a later texture cannot be mistaken for one) keeps as Guid.Hi the number its original source path derived.
// Textures authored after the migration carry a generated GUID and are held only to (1) and (3). That number is no
// longer the handle (the handle is HandleForGuid of the whole GUID, SCNE 28 step 6), but it is what the committed
// `.demat`s still name, and the MATL 3 migration maps it to the GUID through this very relation; (3) no two assets
// fold to one handle.
TEST( TextureAsset, EveryProjectTextureIsAnAssetAndKeepsItsHandle )
{
    const fs::path repo = RepoRoot();
    ASSERT_FALSE( repo.empty() );
    // The sample project's content, opened from its .deproj the way the editor opens it.
    const Desert::TestSupport::ProjectScope project( Desert::TestSupport::kCommittedProjects[0] );
    const auto&                             root    = Common::Constants::Path::CurrentProjectRoot();
    const fs::path                          content = root.ProjectDir / root.AssetsRoot;
    // The 9 images + 2 HDR panoramas the migration produced, relative to the project's content root.
    const std::set<std::string> migrated = {
         "Meshes/shaded.detex",
         "Meshes/texture_diffuse.detex",
         "Meshes/texture_metallic.detex",
         "Meshes/texture_normal.detex",
         "Meshes/texture_pbr.detex",
         "Meshes/texture_roughness.detex",
         "Textures/1k_Dissolve_Noise_Texture.detex",
         "Textures/HDR/PreviewCheck.detex",
         "Textures/HDR/rural_asphalt_road_2k.detex",
         "Textures/T_Checker.detex",
         "Textures/T_NormalWitness.detex",
    };
    std::set<std::string>        seen;
    std::unordered_set<uint64_t> handles;
    for ( const char* sub : { "Textures", "Meshes" } )
    {
        for ( const auto& e : fs::recursive_directory_iterator( content / sub ) )
        {
            std::string ext = e.path().extension().string();
            for ( const char* raw : { ".png", ".jpg", ".jpeg", ".tga", ".bmp", ".hdr", ".exr" } )
                EXPECT_NE( ext, raw ) << e.path() << " is a texture source outside an asset";
            if ( ext != ".detex" )
                continue;
            const auto a = ReadTextureSourceAssetFile( e.path() );
            ASSERT_TRUE( a.IsSuccess() ) << a.GetError();
            const std::string rel = e.path().lexically_relative( content ).generic_string();
            if ( migrated.contains( rel ) )
            {
                seen.insert( rel );
                const uint64_t before =
                     static_cast<uint64_t>( Common::AssetHandle::FromKey( a.GetValue().Import.SourceFile ) );
                EXPECT_EQ( a.GetValue().Guid.Hi, before ) << e.path();
            }
            EXPECT_TRUE( handles.insert( static_cast<uint64_t>( a.GetValue().Handle() ) ).second )
                 << e.path() << " folds to a handle another texture asset already has";
        }
    }
    for ( const std::string& rel : migrated )
        EXPECT_TRUE( seen.contains( rel ) ) << rel << " was migrated and is gone from the tree";
}
