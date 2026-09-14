// What the three channels buy, as numbers.
//
// The corner test is the load-bearing one and it is deliberately NOT about a font: it is a wedge whose
// apex angle, edge lines and interior are known in closed form, so "the corner survived" is a measured
// area against an exact answer rather than an opinion about a screenshot. A font glyph could only ever
// be compared against another rendering of itself.
//
// Every reconstruction here runs the SHADER's own text (SdfTextReference.hpp compiles
// Common/SdfText.glslh as C++), so a mutation that puts single-channel sampling back — `msd.r` in place
// of the median — reddens this file rather than passing it.

#include "SdfTextReference.hpp"

#include <Engine/Text/FontBaker.hpp>
#include <Engine/Text/Msdf.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <vector>

namespace
{
    using Desert::Text::Msdf::Contour;
    using Desert::Text::Msdf::EdgeSegment;
    using Desert::Text::Msdf::Shape;
    using Desert::Text::Msdf::Vec2;

    constexpr int    kDim   = 64;
    constexpr double kRange = Desert::Text::kDistanceRangeTexels;

    // Apex at the top, base at the bottom: a 47 degree wedge, the shape of the junction in A and the
    // outside of a V. Wound so that the interior is on the RIGHT of the travel direction, which is the
    // handedness the field's sign convention is written for (see FontBaker's BuildShape, which measures
    // this rather than assuming it).
    constexpr Vec2 kApex{ 32.0, 10.0 };
    constexpr Vec2 kLeft{ 10.0, 60.0 };
    constexpr Vec2 kRight{ 54.0, 60.0 };

    Shape Wedge()
    {
        auto line = []( Vec2 a, Vec2 b )
        {
            EdgeSegment e;
            e.PointCount = 2;
            e.P[0]       = a;
            e.P[1]       = b;
            return e;
        };
        Contour c;
        c.Edges.push_back( line( kApex, kLeft ) );
        c.Edges.push_back( line( kLeft, kRight ) );
        c.Edges.push_back( line( kRight, kApex ) );
        Shape s;
        s.Contours.push_back( c );
        return s;
    }

    // Signed distance to the LINE through p0->p1, positive on the right of travel.
    double HalfPlane( Vec2 p0, Vec2 p1, double x, double y )
    {
        const double dx  = p1.X - p0.X;
        const double dy  = p1.Y - p0.Y;
        const double len = std::sqrt( dx * dx + dy * dy );
        return ( ( x - p0.X ) * dy - ( y - p0.Y ) * dx ) / len;
    }

    // The exact answer inside the test window: the base edge is 50 texels away, so near the apex the
    // wedge IS the intersection of its two half-planes.
    bool TrulyInside( double x, double y )
    {
        return HalfPlane( kApex, kLeft, x, y ) > 0.0 && HalfPlane( kRight, kApex, x, y ) > 0.0;
    }

    // Exactly what a sampler does: values live at texel centres, so a continuous position maps to
    // (p - 0.5) in texel index space.
    float BilinearChannel( const std::vector<float>& field, int stride, int channel, int w, int h, double x,
                           double y )
    {
        const double fx = std::clamp( x - 0.5, 0.0, static_cast<double>( w - 1 ) );
        const double fy = std::clamp( y - 0.5, 0.0, static_cast<double>( h - 1 ) );
        const int    x0 = static_cast<int>( fx );
        const int    y0 = static_cast<int>( fy );
        const int    x1 = std::min( x0 + 1, w - 1 );
        const int    y1 = std::min( y0 + 1, h - 1 );
        const double tx = fx - x0;
        const double ty = fy - y0;
        auto         at = [&]( int xi, int yi )
        { return static_cast<double>( field[( static_cast<size_t>( yi ) * w + xi ) * stride + channel] ); };
        const double top = at( x0, y0 ) * ( 1.0 - tx ) + at( x1, y0 ) * tx;
        const double bot = at( x0, y1 ) * ( 1.0 - tx ) + at( x1, y1 ) * tx;
        return static_cast<float>( top * ( 1.0 - ty ) + bot * ty );
    }

    glm::vec3 SampleMsdf( const std::vector<float>& f, int w, int h, double x, double y )
    {
        return { BilinearChannel( f, 3, 0, w, h, x, y ), BilinearChannel( f, 3, 1, w, h, x, y ),
                 BilinearChannel( f, 3, 2, w, h, x, y ) };
    }

    std::vector<uint8_t> LoadRoboto()
    {
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

// THE test. Both fields are generated from the SAME outline with the SAME range, reconstructed the same
// way, and scored against the analytic wedge over a disc around the apex. What separates them is only
// the number of channels, so the difference IS the corner.
TEST( MsdfCorner, MedianKeepsTheCornerThatOneChannelRounds )
{
    Shape shape = Wedge();
    Desert::Text::Msdf::ColorEdges( shape );

    std::vector<float> msdf, sdf;
    Desert::Text::Msdf::GenerateMSDF( msdf, kDim, kDim, shape, kRange );
    Desert::Text::Msdf::GenerateSDF( sdf, kDim, kDim, shape, kRange );

    // A 4-texel disc around the apex: far enough to contain the whole corner, small enough that the
    // straight edges (where both fields are exact) do not dilute the score.
    constexpr double kRadius = 4.0;
    constexpr double kStep   = 0.05;

    int total = 0, msdfWrong = 0, sdfWrong = 0;
    for ( double dy = -kRadius; dy <= kRadius; dy += kStep )
    {
        for ( double dx = -kRadius; dx <= kRadius; dx += kStep )
        {
            if ( dx * dx + dy * dy > kRadius * kRadius )
                continue;
            const double x = kApex.X + dx;
            const double y = kApex.Y + dy;
            ++total;

            const bool truth = TrulyInside( x, y );
            const bool byMedian =
                 Desert::Tests::SdfTextRef::SdfTextMedian( SampleMsdf( msdf, kDim, kDim, x, y ) ) > 0.5f;
            const bool byOneChannel = BilinearChannel( sdf, 1, 0, kDim, kDim, x, y ) > 0.5f;

            msdfWrong += ( byMedian != truth ) ? 1 : 0;
            sdfWrong += ( byOneChannel != truth ) ? 1 : 0;
        }
    }

    ASSERT_GT( total, 4000 );
    const double msdfErr = 100.0 * msdfWrong / total;
    const double sdfErr  = 100.0 * sdfWrong / total;
    std::printf( "[corner] disc of %.0f texels around a %.0f degree apex: multi-channel misplaces %.2f%% "
                 "of it, one channel %.2f%%\n",
                 kRadius, 2.0 * std::atan( 22.0 / 50.0 ) * 180.0 / 3.14159265358979323846, msdfErr, sdfErr );

    // The single-channel field cannot be exact here: its interior near the apex is the distance to the
    // nearest edge, which creases along the bisector, and bilinear reconstruction cuts that crease off.
    EXPECT_GT( sdfErr, 1.0 );
    // The multi-channel one reconstructs two straight half-planes, which bilinear interpolation
    // reproduces exactly — what is left is the sampling grid, not the representation.
    EXPECT_LT( msdfErr, 0.5 );
    EXPECT_LT( msdfErr * 4.0, sdfErr );
}

// Two numbers that must agree and live in different languages: the band the baker quantizes with, and
// the band the fragment shader divides by. Disagree and every edge in the engine is the wrong width,
// everywhere, with nothing to see but "text looks a bit soft".
TEST( MsdfCorner, ShaderConstantMatchesTheBakersRange )
{
    EXPECT_FLOAT_EQ( Desert::Tests::SdfTextRef::DESERT_TEXT_DISTANCE_RANGE_TEXELS,
                     Desert::Text::kDistanceRangeTexels );

    // And the same number reaches the GPU through the atlas, not only through the compiler.
    const auto ttf = LoadRoboto();
    ASSERT_FALSE( ttf.empty() );
    const auto font = Desert::Text::BakeFontMSDF( ttf.data(), ttf.size() );
    ASSERT_TRUE( font.Valid() );
    EXPECT_FLOAT_EQ( font.DistanceRangeTexels, Desert::Tests::SdfTextRef::DESERT_TEXT_DISTANCE_RANGE_TEXELS );
}

// Away from corners a multi-channel field must be the ordinary one: all three channels see the same
// nearest edge, so the median is that edge's distance. If this drifts, every straight stem in the font
// is being drawn by a different rule than it used to be.
TEST( MsdfCorner, MedianAgreesWithTheOrdinaryFieldAwayFromCorners )
{
    Shape shape = Wedge();
    Desert::Text::Msdf::ColorEdges( shape );
    std::vector<float> msdf, sdf;
    Desert::Text::Msdf::GenerateMSDF( msdf, kDim, kDim, shape, kRange );
    Desert::Text::Msdf::GenerateSDF( sdf, kDim, kDim, shape, kRange );

    double worst = 0.0;
    int    n     = 0;
    for ( int y = 0; y < kDim; ++y )
    {
        for ( int x = 0; x < kDim; ++x )
        {
            const size_t i = static_cast<size_t>( y ) * kDim + x;

            // Only texels the GPU can tell apart: outside the encoded band everything quantizes to 0 or
            // 255 and the two fields are equal by saturation, which would flatter the comparison.
            if ( std::fabs( sdf[i] - 0.5f ) > 0.45f )
                continue;

            // And only away from the wedge's three corners — the whole point is that the two fields
            // DISAGREE there. Eight texels, which with the band above puts every remaining texel opposite
            // the interior of an edge, where the pseudo-distance and the true distance coincide.
            const double cx = x + 0.5, cy = y + 0.5;
            auto near = [&]( Vec2 c ) { return ( cx - c.X ) * ( cx - c.X ) + ( cy - c.Y ) * ( cy - c.Y ) < 64.0; };
            if ( near( kApex ) || near( kLeft ) || near( kRight ) )
                continue;

            const float m =
                 Desert::Tests::SdfTextRef::SdfTextMedian( { msdf[i * 3 + 0], msdf[i * 3 + 1], msdf[i * 3 + 2] } );
            worst = std::max( worst, static_cast<double>( std::fabs( m - sdf[i] ) ) );
            ++n;
        }
    }
    ASSERT_GT( n, 200 );
    std::printf( "[straight] %d in-band texels opposite an edge: worst median-vs-ordinary gap %.4f of the "
                 "band (%.3f texels)\n",
                 n, worst, worst * kRange );
    // One part in 255 of the band is a quantization step; allow four of them and nothing structural.
    EXPECT_LT( worst, 4.0 / 255.0 );
}

// The sign convention is measured, not assumed (fonts carry both windings). Get it backwards and every
// glyph renders as its own hole, which no unit test of distances alone would notice.
TEST( MsdfBake, InsideOfAStemIsPositive )
{
    const auto ttf = LoadRoboto();
    ASSERT_FALSE( ttf.empty() );
    const auto font = Desert::Text::BakeFontMSDF( ttf.data(), ttf.size() );
    ASSERT_TRUE( font.Valid() );
    ASSERT_TRUE( font.Glyphs.count( 'I' ) );

    const auto& g = font.Glyphs.at( 'I' );
    ASSERT_GT( g.Width, 0.0f );
    // Centre of the cell: 'I' is a single stem, so its cell centre is ink.
    const int  cx    = static_cast<int>( g.U0 * font.AtlasWidth + g.Width * 0.5f );
    const int  cy    = static_cast<int>( g.V0 * font.AtlasHeight + g.Height * 0.5f );
    const auto texel = [&]( int x, int y )
    {
        const size_t i = ( static_cast<size_t>( y ) * font.AtlasWidth + x ) * 4;
        return Desert::Tests::SdfTextRef::SdfTextMedian(
             { font.AtlasRGBA[i] / 255.0f, font.AtlasRGBA[i + 1] / 255.0f, font.AtlasRGBA[i + 2] / 255.0f } );
    };
    EXPECT_GT( texel( cx, cy ), 0.5f ) << "the middle of a stem must read as INSIDE";

    // The cell's own corner is gutter: kGlyphPadding texels of it, so it is outside by construction.
    const int gx = static_cast<int>( g.U0 * font.AtlasWidth );
    const int gy = static_cast<int>( g.V0 * font.AtlasHeight );
    EXPECT_LT( texel( gx, gy ), 0.5f ) << "the padding corner of a cell must read as OUTSIDE";

    // Alpha is opaque everywhere — the atlas is a field, not a mask (see BakedFont::AtlasRGBA).
    for ( size_t i = 3; i < font.AtlasRGBA.size(); i += 4 )
        ASSERT_EQ( font.AtlasRGBA[i], 255 ) << "alpha at texel " << i / 4;
}

// MINIFICATION, WHICH IS THE WORLD-TEXT CASE, AND THE HYPOTHESIS IT DISPROVED.
//
// There are no mips: averaging three channels destroys the median, and the engine's image path only
// knows how to build mips by averaging. So the only lever left is the width of the edge ramp, and the
// first answer written here was "let it widen past a screen pixel, so a shrinking glyph fades instead
// of breaking up". THAT IS WRONG, and this test is what says so: once the encoded band spans less than
// a pixel it also saturates in less than a pixel, so a texel deep inside a stroke can only report the
// band's own half-width and the stroke never reaches full opacity. Scored against the glyph's own
// supersampled coverage it is nearly twice as wrong as flooring the ramp at one pixel.
//
// The floor is a lie about distances the band cannot express. The measurement is why we tell it.
TEST( MsdfMinification, FlooringTheRampAtOnePixelBeatsLettingItWiden )
{
    const auto ttf = LoadRoboto();
    ASSERT_FALSE( ttf.empty() );
    const auto font = Desert::Text::BakeFontMSDF( ttf.data(), ttf.size() );
    ASSERT_TRUE( font.Valid() );
    ASSERT_TRUE( font.Glyphs.count( 'A' ) );
    const auto& g = font.Glyphs.at( 'A' );

    std::vector<float> rgb( static_cast<size_t>( font.AtlasWidth ) * font.AtlasHeight * 3 );
    for ( size_t i = 0; i < rgb.size() / 3; ++i )
        for ( int c = 0; c < 3; ++c )
            rgb[i * 3 + c] = font.AtlasRGBA[i * 4 + c] / 255.0f;

    const double x0 = g.U0 * font.AtlasWidth;
    const double y0 = g.V0 * font.AtlasHeight;

    // Three rendered heights out of a 48 px bake. 10 px is comfortable, 5 px is where the ten-texel band
    // stops covering a screen pixel, 3 px is past it — a world label between the middle distance and the
    // horizon.
    for ( double targetPx : { 10.0, 5.0, 3.0 } )
    {
        const double scale       = targetPx / font.PixelHeight;
        const double texelsPerPx = 1.0 / scale;

        // Through the shader's own function, so the floor under test is the one the GPU applies.
        const glm::vec2 uvDerivative( texelsPerPx / font.AtlasWidth, texelsPerPx / font.AtlasHeight );
        const glm::vec2 atlasSize( font.AtlasWidth, font.AtlasHeight );
        const float     shippedRange = Desert::Tests::SdfTextRef::SdfTextScreenPxRange( uvDerivative, atlasSize );
        const float     unflooredRange = static_cast<float>( Desert::Text::kDistanceRangeTexels / texelsPerPx );

        const int w = static_cast<int>( g.Width * scale ) + 1;
        const int h = static_cast<int>( g.Height * scale ) + 1;

        double sumShipped = 0.0, sumUnfloored = 0.0, maxShipped = 0.0;
        int    pixels = 0;
        for ( int py = 0; py < h; ++py )
        {
            for ( int px = 0; px < w; ++px )
            {
                // Ground truth: this screen pixel's real coverage by the glyph, from 8x8 subsamples of
                // the field's own inside/outside test.
                double covered = 0.0;
                for ( int sy = 0; sy < 8; ++sy )
                    for ( int sx = 0; sx < 8; ++sx )
                    {
                        const double u = x0 + ( px + ( sx + 0.5 ) / 8.0 ) * texelsPerPx;
                        const double v = y0 + ( py + ( sy + 0.5 ) / 8.0 ) * texelsPerPx;
                        covered += Desert::Tests::SdfTextRef::SdfTextMedian(
                                        SampleMsdf( rgb, font.AtlasWidth, font.AtlasHeight, u, v ) ) > 0.5f
                                        ? 1.0
                                        : 0.0;
                    }
                covered /= 64.0;

                const double    u   = x0 + ( px + 0.5 ) * texelsPerPx;
                const double    v   = y0 + ( py + 0.5 ) * texelsPerPx;
                const glm::vec3 msd = SampleMsdf( rgb, font.AtlasWidth, font.AtlasHeight, u, v );

                const double shipped   = Desert::Tests::SdfTextRef::SdfTextAlpha( msd, shippedRange );
                const double unfloored = Desert::Tests::SdfTextRef::SdfTextAlpha( msd, unflooredRange );

                sumShipped += ( shipped - covered ) * ( shipped - covered );
                sumUnfloored += ( unfloored - covered ) * ( unfloored - covered );
                maxShipped = std::max( maxShipped, std::fabs( shipped - covered ) );
                ++pixels;
            }
        }

        ASSERT_GT( pixels, 4 );
        const double rmsShipped   = std::sqrt( sumShipped / pixels );
        const double rmsUnfloored = std::sqrt( sumUnfloored / pixels );
        std::printf( "[minify] 'A' at %.0f px (%d screen pixels, true ramp %.3f px): shipped RMS %.4f "
                     "(worst pixel %.4f), ramp left to widen RMS %.4f\n",
                     targetPx, pixels, unflooredRange, rmsShipped, maxShipped, rmsUnfloored );

        EXPECT_LE( rmsShipped, rmsUnfloored + 1e-6 )
             << "at " << targetPx << " px the floored ramp is supposed to be no worse";
        // And the reconstruction stays a coverage estimate rather than collapsing: no screen pixel is
        // more than half a level of coverage away from the truth at any of these sizes.
        EXPECT_LT( maxShipped, 0.5 ) << "at " << targetPx << " px";
    }
}
