#include <Engine/Text/FontBaker.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <ios>
#include <iterator>
#include <string>
#include <vector>

namespace
{
    constexpr float  kBakeSize       = 48.0F;
    constexpr size_t kFontMagicBytes = 4; // "DFNT", and then the format version
    // A texel reads as inside the outline above this; the atlas encodes the edge one byte above it.
    constexpr uint8_t kInsideByte = 128;

    // Roboto ships with the editor resources — a real Latin font to bake against.
    std::vector<uint8_t> LoadRoboto()
    {
        // The test binary runs from the workspace root (RunTests.sh), so the resource path is stable.
        const std::vector<std::string> candidates = { "Editor/Resources/Fonts/Roboto-Regular.ttf",
                                                      "../../../Editor/Resources/Fonts/Roboto-Regular.ttf" };
        for ( const std::string& candidate : candidates )
        {
            std::ifstream file( candidate, std::ios::binary );
            if ( !file )
            {
                continue;
            }
            return { std::istreambuf_iterator<char>( file ), std::istreambuf_iterator<char>() };
        }
        return {};
    }

    uint8_t MedianByte( const std::vector<uint8_t>& atlas, size_t texel )
    {
        const uint8_t red   = atlas[( texel * 4 ) + 0];
        const uint8_t green = atlas[( texel * 4 ) + 1];
        const uint8_t blue  = atlas[( texel * 4 ) + 2];
        return std::max( std::min( red, green ), std::min( std::max( red, green ), blue ) );
    }
} // namespace

TEST( FontBaker, VerticalMetricsAreSane )
{
    const auto ttf = LoadRoboto();
    ASSERT_FALSE( ttf.empty() ) << "Roboto-Regular.ttf not found (run from workspace root)";
    const auto font = Desert::Text::BakeFontMSDF( ttf.data(), ttf.size(), kBakeSize );
    ASSERT_TRUE( font.Valid() );

    // Ascent up, descent down, positive line height.
    EXPECT_GT( font.Ascent, 0.0F );
    EXPECT_LT( font.Descent, 0.0F );
    EXPECT_GT( font.LineHeight(), 0.0F );
}

TEST( FontBaker, PrintableAsciiGetsCellsAndAdvances )
{
    const auto ttf = LoadRoboto();
    ASSERT_FALSE( ttf.empty() );
    const auto font = Desert::Text::BakeFontMSDF( ttf.data(), ttf.size(), kBakeSize );
    ASSERT_TRUE( font.Valid() );

    ASSERT_TRUE( font.Glyphs.contains( 'A' ) );
    const auto& capitalA = font.Glyphs.at( 'A' );
    EXPECT_GT( capitalA.Advance, 0.0F );
    EXPECT_GT( capitalA.Width, 0.0F );
    EXPECT_GT( capitalA.Height, 0.0F );
    EXPECT_LE( capitalA.U1, 1.0F );
    EXPECT_LE( capitalA.V1, 1.0F );
    EXPECT_LT( capitalA.U0, capitalA.U1 );

    // Space has an advance but no atlas cell.
    ASSERT_TRUE( font.Glyphs.contains( ' ' ) );
    EXPECT_GT( font.Glyphs.at( ' ' ).Advance, 0.0F );
    EXPECT_FLOAT_EQ( font.Glyphs.at( ' ' ).Width, 0.0F );
}

TEST( FontBaker, AtlasIsFourChannelsAndCarriesInk )
{
    const auto ttf = LoadRoboto();
    ASSERT_FALSE( ttf.empty() );
    const auto font = Desert::Text::BakeFontMSDF( ttf.data(), ttf.size(), kBakeSize );
    ASSERT_TRUE( font.Valid() );

    // The atlas's size agrees with its own header.
    ASSERT_EQ( font.AtlasRGBA.size(), static_cast<size_t>( font.AtlasWidth ) * font.AtlasHeight * 4 );

    // And it actually contains ink: at least one texel reads as inside the outline.
    bool         anyInk = false;
    const size_t texels = font.AtlasRGBA.size() / 4;
    for ( size_t texel = 0; texel < texels; ++texel )
    {
        if ( MedianByte( font.AtlasRGBA, texel ) > kInsideByte )
        {
            anyInk = true;
            break;
        }
    }
    EXPECT_TRUE( anyInk );
}

TEST( FontBaker, RejectsGarbage )
{
    const std::vector<uint8_t> junk( 128, 0xAB );
    EXPECT_FALSE( Desert::Text::BakeFontMSDF( junk.data(), junk.size() ).Valid() );
    EXPECT_FALSE( Desert::Text::BakeFontMSDF( nullptr, 0 ).Valid() );
}

TEST( FontBaker, SerializeRoundTrip )
{
    const auto ttf = LoadRoboto();
    ASSERT_FALSE( ttf.empty() );
    const auto font = Desert::Text::BakeFontMSDF( ttf.data(), ttf.size(), kBakeSize );
    ASSERT_TRUE( font.Valid() );

    const std::vector<uint8_t> blob = Desert::Text::SerializeBakedFont( font );
    ASSERT_FALSE( blob.empty() );

    Desert::Text::BakedFont restored;
    ASSERT_EQ( Desert::Text::DeserializeBakedFont( blob.data(), blob.size(), restored ),
               Desert::Text::FontDecodeStatus::Ok );

    // Structural equality: metrics, atlas dimensions/pixels, and every glyph survive the round trip.
    EXPECT_EQ( restored.AtlasWidth, font.AtlasWidth );
    EXPECT_EQ( restored.AtlasHeight, font.AtlasHeight );
    EXPECT_FLOAT_EQ( restored.PixelHeight, font.PixelHeight );
    EXPECT_FLOAT_EQ( restored.Ascent, font.Ascent );
    EXPECT_FLOAT_EQ( restored.Descent, font.Descent );
    EXPECT_FLOAT_EQ( restored.DistanceRangeTexels, font.DistanceRangeTexels );
    EXPECT_EQ( restored.AtlasRGBA, font.AtlasRGBA );
    ASSERT_EQ( restored.Glyphs.size(), font.Glyphs.size() );
    ASSERT_TRUE( restored.Glyphs.contains( 'A' ) );
    EXPECT_FLOAT_EQ( restored.Glyphs.at( 'A' ).Advance, font.Glyphs.at( 'A' ).Advance );
    EXPECT_FLOAT_EQ( restored.Glyphs.at( 'A' ).U1, font.Glyphs.at( 'A' ).U1 );
}

TEST( FontBaker, DeserializeRejectsCorruptBlob )
{
    using Desert::Text::FontDecodeStatus;
    Desert::Text::BakedFont out;
    // Empty, too-short, and wrong-magic inputs must all fail cleanly (never over-read).
    EXPECT_EQ( Desert::Text::DeserializeBakedFont( nullptr, 0, out ), FontDecodeStatus::BadMagic );
    const std::vector<uint8_t> junk( 32, 0xCD );
    EXPECT_EQ( Desert::Text::DeserializeBakedFont( junk.data(), junk.size(), out ), FontDecodeStatus::BadMagic );

    // A valid blob truncated to half its length is corrupt, not a crash — and not a version problem.
    const auto ttf = LoadRoboto();
    ASSERT_FALSE( ttf.empty() );
    const auto blob = Desert::Text::SerializeBakedFont( Desert::Text::BakeFontMSDF( ttf.data(), ttf.size() ) );
    EXPECT_EQ( Desert::Text::DeserializeBakedFont( blob.data(), blob.size() / 2, out ),
               FontDecodeStatus::Corrupt );
}

// An atlas of the PREVIOUS format generation must be told apart from a corrupt one and from a cold
// cache, and must hand back the version it actually carries — that number is what lets FontCache name
// the file instead of silently re-baking it. Before this change the version lived in the cache KEY, so
// an old atlas was not refused at all: it became unreachable, stayed on disk forever, and a packaged
// game full of them paid the bake at every start with nothing in the log to say why.
TEST( FontBaker, DeserializeNamesTheVersionOfAnOlderAtlas )
{
    using Desert::Text::FontDecodeStatus;
    const auto ttf = LoadRoboto();
    ASSERT_FALSE( ttf.empty() );
    std::vector<uint8_t> blob =
         Desert::Text::SerializeBakedFont( Desert::Text::BakeFontMSDF( ttf.data(), ttf.size() ) );
    ASSERT_GT( blob.size(), kFontMagicBytes + sizeof( uint32_t ) );

    // The four bytes after the magic are the version; stamp the previous generation into them.
    const uint32_t older = Desert::Text::kBakedFontCacheVersion - 1;
    // The version is the four bytes after the magic; std::copy over a span keeps the write inside the
    // container rather than off a raw pointer.
    const auto stamp = std::bit_cast<std::array<uint8_t, sizeof( older )>>( older );
    std::copy( stamp.begin(), stamp.end(), std::next( blob.begin(), kFontMagicBytes ) );

    Desert::Text::BakedFont out;
    uint32_t                seen = 0;
    EXPECT_EQ( Desert::Text::DeserializeBakedFont( blob.data(), blob.size(), out, &seen ),
               FontDecodeStatus::VersionMismatch );
    EXPECT_EQ( seen, older );
    EXPECT_FALSE( out.Valid() ) << "a refused atlas must leave the output untouched, not half-filled";
}

namespace
{
    // Does every one of these codepoints have a real atlas cell and an advance?
    bool EveryCodepointHasACell( const Desert::Text::BakedFont& font, const std::vector<uint32_t>& codes )
    {
        return std::ranges::all_of( codes,
                                    [&font]( uint32_t code )
                                    {
                                        const auto found = font.Glyphs.find( code );
                                        return found != font.Glyphs.end() && found->second.Width > 0.0F &&
                                               found->second.Advance > 0.0F;
                                    } );
    }

    // Привет — six Cyrillic codepoints, the case that makes "ask for what the string uses" concrete.
    const std::vector<uint32_t> kCyrillic = { 0x041F, 0x0440, 0x0438, 0x0432, 0x0435, 0x0442 };
} // namespace

// Cyrillic (and anything else outside ASCII) only reaches the atlas when it is asked for — this is what
// makes "Привет" render instead of a row of blanks.
TEST( FontBaker, UnrequestedNonAsciiIsNotBaked )
{
    const auto ttf = LoadRoboto();
    ASSERT_FALSE( ttf.empty() );
    const Desert::Text::BakedFont ascii = Desert::Text::BakeFontMSDF( ttf.data(), ttf.size() );
    ASSERT_TRUE( ascii.Valid() );
    for ( const uint32_t code : kCyrillic )
    {
        EXPECT_FALSE( ascii.Glyphs.contains( code ) ) << "unrequested glyph baked";
    }
}

TEST( FontBaker, RequestedNonAsciiIsBaked )
{
    const auto ttf = LoadRoboto();
    ASSERT_FALSE( ttf.empty() );
    const Desert::Text::BakedFont ascii = Desert::Text::BakeFontMSDF( ttf.data(), ttf.size() );
    const Desert::Text::BakedFont full =
         Desert::Text::BakeFontMSDF( ttf.data(), ttf.size(), kBakeSize, kCyrillic );
    ASSERT_TRUE( full.Valid() );

    EXPECT_TRUE( EveryCodepointHasACell( full, kCyrillic ) );
    EXPECT_EQ( full.Glyphs.size(), ascii.Glyphs.size() + kCyrillic.size() );
}

TEST( FontBaker, ACodepointTheFontLacksIsSkippedNotFailed )
{
    const auto ttf = LoadRoboto();
    ASSERT_FALSE( ttf.empty() );
    const Desert::Text::BakedFont ascii = Desert::Text::BakeFontMSDF( ttf.data(), ttf.size() );
    const Desert::Text::BakedFont missing =
         Desert::Text::BakeFontMSDF( ttf.data(), ttf.size(), kBakeSize, { 0x1F600 } ); // emoji
    ASSERT_TRUE( missing.Valid() );
    EXPECT_EQ( missing.Glyphs.size(), ascii.Glyphs.size() );
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
