// The Sculpt and Smooth strokes (L3, ported from UE 5.8 LandscapeEdModePaintTools.cpp) over the edit cache:
// what one step writes, what it leaves alone, that seam copies agree, and that the stroke's record undoes it
// byte for byte. Plus the census of the editor's controls table (every panel widget is a palette command).

#include <Engine/World/Landscape/LandscapeSculpt.hpp>

#include <Editor/Core/Selection/LandscapeSculptState.hpp>

#include <gtest/gtest.h>

#include "editor_erosion_capture.hpp"

#include <array>
#include <cmath>
#include <map>
#include <set>
#include <utility>
#include <vector>

using namespace Desert::World::Landscape;

namespace
{
    constexpr uint32_t kQuads = 31u;
    constexpr int32_t  kQ     = static_cast<int32_t>( kQuads );

    /// A deterministic bumpy field: large enough amplitude that smoothing has something to remove.
    uint16_t Noise( int32_t gx, int32_t gz )
    {
        // Hashed in uint32_t: the products overflow int32_t, and signed overflow is undefined (UBSan).
        uint32_t h = ( static_cast<uint32_t>( gx ) * 73856093u ) ^ ( static_cast<uint32_t>( gz ) * 19349663u );
        h ^= h >> 13;
        h *= 0x5bd1e995u;
        h ^= h >> 15;
        return static_cast<uint16_t>( 30000 + static_cast<int32_t>( h % 2001u ) );
    }

    /// A ramp along X: 60 steps per sample, so the picked height sits mid-slope.
    uint16_t Slope( int32_t gx, int32_t )
    {
        return static_cast<uint16_t>( 30000 + 60 * gx );
    }

    uint16_t Flat( int32_t, int32_t )
    {
        return kLandscapeMidSample;
    }

    struct World
    {
        LandscapeRoot                                            Root;
        std::map<std::pair<int32_t, int32_t>, LandscapeTileData> Tiles;

        explicit World( uint16_t ( *height )( int32_t, int32_t ) )
        {
            Root.QuadsPerTile = kQuads;
            for ( int32_t tz = 0; tz < 2; ++tz )
                for ( int32_t tx = 0; tx < 2; ++tx )
                {
                    std::vector<uint16_t> samples;
                    for ( int32_t z = 0; z <= kQ; ++z )
                        for ( int32_t x = 0; x <= kQ; ++x )
                            samples.push_back( height( tx * kQ + x, tz * kQ + z ) );
                    auto tile = LandscapeTileData::FromSamples( kQuads + 1u, kQuads + 1u, std::move( samples ) );
                    EXPECT_TRUE( tile.IsSuccess() );
                    Tiles.emplace( std::make_pair( tx, tz ), tile.GetValue() );
                }
        }

        LandscapeTileLookup Lookup()
        {
            return [this]( int32_t tx, int32_t tz ) -> LandscapeTileSlot
            {
                auto it = Tiles.find( { tx, tz } );
                if ( it == Tiles.end() )
                    return { LandscapeTileState::Absent, nullptr };
                return { LandscapeTileState::Present, &it->second };
            };
        }

        LandscapeSampleBounds Bounds() const
        {
            return { 0, 0, 2 * kQ, 2 * kQ };
        }

        /// The value at a global sample, read from the lowest tile that stores it.
        uint16_t At( int32_t gx, int32_t gz ) const
        {
            const int32_t tx = std::min( gx / kQ, 1 );
            const int32_t tz = std::min( gz / kQ, 1 );
            return Tiles.at( { tx, tz } )
                 .Sample( static_cast<uint32_t>( gx - tx * kQ ), static_cast<uint32_t>( gz - tz * kQ ) );
        }

        std::vector<uint16_t> Bytes() const
        {
            std::vector<uint16_t> all;
            for ( const auto& [key, tile] : Tiles )
                for ( uint32_t z = 0; z <= kQuads; ++z )
                    for ( uint32_t x = 0; x <= kQuads; ++x )
                        all.push_back( tile.Sample( x, z ) );
            return all;
        }
    };

    LandscapeBrushSettings Brush( float radiusCm = 800.0f, float strength = 1.0f )
    {
        LandscapeBrushSettings b;
        b.RadiusCm        = radiusCm;
        b.FalloffFraction = 0.5f;
        b.Shape           = LandscapeBrushFalloff::Smooth;
        b.Strength        = strength;
        return b;
    }

    LandscapeBrushWeights Weights( const World& w, const LandscapeBrushSettings& b, glm::vec2 centreCm )
    {
        const std::array<glm::vec2, 1> positions = { centreCm };
        auto                           weights   = ComputeLandscapeBrush( w.Root, b, positions );
        EXPECT_TRUE( weights.IsSuccess() ) << ( weights.IsSuccess() ? "" : weights.GetError() );
        return weights.GetValue();
    }

    struct Moments
    {
        double Mean     = 0.0;
        double Variance = 0.0;
    };

    /// Mean and variance over the samples where the brush weighs at least @p floor. The variance is read where
    /// the brush weighs 1 (the part a stroke fully owns); the mean over everything, because a local average
    /// moves heights between neighbours and only the total is conserved.
    Moments Inner( const World& w, const LandscapeBrushWeights& weights, float floor = 1.0f )
    {
        double sum = 0.0;
        double sq  = 0.0;
        int    n   = 0;
        for ( int32_t z = 0; z <= 2 * kQ; ++z )
            for ( int32_t x = 0; x <= 2 * kQ; ++x )
                if ( weights.At( x, z ) >= floor )
                {
                    const double v = w.At( x, z );
                    sum += v;
                    sq += v * v;
                    ++n;
                }
        EXPECT_GT( n, 20 );
        const double mean = sum / n;
        return { mean, sq / n - mean * mean };
    }
} // namespace

TEST( LandscapeSculpt, StrengthIsUEsFormula )
{
    World w( Flat );
    // Strength · (Radius · 128 / ZScale) · min(dt, 0.1) · 3 = 1 · 800 · 1.28 · 0.3.
    EXPECT_FLOAT_EQ( LandscapeSculptStrength( w.Root, Brush(), { false, 0.1f } ), 307.2f );
    // UE caps dt at 0.1 s and floors a positive strength at one step.
    EXPECT_FLOAT_EQ( LandscapeSculptStrength( w.Root, Brush(), { false, 5.0f } ), 307.2f );
    EXPECT_FLOAT_EQ( LandscapeSculptStrength( w.Root, Brush( 800.0f, 0.001f ), { false, 0.001f } ), 1.0f );
    EXPECT_FLOAT_EQ( LandscapeSculptStrength( w.Root, Brush(), { false, 0.0f } ), 0.0f );
}

TEST( LandscapeSculpt, SculptRaisesEachSampleByStrengthTimesWeightAndNothingOutside )
{
    World                 w( Flat );
    const auto            b       = Brush();
    const auto            weights = Weights( w, b, { 1500.0f, 1500.0f } );
    const float           s       = LandscapeSculptStrength( w.Root, b, { false, 0.1f } );
    LandscapeHeightStroke stroke( w.Root, w.Lookup(), w.Bounds() );
    ASSERT_TRUE( stroke.ApplySculpt( weights, b, { false, 0.1f } ).IsSuccess() );

    EXPECT_EQ( w.At( 15, 15 ), kLandscapeMidSample + 307 ) << "the centre rises by strength x weight 1";
    int outside = 0;
    for ( int32_t z = 0; z <= 2 * kQ; ++z )
        for ( int32_t x = 0; x <= 2 * kQ; ++x )
        {
            const float weight = weights.At( x, z );
            EXPECT_EQ( w.At( x, z ),
                       kLandscapeMidSample + static_cast<int32_t>( std::floor( weight * s + 0.5f ) ) )
                 << x << ", " << z;
            if ( weight == 0.0f )
                ++outside;
        }
    EXPECT_GT( outside, 3000 ) << "most of the landscape is outside the brush and must be untouched";
}

TEST( LandscapeSculpt, ShiftLowersByTheSameAmount )
{
    World                 w( Flat );
    const auto            b = Brush();
    LandscapeHeightStroke stroke( w.Root, w.Lookup(), w.Bounds() );
    ASSERT_TRUE( stroke.ApplySculpt( Weights( w, b, { 1500.0f, 1500.0f } ), b, { true, 0.1f } ).IsSuccess() );
    EXPECT_EQ( w.At( 15, 15 ), kLandscapeMidSample - 307 );
}

TEST( LandscapeSculpt, ToolStrengthIsAppliedOnce )
{
    // ComputeLandscapeBrush folds the strength into the weights; UE applies it once, in the tool.
    World                 w( Flat );
    const auto            b = Brush( 800.0f, 0.5f );
    LandscapeHeightStroke stroke( w.Root, w.Lookup(), w.Bounds() );
    ASSERT_TRUE( stroke.ApplySculpt( Weights( w, b, { 1500.0f, 1500.0f } ), b, { false, 0.1f } ).IsSuccess() );
    EXPECT_EQ( w.At( 15, 15 ), kLandscapeMidSample + 154 )
         << "strength 0.5 raises by half of 307.2, not a quarter";
}

TEST( LandscapeSculpt, SmoothReducesVarianceAndKeepsTheMean )
{
    for ( const bool detail : { false, true } )
    {
        World                   w( Noise );
        const auto              b       = Brush( 1200.0f );
        const auto              weights = Weights( w, b, { 3100.0f, 3100.0f } );
        const Moments           before  = Inner( w, weights );
        const double            total   = Inner( w, weights, 0.0f ).Mean;
        LandscapeSmoothSettings smooth;
        smooth.DetailSmooth = detail;
        smooth.DetailScale  = 0.7f;
        LandscapeHeightStroke stroke( w.Root, w.Lookup(), w.Bounds() );
        ASSERT_TRUE( stroke.ApplySmooth( weights, b, smooth ).IsSuccess() );
        const Moments after = Inner( w, weights );
        EXPECT_LT( after.Variance, 0.5 * before.Variance ) << "detail " << detail;
        // UE's integer average and truncating lerp bias the mean down by under a step; the noise's own
        // spread is ~580 steps, so two steps on the landscape's mean is "kept" with a wide margin and a real shift
        // of the level fails.
        EXPECT_NEAR( Inner( w, weights, 0.0f ).Mean, total, 2.0 ) << "detail " << detail;
    }
}

TEST( LandscapeSculpt, AStrokeAcrossSeamsWritesEqualCopies )
{
    World                 w( Noise );
    const auto            b = Brush( 1000.0f );
    LandscapeHeightStroke stroke( w.Root, w.Lookup(), w.Bounds() );
    ASSERT_TRUE( stroke.ApplySculpt( Weights( w, b, { 3100.0f, 3100.0f } ), b, { false, 0.1f } ).IsSuccess() );
    ASSERT_TRUE( stroke.ApplySmooth( Weights( w, b, { 3100.0f, 2500.0f } ), b, {} ).IsSuccess() );
    int changed = 0;
    for ( int32_t i = 0; i <= kQ; ++i )
    {
        // X seam (gx = 31) between tiles (0, z) and (1, z); Z seam likewise.
        for ( int32_t t = 0; t < 2; ++t )
        {
            const uint32_t u = static_cast<uint32_t>( i );
            EXPECT_EQ( w.Tiles.at( { 0, t } ).Sample( kQuads, u ), w.Tiles.at( { 1, t } ).Sample( 0u, u ) );
            EXPECT_EQ( w.Tiles.at( { t, 0 } ).Sample( u, kQuads ), w.Tiles.at( { t, 1 } ).Sample( u, 0u ) );
        }
        changed += w.At( kQ, i ) != Noise( kQ, i );
    }
    EXPECT_GT( changed, 5 ) << "the seam itself must have been edited, or the equality proves nothing";
}

TEST( LandscapeSculpt, TheStrokeRecordUndoesAndRedoesByteForByte )
{
    World                       w( Noise );
    const std::vector<uint16_t> original = w.Bytes();
    const auto                  b        = Brush( 900.0f );
    LandscapeHeightStroke       stroke( w.Root, w.Lookup(), w.Bounds() );
    // A dragged stroke: overlapping steps, both tools, one crossing the corner where four tiles meet.
    for ( const glm::vec2 p : { glm::vec2( 1200.0f, 1400.0f ), glm::vec2( 2000.0f, 2400.0f ),
                                glm::vec2( 3100.0f, 3100.0f ), glm::vec2( 5900.0f, 200.0f ) } )
        ASSERT_TRUE( stroke.ApplySculpt( Weights( w, b, p ), b, { false, 0.05f } ).IsSuccess() );
    ASSERT_TRUE( stroke.ApplySmooth( Weights( w, b, { 2600.0f, 2600.0f } ), b, {} ).IsSuccess() );
    const std::vector<uint16_t> edited = w.Bytes();
    ASSERT_NE( edited, original );

    auto record = stroke.Finish();
    ASSERT_TRUE( record.IsSuccess() ) << record.GetError();
    const auto& r = record.GetValue();
    ASSERT_TRUE( WriteLandscapeHeights( w.Root, w.Lookup(), r.Rect, r.Before ).IsSuccess() );
    EXPECT_EQ( w.Bytes(), original ) << "undo";
    ASSERT_TRUE( WriteLandscapeHeights( w.Root, w.Lookup(), r.Rect, r.After ).IsSuccess() );
    EXPECT_EQ( w.Bytes(), edited ) << "redo";
}

TEST( LandscapeSculpt, AnUntouchedStrokeHasNoRecordAndBadSmoothSettingsAreRefused )
{
    World                 w( Flat );
    LandscapeHeightStroke stroke( w.Root, w.Lookup(), w.Bounds() );
    EXPECT_FALSE( stroke.Finish().IsSuccess() );
    LandscapeSmoothSettings bad;
    bad.FilterKernelRadius = 32;
    EXPECT_FALSE( stroke.ApplySmooth( Weights( w, Brush(), { 1500.0f, 1500.0f } ), Brush(), bad ).IsSuccess() );
    EXPECT_EQ( w.At( 15, 15 ), kLandscapeMidSample );
}

TEST( LandscapeSculpt, FlattenReachesThePickedHeightWithTheBrushWeight )
{
    World                    w( Slope );
    const auto               b       = Brush( 900.0f );
    const auto               weights = Weights( w, b, { 1500.0f, 1500.0f } );
    LandscapeFlattenSettings f;
    LandscapeHeightStroke    stroke( w.Root, w.Lookup(), w.Bounds() );
    ASSERT_TRUE( stroke.ApplyFlatten( weights, b, f, { 1500.0f, 1500.0f } ).IsSuccess() );
    const uint16_t target = Slope( 15, 15 );
    int            full = 0, partial = 0;
    for ( int32_t z = 0; z <= 2 * kQ; ++z )
        for ( int32_t x = 0; x <= 2 * kQ; ++x )
        {
            const float    wt = weights.At( x, z );
            const uint16_t o  = Slope( x, z );
            if ( wt >= 1.0f )
            {
                EXPECT_EQ( w.At( x, z ), target ) << x << "," << z;
                ++full;
            }
            else if ( wt > 0.0f )
            {
                const float    v      = static_cast<float>( o ) + wt * ( static_cast<float>( target ) - o );
                const uint16_t expect = static_cast<uint16_t>( o > target ? std::floor( v ) : std::ceil( v ) );
                EXPECT_EQ( w.At( x, z ), expect ) << x << "," << z << " weight " << wt;
                partial += expect != o;
            }
            else
                EXPECT_EQ( w.At( x, z ), o );
        }
    EXPECT_GT( full, 5 );
    EXPECT_GT( partial, 5 );

    // The picked height is kept for the stroke: a second step elsewhere flattens to the same value.
    ASSERT_TRUE(
         stroke.ApplyFlatten( Weights( w, b, { 2500.0f, 1500.0f } ), b, f, { 2500.0f, 1500.0f } ).IsSuccess() );
    EXPECT_EQ( w.At( 25, 15 ), target );
}

TEST( LandscapeSculpt, FlattenRaiseNeverLowersAndLowerNeverRaises )
{
    for ( const LandscapeFlattenMode mode : { LandscapeFlattenMode::Raise, LandscapeFlattenMode::Lower } )
    {
        World                    w( Slope );
        const auto               b = Brush( 900.0f, 0.5f );
        LandscapeFlattenSettings f;
        f.Mode = mode;
        LandscapeHeightStroke stroke( w.Root, w.Lookup(), w.Bounds() );
        ASSERT_TRUE( stroke.ApplyFlatten( Weights( w, b, { 1500.0f, 1500.0f } ), b, f, { 1500.0f, 1500.0f } )
                          .IsSuccess() );
        int moved = 0;
        for ( int32_t z = 0; z <= 2 * kQ; ++z )
            for ( int32_t x = 0; x <= 2 * kQ; ++x )
            {
                const int32_t d = static_cast<int32_t>( w.At( x, z ) ) - Slope( x, z );
                if ( mode == LandscapeFlattenMode::Raise )
                    EXPECT_GE( d, 0 ) << x;
                else
                    EXPECT_LE( d, 0 ) << x;
                moved += d != 0;
            }
        EXPECT_GT( moved, 5 ) << "the side of the slope facing the mode must have moved";
    }
}

TEST( LandscapeSculpt, SlopeFlattenOnAPlaneKeepsThePlane )
{
    World                    w( Slope );
    const auto               b = Brush( 900.0f );
    LandscapeFlattenSettings f;
    f.UseSlopeFlatten = true;
    LandscapeHeightStroke stroke( w.Root, w.Lookup(), w.Bounds() );
    ASSERT_TRUE(
         stroke.ApplyFlatten( Weights( w, b, { 1500.0f, 1500.0f } ), b, f, { 1500.0f, 1500.0f } ).IsSuccess() );
    for ( int32_t z = 0; z <= 2 * kQ; ++z )
        for ( int32_t x = 0; x <= 2 * kQ; ++x )
            EXPECT_NEAR( w.At( x, z ), Slope( x, z ), 1 ) << x << "," << z;
}

TEST( LandscapeSculpt, NoiseIsAFunctionOfTheSampleAndCentredInBothMode )
{
    EXPECT_FLOAT_EQ( LandscapeNoiseSample( -7, 3, 16.0f ), LandscapeNoiseSample( 7, 3, 16.0f ) ) << "UE's |x|";
    EXPECT_FLOAT_EQ( LandscapeNoiseSample( 5, 9, 1.0f ), 0.0f ) << "Perlin is zero on its lattice";

    LandscapeNoiseSettings n;
    n.NoiseScale            = 4.0f;
    const auto            b = Brush( 2500.0f, 0.2f );
    World                 w1( Flat ), w2( Flat );
    LandscapeHeightStroke s1( w1.Root, w1.Lookup(), w1.Bounds() );
    LandscapeHeightStroke s2( w2.Root, w2.Lookup(), w2.Bounds() );
    ASSERT_TRUE( s1.ApplyNoise( Weights( w1, b, { 3100.0f, 3100.0f } ), b, n ).IsSuccess() );
    ASSERT_TRUE( s2.ApplyNoise( Weights( w2, b, { 3100.0f, 3100.0f } ), b, n ).IsSuccess() );
    EXPECT_EQ( w1.Bytes(), w2.Bytes() ) << "the same stroke writes the same bytes";

    double sum = 0.0, maxAbs = 0.0;
    int    count = 0;
    for ( int32_t z = 0; z <= 2 * kQ; ++z )
        for ( int32_t x = 0; x <= 2 * kQ; ++x )
        {
            const double d = static_cast<double>( w1.At( x, z ) ) - kLandscapeMidSample;
            sum += d;
            maxAbs = std::max( maxAbs, std::abs( d ) );
            count += d != 0.0;
        }
    ASSERT_GT( count, 200 );
    ASSERT_GT( maxAbs, 20.0 );
    EXPECT_LT( std::abs( sum / count ), 0.1 * maxAbs ) << "Both mode is centred on zero";

    for ( const LandscapeNoiseMode mode : { LandscapeNoiseMode::Add, LandscapeNoiseMode::Sub } )
    {
        World                 w( Flat );
        LandscapeHeightStroke s( w.Root, w.Lookup(), w.Bounds() );
        n.Mode = mode;
        ASSERT_TRUE( s.ApplyNoise( Weights( w, b, { 3100.0f, 3100.0f } ), b, n ).IsSuccess() );
        for ( int32_t z = 0; z <= 2 * kQ; ++z )
            for ( int32_t x = 0; x <= 2 * kQ; ++x )
                if ( mode == LandscapeNoiseMode::Add )
                    EXPECT_GE( w.At( x, z ), kLandscapeMidSample - 1 );
                else
                    EXPECT_LE( w.At( x, z ), kLandscapeMidSample + 1 );
    }
}

TEST( LandscapeSculpt, EraseReturnsToLocalZeroWithoutOvershoot )
{
    World                 w( Noise );
    const auto            b       = Brush( 900.0f, 0.5f );
    const auto            weights = Weights( w, b, { 1500.0f, 1500.0f } );
    LandscapeHeightStroke stroke( w.Root, w.Lookup(), w.Bounds() );
    ASSERT_TRUE( stroke.ApplyErase( weights, b ).IsSuccess() );
    int32_t moved = 0;
    for ( int32_t z = 0; z <= 2 * kQ; ++z )
        for ( int32_t x = 0; x <= 2 * kQ; ++x )
        {
            const int32_t o = Noise( x, z ) - kLandscapeMidSample;
            const int32_t n = w.At( x, z ) - kLandscapeMidSample;
            if ( weights.At( x, z ) <= 0.0f )
            {
                EXPECT_EQ( n, o );
                continue;
            }
            EXPECT_LE( std::abs( n ), std::abs( o ) ) << "towards zero";
            EXPECT_TRUE( n == 0 || ( n > 0 ) == ( o > 0 ) ) << "never across zero";
            moved += n != o;
        }
    EXPECT_GT( moved, 20 );
    // Strength 1 at weight 1 is local zero exactly.
    LandscapeHeightStroke full( w.Root, w.Lookup(), w.Bounds() );
    const auto            one = Brush( 900.0f, 1.0f );
    ASSERT_TRUE( full.ApplyErase( Weights( w, one, { 1500.0f, 1500.0f } ), one ).IsSuccess() );
    EXPECT_EQ( w.At( 15, 15 ), kLandscapeMidSample );
}

TEST( LandscapeSculpt, FlattenNoiseAndEraseWriteEqualSeamCopiesAndUndoByteForByte )
{
    World                       w( Noise );
    const std::vector<uint16_t> original = w.Bytes();
    const auto                  b        = Brush( 1000.0f );
    LandscapeHeightStroke       stroke( w.Root, w.Lookup(), w.Bounds() );
    LandscapeNoiseSettings      n;
    n.NoiseScale = 8.0f;
    ASSERT_TRUE( stroke.ApplyNoise( Weights( w, b, { 3100.0f, 3100.0f } ), b, n ).IsSuccess() );
    ASSERT_TRUE(
         stroke.ApplyFlatten( Weights( w, b, { 3100.0f, 2000.0f } ), b, {}, { 3100.0f, 2000.0f } ).IsSuccess() );
    ASSERT_TRUE( stroke.ApplyErase( Weights( w, b, { 2000.0f, 3100.0f } ), b ).IsSuccess() );
    int changed = 0;
    for ( int32_t i = 0; i <= 2 * kQ; ++i )
    {
        const uint32_t u = static_cast<uint32_t>( i % ( kQ + 1 ) );
        for ( int32_t t = 0; t < 2; ++t )
        {
            EXPECT_EQ( w.Tiles.at( { 0, t } ).Sample( kQuads, u ), w.Tiles.at( { 1, t } ).Sample( 0u, u ) );
            EXPECT_EQ( w.Tiles.at( { t, 0 } ).Sample( u, kQuads ), w.Tiles.at( { t, 1 } ).Sample( u, 0u ) );
        }
        changed += w.At( kQ, i ) != Noise( kQ, i );
        changed += w.At( i, kQ ) != Noise( i, kQ );
    }
    EXPECT_GT( changed, 10 ) << "the seams themselves must have been edited";

    const std::vector<uint16_t> edited = w.Bytes();
    auto                        record = stroke.Finish();
    ASSERT_TRUE( record.IsSuccess() ) << record.GetError();
    const auto& r = record.GetValue();
    ASSERT_TRUE( WriteLandscapeHeights( w.Root, w.Lookup(), r.Rect, r.Before ).IsSuccess() );
    EXPECT_EQ( w.Bytes(), original ) << "undo";
    ASSERT_TRUE( WriteLandscapeHeights( w.Root, w.Lookup(), r.Rect, r.After ).IsSuccess() );
    EXPECT_EQ( w.Bytes(), edited ) << "redo";
}

TEST( LandscapeSculpt, BadFlattenAndNoiseSettingsAreRefused )
{
    World                    w( Flat );
    LandscapeHeightStroke    stroke( w.Root, w.Lookup(), w.Bounds() );
    LandscapeFlattenSettings f;
    f.TerraceIntervalCm = 0.0f;
    EXPECT_FALSE( stroke.ApplyFlatten( Weights( w, Brush(), { 1500.0f, 1500.0f } ), Brush(), f, {} ).IsSuccess() );
    LandscapeNoiseSettings n;
    n.NoiseScale = 0.5f;
    EXPECT_FALSE( stroke.ApplyNoise( Weights( w, Brush(), { 1500.0f, 1500.0f } ), Brush(), n ).IsSuccess() );
}

TEST( LandscapeSculpt, EveryPanelControlIsADistinctCommandThatKeepsTheSettingsValid )
{
    using namespace Desert::Editor::Core;
    const auto            controls = LandscapeToolControls();
    std::set<std::string> labels;
    std::set<std::string> rows;
    for ( const auto& c : controls )
    {
        EXPECT_TRUE( labels.insert( c.Label ).second ) << "duplicate palette label " << c.Label;
        EXPECT_NE( c.Label.find( ": " ), std::string::npos ) << c.Label;
        rows.insert( c.Row );
    }
    // Every setting of LandscapeSculptSettings has a row: tool, radius, falloff, shape, strength, filter radius,
    // detail smooth, detail scale, flatten mode, slope flatten, pick per apply, terrace interval, terrace smooth,
    // noise mode, noise scale, ramp mode, ramp width, side falloff — and the ramp's action row; erosion threshold,
    // iterations, noise mode, noise scale; rain amount, sediment capacity, hydro iterations, rain distribution,
    // rain scale, hydro detail smooth, hydro detail scale; mirror operation, mirror smoothing and the mirror's
    // action row; paste mode and the copy and paste action rows.
    EXPECT_EQ( rows.size(), 36u );

    LandscapeSculptSettings other;
    other.Tool                          = LandscapeTool::Smooth;
    other.Brush.RadiusCm                = 400.0f;
    other.Brush.FalloffFraction         = 0.2f;
    other.Brush.Shape                   = LandscapeBrushFalloff::Tip;
    other.Brush.Strength                = 0.4f;
    other.Smooth.FilterKernelRadius     = 9;
    other.Smooth.DetailSmooth           = true;
    other.Smooth.DetailScale            = 0.5f;
    other.Flatten.Mode                  = LandscapeFlattenMode::Terrace;
    other.Flatten.UseSlopeFlatten       = true;
    other.Flatten.PickValuePerApply     = true;
    other.Flatten.TerraceIntervalCm     = 256.0f;
    other.Flatten.TerraceSmooth         = 0.5f;
    other.Noise.Mode                    = LandscapeNoiseMode::Sub;
    other.Noise.NoiseScale              = 64.0f;
    other.Ramp.Mode                     = LandscapeRampMode::Lower;
    other.Ramp.WidthCm                  = 500.0f;
    other.Ramp.SideFalloff              = 0.7f;
    other.Erosion.Threshold             = 128;
    other.Erosion.Iterations            = 10;
    other.Erosion.NoiseMode             = LandscapeErosionNoiseMode::Both;
    other.Erosion.NoiseScale            = 16.0f;
    other.HydroErosion.RainAmount       = 64;
    other.HydroErosion.SedimentCapacity = 0.5f;
    other.HydroErosion.Iterations       = 20;
    other.HydroErosion.RainMode         = LandscapeRainMode::Positive;
    other.HydroErosion.RainScale        = 16.0f;
    other.HydroErosion.DetailSmooth     = false;
    other.HydroErosion.DetailScale      = 0.5f;
    other.Mirror.Op                     = LandscapeMirrorOp::RotatePlusZToMinusZ;
    other.Mirror.SmoothingWidth         = 20;
    other.PasteMode                     = LandscapePasteMode::Lower;
    auto key                            = []( const LandscapeSculptSettings& s )
    {
        return std::to_string( static_cast<int>( s.Tool ) ) + std::to_string( s.Brush.RadiusCm ) +
               std::to_string( s.Brush.FalloffFraction ) + std::to_string( static_cast<int>( s.Brush.Shape ) ) +
               std::to_string( s.Brush.Strength ) + std::to_string( s.Smooth.FilterKernelRadius ) +
               std::to_string( s.Smooth.DetailSmooth ) + std::to_string( s.Smooth.DetailScale ) +
               std::to_string( static_cast<int>( s.Flatten.Mode ) ) + std::to_string( s.Flatten.UseSlopeFlatten ) +
               std::to_string( s.Flatten.PickValuePerApply ) + std::to_string( s.Flatten.TerraceIntervalCm ) +
               std::to_string( s.Flatten.TerraceSmooth ) + std::to_string( static_cast<int>( s.Noise.Mode ) ) +
               std::to_string( s.Noise.NoiseScale ) + std::to_string( static_cast<int>( s.Ramp.Mode ) ) +
               std::to_string( s.Ramp.WidthCm ) + std::to_string( s.Ramp.SideFalloff ) + "|" +
               std::to_string( s.Erosion.Threshold ) + std::to_string( s.Erosion.Iterations ) +
               std::to_string( static_cast<int>( s.Erosion.NoiseMode ) ) + std::to_string( s.Erosion.NoiseScale ) +
               std::to_string( s.HydroErosion.RainAmount ) + std::to_string( s.HydroErosion.SedimentCapacity ) +
               std::to_string( s.HydroErosion.Iterations ) +
               std::to_string( static_cast<int>( s.HydroErosion.RainMode ) ) +
               std::to_string( s.HydroErosion.RainScale ) + std::to_string( s.HydroErosion.DetailSmooth ) +
               std::to_string( s.HydroErosion.DetailScale ) + "|" +
               std::to_string( static_cast<int>( s.Mirror.Op ) ) + std::to_string( s.Mirror.SmoothingWidth ) +
               std::to_string( static_cast<int>( s.PasteMode ) );
    };
    std::set<LandscapeStrokeRequest> requests;
    for ( const auto& c : controls )
    {
        if ( c.Request != LandscapeStrokeRequest::None )
        {
            // An action row posts a request instead of editing a setting; it must not carry both.
            EXPECT_FALSE( static_cast<bool>( c.Apply ) ) << c.Label;
            EXPECT_TRUE( requests.insert( c.Request ).second ) << "two rows post the request of " << c.Label;
            continue;
        }
        bool changes = false;
        for ( const LandscapeSculptSettings& base : { LandscapeSculptSettings{}, other } )
        {
            LandscapeSculptSettings s = base;
            c.Apply( s );
            changes = changes || key( s ) != key( base );
            EXPECT_TRUE( ValidateLandscapeBrush( s.Brush ).IsSuccess() ) << c.Label;
            EXPECT_TRUE( ValidateLandscapeSmooth( s.Smooth ).IsSuccess() ) << c.Label;
            EXPECT_TRUE( ValidateLandscapeFlatten( s.Flatten ).IsSuccess() ) << c.Label;
            EXPECT_TRUE( ValidateLandscapeNoise( s.Noise ).IsSuccess() ) << c.Label;
            EXPECT_TRUE( ValidateLandscapeRamp( s.Ramp ).IsSuccess() ) << c.Label;
            EXPECT_TRUE( ValidateLandscapeErosion( s.Erosion ).IsSuccess() ) << c.Label;
            EXPECT_TRUE( ValidateLandscapeHydroErosion( s.HydroErosion ).IsSuccess() ) << c.Label;
            EXPECT_GE( s.Mirror.SmoothingWidth, 0 ) << c.Label;
            EXPECT_LE( s.Mirror.SmoothingWidth, kLandscapeMaxMirrorSmoothingUi ) << c.Label;
        }
        EXPECT_TRUE( changes ) << c.Label << " changes nothing from either base: a dead widget";
    }
    EXPECT_EQ( requests, ( std::set<LandscapeStrokeRequest>{
                              LandscapeStrokeRequest::RampStart, LandscapeStrokeRequest::RampEnd,
                              LandscapeStrokeRequest::RampApply, LandscapeStrokeRequest::RampReset,
                              LandscapeStrokeRequest::MirrorPoint, LandscapeStrokeRequest::MirrorApply,
                              LandscapeStrokeRequest::CopyCornerA, LandscapeStrokeRequest::CopyCornerB,
                              LandscapeStrokeRequest::Copy, LandscapeStrokeRequest::Paste } ) );
}

namespace
{
    /// World height (cm) to a sample value, as UE's Points.Z * LANDSCAPE_INV_ZSCALE + MidValue.
    double SampleOf( const World& w, float heightCm )
    {
        return ( heightCm - w.Root.Origin.y ) / w.Root.ZScale * kLandscapeStepsPerLocal + kLandscapeMidSample;
    }

    /// A ramp along X on the Z seam (gz = 31) from sample x 10 to 50, crossing the X seam at gx = 31:
    /// 2000 cm wide = 10 samples each side, side falloff 0.4 = the outer 4 of them.
    struct RampCase
    {
        glm::vec3             Start{ 1000.0f, 200.0f, 3100.0f };
        glm::vec3             End{ 5000.0f, 600.0f, 3100.0f };
        LandscapeRampSettings Ramp;
    };
} // namespace

TEST( LandscapeSculpt, RampIsLinearAlongTheAxisAndFlatAcrossTheInnerWidth )
{
    World                 w( Flat );
    const RampCase        c;
    LandscapeHeightStroke stroke( w.Root, w.Lookup(), w.Bounds() );
    ASSERT_TRUE( stroke.ApplyRamp( c.Start, c.End, c.Ramp ).IsSuccess() );
    const double h0 = SampleOf( w, c.Start.y );
    const double h1 = SampleOf( w, c.End.y );
    ASSERT_GT( h1 - h0, 400.0 ) << "the slope must span many steps, or linearity proves nothing";
    for ( int32_t x = 11; x <= 49; ++x )
    {
        const double line = h0 + ( h1 - h0 ) * ( x - 10 ) / 40.0;
        EXPECT_NEAR( w.At( x, 31 ), std::floor( line ), 1.0 ) << "axis sample x " << x;
        // Inside the inner width (|dz| < 6) the cross-section is flat: the ramp owns the sample outright.
        for ( int32_t dz = -5; dz <= 5; ++dz )
            EXPECT_NEAR( w.At( x, 31 + dz ), w.At( x, 31 ), 1.0 ) << "x " << x << " dz " << dz;
    }
}

TEST( LandscapeSculpt, RampSidesBlendByUEsCosineAndStopAtTheWidth )
{
    World                 w( Flat );
    const RampCase        c;
    LandscapeHeightStroke stroke( w.Root, w.Lookup(), w.Bounds() );
    ASSERT_TRUE( stroke.ApplyRamp( c.Start, c.End, c.Ramp ).IsSuccess() );
    const double mid = kLandscapeMidSample;
    for ( int32_t x = 12; x <= 48; x += 4 )
    {
        const double target = w.At( x, 31 );
        for ( const int32_t dz : { 7, 8, 9 } )
        {
            // The interpolant is 0 at the outer edge (10 samples out) and 1 at the inner edge (6 out).
            const double alphaX = ( 10.0 - dz ) / 4.0;
            const double alpha  = 0.5 - 0.5 * std::cos( alphaX * 3.14159265358979323846 );
            const double want   = mid + ( target - mid ) * alpha;
            EXPECT_NEAR( w.At( x, 31 + dz ), want, 2.0 ) << "x " << x << " dz +" << dz;
            EXPECT_NEAR( w.At( x, 31 - dz ), want, 2.0 ) << "x " << x << " dz -" << dz;
        }
        for ( const int32_t dz : { 10, 11, 14 } )
        {
            EXPECT_EQ( w.At( x, 31 + dz ), kLandscapeMidSample ) << "outside the width, x " << x << " dz " << dz;
            EXPECT_EQ( w.At( x, 31 - dz ), kLandscapeMidSample ) << "outside the width, x " << x << " dz " << dz;
        }
    }
    for ( int32_t z = 0; z <= 2 * kQ; ++z )
        EXPECT_EQ( w.At( 55, z ), kLandscapeMidSample ) << "past the end, z " << z;
}

TEST( LandscapeSculpt, RampRaiseNeverLowersAndLowerNeverRaises )
{
    for ( const LandscapeRampMode mode :
          { LandscapeRampMode::Raise, LandscapeRampMode::Lower, LandscapeRampMode::Both } )
    {
        World      w( Noise );
        const auto before = w.Bytes();
        RampCase   c;
        // Level with the middle of the noise band (30000..32000), so both directions have samples to move.
        c.Start.y = c.End.y =
             static_cast<float>( ( 31000.0 - kLandscapeMidSample ) / kLandscapeStepsPerLocal * w.Root.ZScale );
        c.Ramp.Mode = mode;
        LandscapeHeightStroke stroke( w.Root, w.Lookup(), w.Bounds() );
        ASSERT_TRUE( stroke.ApplyRamp( c.Start, c.End, c.Ramp ).IsSuccess() );
        const auto after = w.Bytes();
        int        up    = 0;
        int        down  = 0;
        for ( size_t i = 0; i < after.size(); ++i )
        {
            up += after[i] > before[i];
            down += after[i] < before[i];
        }
        const char* name = Desert::Editor::Core::LandscapeRampModeName( mode );
        EXPECT_EQ( down > 0, mode != LandscapeRampMode::Raise ) << name << " down " << down;
        EXPECT_EQ( up > 0, mode != LandscapeRampMode::Lower ) << name << " up " << up;
    }
}

TEST( LandscapeSculpt, RampWritesEqualSeamCopiesAndUndoesByteForByte )
{
    World                 w( Noise );
    const auto            original = w.Bytes();
    const RampCase        c;
    LandscapeHeightStroke stroke( w.Root, w.Lookup(), w.Bounds() );
    ASSERT_TRUE( stroke.ApplyRamp( c.Start, c.End, c.Ramp ).IsSuccess() );
    int changedX = 0;
    int changedZ = 0;
    for ( int32_t i = 0; i <= kQ; ++i )
    {
        for ( int32_t t = 0; t < 2; ++t )
        {
            const uint32_t u = static_cast<uint32_t>( i );
            EXPECT_EQ( w.Tiles.at( { 0, t } ).Sample( kQuads, u ), w.Tiles.at( { 1, t } ).Sample( 0u, u ) );
            EXPECT_EQ( w.Tiles.at( { t, 0 } ).Sample( u, kQuads ), w.Tiles.at( { t, 1 } ).Sample( u, 0u ) );
        }
        changedX += w.At( kQ, i ) != Noise( kQ, i );
        changedZ += w.At( i, kQ ) != Noise( i, kQ );
    }
    EXPECT_GT( changedX, 5 ) << "the X seam must have been edited, or the equality proves nothing";
    EXPECT_GT( changedZ, 5 ) << "the Z seam must have been edited, or the equality proves nothing";

    auto record = stroke.Finish();
    ASSERT_TRUE( record.IsSuccess() );
    const auto edited = w.Bytes();
    ASSERT_NE( edited, original );
    const auto& r = record.GetValue();
    ASSERT_TRUE( WriteLandscapeHeights( w.Root, w.Lookup(), r.Rect, r.Before ).IsSuccess() );
    EXPECT_EQ( w.Bytes(), original );
    ASSERT_TRUE( WriteLandscapeHeights( w.Root, w.Lookup(), r.Rect, r.After ).IsSuccess() );
    EXPECT_EQ( w.Bytes(), edited );
}

TEST( LandscapeSculpt, BadRampsAreRefused )
{
    World                 w( Flat );
    LandscapeHeightStroke stroke( w.Root, w.Lookup(), w.Bounds() );
    const glm::vec3       p{ 1000.0f, 0.0f, 1000.0f };
    EXPECT_FALSE( stroke.ApplyRamp( p, p + glm::vec3( 0.0f, 300.0f, 0.0f ), {} ).IsSuccess() )
         << "points one above the other have no direction";
    LandscapeRampSettings narrow;
    narrow.WidthCm = 0.5f;
    EXPECT_FALSE( ValidateLandscapeRamp( narrow ).IsSuccess() );
    LandscapeRampSettings wide;
    wide.SideFalloff = 1.5f;
    EXPECT_FALSE( ValidateLandscapeRamp( wide ).IsSuccess() );
    EXPECT_FALSE( stroke.Touched() );
}

namespace
{
    /// A 41 x 41 field whose heights come from @p height, the brush weighing @p brushValue everywhere inside the
    /// outer ring (UE's grown-by-one rectangle), at global samples offset by @p origin.
    LandscapeErosionField Field( uint16_t ( *height )( int32_t, int32_t ), float brushValue = 1.0f,
                                 int32_t origin = 0 )
    {
        LandscapeErosionField f;
        f.Rect  = { origin, origin, origin + 40, origin + 40 };
        f.Inner = { origin + 1, origin + 1, origin + 39, origin + 39 };
        for ( int32_t z = f.Rect.Z1; z <= f.Rect.Z2; ++z )
            for ( int32_t x = f.Rect.X1; x <= f.Rect.X2; ++x )
                f.Heights.push_back( height( x - origin, z - origin ) );
        f.Brush.assign( 39u * 39u, brushValue );
        return f;
    }

    /// A cliff of 900 steps across x = 20 over a gentle ramp, with a notch so the scan meets both orientations.
    uint16_t Cliff( int32_t x, int32_t z )
    {
        const int32_t notch = ( z > 25 && z < 30 ) ? 3 : 0;
        return static_cast<uint16_t>( 30000 + 10 * x + 5 * z + ( x > 20 + notch ? 900 : 0 ) );
    }

    /// A round hill, 3000 steps high and 15 samples in radius, on a sloping plain: water has somewhere to run.
    uint16_t Hill( int32_t x, int32_t z )
    {
        const double r = std::sqrt( double( ( x - 20 ) * ( x - 20 ) + ( z - 18 ) * ( z - 18 ) ) );
        const double h = r < 15.0 ? 3000.0 * 0.5 * ( 1.0 + std::cos( r / 15.0 * 3.14159265358979 ) ) : 0.0;
        return static_cast<uint16_t>( 30000.0 + 20.0 * x + h );
    }

    /// The steepest 4-neighbour difference between two samples of the field's inner area.
    int32_t SteepestInner( const LandscapeErosionField& f )
    {
        const int32_t width = f.Rect.X2 - f.Rect.X1 + 1;
        auto          at    = [&]( int32_t x, int32_t z ) -> int32_t
        { return f.Heights[static_cast<size_t>( ( z - f.Rect.Z1 ) * width + ( x - f.Rect.X1 ) )]; };
        int32_t steepest = 0;
        for ( int32_t z = f.Inner.Z1; z <= f.Inner.Z2; ++z )
            for ( int32_t x = f.Inner.X1; x <= f.Inner.X2; ++x )
            {
                if ( x < f.Inner.X2 )
                    steepest = std::max( steepest, std::abs( at( x, z ) - at( x + 1, z ) ) );
                if ( z < f.Inner.Z2 )
                    steepest = std::max( steepest, std::abs( at( x, z ) - at( x, z + 1 ) ) );
            }
        return steepest;
    }

    int64_t Mass( const LandscapeErosionField& f )
    {
        int64_t sum = 0;
        for ( const uint16_t h : f.Heights )
            sum += h;
        return sum;
    }
} // namespace

TEST( LandscapeSculpt, ThermalErosionBringsNoSlopeAboveTheSteepestAndOnlyLosesHeight )
{
    LandscapeErosionField    f              = Field( Cliff );
    const int32_t            steepestBefore = SteepestInner( f );
    const int64_t            massBefore     = Mass( f );
    LandscapeErosionSettings s;
    const int32_t            ran           = LandscapeThermalErosion( f, s, 1.0f );
    const int32_t            steepestAfter = SteepestInner( f );
    EXPECT_GT( ran, 1 );
    EXPECT_LT( steepestAfter, steepestBefore ) << "the cliff must have been worn down";
    EXPECT_LE( steepestAfter - s.Threshold, steepestBefore - s.Threshold );
    // Every sample above the threshold that sheds truncates each neighbour's share ((uint16)HeightDiff) and the
    // total it loses ((uint16)TotalHeightDiff) separately: the field can only lose, and under 4 steps per shed.
    const int64_t lost = massBefore - Mass( f );
    EXPECT_GE( lost, 0 ) << "thermal erosion only moves height";
    EXPECT_LT( lost, 4LL * ran * 39 * 39 );
    // Height shed onto the ring the brush does not touch leaves the field (the ground outside is not part of
    // the stroke), so the field loses a little more than UE's truncation.
    EXPECT_LT( static_cast<double>( lost ) / static_cast<double>( massBefore ), 1e-3 );

    // Below the threshold nothing moves: the gentle ramp (10 and 5 steps per sample) is left alone.
    LandscapeErosionField ramp      = Field( []( int32_t x, int32_t z ) -> uint16_t
                                        { return static_cast<uint16_t>( 30000 + 10 * x + 5 * z ); } );
    const auto            untouched = ramp.Heights;
    EXPECT_EQ( LandscapeThermalErosion( ramp, s, 1.0f ), 1 ) << "the first iteration changes nothing and stops";
    EXPECT_EQ( ramp.Heights, untouched );
}

TEST( LandscapeSculpt, ErosionNoiseFollowsItsModeAndTheBrushSize )
{
    LandscapeErosionSettings s;
    for ( const auto mode : { LandscapeErosionNoiseMode::Lower, LandscapeErosionNoiseMode::Raise } )
    {
        s.NoiseMode             = mode;
        LandscapeErosionField f = Field( Flat );
        LandscapeErosionNoise( f, s, 1.0f, 5000.0f );
        int moved = 0;
        for ( const uint16_t h : f.Heights )
        {
            if ( mode == LandscapeErosionNoiseMode::Lower )
                EXPECT_LE( h, kLandscapeMidSample );
            else
                EXPECT_GE( h, kLandscapeMidSample );
            // Amplitude Threshold * strength * radius / MaximumValueRadius = 32 steps, doubled by the mode shift.
            EXPECT_LE( std::abs( int32_t( h ) - int32_t( kLandscapeMidSample ) ), 64 );
            moved += h != kLandscapeMidSample;
        }
        EXPECT_GT( moved, 100 );
    }
}

TEST( LandscapeSculpt, HydraulicErosionIsDeterministicAndStaysInsideTheOriginalRange )
{
    LandscapeHydroErosionSettings s;
    const LandscapeErosionField   original = Field( Hill );
    const auto [lowest, highest] = std::minmax_element( original.Heights.begin(), original.Heights.end() );
    int changedCases             = 0;
    for ( const bool detailSmooth : { true, false } )
        for ( const auto mode : { LandscapeRainMode::Both, LandscapeRainMode::Positive } )
        {
            s.DetailSmooth            = detailSmooth;
            s.RainMode                = mode;
            LandscapeErosionField one = original;
            LandscapeErosionField two = original;
            const int32_t         ran = LandscapeHydraulicErosion( one, s, 1.0f );
            EXPECT_EQ( LandscapeHydraulicErosion( two, s, 1.0f ), ran );
            EXPECT_EQ( one.Heights, two.Heights ) << "no random state: the rain is seeded by position";
            // Near the origin UE's Both-mode rain (noise * RainAmount, truncated) can fall as nothing at all.
            changedCases += one.Heights != original.Heights;
            EXPECT_GT( ran, 1 );
            for ( const uint16_t h : one.Heights )
            {
                EXPECT_GE( h, *lowest );
                EXPECT_LE( h, *highest );
            }
        }
    EXPECT_GE( changedCases, 3 );
    // The rain is a function of the global sample: the same hill elsewhere on the landscape erodes differently.
    LandscapeErosionField here      = original;
    LandscapeErosionField elsewhere = Field( Hill, 1.0f, 37 );
    LandscapeHydraulicErosion( here, s, 1.0f );
    LandscapeHydraulicErosion( elsewhere, s, 1.0f );
    EXPECT_NE( here.Heights, elsewhere.Heights );
    // UE rains only where the brush weighs 1: under a partial brush no water falls and nothing dissolves.
    LandscapeErosionField partial = Field( Hill, 0.9f );
    s.DetailSmooth                = false;
    EXPECT_EQ( LandscapeHydraulicErosion( partial, s, 1.0f ), 1 );
    EXPECT_EQ( partial.Heights, original.Heights );
}

TEST( LandscapeSculpt, ErosionStrokesWriteEqualSeamCopiesAndUndoByteForByte )
{
    World                       w( Noise );
    const std::vector<uint16_t> original = w.Bytes();
    const auto                  b        = Brush( 1000.0f );
    LandscapeHeightStroke       stroke( w.Root, w.Lookup(), w.Bounds() );
    ASSERT_TRUE( stroke.ApplyErosion( Weights( w, b, { 3100.0f, 3100.0f } ), b, {} ).IsSuccess() );
    ASSERT_TRUE( stroke.ApplyHydroErosion( Weights( w, b, { 3100.0f, 2400.0f } ), b, {} ).IsSuccess() );
    ASSERT_TRUE( stroke.ApplyErosion( Weights( w, b, { 5900.0f, 300.0f } ), b, {} ).IsSuccess() )
         << "a brush over the landscape's corner";
    int changedX = 0;
    int changedZ = 0;
    for ( int32_t i = 0; i <= kQ; ++i )
    {
        for ( int32_t t = 0; t < 2; ++t )
        {
            const uint32_t u = static_cast<uint32_t>( i );
            EXPECT_EQ( w.Tiles.at( { 0, t } ).Sample( kQuads, u ), w.Tiles.at( { 1, t } ).Sample( 0u, u ) );
            EXPECT_EQ( w.Tiles.at( { t, 0 } ).Sample( u, kQuads ), w.Tiles.at( { t, 1 } ).Sample( u, 0u ) );
        }
        changedX += w.At( kQ, i ) != Noise( kQ, i );
        changedZ += w.At( i, kQ ) != Noise( i, kQ );
    }
    EXPECT_GT( changedX, 5 ) << "the X seam must have been edited, or the equality proves nothing";
    EXPECT_GT( changedZ, 5 ) << "the Z seam must have been edited, or the equality proves nothing";

    auto record = stroke.Finish();
    ASSERT_TRUE( record.IsSuccess() ) << record.GetError();
    const auto edited = w.Bytes();
    ASSERT_NE( edited, original );
    const auto& r = record.GetValue();
    ASSERT_TRUE( WriteLandscapeHeights( w.Root, w.Lookup(), r.Rect, r.Before ).IsSuccess() );
    EXPECT_EQ( w.Bytes(), original ) << "undo";
    ASSERT_TRUE( WriteLandscapeHeights( w.Root, w.Lookup(), r.Rect, r.After ).IsSuccess() );
    EXPECT_EQ( w.Bytes(), edited ) << "redo";
}

TEST( LandscapeSculpt, BadErosionSettingsAreRefused )
{
    World                    w( Noise );
    const auto               b = Brush();
    LandscapeHeightStroke    stroke( w.Root, w.Lookup(), w.Bounds() );
    LandscapeErosionSettings thermal;
    thermal.Threshold = 300;
    EXPECT_FALSE( stroke.ApplyErosion( Weights( w, b, { 3100.0f, 3100.0f } ), b, thermal ).IsSuccess() );
    thermal.Threshold  = 64;
    thermal.Iterations = 0;
    EXPECT_FALSE( ValidateLandscapeErosion( thermal ).IsSuccess() );
    LandscapeHydroErosionSettings hydro;
    hydro.SedimentCapacity = 0.05f;
    EXPECT_FALSE( stroke.ApplyHydroErosion( Weights( w, b, { 3100.0f, 3100.0f } ), b, hydro ).IsSuccess() );
    hydro.SedimentCapacity = 0.3f;
    hydro.DetailScale      = 1.0f;
    EXPECT_FALSE( ValidateLandscapeHydroErosion( hydro ).IsSuccess() );
    hydro.DetailScale = 0.01f;
    hydro.RainAmount  = 0;
    EXPECT_FALSE( ValidateLandscapeHydroErosion( hydro ).IsSuccess() );
    EXPECT_FALSE( stroke.Touched() );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}

// ---- Mirror and Copy/Paste (L7, ported from UE 5.8 LandscapeEdModeMirrorTool.cpp and
// LandscapeEdModeComponentTools.cpp) ----

namespace
{
    /// Byte-exact undo of a finished stroke, then redo, against the bytes before and after it.
    void ExpectUndoRedo( World& w, const LandscapeHeightStroke& stroke, const std::vector<uint16_t>& original )
    {
        const std::vector<uint16_t> edited = w.Bytes();
        auto                        record = stroke.Finish();
        ASSERT_TRUE( record.IsSuccess() ) << record.GetError();
        const auto& r = record.GetValue();
        ASSERT_TRUE( WriteLandscapeHeights( w.Root, w.Lookup(), r.Rect, r.Before ).IsSuccess() );
        EXPECT_EQ( w.Bytes(), original ) << "undo";
        ASSERT_TRUE( WriteLandscapeHeights( w.Root, w.Lookup(), r.Rect, r.After ).IsSuccess() );
        EXPECT_EQ( w.Bytes(), edited ) << "redo";
    }

    void ExpectSeamsEqual( const World& w )
    {
        for ( int32_t i = 0; i <= kQ; ++i )
            for ( int32_t t = 0; t < 2; ++t )
            {
                const uint32_t u = static_cast<uint32_t>( i );
                EXPECT_EQ( w.Tiles.at( { 0, t } ).Sample( kQuads, u ), w.Tiles.at( { 1, t } ).Sample( 0u, u ) );
                EXPECT_EQ( w.Tiles.at( { t, 0 } ).Sample( u, kQuads ), w.Tiles.at( { t, 1 } ).Sample( u, 0u ) );
            }
    }

    /// UE's blend: Alpha = cos(Frac * PI) * -0.5 + 0.5 over 2W + 1 samples, Frac = (i + 1) / (2W + 2).
    float MirrorAlpha( int32_t i, int32_t width )
    {
        const float frac = static_cast<float>( i + 1 ) / static_cast<float>( 2 * width + 2 );
        return std::cos( frac * 3.14159265358979323846f ) * -0.5f + 0.5f;
    }
} // namespace

TEST( LandscapeSculpt, MirrorMinusXToPlusXCopiesTheLeftHalfMirroredAndLeavesItAlone )
{
    World                       w( Noise );
    const std::vector<uint16_t> original = w.Bytes();
    LandscapeHeightStroke       stroke( w.Root, w.Lookup(), w.Bounds() );
    ASSERT_TRUE( stroke.ApplyMirror( std::nullopt, {} ).IsSuccess() ); // centre: sample 31, the tile seam
    for ( int32_t z = 0; z <= 2 * kQ; ++z )
        for ( int32_t x = 0; x <= 2 * kQ; ++x )
        {
            if ( x <= kQ )
                EXPECT_EQ( w.At( x, z ), Noise( x, z ) ) << "the source half is untouched at " << x << "," << z;
            else
                EXPECT_EQ( w.At( x, z ), Noise( 2 * kQ - x, z ) ) << "mirrored at " << x << "," << z;
        }
    ExpectSeamsEqual( w );
    ExpectUndoRedo( w, stroke, original );
}

TEST( LandscapeSculpt, MirrorOverZWithRotateFlipsTheOtherAxisToo )
{
    World                   w( Noise );
    LandscapeHeightStroke   stroke( w.Root, w.Lookup(), w.Bounds() );
    LandscapeMirrorSettings m;
    m.Op = LandscapeMirrorOp::RotatePlusZToMinusZ;
    // A point off-centre: sample 40 along Z (world 4000 cm); the X coordinate is ignored.
    ASSERT_TRUE( stroke.ApplyMirror( glm::vec3( 123.0f, 0.0f, 4000.0f ), m ).IsSuccess() );
    for ( int32_t z = 0; z <= 2 * kQ; ++z )
        for ( int32_t x = 0; x <= 2 * kQ; ++x )
        {
            if ( z > 40 )
                EXPECT_EQ( w.At( x, z ), Noise( x, z ) ) << x << "," << z;
            else if ( z == 40 )
            {
                // UE's blend with width 0 is the mirror line alone at Alpha 0.5: under Rotate it meets its own
                // flipped copy halfway (Lerp(Line2, Line1, 0.5), truncating).
                const uint16_t a = Noise( 2 * kQ - x, 40 ), b = Noise( x, 40 );
                EXPECT_EQ( w.At( x, z ),
                           static_cast<uint16_t>( a + MirrorAlpha( 0, 0 ) * float( int32_t( b ) - a ) ) )
                     << x;
            }
            else
            {
                // Beyond the landscape the source takes the edge sample (80 - z > 62 reads row 62).
                const int32_t sz = std::min( 80 - z, 2 * kQ );
                EXPECT_EQ( w.At( x, z ), Noise( 2 * kQ - x, sz ) ) << x << "," << z;
            }
        }
    ExpectSeamsEqual( w );
}

TEST( LandscapeSculpt, MirrorSmoothingBlendsUEsWidthEitherSideOfTheLine )
{
    World                   w( Noise );
    LandscapeHeightStroke   stroke( w.Root, w.Lookup(), w.Bounds() );
    LandscapeMirrorSettings m;
    m.Op             = LandscapeMirrorOp::PlusXToMinusX;
    m.SmoothingWidth = 4;
    ASSERT_TRUE( stroke.ApplyMirror( glm::vec3( 3000.0f, 0.0f, 0.0f ), m ).IsSuccess() ); // line at x = 30
    for ( int32_t z = 0; z <= 2 * kQ; z += 7 )
    {
        for ( int32_t x = 0; x < 26; ++x )
            EXPECT_EQ( w.At( x, z ), Noise( 60 - x, z ) ) << "pure mirror at " << x;
        for ( int32_t i = 0; i < 9; ++i ) // x = 26..34: the 2W + 1 blended samples
        {
            const int32_t  x = 26 + i;
            const uint16_t a = Noise( 60 - x, z ), b = Noise( x, z );
            const auto     e = static_cast<uint16_t>( static_cast<float>( a ) +
                                                      MirrorAlpha( i, 4 ) * static_cast<float>( int32_t( b ) - a ) );
            EXPECT_EQ( w.At( x, z ), e ) << "blend at " << x;
        }
        for ( int32_t x = 35; x <= 2 * kQ; ++x )
            EXPECT_EQ( w.At( x, z ), Noise( x, z ) ) << "source side untouched at " << x;
    }
}

TEST( LandscapeSculpt, MirrorRefusesALineOnTheEdge )
{
    World                 w( Noise );
    LandscapeHeightStroke stroke( w.Root, w.Lookup(), w.Bounds() );
    EXPECT_FALSE( stroke.ApplyMirror( glm::vec3( 0.0f ), {} ).IsSuccess() );
    EXPECT_FALSE( stroke.ApplyMirror( glm::vec3( 6200.0f, 0.0f, 0.0f ), {} ).IsSuccess() );
    LandscapeMirrorSettings bad;
    bad.SmoothingWidth = -1;
    EXPECT_FALSE( stroke.ApplyMirror( std::nullopt, bad ).IsSuccess() );
    EXPECT_FALSE( stroke.Touched() );
}

namespace
{
    /// A hard-edged brush wider than every copy the tests paste: weight 1 over the whole copy.
    LandscapeBrushSettings WholeCopy()
    {
        LandscapeBrushSettings brush;
        brush.RadiusCm        = 2000.0f;
        brush.FalloffFraction = 0.0f;
        return brush;
    }

    /// UE's copy gizmo Z (GetLandscapeCenterPos: the region's MinZ): the lowest Noise sample of a region.
    int32_t NoiseMin( int32_t x1, int32_t z1, int32_t x2, int32_t z2 )
    {
        int32_t m = 65535;
        for ( int32_t z = z1; z <= z2; ++z )
            for ( int32_t x = x1; x <= x2; ++x )
                m = std::min( m, int32_t( Noise( x, z ) ) );
        return m;
    }
} // namespace

TEST( LandscapeSculpt, CopyThenPasteElsewhereKeepsTheRelativeHeightsAcrossSeams )
{
    World                       w( Noise );
    const std::vector<uint16_t> original = w.Bytes();
    // Region x 5..15, z 8..20 (11 x 13), centre sample (10, 14).
    auto copied = CopyLandscapeHeights( w.Root, w.Lookup(), w.Bounds(), glm::vec3( 1500.0f, 0.0f, 800.0f ),
                                        glm::vec3( 500.0f, 0.0f, 2000.0f ) );
    ASSERT_TRUE( copied.IsSuccess() ) << copied.GetError();
    const LandscapeCopyBuffer& buffer = copied.GetValue();
    ASSERT_EQ( buffer.SizeX, 11 );
    ASSERT_EQ( buffer.SizeZ, 13 );
    EXPECT_EQ( w.Bytes(), original ) << "copy writes nothing";

    // Paste centred on (31, 31): the corner where four tiles meet.
    LandscapeHeightStroke stroke( w.Root, w.Lookup(), w.Bounds() );
    ASSERT_TRUE(
         stroke.ApplyPaste( buffer, glm::vec3( 3100.0f, 0.0f, 3100.0f ), LandscapePasteMode::Both, WholeCopy() )
              .IsSuccess() );
    const int32_t base = Noise( 31, 31 );
    for ( int32_t dz = -6; dz <= 6; ++dz )
        for ( int32_t dx = -5; dx <= 5; ++dx )
            EXPECT_EQ( int32_t( w.At( 31 + dx, 31 + dz ) ) - base,
                       int32_t( Noise( 10 + dx, 14 + dz ) ) - NoiseMin( 5, 8, 15, 20 ) )
                 << dx << "," << dz;
    EXPECT_EQ( w.At( 25, 31 ), Noise( 25, 31 ) ) << "outside the pasted rectangle";
    EXPECT_EQ( w.At( 31, 38 ), Noise( 31, 38 ) );
    ExpectSeamsEqual( w );
    ExpectUndoRedo( w, stroke, original );
}

namespace
{
    /// A 3000-step cosine hill (radius 4 samples) at (10, 10) on flat 20000, and a flat 30000 plateau for x >= 36.
    uint16_t HillAndPlateau( int32_t x, int32_t z )
    {
        if ( x >= 36 )
            return 30000;
        const double r = std::sqrt( double( ( x - 10 ) * ( x - 10 ) + ( z - 10 ) * ( z - 10 ) ) );
        return static_cast<uint16_t>(
             20000.0 + ( r < 4.0 ? 1500.0 * ( 1.0 + std::cos( r / 4.0 * 3.14159265358979 ) ) : 0.0 ) );
    }
} // namespace

// UE's paste height is the source's height above the copy gizmo plus the paste gizmo's Z
// (LandscapeGizmoActor.cpp:819-866, LandscapeEdModeComponentTools.cpp:1501): a hill copied from low ground and
// pasted on a plateau stands on the plateau with its full height, instead of sinking its peak to the plateau.
TEST( LandscapeSculpt, PasteStandsTheCopyOnThePasteGizmosHeight )
{
    World w( HillAndPlateau );
    auto  copied = CopyLandscapeHeights( w.Root, w.Lookup(), w.Bounds(), glm::vec3( 400.0f, 0.0f, 400.0f ),
                                         glm::vec3( 1600.0f, 0.0f, 1600.0f ) );
    ASSERT_TRUE( copied.IsSuccess() );
    LandscapeHeightStroke stroke( w.Root, w.Lookup(), w.Bounds() );
    ASSERT_TRUE( stroke
                      .ApplyPaste( copied.GetValue(), glm::vec3( 4800.0f, 0.0f, 3000.0f ),
                                   LandscapePasteMode::Both, WholeCopy() )
                      .IsSuccess() );
    EXPECT_EQ( w.At( 48, 30 ), 33000 ) << "the peak stands 3000 steps above the plateau";
    EXPECT_EQ( w.At( 42, 24 ), 30000 ) << "the copy's foot meets the plateau";
    EXPECT_EQ( w.At( 54, 36 ), 30000 );
    EXPECT_EQ( w.At( 10, 10 ), 23000 ) << "the source is untouched";
}

TEST( LandscapeSculpt, PasteRaiseOnlyLiftsAndLowerOnlySinks )
{
    for ( const LandscapePasteMode mode : { LandscapePasteMode::Raise, LandscapePasteMode::Lower } )
    {
        World w( Noise );
        auto  copied = CopyLandscapeHeights( w.Root, w.Lookup(), w.Bounds(), glm::vec3( 4000.0f, 0.0f, 4000.0f ),
                                             glm::vec3( 5000.0f, 0.0f, 5000.0f ) );
        ASSERT_TRUE( copied.IsSuccess() );
        LandscapeHeightStroke stroke( w.Root, w.Lookup(), w.Bounds() );
        ASSERT_TRUE( stroke.ApplyPaste( copied.GetValue(), glm::vec3( 1000.0f, 0.0f, 1000.0f ), mode, WholeCopy() )
                          .IsSuccess() );
        int32_t changed = 0, kept = 0;
        for ( int32_t dz = -5; dz <= 5; ++dz )
            for ( int32_t dx = -5; dx <= 5; ++dx )
            {
                const int32_t orig = Noise( 10 + dx, 10 + dz );
                const int32_t dest =
                     int32_t( Noise( 10, 10 ) ) + Noise( 45 + dx, 45 + dz ) - NoiseMin( 40, 40, 50, 50 );
                const int32_t now  = w.At( 10 + dx, 10 + dz );
                const bool    take = mode == LandscapePasteMode::Raise ? dest > orig : dest < orig;
                EXPECT_EQ( now, take ? dest : orig ) << dx << "," << dz;
                ( take ? changed : kept ) += 1;
            }
        EXPECT_GT( changed, 20 ) << "both branches must be exercised";
        EXPECT_GT( kept, 20 );
    }
}

TEST( LandscapeSculpt, CopyAndPasteRefuseWhatUESilentlySkips )
{
    World w( Noise );
    EXPECT_FALSE( CopyLandscapeHeights( w.Root, w.Lookup(), w.Bounds(), glm::vec3( -900.0f, 0.0f, -900.0f ),
                                        glm::vec3( -500.0f, 0.0f, -500.0f ) )
                       .IsSuccess() );
    LandscapeHeightStroke stroke( w.Root, w.Lookup(), w.Bounds() );
    EXPECT_FALSE(
         stroke.ApplyPaste( {}, glm::vec3( 1000.0f ), LandscapePasteMode::Both, WholeCopy() ).IsSuccess() );
    auto copied = CopyLandscapeHeights( w.Root, w.Lookup(), w.Bounds(), glm::vec3( 0.0f ), glm::vec3( 300.0f ) );
    ASSERT_TRUE( copied.IsSuccess() );
    EXPECT_FALSE( stroke
                       .ApplyPaste( copied.GetValue(), glm::vec3( 9000.0f, 0.0f, 0.0f ), LandscapePasteMode::Both,
                                    WholeCopy() )
                       .IsSuccess() );
    EXPECT_FALSE( stroke.Touched() );
}

TEST( LandscapeSculpt, ThermalErosionAtUEsLimitsRaisesNoPeakAndOnlyLosesHeight )
{
    // UE's ClampMax: strength 10, 300 iterations; thirty strokes over the same cliff, at full and half brush.
    for ( const float brushValue : { 1.0f, 0.5f } )
    {
        LandscapeErosionField    f = Field( Cliff, brushValue );
        LandscapeErosionSettings s;
        s.Iterations       = kLandscapeMaxErosionIterations;
        const auto highest = []( const LandscapeErosionField& e )
        { return *std::max_element( e.Heights.begin(), e.Heights.end() ); };
        const int32_t peak       = highest( f );
        const int64_t massBefore = Mass( f );
        for ( int32_t stroke = 0; stroke < 30; ++stroke )
            LandscapeThermalErosion( f, s, 10.0f );
        EXPECT_LE( highest( f ), peak ) << "erosion only moves height downhill, brush " << brushValue;
        const int64_t lost = massBefore - Mass( f );
        EXPECT_GE( lost, 0 ) << "brush " << brushValue;
        EXPECT_LT( static_cast<double>( lost ) / static_cast<double>( massBefore ), 1e-3 ) << brushValue;
    }
}

TEST( LandscapeSculpt, PasteFadesIntoTheReliefAlongTheBrushFalloff )
{
    World w( Noise );
    auto  copied = CopyLandscapeHeights( w.Root, w.Lookup(), w.Bounds(), glm::vec3( 4000.0f, 0.0f, 4000.0f ),
                                         glm::vec3( 5000.0f, 0.0f, 5000.0f ) );
    ASSERT_TRUE( copied.IsSuccess() );
    // A 5-sample brush inside the 11 x 11 copy: the circle's rim is where the paste must meet the relief.
    LandscapeBrushSettings brush;
    brush.RadiusCm        = 500.0f;
    brush.FalloffFraction = 0.5f;
    LandscapeHeightStroke stroke( w.Root, w.Lookup(), w.Bounds() );
    ASSERT_TRUE( stroke
                      .ApplyPaste( copied.GetValue(), glm::vec3( 1000.0f, 0.0f, 1000.0f ),
                                   LandscapePasteMode::Both, brush )
                      .IsSuccess() );
    EXPECT_EQ( int32_t( w.At( 10, 10 ) ) - Noise( 10, 10 ), Noise( 45, 45 ) - NoiseMin( 40, 40, 50, 50 ) )
         << "the copy's lowest sample stands at the paste point's height";
    EXPECT_EQ( int32_t( w.At( 11, 10 ) ) - Noise( 10, 10 ), Noise( 46, 45 ) - NoiseMin( 40, 40, 50, 50 ) )
         << "inside the full-weight circle the copy lands exactly";
    for ( int32_t d = -5; d <= 5; ++d )
        for ( const auto& [x, z] : { std::pair{ 10 + d, 5 }, std::pair{ 10 + d, 15 }, std::pair{ 5, 10 + d },
                                     std::pair{ 15, 10 + d } } )
            EXPECT_EQ( w.At( x, z ), Noise( x, z ) ) << "no wall on the rim at " << x << "," << z;
}

TEST( LandscapeSculpt, ErosionOnTheEditorsCapturedFieldLeavesNoStepAtTheBrushRim )
{
    // The L7k frame, replayed call for call: the editor applies ApplyErosion once per stroke command, and 25
    // strokes re-read the same rectangle, so the thermal loop and the noise pass run 25 times on the captured
    // field (a half-brush hill on ground that falls ~73 steps per sample - steeper than the threshold, which
    // the flat test worlds above never were). The frame showed the brush circle sunk under the ground outside it
    // with a vertical wall at the rim.
    namespace C = EditorErosionCapture;
    LandscapeErosionField field;
    field.Rect                            = { C::kRect[0], C::kRect[1], C::kRect[2], C::kRect[3] };
    field.Inner                           = { C::kInner[0], C::kInner[1], C::kInner[2], C::kInner[3] };
    field.Heights                         = std::vector<uint16_t>( C::kHeights.begin(), C::kHeights.end() );
    field.Brush                           = std::vector<float>( C::kBrush.begin(), C::kBrush.end() );
    const int32_t                  width  = field.Rect.X2 - field.Rect.X1 + 1;
    const int32_t                  depth  = field.Rect.Z2 - field.Rect.Z1 + 1;
    const std::vector<uint16_t>    before = field.Heights;
    const LandscapeErosionSettings erosion{};
    for ( int32_t i = 0; i < 25; ++i )
    {
        LandscapeThermalErosion( field, erosion, 1.05f );
        LandscapeErosionNoise( field, erosion, 1.05f, 4000.0f );
    }
    // The frame's defect: the brush circle sank under the untouched ground outside it. For every sample the brush
    // touches, next to one it does not, the drop against that neighbour since the capture is at most one
    // threshold (UE: the noise pass takes at most BrushValue * Thresh * strength per stroke, ~0 at the rim).
    int32_t    rimDrop        = 0;
    int32_t    steepestChange = 0;
    const auto brushAt        = [&]( int32_t x, int32_t z )
    {
        const int32_t gx = field.Rect.X1 + x;
        const int32_t gz = field.Rect.Z1 + z;
        if ( gx < field.Inner.X1 || gx > field.Inner.X2 || gz < field.Inner.Z1 || gz > field.Inner.Z2 )
            return 0.0f;
        return field.Brush[static_cast<size_t>( ( gz - field.Inner.Z1 ) * ( field.Inner.X2 - field.Inner.X1 + 1 ) +
                                                ( gx - field.Inner.X1 ) )];
    };
    for ( int32_t z = 1; z + 1 < depth; ++z )
        for ( int32_t x = 1; x + 1 < width; ++x )
        {
            const size_t                                     c      = static_cast<size_t>( z * width + x );
            const std::array<std::pair<int32_t, int32_t>, 4> around = {
                 std::pair{ x - 1, z }, std::pair{ x + 1, z }, std::pair{ x, z - 1 }, std::pair{ x, z + 1 } };
            for ( const auto& [nx, nz] : around )
            {
                const size_t  n      = static_cast<size_t>( nz * width + nx );
                const int32_t change = ( field.Heights[c] - field.Heights[n] ) - ( before[c] - before[n] );
                steepestChange       = std::max( steepestChange, std::abs( change ) );
                if ( brushAt( x, z ) > 0.0f && !( brushAt( nx, nz ) > 0.0f ) )
                    rimDrop = std::max( rimDrop, -change );
            }
        }
    const int32_t centre = ( depth / 2 ) * width + width / 2;
    std::string   profile;
    for ( int32_t x = 0; x < width; x += 4 )
        profile += std::to_string( field.Heights[( depth / 2 ) * width + x] ) + " ";
    EXPECT_LE( rimDrop, erosion.Threshold ) << "largest neighbour-step change anywhere: " << steepestChange
                                            << "; profile through the centre: " << profile;
    EXPECT_LT( field.Heights[centre], before[centre] ) << "the stroke erodes";
}
