/**
 * B17 — THE COOKED TEXTURE CONTAINER: a PNG that survives the round trip AT EVERY LEVEL, and a
 * truncated file that is not a small texture.
 *
 * WHAT WAS MEASURED BEFORE THIS EXISTED, on this tree, on `Editor/Cooked/Textures/T_Checker.tex`
 * (1024x1024 RGBA8, an eleven-level chain). The old path is "read 133 bytes of JSON, then decode the
 * PNG it names, then blit ten levels on the GPU"; the new one is "read the container, copy".
 *
 *      phase                       manifest + PNG     container      ratio
 *      read from disk                   -                 -          (both, see the numbers in the
 *      parse / decode                   -                 -           task report: this header is
 *      build the level chain            -                 -           not the place a number can be
 *                                                                     kept honest, the report is)
 *
 * THE POINT THE MESH CONTAINER MADE AND THIS ONE INHERITS: the interesting row is not the one the
 * programme document leads with. For `.stmesh` the disk ratio bought 12 % of the saving and the PARSE
 * bought 88 %, at 271x rather than 5x. So the phases are measured separately here too, and never as a
 * difference of two whole-frame timings.
 *
 * ── WHAT THESE TESTS ARE ABOUT ────────────────────────────────────────────────────────────────────
 *
 * 1. EVERY LEVEL IS COMPARED, NOT JUST THE BASE. A container that carried the base correctly and a
 *    corrupted chain would pass any test that only looks at level 0, and the chain is the entire
 *    reason this format exists. The reference chain is computed HERE by a separate naive loop rather
 *    than by calling `BuildMipChain`, because a test that checks a function against itself checks
 *    nothing.
 *
 * 2. A TRUNCATED FILE IS NOT A SMALL TEXTURE. This is the property the container was given a declared
 *    `FileSize` for. Under a naive reader both decode "successfully" — the short one simply carrying
 *    fewer bytes than its table claims — so the test asserts the DIFFERENCE between the two outcomes,
 *    not each half alone: each half alone passes on a reader that cannot tell them apart.
 *
 * 3. A PLAUSIBLE CORRUPTION IS REFUSED, NOT FOLLOWED. `MeshBinary` paid for this lesson: flipping one
 *    byte of a section offset moved it somewhere still inside the file, still aligned, still with room
 *    for the declared count, and the decode handed back a mesh of shifted floats. The level table is
 *    therefore DERIVED from the header and checked, not trusted.
 *
 * 4. THE RETIRED MANIFEST IS REFUSED BY NAME. Cooked content is derived data and is not migrated
 *    (owner decision 2026-09-22, the same call `MeshBinary` records). A stale cook must not read like
 *    a corrupt file: the refusal names the remedy.
 *
 * 5. THE COMMITTED CORPUS IS CONVERTED. `Editor/Cooked/Textures/T_Checker.tex` is the one cooked
 *    texture this repository tracks — eleven scenes draw their floor with it and nothing re-cooks it
 *    on a fresh clone. It is parsed here through the reader the engine uses.
 */

#include <Engine/Assets/Serialization/TextureBinary.hpp>

#include <gtest/gtest.h>

#include <stb_image/stb_image.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

using namespace Desert::Assets::Serialization;
using Desert::Core::Formats::ImageFormat;

namespace
{
    std::filesystem::path RepositoryRoot()
    {
        std::filesystem::path here = std::filesystem::current_path();
        for ( int up = 0; up < 8 && !std::filesystem::exists( here / "Editor" / "Cooked" / "Textures" ); ++up )
            here = here.parent_path();
        return here;
    }

    std::string ReadFile( const std::filesystem::path& path )
    {
        std::ifstream      in( path, std::ios::binary );
        std::ostringstream out;
        out << in.rdbuf();
        return out.str();
    }

    /// THE REFERENCE CHAIN, WRITTEN SEPARATELY ON PURPOSE. Same rule — halve down, average the 2x2
    /// block clamped to the source, round rather than truncate — expressed as a plain nested loop over
    /// one level at a time, so it shares no line with the implementation it checks.
    std::vector<std::vector<unsigned char>> ReferenceChainRGBA8( const std::vector<unsigned char>& base,
                                                                 uint32_t width, uint32_t height )
    {
        std::vector<std::vector<unsigned char>> levels{ base };
        uint32_t                                w = width, h = height;
        while ( w > 1 || h > 1 )
        {
            const uint32_t                    nw = w > 1 ? w / 2 : 1;
            const uint32_t                    nh = h > 1 ? h / 2 : 1;
            std::vector<unsigned char>        next( static_cast<size_t>( nw ) * nh * 4 );
            const std::vector<unsigned char>& src = levels.back();
            for ( uint32_t y = 0; y < nh; ++y )
            {
                for ( uint32_t x = 0; x < nw; ++x )
                {
                    for ( uint32_t c = 0; c < 4; ++c )
                    {
                        const uint32_t sx0 = std::min( x * 2u, w - 1u );
                        const uint32_t sx1 = std::min( x * 2u + 1u, w - 1u );
                        const uint32_t sy0 = std::min( y * 2u, h - 1u );
                        const uint32_t sy1 = std::min( y * 2u + 1u, h - 1u );
                        const uint32_t sum =
                             static_cast<uint32_t>( src[( static_cast<size_t>( sy0 ) * w + sx0 ) * 4 + c] ) +
                             static_cast<uint32_t>( src[( static_cast<size_t>( sy0 ) * w + sx1 ) * 4 + c] ) +
                             static_cast<uint32_t>( src[( static_cast<size_t>( sy1 ) * w + sx0 ) * 4 + c] ) +
                             static_cast<uint32_t>( src[( static_cast<size_t>( sy1 ) * w + sx1 ) * 4 + c] );
                        next[( static_cast<size_t>( y ) * nw + x ) * 4 + c] =
                             static_cast<unsigned char>( ( sum + 2 ) / 4 );
                    }
                }
            }
            levels.push_back( std::move( next ) );
            w = nw;
            h = nh;
        }
        return levels;
    }

    /// A deterministic image with structure in it. A flat colour would survive a box filter that
    /// averaged the wrong neighbours, and a chain built from the wrong rows would still look right.
    std::vector<unsigned char> SyntheticRGBA8( const uint32_t width, const uint32_t height )
    {
        std::vector<unsigned char> pixels( static_cast<size_t>( width ) * height * 4 );
        for ( uint32_t y = 0; y < height; ++y )
        {
            for ( uint32_t x = 0; x < width; ++x )
            {
                const size_t at = ( static_cast<size_t>( y ) * width + x ) * 4;
                pixels[at + 0]  = static_cast<unsigned char>( ( x * 7u + y * 13u ) & 0xFFu );
                pixels[at + 1]  = static_cast<unsigned char>( ( x ^ y ) & 0xFFu );
                pixels[at + 2]  = static_cast<unsigned char>( ( x * 3u ) & 0xFFu );
                pixels[at + 3]  = static_cast<unsigned char>( 255u - ( ( y * 5u ) & 0xFFu ) );
            }
        }
        return pixels;
    }

    TextureAssetData Cook( const uint32_t width, const uint32_t height, const std::vector<unsigned char>& base,
                           const std::string& key = "assets:Textures/Probe.png" )
    {
        TextureAssetData data;
        data.Handle            = Common::UUID( 0x0123456789ABCDEFull );
        data.SourcePath        = key;
        data.SourceContentHash = 0x00001234DEADBEEFull;
        data.Width             = width;
        data.Height            = height;
        data.Format            = ImageFormat::RGBA8F;
        auto chain             = BuildMipChain( width, height, ImageFormat::RGBA8F, base, data.Pixels );
        EXPECT_TRUE( chain.IsSuccess() ) << chain.GetError();
        data.Levels = chain.ExtractValue();
        return data;
    }
} // namespace

// ── 1. THE ROUND TRIP IS AN IDENTITY, FIELD BY FIELD AND BYTE BY BYTE ───────────────────────────────

TEST( TextureBinaryFormat, RoundTripIsAnIdentity )
{
    const uint32_t w = 64, h = 32;
    const auto     data    = Cook( w, h, SyntheticRGBA8( w, h ) );
    const auto     encoded = EncodeTextureBinary( data );

    const auto decoded = DecodeTextureBinary( encoded, "round-trip" );
    ASSERT_TRUE( decoded.IsSuccess() ) << decoded.GetError();
    const auto& back = decoded.GetValue();

    EXPECT_EQ( static_cast<uint64_t>( back.Handle ), static_cast<uint64_t>( data.Handle ) );
    EXPECT_EQ( back.SourcePath, data.SourcePath );
    EXPECT_EQ( back.SourceContentHash, data.SourceContentHash );
    EXPECT_EQ( back.Width, data.Width );
    EXPECT_EQ( back.Height, data.Height );
    EXPECT_EQ( back.Format, data.Format );
    ASSERT_EQ( back.Levels.size(), data.Levels.size() );
    for ( size_t i = 0; i < back.Levels.size(); ++i )
    {
        EXPECT_EQ( back.Levels[i].Width, data.Levels[i].Width ) << "level " << i;
        EXPECT_EQ( back.Levels[i].Height, data.Levels[i].Height ) << "level " << i;
        EXPECT_EQ( back.Levels[i].ByteOffset, data.Levels[i].ByteOffset ) << "level " << i;
        EXPECT_EQ( back.Levels[i].ByteSize, data.Levels[i].ByteSize ) << "level " << i;
    }
    ASSERT_EQ( back.Pixels.size(), data.Pixels.size() );
    EXPECT_EQ( std::memcmp( back.Pixels.data(), data.Pixels.data(), back.Pixels.size() ), 0 );
}

// ── 2. EVERY LEVEL, AGAINST A CHAIN THIS FILE COMPUTED ITSELF ───────────────────────────────────────

TEST( TextureBinaryFormat, EveryLevelMatchesAnIndependentChain )
{
    const uint32_t w = 37, h = 20; // deliberately odd: the halving rule and the clamp both have to work
    const auto     base = SyntheticRGBA8( w, h );

    const auto reference = ReferenceChainRGBA8( base, w, h );

    const auto encoded = EncodeTextureBinary( Cook( w, h, base ) );
    const auto decoded = DecodeTextureBinary( encoded, "levels" );
    ASSERT_TRUE( decoded.IsSuccess() ) << decoded.GetError();
    const auto& back = decoded.GetValue();

    ASSERT_EQ( back.Levels.size(), reference.size() )
         << "the chain length disagrees with an independent halving of " << w << "x" << h;

    for ( size_t i = 0; i < reference.size(); ++i )
    {
        ASSERT_EQ( back.Levels[i].ByteSize, reference[i].size() ) << "level " << i << " byte size";
        EXPECT_EQ( std::memcmp( back.Pixels.data() + back.Levels[i].ByteOffset, reference[i].data(),
                                reference[i].size() ),
                   0 )
             << "level " << i << " (" << back.Levels[i].Width << "x" << back.Levels[i].Height
             << ") differs from the reference chain";
    }
}

// ── 3. A REAL PNG, END TO END: SOURCE PIXELS -> CONTAINER -> LOAD ──────────────────────────────────

TEST( TextureBinaryFormat, APngSurvivesTheContainerByteForByte )
{
    const auto png = RepositoryRoot() / "Editor" / "Resources" / "Assets" / "Textures" / "T_Checker.png";
    ASSERT_TRUE( std::filesystem::exists( png ) ) << png.string();

    const std::string bytes = ReadFile( png );
    int               w = 0, h = 0, ch = 0;
    stbi_uc*          raw = stbi_load_from_memory( reinterpret_cast<const stbi_uc*>( bytes.data() ),
                                                   static_cast<int>( bytes.size() ), &w, &h, &ch, 4 );
    ASSERT_NE( raw, nullptr ) << stbi_failure_reason();

    const size_t               baseBytes = static_cast<size_t>( w ) * h * 4;
    std::vector<unsigned char> base( raw, raw + baseBytes );
    stbi_image_free( raw );

    const auto reference = ReferenceChainRGBA8( base, static_cast<uint32_t>( w ), static_cast<uint32_t>( h ) );

    const auto encoded = EncodeTextureBinary(
         Cook( static_cast<uint32_t>( w ), static_cast<uint32_t>( h ), base, "assets:Textures/T_Checker.png" ) );
    const auto decoded = DecodeTextureBinary( encoded, png.string() );
    ASSERT_TRUE( decoded.IsSuccess() ) << decoded.GetError();
    const auto& back = decoded.GetValue();

    // LEVEL 0 IS THE SOURCE'S OWN PIXELS, not merely "close to them". This is the claim the whole
    // container rests on: the decoder that used to run on every load ran once, at cook time, and
    // nothing since has touched the bytes. Addressed through its own offset, because level 0 is the
    // LAST thing in the file -- the levels are stored smallest first.
    ASSERT_EQ( back.Levels[0].ByteSize, baseBytes );
    EXPECT_EQ( std::memcmp( back.Pixels.data() + back.Levels[0].ByteOffset, base.data(), baseBytes ), 0 );

    ASSERT_EQ( back.Levels.size(), reference.size() );
    for ( size_t i = 0; i < reference.size(); ++i )
    {
        EXPECT_EQ( std::memcmp( back.Pixels.data() + back.Levels[i].ByteOffset, reference[i].data(),
                                reference[i].size() ),
                   0 )
             << "T_Checker level " << i;
    }
}

// ── 4. A TRUNCATED FILE IS NOT A SMALL TEXTURE ─────────────────────────────────────────────────────

TEST( TextureBinaryFormat, TruncationIsDistinguishableFromASmallTexture )
{
    // The smallest container there is: one texel, one level. It is LEGAL and must decode.
    std::vector<unsigned char> single( 4 );
    single[0]       = 10;
    single[1]       = 20;
    single[2]       = 30;
    single[3]       = 40;
    const auto tiny = EncodeTextureBinary( Cook( 1, 1, single ) );

    const auto tinyDecoded = DecodeTextureBinary( tiny, "1x1" );
    ASSERT_TRUE( tinyDecoded.IsSuccess() ) << tinyDecoded.GetError();
    EXPECT_EQ( tinyDecoded.GetValue().Levels.size(), 1u );
    EXPECT_EQ( tinyDecoded.GetValue().Pixels.size(), 4u );

    // And a real texture whose bytes stop early is a REFUSAL, not a texture with fewer levels.
    const uint32_t w = 64, h = 64;
    const auto     full = EncodeTextureBinary( Cook( w, h, SyntheticRGBA8( w, h ) ) );
    ASSERT_GT( full.size(), 1024u );

    const std::string cut     = full.substr( 0, full.size() - 512 );
    const auto        cutRead = DecodeTextureBinary( cut, "cut.tex" );
    EXPECT_FALSE( cutRead.IsSuccess() )
         << "a file 512 bytes short decoded as a texture; the declared FileSize did its job nowhere";
    EXPECT_NE( cutRead.GetError().find( "truncated" ), std::string::npos ) << cutRead.GetError();

    // THE DIFFERENCE IS THE ASSERTION. A reader that cannot tell the two apart passes each half of
    // this test on its own; it cannot pass both.
    EXPECT_NE( tinyDecoded.IsSuccess(), cutRead.IsSuccess() );
}

// ── 5. A PLAUSIBLE CORRUPTION IS REFUSED RATHER THAN FOLLOWED ──────────────────────────────────────

TEST( TextureBinaryFormat, ALevelOffsetThatStillFitsIsRefused )
{
    const uint32_t w = 32, h = 32;
    std::string    encoded = EncodeTextureBinary( Cook( w, h, SyntheticRGBA8( w, h ) ) );

    // Level 1's ByteOffset is the first field of the second row, at header(128) + one row(16). Move it
    // forward by 16 bytes: still inside the payload, still level-aligned, still leaving room for the
    // declared size — and the image it would produce is a complete, plausible, WRONG one.
    const size_t offsetField = kTextureBinaryHeaderSize + 16u;
    uint64_t     moved       = 0;
    std::memcpy( &moved, encoded.data() + offsetField, sizeof( moved ) );
    moved += 16;
    std::memcpy( encoded.data() + offsetField, &moved, sizeof( moved ) );

    const auto read = DecodeTextureBinary( encoded, "tampered.tex" );
    EXPECT_FALSE( read.IsSuccess() ) << "a level offset moved 64 bytes forward was followed";
    EXPECT_NE( read.GetError().find( "level 1" ), std::string::npos ) << read.GetError();
}

TEST( TextureBinaryFormat, ADeclaredPayloadTotalThatTheTableContradictsIsRefused )
{
    // THIS TEST EXISTS BECAUSE THE MUTATION STAYED GREEN WITHOUT IT. Deleting the PayloadBytes
    // comparison outright changed nothing that any other test could see: the field is a SECOND
    // statement of a quantity the table already carries, so nothing reads it unless something forges
    // it. A guard nobody exercises is a guard that is not there, whatever the source says.
    std::string    encoded = EncodeTextureBinary( Cook( 16, 16, SyntheticRGBA8( 16, 16 ) ) );
    const uint64_t lie     = 12345;
    std::memcpy( encoded.data() + 64, &lie, sizeof( lie ) ); // PayloadBytes

    const auto read = DecodeTextureBinary( encoded, "miscounted.tex" );
    EXPECT_FALSE( read.IsSuccess() );
    EXPECT_NE( read.GetError().find( "level table adds up" ), std::string::npos ) << read.GetError();
}

TEST( TextureBinaryFormat, AVersionThisBuildDoesNotReadIsRefusedByName )
{
    std::string encoded = EncodeTextureBinary( Cook( 8, 8, SyntheticRGBA8( 8, 8 ) ) );

    const uint32_t future = kTextureBinaryVersion + 1;
    std::memcpy( encoded.data() + 12, &future, sizeof( future ) ); // magic(8) + byte order(4)

    const auto read = DecodeTextureBinary( encoded, "future.tex" );
    EXPECT_FALSE( read.IsSuccess() );
    EXPECT_NE( read.GetError().find( "version" ), std::string::npos ) << read.GetError();
    EXPECT_NE( read.GetError().find( "Re-cook" ), std::string::npos )
         << "a version refusal has to name the remedy: " << read.GetError();
}

TEST( TextureBinaryFormat, AForeignByteOrderIsRefusedByName )
{
    std::string encoded = EncodeTextureBinary( Cook( 8, 8, SyntheticRGBA8( 8, 8 ) ) );

    const uint32_t swapped = 0x01020304u; // what a big-endian host's tag reads back as here
    std::memcpy( encoded.data() + 8, &swapped, sizeof( swapped ) );

    const auto read = DecodeTextureBinary( encoded, "bigendian.tex" );
    EXPECT_FALSE( read.IsSuccess() );
    EXPECT_NE( read.GetError().find( "byte order" ), std::string::npos ) << read.GetError();
}

// ── 6. THE RETIRED MANIFEST IS REFUSED BY NAME, NOT PARSED ─────────────────────────────────────────

TEST( TextureBinaryFormat, TheRetiredJsonManifestIsRefusedWithItsRemedy )
{
    // Byte for byte what `Editor/Cooked/Textures/T_Checker.tex` held before this change.
    const std::string manifest =
         R"({"Handle":4588246833979984450,"SourcePath":"assets:Textures/T_Checker.png","Width":1024,)"
         R"("Height":1024,"Channels":4,"Format":"RGBA8F"})";

    EXPECT_FALSE( LooksLikeTextureBinary( manifest ) );

    const auto read = DecodeTextureBinary( manifest, "old.tex" );
    ASSERT_FALSE( read.IsSuccess() );
    EXPECT_NE( read.GetError().find( "cook again" ), std::string::npos )
         << "a stale cook must not read like a corrupt file: " << read.GetError();
}

// ── 7. THE HEADER ALONE ANSWERS THE METADATA QUESTIONS ─────────────────────────────────────────────

TEST( TextureBinaryFormat, TheHeaderPrefixAnswersWithoutThePayload )
{
    const uint32_t w = 128, h = 64;
    const auto     data    = Cook( w, h, SyntheticRGBA8( w, h ) );
    const auto     encoded = EncodeTextureBinary( data );

    const uint64_t needed = TextureBinaryMetadataBytes( encoded );
    ASSERT_GT( needed, 0u );
    ASSERT_LT( needed, encoded.size() ) << "the metadata cannot need the whole file";

    const auto fromPrefix = DecodeTextureHeader( std::string_view( encoded ).substr( 0, needed ), "prefix" );
    ASSERT_TRUE( fromPrefix.IsSuccess() ) << fromPrefix.GetError();

    EXPECT_EQ( static_cast<uint64_t>( fromPrefix.GetValue().Handle ), static_cast<uint64_t>( data.Handle ) );
    EXPECT_EQ( fromPrefix.GetValue().SourcePath, data.SourcePath );
    EXPECT_EQ( fromPrefix.GetValue().SourceContentHash, data.SourceContentHash );
    EXPECT_EQ( fromPrefix.GetValue().Width, w );
    EXPECT_EQ( fromPrefix.GetValue().Height, h );
    EXPECT_EQ( fromPrefix.GetValue().LevelCount, data.Levels.size() );
    EXPECT_EQ( fromPrefix.GetValue().FileSize, encoded.size() );
    uint64_t sumOfLevels = 0;
    for ( const TextureLevel& level : data.Levels )
        sumOfLevels += level.ByteSize;
    EXPECT_EQ( fromPrefix.GetValue().PayloadBytes, sumOfLevels )
         << "the declared payload total is a second statement of the level table's own sum";

    // ONE BYTE SHORT OF WHAT IT NEEDS IS A REFUSAL, not a header with an empty source key. A prefix
    // reader that silently accepted a short key would hand back a texture whose source is "" — and
    // "" resolves to nothing, silently, which is the empty successful answer the contract forbids.
    const auto tooShort = DecodeTextureHeader( std::string_view( encoded ).substr( 0, needed - 1 ), "short" );
    EXPECT_FALSE( tooShort.IsSuccess() ) << "a prefix one byte short of the source key was accepted";
}

TEST( TextureBinaryFormat, ASourceKeyLongerThanTheReadWindowStillDecodes )
{
    // The second-read branch in TextureAsset exists for this file, so the file has to exist. A key
    // this long is not something an artist produces, which is exactly why the branch would otherwise
    // never be executed by anything.
    const std::string longKey = "assets:Textures/" + std::string( kTextureBinaryPrefixBytes, 'd' ) + ".png";
    const auto        encoded = EncodeTextureBinary( Cook( 4, 4, SyntheticRGBA8( 4, 4 ), longKey ) );

    const uint64_t needed = TextureBinaryMetadataBytes( encoded );
    EXPECT_GT( needed, kTextureBinaryPrefixBytes )
         << "the fixture no longer exceeds the read window, so it stopped testing the branch";

    const auto read = DecodeTextureBinary( encoded, "longkey.tex" );
    ASSERT_TRUE( read.IsSuccess() ) << read.GetError();
    EXPECT_EQ( read.GetValue().SourcePath, longKey );
}

// ── 8. THE MIP BUILDER REFUSES WHAT IT CANNOT DO, INSTEAD OF GUESSING ──────────────────────────────

TEST( TextureBinaryFormat, TheMipBuilderRefusesAMismatchedBase )
{
    std::vector<unsigned char> tooSmall( 16 ); // 2x2 worth of bytes for a 64x64 image
    std::vector<unsigned char> out;
    const auto                 chain = BuildMipChain( 64, 64, ImageFormat::RGBA8F, tooSmall, out );
    EXPECT_FALSE( chain.IsSuccess() );
    EXPECT_NE( chain.GetError().find( "16384" ), std::string::npos ) << chain.GetError();
}

TEST( TextureBinaryFormat, TheMipBuilderRefusesAFormatItCannotFilter )
{
    std::vector<unsigned char> base( 8ull * 8ull * 8ull ); // RGBA16F would be 8 bytes per pixel
    std::vector<unsigned char> out;
    const auto                 chain = BuildMipChain( 8, 8, ImageFormat::RGBA16F, base, out );
    EXPECT_FALSE( chain.IsSuccess() );
    EXPECT_NE( chain.GetError().find( "RGBA8F and RGBA32F" ), std::string::npos ) << chain.GetError();
}

TEST( TextureBinaryFormat, HdrSourcesKeepTheirRange )
{
    // Four float texels whose values are outside [0,1] — the whole reason an HDR source cannot be
    // stored as RGBA8. The chain has to average them as floats, not as bytes.
    const uint32_t             w = 2, h = 2;
    std::vector<float>         values{ 4.0f, 0.0f, 0.0f,  1.0f, 0.0f, 8.0f, 0.0f, 1.0f,
                               0.0f, 0.0f, 16.0f, 1.0f, 2.0f, 2.0f, 2.0f, 1.0f };
    std::vector<unsigned char> base( values.size() * sizeof( float ) );
    std::memcpy( base.data(), values.data(), base.size() );

    TextureAssetData data;
    data.Width  = w;
    data.Height = h;
    data.Format = ImageFormat::RGBA32F;
    auto chain  = BuildMipChain( w, h, ImageFormat::RGBA32F, base, data.Pixels );
    ASSERT_TRUE( chain.IsSuccess() ) << chain.GetError();
    data.Levels = chain.ExtractValue();

    const auto read = DecodeTextureBinary( EncodeTextureBinary( data ), "hdr" );
    ASSERT_TRUE( read.IsSuccess() ) << read.GetError();
    ASSERT_EQ( read.GetValue().Levels.size(), 2u );

    float mip1[4] = {};
    std::memcpy( mip1, read.GetValue().Pixels.data() + read.GetValue().Levels[1].ByteOffset, sizeof( mip1 ) );
    EXPECT_FLOAT_EQ( mip1[0], ( 4.0f + 0.0f + 0.0f + 2.0f ) / 4.0f );
    EXPECT_FLOAT_EQ( mip1[1], ( 0.0f + 8.0f + 0.0f + 2.0f ) / 4.0f );
    EXPECT_FLOAT_EQ( mip1[2], ( 0.0f + 0.0f + 16.0f + 2.0f ) / 4.0f );
    EXPECT_FLOAT_EQ( mip1[3], 1.0f );
}

// ── 9. THE COMMITTED CORPUS IS CONVERTED ───────────────────────────────────────────────────────────

TEST( TextureBinaryFormat, TheTrackedCheckerTextureIsAContainer )
{
    const auto        path  = RepositoryRoot() / "Editor" / "Cooked" / "Textures" / "T_Checker.tex";
    const std::string bytes = ReadFile( path );
    ASSERT_FALSE( bytes.empty() ) << path.string();

    ASSERT_TRUE( LooksLikeTextureBinary( bytes ) )
         << "the one cooked texture this repository tracks is still the retired manifest; nothing "
            "re-cooks it on a fresh clone, so eleven scenes would draw an untextured floor";

    const auto read = DecodeTextureBinary( bytes, path.string() );
    ASSERT_TRUE( read.IsSuccess() ) << read.GetError();
    EXPECT_EQ( read.GetValue().Width, 1024u );
    EXPECT_EQ( read.GetValue().Height, 1024u );
    EXPECT_EQ( read.GetValue().Format, ImageFormat::RGBA8F );
    EXPECT_EQ( read.GetValue().Levels.size(), 11u ); // floor(log2(1024)) + 1
    EXPECT_EQ( read.GetValue().SourcePath, "assets:Textures/T_Checker.png" );

    // The handle eleven scenes resolve their floor material through. It is derived from the SOURCE
    // image's project-relative key, so this number is the same on every machine.
    EXPECT_EQ( static_cast<uint64_t>( read.GetValue().Handle ), 4588246833979984450ull );
}

// ── 10. THE LEVELS ARE STORED SMALLEST FIRST, AND THAT IS WHAT THE NEXT STEP BUYS ─────────────────

TEST( TextureBinaryFormat, TheResidentTailIsAContiguousPrefixOfTheFile )
{
    // The decision T3 S5c calls the layout's main one. Its whole value is that the small levels — the
    // ones a streaming build keeps resident for every texture so that "an object on screen with no
    // texture" stops being a reachable state — sit at the FRONT, next to the header and the table, and
    // are read by one sequential read. Stored in image order they would be at the end of every file
    // and cost a seek per texture, for ever.
    const uint32_t w = 256, h = 256;
    const auto     data    = Cook( w, h, SyntheticRGBA8( w, h ) );
    const auto     encoded = EncodeTextureBinary( data );

    const auto read = DecodeTextureBinary( encoded, "order" );
    ASSERT_TRUE( read.IsSuccess() ) << read.GetError();
    const auto& levels = read.GetValue().Levels;

    // Table order is MIP order: Levels[0] is the full-size image whatever the bytes do.
    EXPECT_EQ( levels[0].Width, w );
    EXPECT_EQ( levels[0].Height, h );
    EXPECT_EQ( levels.back().Width, 1u );
    EXPECT_EQ( levels.back().Height, 1u );

    // Byte order is the reverse, and every level is 16-byte aligned.
    for ( size_t i = 0; i + 1 < levels.size(); ++i )
    {
        EXPECT_GT( levels[i].ByteOffset, levels[i + 1].ByteOffset )
             << "level " << i << " is stored before level " << ( i + 1 ) << "; the tail is not a prefix";
    }
    for ( const TextureLevel& level : levels )
        EXPECT_EQ( level.ByteOffset % kTextureLevelAlignment, 0u );

    // Everything at 16x16 and below, plus the header and the table, inside one small read.
    uint64_t tailEnd = 0;
    for ( const TextureLevel& level : levels )
        if ( level.Width <= 16 && level.Height <= 16 )
            tailEnd = std::max( tailEnd, level.ByteOffset + level.ByteSize );
    EXPECT_LT( tailEnd, 2048u ) << "the resident tail does not fit in one small read";
}

TEST( TextureBinaryFormat, AFlagOrAnEncoderThisVersionCannotHonourIsRefused )
{
    // Both fields are written as zero by version 1 and both are guards rather than dead space: a file
    // that sets one was made by a build that knows something this one does not, and decoding it anyway
    // would draw the texture in the wrong colour space or out of the wrong bytes -- silently.
    {
        std::string    encoded = EncodeTextureBinary( Cook( 8, 8, SyntheticRGBA8( 8, 8 ) ) );
        const uint32_t srgb    = 1u;
        std::memcpy( encoded.data() + 36, &srgb, sizeof( srgb ) ); // Flags
        const auto read = DecodeTextureBinary( encoded, "flagged.tex" );
        EXPECT_FALSE( read.IsSuccess() );
        EXPECT_NE( read.GetError().find( "content flags" ), std::string::npos ) << read.GetError();
    }
    {
        std::string    encoded  = EncodeTextureBinary( Cook( 8, 8, SyntheticRGBA8( 8, 8 ) ) );
        const uint64_t settings = 0x1122334455667788ull;
        std::memcpy( encoded.data() + 56, &settings, sizeof( settings ) ); // EncoderHash
        const auto read = DecodeTextureBinary( encoded, "encoded.tex" );
        EXPECT_FALSE( read.IsSuccess() );
        EXPECT_NE( read.GetError().find( "encoder settings" ), std::string::npos ) << read.GetError();
    }
}

TEST( TextureBinaryFormat, TheSourceSignatureSeparatesFilesOfTheSameLengthAndOfTheSameContent )
{
    const std::string a = "the same length, different bytes";
    const std::string b = "THE SAME LENGTH, DIFFERENT BYTES";
    ASSERT_EQ( a.size(), b.size() );
    EXPECT_NE( SourceSignature( a.data(), a.size() ), SourceSignature( b.data(), b.size() ) );

    // And the upper half is the length, so two files can never share a signature while differing in
    // size -- the collision class a bare 32-bit CRC leaves open.
    const std::string longer = a + "!";
    EXPECT_NE( SourceSignature( a.data(), a.size() ) >> 32,
               SourceSignature( longer.data(), longer.size() ) >> 32 );
    EXPECT_EQ( SourceSignature( a.data(), a.size() ) >> 32, a.size() );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
