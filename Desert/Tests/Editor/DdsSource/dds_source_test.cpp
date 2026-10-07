// A `.dds` texture source decodes to the image it stands for, or is refused NAMING ITS FORMAT.
//
// Every file here is built in memory, byte by byte, from the DDS layout — no sample assets, so each
// expected texel is derivable from the bytes written next to it.

#include <Editor/Import/DdsSource.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using Desert::Editor::DecodeDdsSource;
using Desert::Editor::IsDdsSource;

namespace
{
    constexpr uint32_t kFourCCFlag = 0x4, kRGBFlag = 0x40, kAlphaFlag = 0x1;

    constexpr uint32_t FourCC( char a, char b, char c, char d )
    {
        return uint32_t( uint8_t( a ) ) | ( uint32_t( uint8_t( b ) ) << 8 ) | ( uint32_t( uint8_t( c ) ) << 16 ) |
               ( uint32_t( uint8_t( d ) ) << 24 );
    }

    void Put( std::vector<unsigned char>& out, std::size_t at, uint32_t v )
    {
        for ( int i = 0; i < 4; ++i )
            out[at + i] = static_cast<unsigned char>( v >> ( 8 * i ) );
    }

    struct Spec
    {
        uint32_t Width = 4, Height = 4, PfFlags = kFourCCFlag, FourCode = 0, Bpp = 0;
        uint32_t R = 0, G = 0, B = 0, A = 0, Caps2 = 0, Depth = 0;
        int64_t  Dxgi = -1; ///< >= 0: write the DX10 header with this DXGI_FORMAT
    };

    std::vector<unsigned char> MakeDds( const Spec& s, const std::vector<unsigned char>& payload )
    {
        const std::size_t          headers = 128 + ( s.Dxgi >= 0 ? 20 : 0 );
        std::vector<unsigned char> out( headers, 0 );
        std::memcpy( out.data(), "DDS ", 4 );
        Put( out, 4, 124 );
        Put( out, 12, s.Height );
        Put( out, 16, s.Width );
        Put( out, 24, s.Depth );
        Put( out, 76, 32 );
        Put( out, 80, s.PfFlags );
        Put( out, 84, s.Dxgi >= 0 ? FourCC( 'D', 'X', '1', '0' ) : s.FourCode );
        Put( out, 88, s.Bpp );
        Put( out, 92, s.R );
        Put( out, 96, s.G );
        Put( out, 100, s.B );
        Put( out, 104, s.A );
        Put( out, 112, s.Caps2 );
        if ( s.Dxgi >= 0 )
        {
            Put( out, 128, static_cast<uint32_t>( s.Dxgi ) );
            Put( out, 132, 3 ); // TEXTURE2D
            Put( out, 140, 1 ); // arraySize
        }
        out.insert( out.end(), payload.begin(), payload.end() );
        return out;
    }

    std::vector<unsigned char> Repeat( const std::vector<unsigned char>& block, int times )
    {
        std::vector<unsigned char> out;
        for ( int i = 0; i < times; ++i )
            out.insert( out.end(), block.begin(), block.end() );
        return out;
    }

    // BC1, colour0 = 0xF800 (pure red in 5:6:5), colour1 = 0x001F (pure blue); colour0 > colour1 is the
    // four-colour mode, and an all-zero index word selects colour0 for every texel.
    const std::vector<unsigned char> kBc1Red = { 0x00, 0xF8, 0x1F, 0x00, 0, 0, 0, 0 };
} // namespace

TEST( DdsSource, Dxt1DecodesAndDropsTheEdgeBlockTexelsOutsideTheImage )
{
    // 6x5: two by two blocks, of which only the first 2 columns / 1 row of the edge blocks are real.
    const auto file =
         MakeDds( { .Width = 6, .Height = 5, .FourCode = FourCC( 'D', 'X', 'T', '1' ) }, Repeat( kBc1Red, 4 ) );
    auto r = DecodeDdsSource( file.data(), file.size() );
    ASSERT_TRUE( r.IsSuccess() ) << r.GetError();
    const auto img = r.ExtractValue();
    EXPECT_EQ( img.Width, 6u );
    EXPECT_EQ( img.Height, 5u );
    EXPECT_FALSE( img.IsFloat );
    EXPECT_EQ( img.Format, "DXT1" );
    ASSERT_EQ( img.Rgba8.size(), 6u * 5u * 4u );
    for ( std::size_t i = 0; i < 30; ++i )
    {
        EXPECT_EQ( img.Rgba8[i * 4 + 0], 255 ) << i;
        EXPECT_EQ( img.Rgba8[i * 4 + 1], 0 ) << i;
        EXPECT_EQ( img.Rgba8[i * 4 + 2], 0 ) << i;
        EXPECT_EQ( img.Rgba8[i * 4 + 3], 255 ) << i;
    }
}

TEST( DdsSource, Bc7Mode6FromTheDx10Header )
{
    // Mode 6 (bit 6), then 56 endpoint bits and two p-bits all ONE — every endpoint is 255 — and the
    // 63 index bits zero: every texel is opaque white.
    std::vector<unsigned char> block( 16, 0 );
    block[0] = 0xC0;
    for ( int i = 1; i < 8; ++i )
        block[i] = 0xFF;
    block[8]        = 0x01;
    const auto file = MakeDds( { .Dxgi = 99 }, block );
    auto       r    = DecodeDdsSource( file.data(), file.size() );
    ASSERT_TRUE( r.IsSuccess() ) << r.GetError();
    const auto img = r.ExtractValue();
    EXPECT_EQ( img.Format, "BC7_UNORM_SRGB (DX10)" );
    for ( unsigned char c : img.Rgba8 )
        EXPECT_EQ( c, 255 );
}

TEST( DdsSource, Bc4IsAGreyImage )
{
    const auto file = MakeDds( { .Dxgi = 80 }, { 200, 10, 0, 0, 0, 0, 0, 0 } );
    auto       r    = DecodeDdsSource( file.data(), file.size() );
    ASSERT_TRUE( r.IsSuccess() ) << r.GetError();
    const auto img = r.ExtractValue();
    for ( std::size_t i = 0; i < 16; ++i )
    {
        EXPECT_EQ( img.Rgba8[i * 4 + 0], 200 );
        EXPECT_EQ( img.Rgba8[i * 4 + 1], 200 );
        EXPECT_EQ( img.Rgba8[i * 4 + 2], 200 );
        EXPECT_EQ( img.Rgba8[i * 4 + 3], 255 );
    }
}

TEST( DdsSource, Bc5ReconstructsTheNormalsZ )
{
    // X = Y = 128 (the flat normal's 0.0039): Z must come back ~1, i.e. blue 255 — not the 0 a GPU
    // sampler would return for the absent third channel.
    const std::vector<unsigned char> half = { 128, 128, 0, 0, 0, 0, 0, 0 };
    const auto file = MakeDds( { .FourCode = FourCC( 'A', 'T', 'I', '2' ) }, Repeat( half, 2 ) );
    auto       r    = DecodeDdsSource( file.data(), file.size() );
    ASSERT_TRUE( r.IsSuccess() ) << r.GetError();
    const auto img = r.ExtractValue();
    EXPECT_EQ( img.Rgba8[0], 128 );
    EXPECT_EQ( img.Rgba8[1], 128 );
    EXPECT_EQ( img.Rgba8[2], 255 );
    EXPECT_EQ( img.Rgba8[3], 255 );
}

TEST( DdsSource, HalfFloatStaysFloat )
{
    // D3DFMT_A16B16G16R16F (113): 1.0, 2.0, 0.5, 1.0 in halves.
    const auto file = MakeDds( { .Width = 1, .Height = 1, .FourCode = 113 },
                               { 0x00, 0x3C, 0x00, 0x40, 0x00, 0x38, 0x00, 0x3C } );
    auto       r    = DecodeDdsSource( file.data(), file.size() );
    ASSERT_TRUE( r.IsSuccess() ) << r.GetError();
    const auto img = r.ExtractValue();
    ASSERT_TRUE( img.IsFloat );
    ASSERT_EQ( img.RgbaF.size(), 4u );
    EXPECT_FLOAT_EQ( img.RgbaF[0], 1.0f );
    EXPECT_FLOAT_EQ( img.RgbaF[1], 2.0f );
    EXPECT_FLOAT_EQ( img.RgbaF[2], 0.5f );
    EXPECT_FLOAT_EQ( img.RgbaF[3], 1.0f );
}

TEST( DdsSource, LegacyA8R8G8B8ReadsByItsMasks )
{
    const auto file = MakeDds( { .Width   = 1,
                                 .Height  = 1,
                                 .PfFlags = kRGBFlag | kAlphaFlag,
                                 .Bpp     = 32,
                                 .R       = 0x00FF0000,
                                 .G       = 0x0000FF00,
                                 .B       = 0x000000FF,
                                 .A       = 0xFF000000 },
                               { 10, 20, 30, 40 } ); // B, G, R, A in memory
    auto       r    = DecodeDdsSource( file.data(), file.size() );
    ASSERT_TRUE( r.IsSuccess() ) << r.GetError();
    const auto img = r.ExtractValue();
    EXPECT_EQ( img.Rgba8, ( std::vector<unsigned char>{ 30, 20, 10, 40 } ) );
}

TEST( DdsSource, RefusalsNameTheFormat )
{
    {
        const auto file = MakeDds( { .Dxgi = 45 }, std::vector<unsigned char>( 64, 0 ) ); // D24_UNORM_S8_UINT
        auto       r    = DecodeDdsSource( file.data(), file.size() );
        ASSERT_FALSE( r.IsSuccess() );
        EXPECT_NE( r.GetError().find( "DXGI_FORMAT 45" ), std::string::npos ) << r.GetError();
    }
    {
        const auto file =
             MakeDds( { .FourCode = FourCC( 'E', 'T', 'C', '1' ) }, std::vector<unsigned char>( 8, 0 ) );
        auto r = DecodeDdsSource( file.data(), file.size() );
        ASSERT_FALSE( r.IsSuccess() );
        EXPECT_NE( r.GetError().find( "'ETC1'" ), std::string::npos ) << r.GetError();
    }
    {
        // 8x8 DXT1 needs four blocks; three are present.
        const auto file = MakeDds( { .Width = 8, .Height = 8, .FourCode = FourCC( 'D', 'X', 'T', '1' ) },
                                   Repeat( kBc1Red, 3 ) );
        auto       r    = DecodeDdsSource( file.data(), file.size() );
        ASSERT_FALSE( r.IsSuccess() );
        EXPECT_NE( r.GetError().find( "DXT1" ), std::string::npos ) << r.GetError();
        EXPECT_NE( r.GetError().find( "truncated" ), std::string::npos ) << r.GetError();
    }
    {
        const auto file = MakeDds( { .FourCode = FourCC( 'D', 'X', 'T', '1' ), .Caps2 = 0x200000, .Depth = 4 },
                                   Repeat( kBc1Red, 4 ) );
        auto       r    = DecodeDdsSource( file.data(), file.size() );
        ASSERT_FALSE( r.IsSuccess() );
        EXPECT_NE( r.GetError().find( "volume" ), std::string::npos ) << r.GetError();
    }
}

TEST( DdsSource, RecognisedByMagicOrSpelling )
{
    EXPECT_TRUE( IsDdsSource( std::string_view( "DDS \x7c", 5 ), "Textures/a.png" ) );
    EXPECT_TRUE( IsDdsSource( "garbage", "Textures/a.DDS" ) );
    EXPECT_FALSE( IsDdsSource( "\x89PNG", "Textures/a.png" ) );
}
