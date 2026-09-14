#include <Engine/Core/Formats/SdfAtlasEncoding.hpp>
#include <Engine/Vector/VectorImage.hpp>

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace Desert::Vector;

// The edge byte is a property of the ENCODING both distance-field atlases share, not of the icon
// rasterizer — see Engine/Core/Formats/SdfAtlasEncoding.hpp.
using Desert::Core::Formats::kSdfAtlasOnEdgeByte;

namespace
{
    // Rasterises at a fixed size and reports whether the texel at (x,y) of the INNER box is inside the
    // shape (SDF >= the on-edge value). Keeps the assertions readable: "the middle is filled, the corner
    // is not" is the property that actually matters for an icon.
    struct Raster
    {
        std::vector<uint8_t> Sdf;
        uint32_t             Dim = 0, Pad = 0, Size = 0;

        bool Inside( uint32_t x, uint32_t y ) const
        {
            const uint32_t px = x + Pad, py = y + Pad;
            return px < Dim && py < Dim && Sdf[static_cast<size_t>( py ) * Dim + px] >= kSdfAtlasOnEdgeByte;
        }
    };

    Raster Rasterise( const std::string& svg, uint32_t size = 32, int pad = 4 )
    {
        const VectorImage img = ParseSvg( svg.c_str(), svg.size() );
        Raster            r;
        r.Size = size;
        r.Pad  = static_cast<uint32_t>( pad );
        r.Dim  = size + 2u * r.Pad;
        r.Sdf  = RasterizeSdf( img, size, pad );
        return r;
    }
} // namespace

TEST( VectorImageParse, RejectsGarbage )
{
    const std::string junk = "this is not an svg";
    EXPECT_FALSE( ParseSvg( junk.c_str(), junk.size() ).Valid() );
    EXPECT_FALSE( ParseSvg( nullptr, 0 ).Valid() );
    EXPECT_FALSE( ParseSvg( "", 0 ).Valid() );
}

TEST( VectorImageParse, ReadsViewBoxAndShapes )
{
    const std::string svg = R"SVG(<svg viewBox="0 0 24 16"><circle cx="12" cy="8" r="6" fill="#fff"/></svg>)SVG";
    const VectorImage img = ParseSvg( svg.c_str(), svg.size() );
    ASSERT_TRUE( img.Valid() );
    EXPECT_FLOAT_EQ( img.Width, 24.0f );
    EXPECT_FLOAT_EQ( img.Height, 16.0f );
    EXPECT_EQ( img.Shapes.size(), 1u );
}

TEST( VectorImageParse, SkipsUnfilledAndUnknownElements )
{
    // A stroke-only path contributes nothing to a filled SDF, and <text>/<defs> are outside the subset.
    const std::string svg = R"SVG(<svg viewBox="0 0 24 24">
        <defs><linearGradient id="g"/></defs>
        <path d="M2 2 L22 22" fill="none"/>
        <text x="1" y="1">hi</text>
        <rect x="4" y="4" width="16" height="16" fill="#fff"/>
      </svg>)SVG";
    const VectorImage img = ParseSvg( svg.c_str(), svg.size() );
    ASSERT_TRUE( img.Valid() );
    EXPECT_EQ( img.Shapes.size(), 1u ); // only the rect
}

TEST( VectorImageRaster, FillsASolidCircle )
{
    const Raster r =
         Rasterise( R"SVG(<svg viewBox="0 0 24 24"><circle cx="12" cy="12" r="11" fill="#fff"/></svg>)SVG" );
    ASSERT_FALSE( r.Sdf.empty() );
    EXPECT_TRUE( r.Inside( r.Size / 2, r.Size / 2 ) ); // centre is solid
    EXPECT_FALSE( r.Inside( 0, 0 ) );                  // corners fall outside the disc
    EXPECT_FALSE( r.Inside( r.Size - 1, r.Size - 1 ) );
}

TEST( VectorImageRaster, HoleIsCutByNonZeroWinding )
{
    // A ring: outer circle clockwise, inner circle counter-clockwise -> the middle must stay EMPTY.
    const Raster r = Rasterise( R"SVG(<svg viewBox="0 0 24 24"><path fill="#fff"
        d="M12 1 A11 11 0 1 0 12 23 A11 11 0 1 0 12 1 Z M12 7 A5 5 0 1 1 12 17 A5 5 0 1 1 12 7 Z"/></svg>)SVG" );
    ASSERT_FALSE( r.Sdf.empty() );
    EXPECT_FALSE( r.Inside( r.Size / 2, r.Size / 2 ) ); // the hole

    // Sampled at the MIDDLE of the rim, not at its outer edge. The ring maps to a raster circle centred
    // on (16,16) with radius 11 * 32/24 = 14.67 px, so the row-1 sample this used to take sits 15 px out
    // — past the outer edge, and it only ever read as solid because the apex of the arc rounded its way
    // back over the pixel centre by a sixth of a pixel. MSVC flattened the arc a hair differently and
    // the test failed there and nowhere else. Mid-rim is (6.67 + 14.67) / 2 = 10.7 px above the centre,
    // which no rounding is going to argue with.
    EXPECT_TRUE( r.Inside( r.Size / 2, 5 ) ); // the rim above the hole
}

TEST( VectorImageRaster, PaddingStaysOutside )
{
    // Everything in the padding gutter is outside, so glyph-style bilinear sampling never bleeds.
    const std::string svg =
         R"SVG(<svg viewBox="0 0 24 24"><rect x="0" y="0" width="24" height="24" fill="#fff"/></svg>)SVG";
    const VectorImage          img  = ParseSvg( svg.c_str(), svg.size() );
    const uint32_t             size = 24, pad = 4, dim = size + 2 * pad;
    const std::vector<uint8_t> sdf = RasterizeSdf( img, size, static_cast<int>( pad ) );
    ASSERT_EQ( sdf.size(), static_cast<size_t>( dim ) * dim );
    for ( uint32_t x = 0; x < dim; ++x )
    {
        EXPECT_LT( sdf[x], kSdfAtlasOnEdgeByte ) << "top gutter row is inside at x=" << x;
        EXPECT_LT( sdf[static_cast<size_t>( dim - 1 ) * dim + x], kSdfAtlasOnEdgeByte ) << "bottom gutter";
    }
}

TEST( VectorImageRaster, TransformsAreApplied )
{
    // The same rect, once shifted right by a group transform: the left half empties, the right fills.
    const Raster plain = Rasterise(
         R"SVG(<svg viewBox="0 0 24 24"><rect x="0" y="0" width="12" height="24" fill="#fff"/></svg>)SVG" );
    const Raster moved = Rasterise(
         R"SVG(<svg viewBox="0 0 24 24"><g transform="translate(12,0)"><rect x="0" y="0" width="12" height="24" fill="#fff"/></g></svg>)SVG" );
    ASSERT_FALSE( plain.Sdf.empty() );
    ASSERT_FALSE( moved.Sdf.empty() );

    EXPECT_TRUE( plain.Inside( 2, plain.Size / 2 ) );
    EXPECT_FALSE( plain.Inside( plain.Size - 3, plain.Size / 2 ) );
    EXPECT_FALSE( moved.Inside( 2, moved.Size / 2 ) );
    EXPECT_TRUE( moved.Inside( moved.Size - 3, moved.Size / 2 ) );
}

TEST( VectorImageRaster, EmptyImageRasterisesToNothing )
{
    VectorImage empty;
    EXPECT_TRUE( RasterizeSdf( empty, 32, 4 ).empty() );
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
