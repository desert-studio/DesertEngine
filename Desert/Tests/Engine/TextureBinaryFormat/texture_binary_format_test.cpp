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

// For the compression threshold, which the container shares with the archive rather than restating.
#include <Common/Utilities/PakFile.hpp>

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

    /// A deterministic image the codec can do something with. `SyntheticRGBA8` is deliberately
    /// noise-like — that is what makes it a good test of a box filter — and noise is exactly what the
    /// compression threshold refuses, so a test about the codec needs its own corpus.
    std::vector<unsigned char> CompressibleRGBA8( const uint32_t width, const uint32_t height )
    {
        std::vector<unsigned char> pixels( static_cast<size_t>( width ) * height * 4 );
        for ( uint32_t y = 0; y < height; ++y )
        {
            for ( uint32_t x = 0; x < width; ++x )
            {
                const size_t at = ( static_cast<size_t>( y ) * width + x ) * 4;
                // Broad bands rather than a flat fill: a mip chain of one colour is a chain the filter
                // could get wrong in every level identically and still look compressible.
                const unsigned char band = static_cast<unsigned char>( ( ( x / 32u ) & 1u ) ? 0xE0u : 0x20u );
                pixels[at + 0]           = band;
                pixels[at + 1]           = static_cast<unsigned char>( ( y / 32u ) & 1u ? 0xC0u : 0x40u );
                pixels[at + 2]           = 0x80u;
                pixels[at + 3]           = 0xFFu;
            }
        }
        return pixels;
    }

    /// The width of one level row, READ OUT OF THE FILE. The table lies between the header and the
    /// source key, so the stride is that span over the level count — a number the file states rather
    /// than one a test remembers and stops being right about.
    size_t RowStride( const std::string& encoded )
    {
        uint32_t levelCount = 0, layerCount = 0, keyOffset = 0;
        std::memcpy( &levelCount, encoded.data() + 32, sizeof( levelCount ) ); // FileHeader::LevelCount
        std::memcpy( &keyOffset, encoded.data() + 44, sizeof( keyOffset ) );   // FileHeader::SourceKeyOffset
        std::memcpy( &layerCount, encoded.data() + 96, sizeof( layerCount ) ); // FileHeader::LayerCount (v3)
        EXPECT_GT( levelCount, 0u );
        EXPECT_GT( layerCount, 0u );
        const uint64_t rows = static_cast<uint64_t>( levelCount ) * layerCount;
        return rows == 0 ? 0 : static_cast<size_t>( ( keyOffset - kTextureBinaryHeaderSize ) / rows );
    }

    /// A cube whose six faces are all DIFFERENT, so a reader that dropped one, permuted two or wrote
    /// the same face six times cannot pass. `SyntheticRGBA8` is seeded per face for exactly that.
    std::vector<unsigned char> SyntheticFace( const uint32_t side, const uint32_t face )
    {
        std::vector<unsigned char> pixels( static_cast<size_t>( side ) * side * 4 );
        for ( uint32_t y = 0; y < side; ++y )
        {
            for ( uint32_t x = 0; x < side; ++x )
            {
                const size_t at = ( static_cast<size_t>( y ) * side + x ) * 4;
                pixels[at + 0]  = static_cast<unsigned char>( ( x * 7u + y * 13u + face * 41u ) & 0xFFu );
                pixels[at + 1]  = static_cast<unsigned char>( ( ( x ^ y ) + face ) & 0xFFu );
                pixels[at + 2]  = static_cast<unsigned char>( face * 40u );
                pixels[at + 3]  = 0xFFu;
            }
        }
        return pixels;
    }

    /// Every level of every face, tightly packed in TABLE ORDER — level 0's six faces, then level 1's.
    /// This is the shape a cube arrives in from the device (`VulkanImageCube::RT_ReadAllLevels`), so the
    /// tests below drive `BuildLevelTable` with the same layout the engine hands it.
    std::vector<unsigned char> TightCubeChain( const uint32_t faceSize, const uint32_t levelCount )
    {
        std::vector<unsigned char> out;
        uint32_t                   side = faceSize;
        for ( uint32_t level = 0; level < levelCount; ++level )
        {
            for ( uint32_t face = 0; face < kTextureCubeLayerCount; ++face )
            {
                // The seed carries the LEVEL as well, so a chain that placed a level's faces at
                // another level's offsets is a chain the identity check below cannot survive.
                const auto one = SyntheticFace( side, face + level * 11u );
                out.insert( out.end(), one.begin(), one.end() );
            }
            side = side > 1u ? side / 2u : 1u;
        }
        return out;
    }

    TextureAssetData CookCube( const uint32_t faceSize, const uint32_t levelCount,
                               const std::string& key = "assets:Textures/Env.hdr" )
    {
        TextureAssetData data;
        data.Handle            = Common::UUID( 0x0123456789ABCDEFull );
        data.SourcePath        = key;
        data.SourceContentHash = 0x00001234DEADBEEFull;
        data.Width             = faceSize;
        data.Height            = faceSize;
        data.LayerCount        = kTextureCubeLayerCount;
        data.Kind              = TextureKind::Cube;
        data.Format            = ImageFormat::RGBA8F;

        const auto tight = TightCubeChain( faceSize, levelCount );
        auto table = BuildLevelTable( faceSize, faceSize, levelCount, kTextureCubeLayerCount, ImageFormat::RGBA8F,
                                      tight, data.Pixels );
        EXPECT_TRUE( table.IsSuccess() ) << table.GetError();
        data.Levels = table.ExtractValue();
        return data;
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
        data.LayerCount        = 1;
        data.Kind              = TextureKind::Texture2D;
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

    // Level 1's ByteOffset is the first field of the second row. THE ROW'S WIDTH IS DERIVED FROM THE
    // FILE, not remembered here: the table runs from the end of the header to the source key, so the
    // stride is that span over the level count. A literal 16 was right for v1 and silently addressed
    // the middle of row 0 the day the row grew.
    const size_t rowStride   = RowStride( encoded );
    const size_t offsetField = kTextureBinaryHeaderSize + rowStride;
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
    // BC7 SINCE T3.4, AND THE FILE IS WHERE THAT IS TRUE OR NOT. The cook encodes an LDR texture to
    // BC7 when it can show the result holds up, and this texture clears both of its gates -- measured
    // over the whole chain at cook time, 54.02 dB with a worst texel off by 4. A format assertion here
    // is what stops "the encoder exists" from being mistaken for "the shipped content uses it": the
    // one cooked texture this repository tracks is the whole shipped corpus.
    EXPECT_EQ( read.GetValue().Format, ImageFormat::BC7_UNORM );
    EXPECT_TRUE( Desert::Core::Formats::IsBlockCompressed( read.GetValue().Format ) );
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

    // AND THE SAME CLAIM ABOUT THE FILE, which is the one that actually matters and which the loop
    // above cannot make: `TextureLevel::ByteOffset` is an offset into the DECODED buffer. Since v2
    // those are two different layouts, so the property has to be asserted in both spellings or it is
    // being asserted about the wrong bytes.
    const auto header = DecodeTextureHeader( encoded, "order" );
    ASSERT_TRUE( header.IsSuccess() ) << header.GetError();
    const auto& located = header.GetValue().Levels;
    ASSERT_EQ( located.size(), levels.size() );
    for ( size_t i = 0; i + 1 < located.size(); ++i )
    {
        EXPECT_GT( located[i].FileOffset, located[i + 1].FileOffset )
             << "level " << i << " lies before level " << ( i + 1 ) << " IN THE FILE";
    }
    uint64_t fileTailEnd = 0;
    for ( size_t i = 0; i < located.size(); ++i )
        if ( levels[i].Width <= 16 && levels[i].Height <= 16 )
            fileTailEnd = std::max( fileTailEnd, located[i].FileOffset + located[i].StoredSize );
    EXPECT_LE( fileTailEnd, kTextureBinaryPrefixBytes )
         << "the resident tail no longer fits in the prefix a loader reads first";
}

// ── 11. PER-LEVEL COMPRESSION ──────────────────────────────────────────────────────────────────────
//
// WHY THE LEVEL AND NOT THE FILE. A `.tex` is a mip chain of raw pixels, and raw pixels compress: the
// one texture this repository tracks goes from 5 592 752 bytes to 65 825 under the archive's own codec,
// which is 98.8 % against a threshold of 37.5 %. So the question was never whether the container would
// be compressed — it was WHERE, and compressing the ENTRY brings back exactly what the container was
// written to cure: to read mip 4 you must decode everything. The codec therefore lives in the level
// row, and the archive is told to keep its hands off the file (`Common/Utilities/PakFile.hpp`,
// kStoredVerbatimRules, pinned by `Desert/Tests/Common/Pak`).

TEST( TextureBinaryFormat, CompressionIsTransparentToTheRoundTrip )
{
    // THE RELATION, not the bytes: the two encodings are DIFFERENT FILES and must decode to the SAME
    // texture, field for field and byte for byte. A codec that is merely present would pass a test
    // that only checked the compressed file round-trips against itself.
    const uint32_t w = 128, h = 96;
    const auto     data = Cook( w, h, CompressibleRGBA8( w, h ) );

    const std::string packed = EncodeTextureBinary( data, TextureEncodeOptions{ true } );
    const std::string plain  = EncodeTextureBinary( data, TextureEncodeOptions{ false } );
    EXPECT_LT( packed.size(), plain.size() ) << "compression produced a file no smaller than the raw one";
    EXPECT_NE( packed, plain );

    const auto fromPacked = DecodeTextureBinary( packed, "packed.tex" );
    const auto fromPlain  = DecodeTextureBinary( plain, "plain.tex" );
    ASSERT_TRUE( fromPacked.IsSuccess() ) << fromPacked.GetError();
    ASSERT_TRUE( fromPlain.IsSuccess() ) << fromPlain.GetError();

    const auto& a = fromPacked.GetValue();
    const auto& b = fromPlain.GetValue();
    ASSERT_EQ( a.Levels.size(), b.Levels.size() );
    for ( size_t i = 0; i < a.Levels.size(); ++i )
    {
        EXPECT_EQ( a.Levels[i].Width, b.Levels[i].Width ) << "level " << i;
        EXPECT_EQ( a.Levels[i].Height, b.Levels[i].Height ) << "level " << i;
        EXPECT_EQ( a.Levels[i].ByteOffset, b.Levels[i].ByteOffset ) << "level " << i;
        EXPECT_EQ( a.Levels[i].ByteSize, b.Levels[i].ByteSize ) << "level " << i;
        EXPECT_EQ( a.Levels[i].RowPitch, b.Levels[i].RowPitch ) << "level " << i;
    }
    ASSERT_EQ( a.Pixels.size(), b.Pixels.size() );
    EXPECT_EQ( std::memcmp( a.Pixels.data(), b.Pixels.data(), a.Pixels.size() ), 0 );

    // And against the input, so "the same as each other" cannot mean "the same wrong thing".
    ASSERT_EQ( a.Pixels.size(), data.Pixels.size() );
    EXPECT_EQ( std::memcmp( a.Pixels.data(), data.Pixels.data(), a.Pixels.size() ), 0 );

    // THE OTHER END OF THE POLICY. On a corpus the codec cannot help — `SyntheticRGBA8` is noise by
    // design — no level clears the threshold, so asking for compression must produce the SAME FILE as
    // not asking. A codec that wrote a different container for the same bytes would mean the cook's
    // output depended on a flag nobody set.
    const auto        noisy    = Cook( w, h, SyntheticRGBA8( w, h ) );
    const std::string asked    = EncodeTextureBinary( noisy, TextureEncodeOptions{ true } );
    const std::string notAsked = EncodeTextureBinary( noisy, TextureEncodeOptions{ false } );
    EXPECT_EQ( asked, notAsked ) << "nothing in this image compresses, so the two encodings must agree";
}

TEST( TextureBinaryFormat, EveryLevelDecidesItsOwnCodecAndBothVerdictsHappen )
{
    // A large, very compressible image, so the big levels clear the threshold and the tail of the chain
    // cannot: a 1x1 level is four bytes and no codec shrinks four bytes. That is the whole reason the
    // column is per level, and a chain where every level agreed would leave it untested.
    const uint32_t             w = 256, h = 256;
    std::vector<unsigned char> flat( static_cast<size_t>( w ) * h * 4, 0u );
    for ( size_t at = 0; at < flat.size(); at += 4 )
    {
        flat[at + 0] = 0x40;
        flat[at + 1] = 0x80;
        flat[at + 2] = 0xC0;
        flat[at + 3] = 0xFF;
    }

    const std::string encoded = EncodeTextureBinary( Cook( w, h, flat ) );
    const auto        header  = DecodeTextureHeader( encoded, "mixed.tex" );
    ASSERT_TRUE( header.IsSuccess() ) << header.GetError();
    const auto& levels = header.GetValue().Levels;

    size_t compressed = 0, stored = 0;
    for ( size_t i = 0; i < levels.size(); ++i )
    {
        if ( levels[i].Codec == TextureLevelCodec::LZ4 )
        {
            ++compressed;
            // The threshold is a RELATION and is asserted as one, against the archive's constants
            // rather than a number remembered here.
            EXPECT_LE( static_cast<uint64_t>( levels[i].StoredSize ) * Common::Utils::kCompressionDenominator,
                       static_cast<uint64_t>( levels[i].ByteSize ) * Common::Utils::kCompressionNumerator )
                 << "level " << i << " was kept compressed without clearing the threshold";
        }
        else
        {
            ++stored;
            EXPECT_EQ( levels[i].StoredSize, levels[i].ByteSize ) << "level " << i << " is stored and short";
        }
    }
    EXPECT_GT( compressed, 0u ) << "nothing compressed; the codec path was never taken";
    EXPECT_GT( stored, 0u ) << "everything compressed; the stored path was never taken";

    // The two declared totals are the two things the file holds and the buffer needs, and they are not
    // the same number once anything compressed.
    uint64_t declared = 0, storedSum = 0;
    for ( const TextureLevelLocation& level : levels )
    {
        declared += level.ByteSize;
        storedSum += level.StoredSize;
    }
    EXPECT_EQ( header.GetValue().PayloadBytes, declared );
    EXPECT_EQ( header.GetValue().StoredPayloadBytes, storedSum );
    EXPECT_LT( header.GetValue().StoredPayloadBytes, header.GetValue().PayloadBytes );
}

TEST( TextureBinaryFormat, TheFileChainsOnStoredSizesAndTheBufferChainsOnDecodedOnes )
{
    // THE MIDDLE LINK. Both ends of this format look right whichever size the chain is built on, and a
    // decoder that chained the file on the DECODED sizes would validate a compressed container only by
    // accident — namely when nothing compressed. So both layouts are walked here, independently.
    const uint32_t    w = 64, h = 64;
    const auto        data    = Cook( w, h, SyntheticRGBA8( w, h ) );
    const std::string encoded = EncodeTextureBinary( data );

    const auto header = DecodeTextureHeader( encoded, "chain.tex" );
    ASSERT_TRUE( header.IsSuccess() ) << header.GetError();
    const auto decoded = DecodeTextureBinary( encoded, "chain.tex" );
    ASSERT_TRUE( decoded.IsSuccess() ) << decoded.GetError();

    const auto& located = header.GetValue().Levels;
    const auto& levels  = decoded.GetValue().Levels;
    ASSERT_EQ( located.size(), levels.size() );

    const auto alignUp = []( const uint64_t at ) { return ( at + 15u ) & ~static_cast<uint64_t>( 15u ); };

    uint64_t fileAt   = located.back().FileOffset; // physical order: the smallest level is first
    uint64_t bufferAt = 0;
    for ( size_t i = 0; i < located.size(); ++i )
    {
        const size_t level = located.size() - 1 - i;
        EXPECT_EQ( located[level].FileOffset, fileAt ) << "level " << level << " is not where the STORED "
                                                       << "sizes put it in the file";
        EXPECT_EQ( levels[level].ByteOffset, bufferAt ) << "level " << level << " is not where the DECODED "
                                                        << "sizes put it in the buffer";
        fileAt   = alignUp( fileAt + located[level].StoredSize );
        bufferAt = alignUp( bufferAt + located[level].ByteSize );
    }

    // The file ends at the last stored level, with no trailing pad.
    EXPECT_EQ( located.front().FileOffset + located.front().StoredSize, header.GetValue().FileSize );
    EXPECT_EQ( header.GetValue().FileSize, encoded.size() );
}

TEST( TextureBinaryFormat, ALevelCodecThisVersionDoesNotKnowIsRefused )
{
    std::string encoded = EncodeTextureBinary( Cook( 8, 8, SyntheticRGBA8( 8, 8 ) ) );
    // The Codec column of the first row: header (128) + 20 bytes into a 24-byte row.
    const uint32_t future = 7u;
    std::memcpy( encoded.data() + 128 + 20, &future, sizeof( future ) );

    const auto read = DecodeTextureBinary( encoded, "futurecodec.tex" );
    EXPECT_FALSE( read.IsSuccess() );
    EXPECT_NE( read.GetError().find( "codec 7" ), std::string::npos ) << read.GetError();
}

TEST( TextureBinaryFormat, AStoredLevelThatIsShorterThanItsPixelsIsRefused )
{
    std::string encoded =
         EncodeTextureBinary( Cook( 8, 8, SyntheticRGBA8( 8, 8 ) ), TextureEncodeOptions{ false } );
    // The StoredSize column of the first row, made to disagree with ByteSize while the codec still
    // says Store. Under a reader that took StoredSize on trust this is a texture whose level 0 is
    // partly whatever followed it.
    const uint32_t shorter = 16u;
    std::memcpy( encoded.data() + 128 + 16, &shorter, sizeof( shorter ) );

    const auto read = DecodeTextureBinary( encoded, "shortstore.tex" );
    EXPECT_FALSE( read.IsSuccess() );
    EXPECT_NE( read.GetError().find( "stored uncompressed" ), std::string::npos ) << read.GetError();
}

TEST( TextureBinaryFormat, ACompressedLevelThatDoesNotDecodeIsAFailedReadAndNotAShortLevel )
{
    const uint32_t             w = 64, h = 64;
    std::vector<unsigned char> flat( static_cast<size_t>( w ) * h * 4, 0x5Au );
    std::string                encoded = EncodeTextureBinary( Cook( w, h, flat ) );

    const auto header = DecodeTextureHeader( encoded, "block.tex" );
    ASSERT_TRUE( header.IsSuccess() ) << header.GetError();
    ASSERT_EQ( header.GetValue().Levels[0].Codec, TextureLevelCodec::LZ4 )
         << "level 0 of a flat image did not compress; this test has nothing to corrupt";

    // Zero the block. An LZ4 token of 0x00 asks for a match at offset 0, which no block may name, so
    // the decoder refuses rather than producing something of the right length out of nothing.
    const auto& level = header.GetValue().Levels[0];
    std::fill_n( encoded.data() + level.FileOffset, level.StoredSize, '\0' );

    const auto read = DecodeTextureBinary( encoded, "block.tex" );
    EXPECT_FALSE( read.IsSuccess() );
    EXPECT_NE( read.GetError().find( "did not decode" ), std::string::npos ) << read.GetError();
}

TEST( TextureBinaryFormat, TheTrackedCheckerTextureCarriesItsLevelsCompressed )
{
    // THE COMMITTED FILE IS THE CLAIM. A format that can compress and a cook that never does are the
    // same thing on disk, and the disk is what ships.
    const auto        path  = RepositoryRoot() / "Editor" / "Cooked" / "Textures" / "T_Checker.tex";
    const std::string bytes = ReadFile( path );
    ASSERT_FALSE( bytes.empty() ) << path.string();

    const auto header = DecodeTextureHeader( bytes, path.string() );
    ASSERT_TRUE( header.IsSuccess() ) << header.GetError();

    size_t compressed = 0;
    for ( const TextureLevelLocation& level : header.GetValue().Levels )
        if ( level.Codec == TextureLevelCodec::LZ4 )
            ++compressed;
    EXPECT_GT( compressed, 0u ) << "the tracked texture was cooked with every level stored; the cook is "
                                   "no longer compressing what it ships";

    EXPECT_LT( header.GetValue().StoredPayloadBytes, header.GetValue().PayloadBytes );
    EXPECT_EQ( header.GetValue().FileSize, bytes.size() );

    // The declared decoded total, derived here rather than remembered: eleven levels of a 1024x1024
    // chain, PADDING EXCLUDED — which is why it is not the size of the pixel buffer.
    //
    // IN BLOCKS, AND THE SUM IS NOT A QUARTER OF THE OLD ONE. A 1x1 BC7 level is a WHOLE 16-byte block
    // and so are 2x2 and 4x4, so the smallest five levels of this chain occupy the same bytes as one
    // 4x4 level each. Writing `w * h * 4 / 4` here would be short by 60 bytes and would still look
    // right; the derivation has to be the format table's, which is what `CalculateImageSize` is.
    const ImageFormat format = header.GetValue().Format;
    uint64_t          chain  = 0;
    for ( uint32_t w = 1024, h = 1024;; w = w > 1 ? w / 2 : 1, h = h > 1 ? h / 2 : 1 )
    {
        chain += Desert::Core::Formats::CalculateImageSize( w, h, format );
        if ( w == 1 && h == 1 )
            break;
    }
    EXPECT_EQ( header.GetValue().PayloadBytes, chain );
}

TEST( TextureBinaryFormat, AFlagThisVersionCannotHonourIsRefused )
{
    // `Flags` is written as zero by every version so far and it is a guard rather than dead space: a
    // file that sets one was made by a build that knows something this one does not -- an sRGB marking
    // it would have to honour -- and decoding it anyway would draw the texture in the wrong colour
    // space, silently.
    std::string    encoded = EncodeTextureBinary( Cook( 8, 8, SyntheticRGBA8( 8, 8 ) ) );
    const uint32_t srgb    = 1u;
    std::memcpy( encoded.data() + 36, &srgb, sizeof( srgb ) ); // Flags
    const auto read = DecodeTextureBinary( encoded, "flagged.tex" );
    EXPECT_FALSE( read.IsSuccess() );
    EXPECT_NE( read.GetError().find( "content flags" ), std::string::npos ) << read.GetError();
}

TEST( TextureBinaryFormat, AnEncoderSignatureIsCarriedRatherThanRefused )
{
    // ITS SIBLING WENT THE OTHER WAY IN v3, AND THE PAIR IS THE POINT. `Flags` above says something the
    // READER would have to honour, so an unknown value must stop it. `EncoderHash` says what the cook's
    // settings were, which only a caller with an expectation can judge -- so the container carries it
    // and refuses nothing. A texture importer records 0 and this asserts both halves of that.
    TextureAssetData data = Cook( 8, 8, SyntheticRGBA8( 8, 8 ) );
    EXPECT_EQ( data.EncoderHash, 0ull ) << "a plain cook records no settings";

    data.EncoderHash = 0x1122334455667788ull;
    const auto read  = DecodeTextureBinary( EncodeTextureBinary( data ), "baked.tex" );
    ASSERT_TRUE( read.IsSuccess() ) << read.GetError();
    EXPECT_EQ( read.GetValue().EncoderHash, 0x1122334455667788ull );

    const auto header = DecodeTextureHeader( EncodeTextureBinary( data ), "baked.tex" );
    ASSERT_TRUE( header.IsSuccess() ) << header.GetError();
    EXPECT_EQ( header.GetValue().EncoderHash, 0x1122334455667788ull )
         << "a reader that only reads the prefix must be able to ask whether the bake is the one it "
            "wants, without reading a single pixel";
}

TEST( TextureBinaryFormat, TheAuthoredIntentIsCarriedAndAnUnknownOneIsRefused )
{
    // THE THIRD MEMBER OF THE `Flags` / `EncoderHash` PAIR, and it behaves like `Flags`: an intent this
    // build does not know was written by a NEWER cook, and reading it as some other enumerator would
    // give the file a meaning nobody wrote.
    TextureAssetData data = Cook( 8, 8, SyntheticRGBA8( 8, 8 ) );
    EXPECT_EQ( data.Intent, Desert::Core::Formats::TextureIntent::Unspecified )
         << "a texture nobody authored an intent for records none";

    data.Intent     = Desert::Core::Formats::TextureIntent::NormalMap;
    const auto read = DecodeTextureBinary( EncodeTextureBinary( data ), "normal.tex" );
    ASSERT_TRUE( read.IsSuccess() ) << read.GetError();
    EXPECT_EQ( read.GetValue().Intent, Desert::Core::Formats::TextureIntent::NormalMap );

    const auto header = DecodeTextureHeader( EncodeTextureBinary( data ), "normal.tex" );
    ASSERT_TRUE( header.IsSuccess() ) << header.GetError();
    EXPECT_EQ( header.GetValue().Intent, Desert::Core::Formats::TextureIntent::NormalMap )
         << "the intent is header-only information and must survive a prefix read";

    std::string    encoded = EncodeTextureBinary( data );
    const uint32_t future  = Desert::Core::Formats::kTextureIntentCount + 3u;
    std::memcpy( encoded.data() + 104, &future, sizeof( future ) ); // Intent
    const auto refused = DecodeTextureBinary( encoded, "fromthefuture.tex" );
    EXPECT_FALSE( refused.IsSuccess() );
    EXPECT_NE( refused.GetError().find( "texture intent" ), std::string::npos ) << refused.GetError();
}

TEST( TextureBinaryFormat, AnUnauthoredFileIsBYTEForBYTEWhatItWasBeforeTheIntentFieldExisted )
{
    // THIS IS THE WHOLE MIGRATION, AND IT IS EMPTY ON PURPOSE. The intent spent a reserved word rather
    // than moving the container's version, which is only defensible if zero — the value every `.tex`
    // ever written already holds there — is a DEFINED enumerator meaning exactly what the cook writes
    // for a texture nobody classified. So an old file and a new unauthored one have to be the same
    // bytes, and this is what says so: the reserve is still all zeros at the word the intent took, and
    // the file's length has not moved.
    //
    // It is asserted against the literal 128-byte header rather than against a stored fixture, because
    // a fixture would be a second copy of the format that this suite would then be testing instead of
    // the format.
    const TextureAssetData data    = Cook( 16, 16, SyntheticRGBA8( 16, 16 ) );
    const std::string      encoded = EncodeTextureBinary( data );
    ASSERT_GE( encoded.size(), 128u );

    for ( std::size_t at = 104; at < 128; ++at )
    {
        EXPECT_EQ( static_cast<unsigned char>( encoded[at] ), 0u )
             << "byte " << at
             << " of the header is not zero; an unauthored cook must be indistinguishable "
                "from one written before the intent field existed";
    }

    // And the one committed cooked texture in this repository still decodes, with no intent, through the
    // reader that now knows about the field. If this fails the field was NOT free and every `.tex` in
    // the tree needs re-cooking.
    const std::filesystem::path tracked = RepositoryRoot() / "Editor" / "Cooked" / "Textures" / "T_Checker.tex";
    if ( std::filesystem::exists( tracked ) )
    {
        const auto committed = DecodeTextureHeader( ReadFile( tracked ), tracked.string() );
        ASSERT_TRUE( committed.IsSuccess() ) << committed.GetError();
        EXPECT_EQ( committed.GetValue().Intent, Desert::Core::Formats::TextureIntent::Unspecified )
             << "the committed cook predates the field and must read back as 'nobody said'";
    }
}

// ── 6. A LEVEL IS A LEVEL OF EVERY LAYER (v3) ──────────────────────────────────────────────────────

TEST( TextureBinaryFormat, ACubeRoundTripsWithEveryFaceDistinct )
{
    const uint32_t face = 32, levels = 6;
    const auto     source = CookCube( face, levels );
    const auto     read   = DecodeTextureBinary( EncodeTextureBinary( source ), "env.tex" );
    ASSERT_TRUE( read.IsSuccess() ) << read.GetError();
    const TextureAssetData& back = read.GetValue();

    EXPECT_EQ( back.Kind, TextureKind::Cube );
    EXPECT_EQ( back.LayerCount, kTextureCubeLayerCount );
    EXPECT_EQ( back.LevelCount(), levels );
    EXPECT_EQ( back.Levels.size(), static_cast<size_t>( levels ) * kTextureCubeLayerCount );

    // FACE BY FACE, NOT BLOB BY BLOB. Comparing the payloads alone would pass for a container that
    // stored the six faces in the wrong order, because the same bytes are all there.
    for ( uint32_t level = 0; level < levels; ++level )
    {
        for ( uint32_t layer = 0; layer < kTextureCubeLayerCount; ++layer )
        {
            const size_t row = TextureLevelIndex( level, layer, kTextureCubeLayerCount );
            ASSERT_EQ( back.Levels[row].Width, source.Levels[row].Width ) << level << "/" << layer;
            ASSERT_EQ( back.Levels[row].ByteSize, source.Levels[row].ByteSize ) << level << "/" << layer;
            ASSERT_EQ( std::memcmp( back.Pixels.data() + back.Levels[row].ByteOffset,
                                    source.Pixels.data() + source.Levels[row].ByteOffset,
                                    static_cast<size_t>( source.Levels[row].ByteSize ) ),
                       0 )
                 << "level " << level << " face " << layer << " came back as different pixels";
        }
    }
}

TEST( TextureBinaryFormat, TheSixFacesOfALevelSitTogetherAndTheSmallestLevelComesFirst )
{
    // THE RELATION THE RESIDENT TAIL DEPENDS ON. It is not enough that the file holds all 6N images:
    // the property the container exists for is that the smallest levels are a CONTIGUOUS PREFIX, and
    // for a cube that means all six faces of level N-1 before any face of level N-2.
    const uint32_t face = 32, levels = 6;
    const auto     header = DecodeTextureHeader( EncodeTextureBinary( CookCube( face, levels ) ), "env.tex" );
    ASSERT_TRUE( header.IsSuccess() ) << header.GetError();
    const auto& table = header.GetValue().Levels;

    // The run starts where the header, the table and the source key end -- the first row's own offset,
    // taken from the file rather than recomputed here.
    uint64_t previousEnd = table[TextureLevelIndex( levels - 1, 0, kTextureCubeLayerCount )].FileOffset;
    for ( uint32_t i = 0; i < levels; ++i )
    {
        const uint32_t level = levels - 1 - i; // physical order
        for ( uint32_t layer = 0; layer < kTextureCubeLayerCount; ++layer )
        {
            const auto& row = table[TextureLevelIndex( level, layer, kTextureCubeLayerCount )];
            EXPECT_GE( row.FileOffset, previousEnd )
                 << "level " << level << " face " << layer << " starts before the previous one ended";
            EXPECT_LT( row.FileOffset - previousEnd, kTextureLevelAlignment )
                 << "level " << level << " face " << layer
                 << " leaves more than a level boundary's worth of bytes nobody reads";
            previousEnd = row.FileOffset + row.StoredSize;
        }
    }

    // And the tail really is a prefix: the header, the table, the key and the smallest level's six
    // faces are all inside one read window.
    const auto& smallest =
         table[TextureLevelIndex( levels - 1, kTextureCubeLayerCount - 1, kTextureCubeLayerCount )];
    EXPECT_LE( smallest.FileOffset + smallest.StoredSize, kTextureBinaryPrefixBytes )
         << "the smallest level of a cube no longer fits in the one read a loader makes";
}

TEST( TextureBinaryFormat, TheLastBytesOfACubeBelongToTheLastFaceOfLevelZero )
{
    // THE ONE LINE THIS CHANGE COULD HAVE DROPPED A PROPERTY IN. The truncation check asks where the
    // file's last stored bytes are; with one layer that was always row 0, and a cube's last row is 5.
    // A reader that kept `front()` would declare every healthy cube short by five faces -- and would
    // ACCEPT a cube whose last five faces had been cut off, which is the direction that draws.
    const auto encoded = EncodeTextureBinary( CookCube( 16, 5 ) );
    const auto header  = DecodeTextureHeader( encoded, "env.tex" );
    ASSERT_TRUE( header.IsSuccess() ) << header.GetError();
    const auto& last =
         header.GetValue().Levels[TextureLevelIndex( 0, kTextureCubeLayerCount - 1, kTextureCubeLayerCount )];
    EXPECT_EQ( last.FileOffset + last.StoredSize, encoded.size() );

    const auto& firstFaceOfLevelZero = header.GetValue().Levels[TextureLevelIndex( 0, 0, kTextureCubeLayerCount )];
    EXPECT_LT( firstFaceOfLevelZero.FileOffset + firstFaceOfLevelZero.StoredSize, encoded.size() )
         << "face 0 of level 0 ends the file, so five faces are missing and nothing said so";
}

TEST( TextureBinaryFormat, ACubeThatIsNotSixSquareFacesIsRefusedByName )
{
    // Each of these is a shape the header can SPELL and the sampler cannot use, so each has to be
    // refused where the numbers are still in hand rather than at a descriptor.
    {
        TextureAssetData data    = CookCube( 16, 5 );
        std::string      encoded = EncodeTextureBinary( data );
        const uint32_t   five    = 5u;
        std::memcpy( encoded.data() + 96, &five, sizeof( five ) ); // FileHeader::LayerCount
        const auto read = DecodeTextureBinary( encoded, "fiveface.tex" );
        EXPECT_FALSE( read.IsSuccess() );
        EXPECT_NE( read.GetError().find( "a cube is 6 layers" ), std::string::npos ) << read.GetError();
    }
    {
        std::string    encoded = EncodeTextureBinary( CookCube( 16, 5 ) );
        const uint32_t eight   = 8u;
        std::memcpy( encoded.data() + 24, &eight, sizeof( eight ) ); // FileHeader::Height
        const auto read = DecodeTextureBinary( encoded, "oblong.tex" );
        EXPECT_FALSE( read.IsSuccess() );
        EXPECT_NE( read.GetError().find( "a cube face is square" ), std::string::npos ) << read.GetError();
    }
    {
        // Six layers that do not say they are a cube, and a 2D texture that says it has six.
        std::string    encoded = EncodeTextureBinary( Cook( 8, 8, SyntheticRGBA8( 8, 8 ) ) );
        const uint32_t six     = 6u;
        std::memcpy( encoded.data() + 96, &six, sizeof( six ) ); // LayerCount, Kind still Texture2D
        const auto read = DecodeTextureBinary( encoded, "arrayish.tex" );
        EXPECT_FALSE( read.IsSuccess() );
        EXPECT_NE( read.GetError().find( "one layer per 2D texture" ), std::string::npos ) << read.GetError();
    }
    {
        std::string    encoded = EncodeTextureBinary( Cook( 8, 8, SyntheticRGBA8( 8, 8 ) ) );
        const uint32_t alien   = 7u;
        std::memcpy( encoded.data() + 100, &alien, sizeof( alien ) ); // FileHeader::Kind
        const auto read = DecodeTextureBinary( encoded, "alien.tex" );
        EXPECT_FALSE( read.IsSuccess() );
        EXPECT_NE( read.GetError().find( "not one this version knows" ), std::string::npos ) << read.GetError();
    }
}

TEST( TextureBinaryFormat, EveryFaceOfEveryLevelDecidesItsOwnCodec )
{
    // The per-level codec column is now a per-(level, face) column, and it has to be: face 0 of a
    // 1024-face environment cube compresses and the 1x1 tail of every face does not.
    TextureAssetData data;
    data.SourcePath = "assets:Textures/Env.hdr";
    data.Width = data.Height = 32;
    data.LayerCount          = kTextureCubeLayerCount;
    data.Kind                = TextureKind::Cube;
    data.Format              = ImageFormat::RGBA8F;

    const uint32_t             levels = 6;
    std::vector<unsigned char> tight;
    uint32_t                   side = 32;
    for ( uint32_t level = 0; level < levels; ++level )
    {
        for ( uint32_t face = 0; face < kTextureCubeLayerCount; ++face )
        {
            const auto one = CompressibleRGBA8( side, side );
            tight.insert( tight.end(), one.begin(), one.end() );
        }
        side = side > 1u ? side / 2u : 1u;
    }
    auto table =
         BuildLevelTable( 32, 32, levels, kTextureCubeLayerCount, ImageFormat::RGBA8F, tight, data.Pixels );
    ASSERT_TRUE( table.IsSuccess() ) << table.GetError();
    data.Levels = table.ExtractValue();

    const auto header = DecodeTextureHeader( EncodeTextureBinary( data ), "env.tex" );
    ASSERT_TRUE( header.IsSuccess() ) << header.GetError();

    int compressed = 0, stored = 0;
    for ( const auto& row : header.GetValue().Levels )
        ( row.Codec == TextureLevelCodec::LZ4 ? compressed : stored )++;
    EXPECT_GT( compressed, 0 ) << "no face repaid its codec, so this corpus proves nothing";
    EXPECT_GT( stored, 0 ) << "every face compressed, so the per-row verdict was never exercised";

    // And it is still transparent: the decoded cube is the cube that went in.
    const auto read = DecodeTextureBinary( EncodeTextureBinary( data ), "env.tex" );
    ASSERT_TRUE( read.IsSuccess() ) << read.GetError();
    EXPECT_EQ( read.GetValue().Pixels, data.Pixels );
}

TEST( TextureBinaryFormat, TheLevelPlacerRefusesAnInputThatIsNotTheShapeItWasToldAbout )
{
    std::vector<unsigned char> payload;
    {
        // One face short -- the exact mistake a readback that forgot a layer would make.
        auto tight = TightCubeChain( 16, 5 );
        tight.resize( tight.size() - 4u );
        const auto table =
             BuildLevelTable( 16, 16, 5, kTextureCubeLayerCount, ImageFormat::RGBA8F, tight, payload );
        EXPECT_FALSE( table.IsSuccess() );
        EXPECT_NE( table.GetError().find( "tightly packed" ), std::string::npos ) << table.GetError();
    }
    {
        // More levels than the face has.
        const auto table = BuildLevelTable( 16, 16, 9, kTextureCubeLayerCount, ImageFormat::RGBA8F,
                                            TightCubeChain( 16, 9 ), payload );
        EXPECT_FALSE( table.IsSuccess() );
        EXPECT_NE( table.GetError().find( "at most 5 levels" ), std::string::npos ) << table.GetError();
    }
    {
        // And the size it demands is the size it publishes, so a caller sizing a GPU readback with
        // `TightlyPackedChainBytes` cannot be handed a refusal for having believed it.
        EXPECT_EQ( TightlyPackedChainBytes( 16, 16, 5, kTextureCubeLayerCount, ImageFormat::RGBA8F ),
                   TightCubeChain( 16, 5 ).size() );
    }
}

TEST( TextureBinaryFormat, HowManyLevelsAFileOwesDependsOnItsKind )
{
    // THE RULE THAT WAS A 2D RULE APPLIED TO EVERYTHING. A cooked PNG owes its whole chain, because the
    // resident tail is what this container exists for. A baked environment cube owes the levels it was
    // baked with: `kSkyEnvRadianceMips` is 1 on a 1024 face by measurement, and the prefiltered cube's
    // levels are a GGX roughness ramp rather than a minification chain. Demanding a full chain of a cube
    // refused the first environment this container ever wrote, on its own second load.
    {
        // One level of a 32-texel face -- legal, and the shape the radiance cube actually has.
        const auto read = DecodeTextureBinary( EncodeTextureBinary( CookCube( 32, 1 ) ), "radiance.tex" );
        ASSERT_TRUE( read.IsSuccess() ) << read.GetError();
        EXPECT_EQ( read.GetValue().LevelCount(), 1u );
    }
    {
        // ... and the DEVICE's bound still holds: more levels than the face supports is an invalid
        // vkCreateImage, so it is refused here where the numbers are in hand.
        // Shrink the FACE rather than grow the table: the table's length is checked against the source
        // key's offset before this, so the only way to reach the level bound is a file whose extent
        // cannot carry the levels it declares -- which is exactly the corruption it guards.
        std::string    encoded = EncodeTextureBinary( CookCube( 16, 5 ) );
        const uint32_t four    = 4u;
        std::memcpy( encoded.data() + 20, &four, sizeof( four ) ); // FileHeader::Width
        std::memcpy( encoded.data() + 24, &four, sizeof( four ) ); // FileHeader::Height
        const auto read = DecodeTextureBinary( encoded, "toomany.tex" );
        EXPECT_FALSE( read.IsSuccess() );
        EXPECT_NE( read.GetError().find( "supports at most 3" ), std::string::npos ) << read.GetError();
    }
    {
        // A 2D texture keeps the stricter rule, and losing it is the thing this test exists to stop.
        TextureAssetData data = Cook( 32, 32, SyntheticRGBA8( 32, 32 ) );
        data.Levels.resize( 3 );
        data.Pixels.resize( static_cast<size_t>( data.Levels[2].ByteOffset + data.Levels[2].ByteSize ) );
        const auto read = DecodeTextureBinary( EncodeTextureBinary( data ), "partial.tex" );
        EXPECT_FALSE( read.IsSuccess() );
        EXPECT_NE( read.GetError().find( "The cook is partial" ), std::string::npos ) << read.GetError();
    }
}

TEST( TextureBinaryFormat, AOneLayerTextureIsLaidOutExactlyAsItWasBeforeLayersExisted )
{
    // THE REGRESSION THIS BUMP COULD HAVE CAUSED SILENTLY. Every 2D texture in the project is one
    // layer, so `TextureLevelIndex(level, 0, 1) == level` and the payload must be byte-for-byte what
    // the level-only container produced. Asserted as a RELATION against the placer, which knows
    // nothing about mip filtering, rather than against a remembered byte string.
    const uint32_t w = 32, h = 16;
    const auto     base = SyntheticRGBA8( w, h );

    std::vector<unsigned char> viaChain;
    const auto                 chain = BuildMipChain( w, h, ImageFormat::RGBA8F, base, viaChain );
    ASSERT_TRUE( chain.IsSuccess() ) << chain.GetError();

    // The same levels, handed to the placer tightly packed in table order.
    std::vector<unsigned char> tight;
    for ( const auto& level : chain.GetValue() )
    {
        tight.insert( tight.end(), viaChain.begin() + static_cast<long>( level.ByteOffset ),
                      viaChain.begin() + static_cast<long>( level.ByteOffset + level.ByteSize ) );
    }

    std::vector<unsigned char> viaPlacer;
    const auto placed = BuildLevelTable( w, h, static_cast<uint32_t>( chain.GetValue().size() ), 1u,
                                         ImageFormat::RGBA8F, tight, viaPlacer );
    ASSERT_TRUE( placed.IsSuccess() ) << placed.GetError();
    EXPECT_EQ( viaPlacer, viaChain ) << "the two layouts disagree, so one of them is not the file's";

    for ( size_t level = 0; level < chain.GetValue().size(); ++level )
    {
        EXPECT_EQ( TextureLevelIndex( static_cast<uint32_t>( level ), 0u, 1u ), level );
        EXPECT_EQ( placed.GetValue()[level].ByteOffset, chain.GetValue()[level].ByteOffset );
        EXPECT_EQ( placed.GetValue()[level].RowPitch, chain.GetValue()[level].RowPitch );
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
