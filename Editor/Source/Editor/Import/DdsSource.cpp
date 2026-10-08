#include "DdsSource.hpp"

// bcdec is compiled HERE and nowhere else: this is its one consumer, and `BCDEC_STATIC` keeps its symbols
// internal to this translation unit so a second copy elsewhere could never collide at link time.
// BCDEC_BC4BC5_PRECISE is what gives the signed BC4/BC5 variants (and exact rather than approximated
// interpolation for the unsigned ones).
#define BCDEC_STATIC
#define BCDEC_BC4BC5_PRECISE
#define BCDEC_IMPLEMENTATION
// BCDEC_STATIC makes the float BC4/BC5 entry points this file does not call unused statics.
#if defined( __clang__ ) || defined( __GNUC__ )
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#elif defined( _MSC_VER )
#pragma warning( push )
#pragma warning( disable : 4505 )
#endif
#include "../../../../ThirdParty/bcdec/bcdec.h" // vendored, MIT (ThirdParty/bcdec/LICENSE)
#if defined( __clang__ ) || defined( __GNUC__ )
#pragma GCC diagnostic pop
#elif defined( _MSC_VER )
#pragma warning( pop )
#endif

#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <optional>

namespace Desert::Editor
{
    namespace
    {
        // ── THE FILE LAYOUT (Microsoft "DDS_HEADER" / "DDS_HEADER_DXT10"; UE DDSFile.h FDDSHeader) ──────
        constexpr std::size_t kMagicSize      = 4;
        constexpr std::size_t kHeaderSize     = 124;
        constexpr std::size_t kDx10HeaderSize = 20;
        constexpr std::size_t kPixelFormatAt  = kMagicSize + 72; // DDS_PIXELFORMAT inside DDS_HEADER

        constexpr uint32_t kPixelFormatAlphaPixels = 0x1;
        constexpr uint32_t kPixelFormatFourCC      = 0x4;
        constexpr uint32_t kPixelFormatRGB         = 0x40;
        constexpr uint32_t kCaps2Volume            = 0x200000;
        constexpr uint32_t kDx10Texture3D          = 4;

        constexpr uint32_t FourCC( char a, char b, char c, char d )
        {
            return static_cast<uint32_t>( static_cast<unsigned char>( a ) ) |
                   ( static_cast<uint32_t>( static_cast<unsigned char>( b ) ) << 8 ) |
                   ( static_cast<uint32_t>( static_cast<unsigned char>( c ) ) << 16 ) |
                   ( static_cast<uint32_t>( static_cast<unsigned char>( d ) ) << 24 );
        }

        uint32_t ReadU32( const unsigned char* p )
        {
            return static_cast<uint32_t>( p[0] ) | ( static_cast<uint32_t>( p[1] ) << 8 ) |
                   ( static_cast<uint32_t>( p[2] ) << 16 ) | ( static_cast<uint32_t>( p[3] ) << 24 );
        }

        /// How the top mip's bytes are laid out — one decoder per entry, nothing shared by guessing.
        enum class Layout
        {
            BC1,
            BC2,
            BC3,
            BC4U,
            BC4S,
            BC5U,
            BC5S,
            BC6HU,
            BC6HS,
            BC7,
            RGBA8,
            BGRA8,
            BGRX8,
            RGBA16UNORM,
            RGBA16F,
            RGBA32F,
            Masked32, ///< DX9 32-bit RGB with arbitrary 8-bit channel masks
        };

        struct Recognised
        {
            Layout      Kind;
            std::string Name;
        };

        bool IsBlock( Layout k )
        {
            return k <= Layout::BC7;
        }

        std::size_t BlockBytes( Layout k )
        {
            return ( k == Layout::BC1 || k == Layout::BC4U || k == Layout::BC4S ) ? 8u : 16u;
        }

        std::size_t TexelBytes( Layout k )
        {
            switch ( k )
            {
                case Layout::RGBA16UNORM:
                case Layout::RGBA16F:
                    return 8;
                case Layout::RGBA32F:
                    return 16;
                default:
                    return 4;
            }
        }

        /// DXGI_FORMAT codes this importer decodes (dxgiformat.h). Every other code is refused BY NUMBER.
        std::optional<Recognised> FromDxgi( uint32_t dxgi )
        {
            switch ( dxgi )
            {
                case 2:
                    return Recognised{ Layout::RGBA32F, "R32G32B32A32_FLOAT" };
                case 10:
                    return Recognised{ Layout::RGBA16F, "R16G16B16A16_FLOAT" };
                case 11:
                    return Recognised{ Layout::RGBA16UNORM, "R16G16B16A16_UNORM" };
                case 27:
                    return Recognised{ Layout::RGBA8, "R8G8B8A8_TYPELESS" };
                case 28:
                    return Recognised{ Layout::RGBA8, "R8G8B8A8_UNORM" };
                case 29:
                    return Recognised{ Layout::RGBA8, "R8G8B8A8_UNORM_SRGB" };
                case 70:
                    return Recognised{ Layout::BC1, "BC1_TYPELESS" };
                case 71:
                    return Recognised{ Layout::BC1, "BC1_UNORM" };
                case 72:
                    return Recognised{ Layout::BC1, "BC1_UNORM_SRGB" };
                case 73:
                    return Recognised{ Layout::BC2, "BC2_TYPELESS" };
                case 74:
                    return Recognised{ Layout::BC2, "BC2_UNORM" };
                case 75:
                    return Recognised{ Layout::BC2, "BC2_UNORM_SRGB" };
                case 76:
                    return Recognised{ Layout::BC3, "BC3_TYPELESS" };
                case 77:
                    return Recognised{ Layout::BC3, "BC3_UNORM" };
                case 78:
                    return Recognised{ Layout::BC3, "BC3_UNORM_SRGB" };
                case 79:
                    return Recognised{ Layout::BC4U, "BC4_TYPELESS" };
                case 80:
                    return Recognised{ Layout::BC4U, "BC4_UNORM" };
                case 81:
                    return Recognised{ Layout::BC4S, "BC4_SNORM" };
                case 82:
                    return Recognised{ Layout::BC5U, "BC5_TYPELESS" };
                case 83:
                    return Recognised{ Layout::BC5U, "BC5_UNORM" };
                case 84:
                    return Recognised{ Layout::BC5S, "BC5_SNORM" };
                case 87:
                    return Recognised{ Layout::BGRA8, "B8G8R8A8_UNORM" };
                case 88:
                    return Recognised{ Layout::BGRX8, "B8G8R8X8_UNORM" };
                case 90:
                    return Recognised{ Layout::BGRA8, "B8G8R8A8_TYPELESS" };
                case 91:
                    return Recognised{ Layout::BGRA8, "B8G8R8A8_UNORM_SRGB" };
                case 92:
                    return Recognised{ Layout::BGRX8, "B8G8R8X8_TYPELESS" };
                case 93:
                    return Recognised{ Layout::BGRX8, "B8G8R8X8_UNORM_SRGB" };
                case 94:
                    return Recognised{ Layout::BC6HU, "BC6H_TYPELESS" };
                case 95:
                    return Recognised{ Layout::BC6HU, "BC6H_UF16" };
                case 96:
                    return Recognised{ Layout::BC6HS, "BC6H_SF16" };
                case 97:
                    return Recognised{ Layout::BC7, "BC7_TYPELESS" };
                case 98:
                    return Recognised{ Layout::BC7, "BC7_UNORM" };
                case 99:
                    return Recognised{ Layout::BC7, "BC7_UNORM_SRGB" };
                default:
                    return std::nullopt;
            }
        }

        /// The DX9 four-character codes, and the D3DFMT numbers legacy writers put in the same field
        /// (UE DDSFile.cpp's FourCC table is the list of spellings honoured).
        std::optional<Recognised> FromFourCC( uint32_t code )
        {
            if ( code == FourCC( 'D', 'X', 'T', '1' ) )
                return Recognised{ Layout::BC1, "DXT1" };
            if ( code == FourCC( 'D', 'X', 'T', '2' ) || code == FourCC( 'D', 'X', 'T', '3' ) )
                return Recognised{ Layout::BC2, code == FourCC( 'D', 'X', 'T', '2' ) ? "DXT2" : "DXT3" };
            if ( code == FourCC( 'D', 'X', 'T', '4' ) || code == FourCC( 'D', 'X', 'T', '5' ) )
                return Recognised{ Layout::BC3, code == FourCC( 'D', 'X', 'T', '4' ) ? "DXT4" : "DXT5" };
            if ( code == FourCC( 'A', 'T', 'I', '1' ) || code == FourCC( 'B', 'C', '4', 'U' ) )
                return Recognised{ Layout::BC4U, code == FourCC( 'A', 'T', 'I', '1' ) ? "ATI1" : "BC4U" };
            if ( code == FourCC( 'B', 'C', '4', 'S' ) )
                return Recognised{ Layout::BC4S, "BC4S" };
            if ( code == FourCC( 'A', 'T', 'I', '2' ) || code == FourCC( 'B', 'C', '5', 'U' ) )
                return Recognised{ Layout::BC5U, code == FourCC( 'A', 'T', 'I', '2' ) ? "ATI2" : "BC5U" };
            if ( code == FourCC( 'B', 'C', '5', 'S' ) )
                return Recognised{ Layout::BC5S, "BC5S" };
            if ( code == FourCC( 'B', 'C', '6', 'H' ) )
                return Recognised{ Layout::BC6HU, "BC6H" };
            if ( code == FourCC( 'B', 'C', '7', 'L' ) || code == FourCC( 'B', 'C', '7', '\0' ) )
                return Recognised{ Layout::BC7, "BC7" };
            if ( code == 36 )
                return Recognised{ Layout::RGBA16UNORM, "D3DFMT_A16B16G16R16" };
            if ( code == 113 )
                return Recognised{ Layout::RGBA16F, "D3DFMT_A16B16G16R16F" };
            if ( code == 116 )
                return Recognised{ Layout::RGBA32F, "D3DFMT_A32B32G32R32F" };
            return std::nullopt;
        }

        /// A FourCC as text when it is printable, as a number when it is a D3DFMT code — the refusal has
        /// to name what the file said, in the spelling a person can look up.
        std::string DescribeFourCC( uint32_t code )
        {
            std::string text;
            for ( int i = 0; i < 4; ++i )
            {
                const auto c = static_cast<unsigned char>( ( code >> ( 8 * i ) ) & 0xFFu );
                if ( c < 0x20 || c > 0x7E )
                    return std::format( "FourCC {}", code );
                text.push_back( static_cast<char>( c ) );
            }
            return std::format( "FourCC '{}'", text );
        }

        /// One 8-bit channel out of a 32-bit texel by its mask, or `fallback` when the mask is empty.
        unsigned char Channel( uint32_t texel, uint32_t mask, unsigned char fallback )
        {
            if ( mask == 0 )
                return fallback;
            return static_cast<unsigned char>( ( texel & mask ) >> std::countr_zero( mask ) );
        }

        bool IsByteMask( uint32_t mask )
        {
            return mask == 0 || ( std::popcount( mask ) == 8 && ( mask >> std::countr_zero( mask ) ) == 0xFFu );
        }

        unsigned char SnormToUnorm8( signed char s )
        {
            // bcdec clamps -128 to -127, so the range is the symmetric [-127, 127] the format defines.
            const float v =
                 static_cast<float>( std::max( static_cast<int>( s ), -127 ) + 127 ) * ( 255.0f / 254.0f );
            return static_cast<unsigned char>( std::lround( v ) );
        }

        /// BC5 is X and Y of a unit tangent-space normal stored as [0,255]; Z is what makes it unit length.
        unsigned char ReconstructZ( unsigned char x8, unsigned char y8 )
        {
            const float x = static_cast<float>( x8 ) / 255.0f * 2.0f - 1.0f;
            const float y = static_cast<float>( y8 ) / 255.0f * 2.0f - 1.0f;
            const float z = std::sqrt( std::max( 0.0f, 1.0f - x * x - y * y ) );
            return static_cast<unsigned char>( std::lround( ( z * 0.5f + 0.5f ) * 255.0f ) );
        }

        /// Decode one 4x4 block into 16 RGBA texels (LDR: bytes, BC6H: floats), row-major.
        void DecodeBlock( Layout k, const unsigned char* block, unsigned char* rgba8, float* rgbaF )
        {
            switch ( k )
            {
                case Layout::BC1:
                    bcdec_bc1( block, rgba8, 16 );
                    break;
                case Layout::BC2:
                    bcdec_bc2( block, rgba8, 16 );
                    break;
                case Layout::BC3:
                    bcdec_bc3( block, rgba8, 16 );
                    break;
                case Layout::BC7:
                    bcdec_bc7( block, rgba8, 16 );
                    break;
                case Layout::BC4U:
                case Layout::BC4S:
                {
                    std::array<unsigned char, 16> r{};
                    bcdec_bc4( block, r.data(), 4, k == Layout::BC4S ? 1 : 0 );
                    for ( int i = 0; i < 16; ++i )
                    {
                        const unsigned char v =
                             k == Layout::BC4S ? SnormToUnorm8( static_cast<signed char>( r[i] ) ) : r[i];
                        rgba8[i * 4 + 0] = v;
                        rgba8[i * 4 + 1] = v;
                        rgba8[i * 4 + 2] = v;
                        rgba8[i * 4 + 3] = 255;
                    }
                    break;
                }
                case Layout::BC5U:
                case Layout::BC5S:
                {
                    std::array<unsigned char, 32> rg{};
                    bcdec_bc5( block, rg.data(), 8, k == Layout::BC5S ? 1 : 0 );
                    for ( int i = 0; i < 16; ++i )
                    {
                        unsigned char x = rg[i * 2 + 0];
                        unsigned char y = rg[i * 2 + 1];
                        if ( k == Layout::BC5S )
                        {
                            x = SnormToUnorm8( static_cast<signed char>( x ) );
                            y = SnormToUnorm8( static_cast<signed char>( y ) );
                        }
                        rgba8[i * 4 + 0] = x;
                        rgba8[i * 4 + 1] = y;
                        rgba8[i * 4 + 2] = ReconstructZ( x, y );
                        rgba8[i * 4 + 3] = 255;
                    }
                    break;
                }
                case Layout::BC6HU:
                case Layout::BC6HS:
                {
                    std::array<float, 48> rgb{};
                    bcdec_bc6h_float( block, rgb.data(), 12, k == Layout::BC6HS ? 1 : 0 );
                    for ( int i = 0; i < 16; ++i )
                    {
                        rgbaF[i * 4 + 0] = rgb[i * 3 + 0];
                        rgbaF[i * 4 + 1] = rgb[i * 3 + 1];
                        rgbaF[i * 4 + 2] = rgb[i * 3 + 2];
                        rgbaF[i * 4 + 3] = 1.0f;
                    }
                    break;
                }
                default:
                    break;
            }
        }

        struct Masks
        {
            uint32_t R = 0, G = 0, B = 0, A = 0;
        };

        /// THE HEADER, READ ONCE for both questions this file answers (the decoded top mip and the chain of
        /// blocks as stored). Every refusal names the format the file spelled; the top mip is checked to be
        /// present, the levels below it are checked by the one reader that needs them.
        struct Parsed
        {
            Recognised  Format{ Layout::RGBA8, {} };
            uint32_t    Width    = 0;
            uint32_t    Height   = 0;
            uint32_t    MipCount = 1;
            std::size_t DataAt   = 0;
            uint32_t    PfFlags  = 0;
            Masks       ChannelMasks;
        };

        Common::ResultStr<Parsed> ParseHeader( const unsigned char* bytes, std::size_t size )
        {

            if ( size < kMagicSize + kHeaderSize || std::memcmp( bytes, "DDS ", 4 ) != 0 )
                return Common::MakeError<Parsed>(
                     std::format( "not a DDS file: {} bytes, no 'DDS ' magic and 124-byte header", size ) );
            const unsigned char* header = bytes + kMagicSize;
            if ( ReadU32( header ) != kHeaderSize || ReadU32( bytes + kPixelFormatAt ) != 32 )
                return Common::MakeError<Parsed>(
                     std::format( "DDS header is malformed (dwSize {}, pixel format size {}; 124 and 32 expected)",
                                  ReadU32( header ), ReadU32( bytes + kPixelFormatAt ) ) );

            const uint32_t height  = ReadU32( header + 8 );
            const uint32_t width   = ReadU32( header + 12 );
            const uint32_t depth   = ReadU32( header + 20 );
            const uint32_t pfFlags = ReadU32( bytes + kPixelFormatAt + 4 );
            const uint32_t fourCC  = ReadU32( bytes + kPixelFormatAt + 8 );
            const uint32_t bpp     = ReadU32( bytes + kPixelFormatAt + 12 );
            const Masks    masks{ ReadU32( bytes + kPixelFormatAt + 16 ), ReadU32( bytes + kPixelFormatAt + 20 ),
                               ReadU32( bytes + kPixelFormatAt + 24 ), ReadU32( bytes + kPixelFormatAt + 28 ) };
            const uint32_t caps2 = ReadU32( header + 108 );
            // dwMipMapCount: writers leave it 0 for "one level" as often as they clear DDSD_MIPMAPCOUNT, so 0 is 1
            // (DirectXTex DDSTextureLoader reads it the same way).
            const uint32_t mipCount = std::max( ReadU32( header + 24 ), 1u );

            std::size_t               dataAt = kMagicSize + kHeaderSize;
            std::optional<Recognised> format;
            bool                      isVolume = ( caps2 & kCaps2Volume ) != 0 && depth > 1;

            if ( ( pfFlags & kPixelFormatFourCC ) != 0 && fourCC == FourCC( 'D', 'X', '1', '0' ) )
            {
                if ( size < dataAt + kDx10HeaderSize )
                    return Common::MakeError<Parsed>( "DDS declares a DX10 header but the file ends before it" );
                const uint32_t dxgi      = ReadU32( bytes + dataAt );
                const uint32_t dimension = ReadU32( bytes + dataAt + 4 );
                dataAt += kDx10HeaderSize;
                isVolume = isVolume || dimension == kDx10Texture3D;
                format   = FromDxgi( dxgi );
                if ( !format )
                    return Common::MakeError<Parsed>( std::format(
                         "DDS pixel format DXGI_FORMAT {} (DX10) is not a supported texture source "
                         "(BC1-BC7, R8G8B8A8/B8G8R8A8/B8G8R8X8, R16G16B16A16 UNORM/FLOAT, R32G32B32A32_FLOAT)",
                         dxgi ) );
                format->Name += " (DX10)";
            }
            else if ( ( pfFlags & kPixelFormatFourCC ) != 0 )
            {
                format = FromFourCC( fourCC );
                if ( !format )
                    return Common::MakeError<Parsed>( std::format(
                         "DDS pixel format {} is not a supported texture source", DescribeFourCC( fourCC ) ) );
            }
            else if ( ( pfFlags & kPixelFormatRGB ) != 0 && bpp == 32 && IsByteMask( masks.R ) &&
                      IsByteMask( masks.G ) && IsByteMask( masks.B ) && IsByteMask( masks.A ) && masks.R != 0 )
            {
                format = Recognised{ Layout::Masked32,
                                     std::format( "32-bit RGB masks R={:#010x} G={:#010x} B={:#010x} A={:#010x}",
                                                  masks.R, masks.G, masks.B,
                                                  ( pfFlags & kPixelFormatAlphaPixels ) != 0 ? masks.A : 0u ) };
            }
            else
            {
                return Common::MakeError<Parsed>( std::format(
                     "DDS legacy pixel format (flags {:#x}, {} bits, masks R={:#010x} G={:#010x} B={:#010x} "
                     "A={:#010x}) is not a supported texture source — only 32-bit RGB(A) with 8-bit channels is",
                     pfFlags, bpp, masks.R, masks.G, masks.B, masks.A ) );
            }

            if ( !format.has_value() )
                return Common::MakeError<Parsed>( "DDS pixel format was not recognised" );
            const Recognised& recognised = *format;

            if ( isVolume )
                return Common::MakeError<Parsed>( std::format(
                     "DDS {} is a volume texture; a texture source is one 2D image", recognised.Name ) );
            if ( width == 0 || height == 0 || width > 16384 || height > 16384 )
                return Common::MakeError<Parsed>( std::format(
                     "DDS {} has extent {}x{}; 1..16384 per side is accepted", recognised.Name, width, height ) );

            const Layout      kind    = recognised.Kind;
            const std::size_t texels  = static_cast<std::size_t>( width ) * height;
            const std::size_t blocksX = ( width + 3u ) / 4u;
            const std::size_t blocksY = ( height + 3u ) / 4u;
            const std::size_t topMipLen =
                 IsBlock( kind ) ? blocksX * blocksY * BlockBytes( kind ) : texels * TexelBytes( kind );
            if ( size - dataAt < topMipLen )
                return Common::MakeError<Parsed>(
                     std::format( "DDS {} {}x{} is truncated: the top mip needs {} bytes, the file holds {}",
                                  recognised.Name, width, height, topMipLen, size - dataAt ) );

            Parsed parsed;
            parsed.Format       = recognised;
            parsed.Width        = width;
            parsed.Height       = height;
            parsed.MipCount     = mipCount;
            parsed.DataAt       = dataAt;
            parsed.PfFlags      = pfFlags;
            parsed.ChannelMasks = masks;
            return Common::MakeSuccess( std::move( parsed ) );
        }
    } // namespace

    bool IsDdsSource( std::string_view bytes, std::string_view sourceKey )
    {
        if ( bytes.starts_with( std::string_view( "DDS ", 4 ) ) )
            return true;
        std::string ext = std::filesystem::path( sourceKey ).extension().string();
        std::ranges::transform( ext, ext.begin(),
                                []( unsigned char c ) { return static_cast<char>( std::tolower( c ) ); } );
        return ext == ".dds";
    }

    Common::ResultStr<DdsSourceImage> DecodeDdsSource( const unsigned char* bytes, std::size_t size )
    {
        auto header = ParseHeader( bytes, size );
        if ( !header.IsSuccess() )
            return Common::MakeError<DdsSourceImage>( header.GetError() );
        const Parsed&     parsed  = header.GetValue();
        const Layout      kind    = parsed.Format.Kind;
        const uint32_t    width   = parsed.Width;
        const uint32_t    height  = parsed.Height;
        const uint32_t    pfFlags = parsed.PfFlags;
        const Masks&      masks   = parsed.ChannelMasks;
        const std::size_t dataAt  = parsed.DataAt;
        const std::size_t texels  = static_cast<std::size_t>( width ) * height;
        const std::size_t blocksX = ( width + 3u ) / 4u;

        DdsSourceImage out;
        out.Width  = width;
        out.Height = height;
        out.Format = parsed.Format.Name;
        out.IsFloat =
             kind == Layout::BC6HU || kind == Layout::BC6HS || kind == Layout::RGBA16F || kind == Layout::RGBA32F;
        if ( out.IsFloat )
            out.RgbaF.resize( texels * 4u );
        else
            out.Rgba8.resize( texels * 4u );

        const unsigned char* data = bytes + dataAt;
        if ( IsBlock( kind ) )
        {
            std::array<unsigned char, 64> block8{};
            std::array<float, 64>         blockF{};
            for ( std::size_t by = 0; by < blocksY; ++by )
            {
                for ( std::size_t bx = 0; bx < blocksX; ++bx )
                {
                    DecodeBlock( kind, data + ( by * blocksX + bx ) * BlockBytes( kind ), block8.data(),
                                 blockF.data() );
                    // The texels of an edge block that fall outside the image are dropped, not wrapped.
                    for ( std::size_t y = 0; y < 4 && by * 4 + y < height; ++y )
                    {
                        for ( std::size_t x = 0; x < 4 && bx * 4 + x < width; ++x )
                        {
                            const std::size_t dst = ( ( by * 4 + y ) * width + ( bx * 4 + x ) ) * 4u;
                            const std::size_t src = ( y * 4 + x ) * 4u;
                            if ( out.IsFloat )
                                std::copy_n( blockF.data() + src, 4, out.RgbaF.data() + dst );
                            else
                                std::copy_n( block8.data() + src, 4, out.Rgba8.data() + dst );
                        }
                    }
                }
            }
            return Common::MakeSuccess( std::move( out ) );
        }

        for ( std::size_t i = 0; i < texels; ++i )
        {
            const unsigned char* t = data + i * TexelBytes( kind );
            switch ( kind )
            {
                case Layout::RGBA8:
                    std::copy_n( t, 4, out.Rgba8.data() + i * 4 );
                    break;
                case Layout::BGRA8:
                case Layout::BGRX8:
                    out.Rgba8[i * 4 + 0] = t[2];
                    out.Rgba8[i * 4 + 1] = t[1];
                    out.Rgba8[i * 4 + 2] = t[0];
                    out.Rgba8[i * 4 + 3] = kind == Layout::BGRX8 ? 255 : t[3];
                    break;
                case Layout::Masked32:
                {
                    const uint32_t texel = ReadU32( t );
                    const uint32_t aMask = ( pfFlags & kPixelFormatAlphaPixels ) != 0 ? masks.A : 0u;
                    out.Rgba8[i * 4 + 0] = Channel( texel, masks.R, 0 );
                    out.Rgba8[i * 4 + 1] = Channel( texel, masks.G, 0 );
                    out.Rgba8[i * 4 + 2] = Channel( texel, masks.B, 0 );
                    out.Rgba8[i * 4 + 3] = Channel( texel, aMask, 255 );
                    break;
                }
                case Layout::RGBA16UNORM:
                    for ( std::size_t c = 0; c < 4; ++c )
                    {
                        const unsigned v     = t[c * 2] | ( static_cast<unsigned>( t[c * 2 + 1] ) << 8 );
                        out.Rgba8[i * 4 + c] = static_cast<unsigned char>( ( v * 255u + 32767u ) / 65535u );
                    }
                    break;
                case Layout::RGBA16F:
                    for ( std::size_t c = 0; c < 4; ++c )
                    {
                        const auto h         = static_cast<uint16_t>( t[c * 2] | ( t[c * 2 + 1] << 8 ) );
                        out.RgbaF[i * 4 + c] = glm::unpackHalf1x16( h );
                    }
                    break;
                case Layout::RGBA32F:
                    std::memcpy( out.RgbaF.data() + i * 4, t, 16 );
                    break;
                default:
                    break;
            }
        }
        return Common::MakeSuccess( std::move( out ) );
    }
    Common::ResultStr<DdsBlockChain> ReadDdsBlockChain( const unsigned char* bytes, std::size_t size )
    {
        namespace Fmt = ::Desert::Core::Formats;
        auto header   = ParseHeader( bytes, size );
        if ( !header.IsSuccess() )
            return Common::MakeError<DdsBlockChain>( header.GetError() );
        const Parsed& parsed = header.GetValue();
        const Layout  kind   = parsed.Format.Kind;

        DdsBlockChain chain;
        chain.Width      = parsed.Width;
        chain.Height     = parsed.Height;
        chain.LevelCount = parsed.MipCount;
        chain.SourceName = parsed.Format.Name;
        // The engine's twin is the format whose blocks are the SAME BYTES (`ImageFormat.hpp`: BC4/BC5/BC7 UNORM,
        // BC6H UFLOAT). The sRGB spellings are twins too: colour space is the asset's intent, not the block's.
        // The nearest is what an UNSPECIFIED intent re-encodes to: the signed BC4/BC5 keep their channel count,
        // the DX9 colour blocks become BC7; a float source has none (the cook keeps floats uncompressed).
        switch ( kind )
        {
            case Layout::BC7:
                chain.Twin = chain.Nearest = Fmt::ImageFormat::BC7_UNORM;
                break;
            case Layout::BC5U:
                chain.Twin = chain.Nearest = Fmt::ImageFormat::BC5_UNORM;
                break;
            case Layout::BC4U:
                chain.Twin = chain.Nearest = Fmt::ImageFormat::BC4_UNORM;
                break;
            case Layout::BC6HU:
                chain.Twin = Fmt::ImageFormat::BC6H_UFLOAT;
                break;
            case Layout::BC5S:
                chain.Nearest = Fmt::ImageFormat::BC5_UNORM;
                break;
            case Layout::BC4S:
                chain.Nearest = Fmt::ImageFormat::BC4_UNORM;
                break;
            case Layout::BC1:
            case Layout::BC2:
            case Layout::BC3:
                chain.Nearest = Fmt::ImageFormat::BC7_UNORM;
                break;
            default:
                break;
        }
        if ( chain.Twin == Fmt::ImageFormat::Count )
            return Common::MakeSuccess( std::move( chain ) );

        // THE FIRST SLICE'S LEVELS ARE CONTIGUOUS (array slices and cube faces are stored slice-major, each with
        // its whole chain), and every level is whole blocks — a 1x1 level is one block — which is the layout
        // `TextureBinary::BuildLevelTable` takes, so the bytes are copied and never re-arranged.
        std::size_t total = 0;
        for ( uint32_t level = 0; level < chain.LevelCount; ++level )
        {
            const std::size_t w = std::max( chain.Width >> level, 1u );
            const std::size_t h = std::max( chain.Height >> level, 1u );
            total += ( ( w + 3u ) / 4u ) * ( ( h + 3u ) / 4u ) * BlockBytes( kind );
        }
        if ( size - parsed.DataAt < total )
            return Common::MakeError<DdsBlockChain>( std::format(
                 "DDS {} {}x{} declares {} mip levels ({} bytes) but the file holds {}", parsed.Format.Name,
                 chain.Width, chain.Height, chain.LevelCount, total, size - parsed.DataAt ) );
        chain.Images.assign( bytes + parsed.DataAt, bytes + parsed.DataAt + total );
        return Common::MakeSuccess( std::move( chain ) );
    }
} // namespace Desert::Editor
