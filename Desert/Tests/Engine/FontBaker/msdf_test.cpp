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

#include <glm/ext/vector_float2.hpp>
#include <glm/ext/vector_float3.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <numbers>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    using Desert::Text::Msdf::Contour;
    using Desert::Text::Msdf::EdgeSegment;
    using Desert::Text::Msdf::Shape;
    using Desert::Text::Msdf::Vec2;

    constexpr int    kFieldDim  = 64;
    constexpr double kRange     = Desert::Text::kDistanceRangeTexels;
    constexpr double kHalf      = 0.5;
    constexpr float  kHalfFloat = 0.5F;
    constexpr float  kByteScale = 255.0F;
    constexpr int    kSubSteps  = 8; // 8x8 subsamples when a pixel's true coverage is needed
    constexpr double kSubTotal  = static_cast<double>( kSubSteps * kSubSteps );
    constexpr float  kBakeSize  = 48.0F;

    // Apex at the top, base at the bottom: a 47 degree wedge, the shape of the junction in A and the
    // outside of a V. Wound so that the interior is on the RIGHT of the travel direction, which is the
    // handedness the field's sign convention is written for (see FontBaker's BuildShape, which measures
    // this rather than assuming it).
    constexpr Vec2   kApex{ 32.0, 10.0 };
    constexpr Vec2   kLeft{ 10.0, 60.0 };
    constexpr Vec2   kRight{ 54.0, 60.0 };
    constexpr double kApexHalfRun  = 22.0; // the apex's horizontal half-width over kApexRise
    constexpr double kApexRise     = 50.0;
    constexpr double kDegreesPerPi = 180.0;

    Shape Wedge()
    {
        auto line = []( Vec2 from, Vec2 dest )
        {
            EdgeSegment edge;
            edge.PointCount = 2;
            edge.Points[0]  = from;
            edge.Points[1]  = dest;
            return edge;
        };
        Contour contour;
        contour.Edges.push_back( line( kApex, kLeft ) );
        contour.Edges.push_back( line( kLeft, kRight ) );
        contour.Edges.push_back( line( kRight, kApex ) );
        Shape shape;
        shape.Contours.push_back( contour );
        return shape;
    }

    // Signed distance to the LINE through from->dest, positive on the right of travel.
    double HalfPlane( Vec2 from, Vec2 dest, double posX, double posY )
    {
        const double runX   = dest.X - from.X;
        const double runY   = dest.Y - from.Y;
        const double length = std::sqrt( ( runX * runX ) + ( runY * runY ) );
        return ( ( ( posX - from.X ) * runY ) - ( ( posY - from.Y ) * runX ) ) / length;
    }

    // The exact answer inside the test window: the base edge is 50 texels away, so near the apex the
    // wedge IS the intersection of its two half-planes.
    bool TrulyInside( double posX, double posY )
    {
        return HalfPlane( kApex, kLeft, posX, posY ) > 0.0 && HalfPlane( kRight, kApex, posX, posY ) > 0.0;
    }

    // Exactly what a sampler does: values live at texel centres, so a continuous position maps to
    // (p - 0.5) in texel index space.
    float BilinearChannel( const std::vector<float>& field, int stride, int channel, int width, int height,
                           double posX, double posY )
    {
        const double clampedX = std::clamp( posX - kHalf, 0.0, static_cast<double>( width - 1 ) );
        const double clampedY = std::clamp( posY - kHalf, 0.0, static_cast<double>( height - 1 ) );
        const auto   lowX     = static_cast<int>( clampedX );
        const auto   lowY     = static_cast<int>( clampedY );
        const int    highX    = std::min( lowX + 1, width - 1 );
        const int    highY    = std::min( lowY + 1, height - 1 );
        const double fracX    = clampedX - lowX;
        const double fracY    = clampedY - lowY;

        const auto sample = [&]( int texelX, int texelY )
        {
            const size_t index = ( ( static_cast<size_t>( texelY ) * width ) + texelX ) * stride + channel;
            return static_cast<double>( field[index] );
        };
        const double top    = ( sample( lowX, lowY ) * ( 1.0 - fracX ) ) + ( sample( highX, lowY ) * fracX );
        const double bottom = ( sample( lowX, highY ) * ( 1.0 - fracX ) ) + ( sample( highX, highY ) * fracX );
        return static_cast<float>( ( top * ( 1.0 - fracY ) ) + ( bottom * fracY ) );
    }

    glm::vec3 SampleMsdf( const std::vector<float>& field, int width, int height, double posX, double posY )
    {
        return { BilinearChannel( field, 3, 0, width, height, posX, posY ),
                 BilinearChannel( field, 3, 1, width, height, posX, posY ),
                 BilinearChannel( field, 3, 2, width, height, posX, posY ) };
    }

    std::vector<uint8_t> LoadRoboto()
    {
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

    // The atlas as float RGB, which is what the shader's own reconstruction wants.
    std::vector<float> AtlasAsFloats( const Desert::Text::BakedFont& font )
    {
        const size_t       texels = font.AtlasRGBA.size() / 4;
        std::vector<float> field( texels * 3 );
        for ( size_t texel = 0; texel < texels; ++texel )
        {
            for ( size_t channel = 0; channel < 3; ++channel )
            {
                field[( texel * 3 ) + channel] =
                     static_cast<float>( font.AtlasRGBA[( texel * 4 ) + channel] ) / kByteScale;
            }
        }
        return field;
    }

    // One screen pixel's real coverage by the glyph, from 8x8 subsamples of the field's own
    // inside/outside test. The ground truth every reconstruction below is scored against.
    double TrueCoverage( const std::vector<float>& field, const Desert::Text::BakedFont& font, double originX,
                         double originY, int pixelX, int pixelY, double texelsPerPixel )
    {
        double covered = 0.0;
        for ( int subY = 0; subY < kSubSteps; ++subY )
        {
            for ( int subX = 0; subX < kSubSteps; ++subX )
            {
                const double posX   = originX + ( ( pixelX + ( ( subX + kHalf ) / kSubSteps ) ) * texelsPerPixel );
                const double posY   = originY + ( ( pixelY + ( ( subY + kHalf ) / kSubSteps ) ) * texelsPerPixel );
                const float  median = Desert::Tests::SdfTextRef::SdfTextMedian(
                     SampleMsdf( field, static_cast<int>( font.AtlasWidth ), static_cast<int>( font.AtlasHeight ),
                                  posX, posY ) );
                if ( median > kHalfFloat )
                {
                    covered += 1.0;
                }
            }
        }
        return covered / kSubTotal;
    }

    // Test output goes through a stream rather than printf: one place decides where a measurement is
    // printed, and a C vararg call is not it.
    void Report( const std::string& line )
    {
        std::cout << line << '\n';
    }

    // How often a reconstruction puts a point on the wrong side of the outline, over a disc around the
    // wedge's apex. Lifted out of the test so the test states the claim and this states the method.
    struct CornerScore
    {
        int Total           = 0;
        int MedianWrong     = 0;
        int OneChannelWrong = 0;
    };

    CornerScore ScoreCornerDisc( const std::vector<float>& msdf, const std::vector<float>& sdf, double radius,
                                 double step )
    {
        CornerScore score;
        for ( double offY = -radius; offY <= radius; offY += step )
        {
            for ( double offX = -radius; offX <= radius; offX += step )
            {
                if ( ( offX * offX ) + ( offY * offY ) > radius * radius )
                {
                    continue;
                }
                const double posX = kApex.X + offX;
                const double posY = kApex.Y + offY;
                ++score.Total;

                const bool truth    = TrulyInside( posX, posY );
                const bool byMedian = Desert::Tests::SdfTextRef::SdfTextMedian(
                                           SampleMsdf( msdf, kFieldDim, kFieldDim, posX, posY ) ) > kHalfFloat;
                const bool byOneChannel =
                     BilinearChannel( sdf, 1, 0, kFieldDim, kFieldDim, posX, posY ) > kHalfFloat;

                score.MedianWrong += ( byMedian != truth ) ? 1 : 0;
                score.OneChannelWrong += ( byOneChannel != truth ) ? 1 : 0;
            }
        }
        return score;
    }

    // Is this texel inside the encoded band AND clear of every corner? The two conditions the
    // "away from corners" comparison needs, and the only reason its loop had any branching left.
    bool IsStraightRunTexel( const std::vector<float>& sdf, size_t index, int row, int col )
    {
        constexpr float  kInBand       = 0.45F;
        constexpr double kCornerRadius = 8.0;

        if ( std::fabs( sdf[index] - kHalfFloat ) > kInBand )
        {
            return false; // outside the band both fields are equal by saturation
        }
        const double centreX = col + kHalf;
        const double centreY = row + kHalf;
        const auto   nearBy  = [centreX, centreY]( Vec2 corner )
        {
            const double runX = centreX - corner.X;
            const double runY = centreY - corner.Y;
            return ( runX * runX ) + ( runY * runY ) < kCornerRadius * kCornerRadius;
        };
        return !nearBy( kApex ) && !nearBy( kLeft ) && !nearBy( kRight );
    }

    // One rendered size, two reconstructions, scored against the glyph's own supersampled coverage.
    struct RampScore
    {
        double FirstRms  = 0.0;
        double SecondRms = 0.0;
        double FirstMax  = 0.0;
        int    Pixels    = 0;
    };

    struct GlyphView
    {
        const std::vector<float>*      Field   = nullptr;
        const Desert::Text::BakedFont* Font    = nullptr;
        double                         OriginX = 0.0;
        double                         OriginY = 0.0;
    };

    GlyphView ViewOf( const std::vector<float>& field, const Desert::Text::BakedFont& font,
                      const Desert::Text::Glyph& glyph )
    {
        return { &field, &font, glyph.U0 * static_cast<float>( font.AtlasWidth ),
                 glyph.V0 * static_cast<float>( font.AtlasHeight ) };
    }

    // `first` and `second` each turn a sampled MSDF into coverage; the score says which is closer to the
    // truth. Passing them in is what lets one loop serve both comparisons this file makes.
    template <class FirstFn, class SecondFn>
    RampScore ScoreReconstruction( const GlyphView& view, const Desert::Text::Glyph& glyph, double scale,
                                   FirstFn first, SecondFn second )
    {
        const double texelsPerPx = 1.0 / scale;
        const auto   pixelsWide  = static_cast<int>( glyph.Width * scale ) + 1;
        const auto   pixelsHigh  = static_cast<int>( glyph.Height * scale ) + 1;

        double    sumFirst  = 0.0;
        double    sumSecond = 0.0;
        RampScore score;
        for ( int pixelY = 0; pixelY < pixelsHigh; ++pixelY )
        {
            for ( int pixelX = 0; pixelX < pixelsWide; ++pixelX )
            {
                const double covered = TrueCoverage( *view.Field, *view.Font, view.OriginX, view.OriginY, pixelX,
                                                     pixelY, texelsPerPx );
                const double posX    = view.OriginX + ( ( pixelX + kHalf ) * texelsPerPx );
                const double posY    = view.OriginY + ( ( pixelY + kHalf ) * texelsPerPx );
                const glm::vec3 msd  = SampleMsdf( *view.Field, static_cast<int>( view.Font->AtlasWidth ),
                                                   static_cast<int>( view.Font->AtlasHeight ), posX, posY );

                const double asFirst  = first( msd );
                const double asSecond = second( msd );
                sumFirst += ( asFirst - covered ) * ( asFirst - covered );
                sumSecond += ( asSecond - covered ) * ( asSecond - covered );
                score.FirstMax = std::max( score.FirstMax, std::fabs( asFirst - covered ) );
                ++score.Pixels;
            }
        }
        if ( score.Pixels > 0 )
        {
            score.FirstRms  = std::sqrt( sumFirst / score.Pixels );
            score.SecondRms = std::sqrt( sumSecond / score.Pixels );
        }
        return score;
    }

    // The UV derivative one screen pixel spans, without touching glm's unions.
    glm::vec2 UvDerivative( const Desert::Text::BakedFont& font, double texelsPerPixel )
    {
        return { static_cast<float>( texelsPerPixel ) / static_cast<float>( font.AtlasWidth ),
                 static_cast<float>( texelsPerPixel ) / static_cast<float>( font.AtlasHeight ) };
    }

    float MedianAt( const Desert::Text::BakedFont& font, size_t posX, size_t posY )
    {
        const size_t index = ( ( posY * font.AtlasWidth ) + posX ) * 4;
        return Desert::Tests::SdfTextRef::SdfTextMedian(
             { static_cast<float>( font.AtlasRGBA[index + 0] ) / kByteScale,
               static_cast<float>( font.AtlasRGBA[index + 1] ) / kByteScale,
               static_cast<float>( font.AtlasRGBA[index + 2] ) / kByteScale } );
    }

    bool EveryAlphaIsOpaque( const Desert::Text::BakedFont& font )
    {
        constexpr uint8_t kOpaque = 255;
        const size_t      texels  = font.AtlasRGBA.size() / 4;
        for ( size_t index = 0; index < texels; ++index )
        {
            if ( font.AtlasRGBA[( index * 4 ) + 3] != kOpaque )
            {
                return false;
            }
        }
        return true;
    }

    // GLSL's smoothstep, which is what the OLD text shader ramped its edge with.
    double Smoothstep( double edge0, double edge1, double value )
    {
        constexpr double kCubicTerm     = 3.0;
        constexpr double kQuadraticTerm = 2.0;
        const double     ramp           = std::clamp( ( value - edge0 ) / ( edge1 - edge0 ), 0.0, 1.0 );
        return ramp * ramp * ( kCubicTerm - ( kQuadraticTerm * ramp ) );
    }

    glm::vec2 AtlasSize( const Desert::Text::BakedFont& font )
    {
        return { static_cast<float>( font.AtlasWidth ), static_cast<float>( font.AtlasHeight ) };
    }
} // namespace

// THE test. Both fields are generated from the SAME outline with the SAME range, reconstructed the same
// way, and scored against the analytic wedge over a disc around the apex. What separates them is only
// the number of channels, so the difference IS the corner.
TEST( MsdfCorner, MedianKeepsTheCornerThatOneChannelRounds )
{
    Shape shape = Wedge();
    Desert::Text::Msdf::ColorEdges( shape );

    std::vector<float> msdf;
    std::vector<float> sdf;
    Desert::Text::Msdf::GenerateMSDF( msdf, kFieldDim, kFieldDim, shape, kRange );
    Desert::Text::Msdf::GenerateSDF( sdf, kFieldDim, kFieldDim, shape, kRange );

    // A 4-texel disc around the apex: far enough to contain the whole corner, small enough that the
    // straight edges (where both fields are exact) do not dilute the score.
    constexpr double kRadius = 4.0;
    constexpr double kStep   = 0.05;

    const CornerScore score = ScoreCornerDisc( msdf, sdf, kRadius, kStep );
    ASSERT_GT( score.Total, 4000 );

    const double msdfErr = 100.0 * score.MedianWrong / score.Total;
    const double sdfErr  = 100.0 * score.OneChannelWrong / score.Total;
    const double apexDeg = 2.0 * std::atan( kApexHalfRun / kApexRise ) * kDegreesPerPi / std::numbers::pi;

    std::ostringstream line;
    line << "[corner] disc of " << kRadius << " texels around a " << apexDeg
         << " degree apex: multi-channel misplaces " << msdfErr << "% of it, one channel " << sdfErr << "%";
    Report( line.str() );

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
    std::vector<float> msdf;
    std::vector<float> sdf;
    Desert::Text::Msdf::GenerateMSDF( msdf, kFieldDim, kFieldDim, shape, kRange );
    Desert::Text::Msdf::GenerateSDF( sdf, kFieldDim, kFieldDim, shape, kRange );

    double worst = 0.0;
    int    seen  = 0;
    for ( int row = 0; row < kFieldDim; ++row )
    {
        for ( int col = 0; col < kFieldDim; ++col )
        {
            const size_t index = ( static_cast<size_t>( row ) * kFieldDim ) + col;
            if ( !IsStraightRunTexel( sdf, index, row, col ) )
            {
                continue;
            }
            const float median = Desert::Tests::SdfTextRef::SdfTextMedian(
                 { msdf[( index * 3 ) + 0], msdf[( index * 3 ) + 1], msdf[( index * 3 ) + 2] } );
            worst = std::max( worst, static_cast<double>( std::fabs( median - sdf[index] ) ) );
            ++seen;
        }
    }

    ASSERT_GT( seen, 200 );
    std::ostringstream line;
    line << "[straight] " << seen << " in-band texels opposite an edge: worst median-vs-ordinary gap " << worst
         << " of the band (" << ( worst * kRange ) << " texels)";
    Report( line.str() );

    // One part in 255 of the band is a quantization step; allow four of them and nothing structural.
    EXPECT_LT( worst, 4.0 / kByteScale );
}

// The sign convention is measured, not assumed (fonts carry both windings). Get it backwards and every
// glyph renders as its own hole, which no unit test of distances alone would notice.
TEST( MsdfBake, InsideOfAStemIsPositive )
{
    const auto ttf = LoadRoboto();
    ASSERT_FALSE( ttf.empty() );
    const auto font = Desert::Text::BakeFontMSDF( ttf.data(), ttf.size() );
    ASSERT_TRUE( font.Valid() );
    ASSERT_TRUE( font.Glyphs.contains( 'I' ) );

    const auto& glyph = font.Glyphs.at( 'I' );
    ASSERT_GT( glyph.Width, 0.0F );

    // Centre of the cell: 'I' is a single stem, so its cell centre is ink.
    const auto cellX   = static_cast<size_t>( glyph.U0 * static_cast<float>( font.AtlasWidth ) );
    const auto cellY   = static_cast<size_t>( glyph.V0 * static_cast<float>( font.AtlasHeight ) );
    const auto centreX = cellX + static_cast<size_t>( glyph.Width * kHalfFloat );
    const auto centreY = cellY + static_cast<size_t>( glyph.Height * kHalfFloat );
    EXPECT_GT( MedianAt( font, centreX, centreY ), kHalfFloat ) << "the middle of a stem must read as INSIDE";

    // The cell's own corner is gutter: kGlyphPadding texels of it, so it is outside by construction.
    EXPECT_LT( MedianAt( font, cellX, cellY ), kHalfFloat ) << "the padding corner of a cell must read as OUTSIDE";

    // Alpha is opaque everywhere — the atlas is a field, not a mask (see BakedFont::AtlasRGBA).
    EXPECT_TRUE( EveryAlphaIsOpaque( font ) );
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
// One rendered glyph height, as a test parameter: the sizes are the interesting axis, and a loop of
// EXPECTs would report all three under one name and stop at the first failure.
class MsdfAtSize : public testing::TestWithParam<double>
{
protected:
    void SetUp() override
    {
        const auto ttf = LoadRoboto();
        ASSERT_FALSE( ttf.empty() );
        m_Font = Desert::Text::BakeFontMSDF( ttf.data(), ttf.size(), kBakeSize );
        ASSERT_TRUE( m_Font.Valid() );
        ASSERT_TRUE( m_Font.Glyphs.contains( 'A' ) );
        ASSERT_TRUE( m_Font.Glyphs.contains( 'a' ) );
        m_Field = AtlasAsFloats( m_Font );
    }

    Desert::Text::BakedFont m_Font;
    std::vector<float>      m_Field;
};

TEST_P( MsdfAtSize, FlooringTheRampAtOnePixelBeatsLettingItWiden )
{
    const Desert::Text::BakedFont& font  = m_Font;
    const auto&                    glyph = font.Glyphs.at( 'A' );
    const GlyphView                view  = ViewOf( m_Field, font, glyph );

    const double targetPx    = GetParam();
    const double scale       = targetPx / font.PixelHeight;
    const double texelsPerPx = 1.0 / scale;
    // Through the shader's own function, so the floor under test is the one the GPU applies.
    const float shippedRange =
         Desert::Tests::SdfTextRef::SdfTextScreenPxRange( UvDerivative( font, texelsPerPx ), AtlasSize( font ) );
    const auto unflooredRange = static_cast<float>( Desert::Text::kDistanceRangeTexels / texelsPerPx );

    const RampScore score = ScoreReconstruction(
         view, glyph, scale, [shippedRange]( const glm::vec3& msd )
         { return Desert::Tests::SdfTextRef::SdfTextAlpha( msd, shippedRange ); },
         [unflooredRange]( const glm::vec3& msd )
         { return Desert::Tests::SdfTextRef::SdfTextAlpha( msd, unflooredRange ); } );

    ASSERT_GT( score.Pixels, 4 );
    std::ostringstream line;
    line << "[minify] 'A' at " << targetPx << " px (" << score.Pixels << " screen pixels, true ramp "
         << unflooredRange << " px): shipped RMS " << score.FirstRms << " (worst pixel " << score.FirstMax
         << "), ramp left to widen RMS " << score.SecondRms;
    Report( line.str() );

    EXPECT_LE( score.FirstRms, score.SecondRms + 1e-6 ) << "the floored ramp is supposed to be no worse";
    // And the reconstruction stays a coverage estimate rather than collapsing: no screen pixel is more
    // than half a level of coverage away from the truth at any of these sizes.
    EXPECT_LT( score.FirstMax, kHalf );
}

TEST_P( MsdfAtSize, OnePixelLinearRampBeatsTheOldTwoPixelSmoothstep )
{
    const Desert::Text::BakedFont& font  = m_Font;
    const auto&                    glyph = font.Glyphs.at( 'a' );
    const GlyphView                view  = ViewOf( m_Field, font, glyph );

    const double targetPx    = GetParam();
    const double scale       = targetPx / font.PixelHeight;
    const double texelsPerPx = 1.0 / scale;
    const float  pxRange =
         Desert::Tests::SdfTextRef::SdfTextScreenPxRange( UvDerivative( font, texelsPerPx ), AtlasSize( font ) );
    // fwidth of the SAMPLED, normalized distance — what the old shader fed its smoothstep.
    const double oldHalfWidth = texelsPerPx / Desert::Text::kDistanceRangeTexels;

    const RampScore score = ScoreReconstruction(
         view, glyph, scale,
         [pxRange]( const glm::vec3& msd ) { return Desert::Tests::SdfTextRef::SdfTextAlpha( msd, pxRange ); },
         [oldHalfWidth]( const glm::vec3& msd )
         {
             const double distance = Desert::Tests::SdfTextRef::SdfTextMedian( msd );
             return Smoothstep( kHalf - oldHalfWidth, kHalf + oldHalfWidth, distance );
         } );

    ASSERT_GT( score.Pixels, 4 );
    std::ostringstream line;
    line << "[ramp] 'a' at " << targetPx << " px (" << score.Pixels << " screen pixels): one-pixel linear RMS "
         << score.FirstRms << ", old two-pixel smoothstep RMS " << score.SecondRms;
    Report( line.str() );

    EXPECT_LT( score.FirstRms, score.SecondRms );
}

// 34 and 20 px are UI sizes; 12 px is the smallest a label is set at; 10, 5 and 3 px are what a world
// label reaches between the middle distance and the horizon. 5 px is where the ten-texel band stops
// covering a screen pixel and 3 px is past it, which is the regime the floor exists for.
constexpr std::array<double, 6> kRenderedHeightsPx = { 34.0, 20.0, 12.0, 10.0, 5.0, 3.0 };
INSTANTIATE_TEST_SUITE_P( RenderedHeights, MsdfAtSize, testing::ValuesIn( kRenderedHeightsPx ) );
