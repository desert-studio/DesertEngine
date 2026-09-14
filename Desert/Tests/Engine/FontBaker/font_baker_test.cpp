#include <Engine/Text/FontBaker.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace
{
    // Roboto ships with the editor resources — a real Latin font to bake against.
    std::vector<uint8_t> LoadRoboto()
    {
        // The test binary runs from the workspace root (RunTests.sh), so the resource path is stable.
        const char* candidates[] = { "Editor/Resources/Fonts/Roboto-Regular.ttf",
                                     "../../../Editor/Resources/Fonts/Roboto-Regular.ttf" };
        for ( const char* c : candidates )
        {
            std::ifstream f( c, std::ios::binary );
            if ( !f )
                continue;
            return std::vector<uint8_t>( ( std::istreambuf_iterator<char>( f ) ),
                                         std::istreambuf_iterator<char>() );
        }
        return {};
    }
} // namespace

TEST( FontBaker, BakesRobotoMsdfAtlas )
{
    const auto ttf = LoadRoboto();
    ASSERT_FALSE( ttf.empty() ) << "Roboto-Regular.ttf not found (run from workspace root)";

    const auto font = Desert::Text::BakeFontMSDF( ttf.data(), ttf.size(), 48.0f );
    ASSERT_TRUE( font.Valid() );

    // Vertical metrics are sane (ascent up, descent down, positive line height).
    EXPECT_GT( font.Ascent, 0.0f );
    EXPECT_LT( font.Descent, 0.0f );
    EXPECT_GT( font.LineHeight(), 0.0f );

    // Printable ASCII was baked; 'A' has a real bitmap cell + a positive advance.
    ASSERT_TRUE( font.Glyphs.count( 'A' ) );
    const auto& A = font.Glyphs.at( 'A' );
    EXPECT_GT( A.Advance, 0.0f );
    EXPECT_GT( A.Width, 0.0f );
    EXPECT_GT( A.Height, 0.0f );
    EXPECT_LE( A.U1, 1.0f );
    EXPECT_LE( A.V1, 1.0f );
    EXPECT_LT( A.U0, A.U1 );

    // Space has an advance but no atlas cell.
    ASSERT_TRUE( font.Glyphs.count( ' ' ) );
    EXPECT_GT( font.Glyphs.at( ' ' ).Advance, 0.0f );
    EXPECT_FLOAT_EQ( font.Glyphs.at( ' ' ).Width, 0.0f );

    // The atlas is four channels wide and its size agrees with its own header.
    EXPECT_EQ( font.AtlasRGBA.size(), static_cast<size_t>( font.AtlasWidth ) * font.AtlasHeight * 4 );

    // The atlas actually contains ink: at least one texel reads as inside the outline.
    bool anyInk = false;
    for ( size_t i = 0; i + 3 < font.AtlasRGBA.size(); i += 4 )
    {
        const uint8_t r = font.AtlasRGBA[i], g = font.AtlasRGBA[i + 1], b = font.AtlasRGBA[i + 2];
        const uint8_t med = std::max( std::min( r, g ), std::min( std::max( r, g ), b ) );
        if ( med > 128 )
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
    const auto font = Desert::Text::BakeFontMSDF( ttf.data(), ttf.size(), 48.0f );
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
    ASSERT_TRUE( restored.Glyphs.count( 'A' ) );
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
    ASSERT_GT( blob.size(), 8u );

    // The four bytes after the magic are the version; stamp the previous generation into them.
    const uint32_t older = Desert::Text::kBakedFontCacheVersion - 1;
    std::memcpy( blob.data() + 4, &older, sizeof( older ) );

    Desert::Text::BakedFont out;
    uint32_t                seen = 0;
    EXPECT_EQ( Desert::Text::DeserializeBakedFont( blob.data(), blob.size(), out, &seen ),
               FontDecodeStatus::VersionMismatch );
    EXPECT_EQ( seen, older );
    EXPECT_FALSE( out.Valid() ) << "a refused atlas must leave the output untouched, not half-filled";
}

TEST( FontBaker, BakesRequestedNonAsciiCodepoints )
{
    // Cyrillic (and anything else outside ASCII) only reaches the atlas when it is asked for — this is
    // what makes "Привет" render instead of a row of blanks.
    const auto ttf = LoadRoboto();
    ASSERT_FALSE( ttf.empty() );

    const std::vector<uint32_t> cyrillic = { 0x041F, 0x0440, 0x0438, 0x0432, 0x0435, 0x0442 }; // Привет

    const Desert::Text::BakedFont ascii = Desert::Text::BakeFontMSDF( ttf.data(), ttf.size() );
    ASSERT_TRUE( ascii.Valid() );
    for ( uint32_t cp : cyrillic )
        EXPECT_EQ( ascii.Glyphs.count( cp ), 0u ) << "unrequested glyph baked";

    const Desert::Text::BakedFont full = Desert::Text::BakeFontMSDF( ttf.data(), ttf.size(), 48.0f, cyrillic );
    ASSERT_TRUE( full.Valid() );
    for ( uint32_t cp : cyrillic )
    {
        ASSERT_EQ( full.Glyphs.count( cp ), 1u ) << "missing U+" << std::hex << cp;
        const auto& g = full.Glyphs.at( cp );
        EXPECT_GT( g.Width, 0.0f );
        EXPECT_GT( g.Advance, 0.0f );
    }
    EXPECT_EQ( full.Glyphs.size(), ascii.Glyphs.size() + cyrillic.size() );

    // A codepoint the font has no glyph for is skipped, not an error.
    const Desert::Text::BakedFont missing =
         Desert::Text::BakeFontMSDF( ttf.data(), ttf.size(), 48.0f, { 0x1F600 } ); // emoji
    ASSERT_TRUE( missing.Valid() );
    EXPECT_EQ( missing.Glyphs.size(), ascii.Glyphs.size() );
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
