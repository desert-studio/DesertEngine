// The Sculpt and Smooth strokes (L3, ported from UE 5.8 LandscapeEdModePaintTools.cpp) over the edit cache:
// what one step writes, what it leaves alone, that seam copies agree, and that the stroke's record undoes it
// byte for byte. Plus the census of the editor's controls table (every panel widget is a palette command).

#include <Engine/World/Landscape/LandscapeSculpt.hpp>

#include <Editor/Core/Selection/LandscapeSculptState.hpp>

#include <gtest/gtest.h>

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
        uint32_t h = static_cast<uint32_t>( gx * 73856093 ) ^ static_cast<uint32_t>( gz * 19349663 );
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
    // noise mode, noise scale.
    EXPECT_EQ( rows.size(), 15u );

    LandscapeSculptSettings other;
    other.Tool                      = LandscapeTool::Smooth;
    other.Brush.RadiusCm            = 400.0f;
    other.Brush.FalloffFraction     = 0.2f;
    other.Brush.Shape               = LandscapeBrushFalloff::Tip;
    other.Brush.Strength            = 0.4f;
    other.Smooth.FilterKernelRadius = 9;
    other.Smooth.DetailSmooth       = true;
    other.Smooth.DetailScale        = 0.5f;
    other.Flatten.Mode              = LandscapeFlattenMode::Terrace;
    other.Flatten.UseSlopeFlatten   = true;
    other.Flatten.PickValuePerApply = true;
    other.Flatten.TerraceIntervalCm = 256.0f;
    other.Flatten.TerraceSmooth     = 0.5f;
    other.Noise.Mode                = LandscapeNoiseMode::Sub;
    other.Noise.NoiseScale          = 64.0f;
    auto key                        = []( const LandscapeSculptSettings& s )
    {
        return std::to_string( static_cast<int>( s.Tool ) ) + std::to_string( s.Brush.RadiusCm ) +
               std::to_string( s.Brush.FalloffFraction ) + std::to_string( static_cast<int>( s.Brush.Shape ) ) +
               std::to_string( s.Brush.Strength ) + std::to_string( s.Smooth.FilterKernelRadius ) +
               std::to_string( s.Smooth.DetailSmooth ) + std::to_string( s.Smooth.DetailScale ) +
               std::to_string( static_cast<int>( s.Flatten.Mode ) ) + std::to_string( s.Flatten.UseSlopeFlatten ) +
               std::to_string( s.Flatten.PickValuePerApply ) + std::to_string( s.Flatten.TerraceIntervalCm ) +
               std::to_string( s.Flatten.TerraceSmooth ) + std::to_string( static_cast<int>( s.Noise.Mode ) ) +
               std::to_string( s.Noise.NoiseScale );
    };
    for ( const auto& c : controls )
    {
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
        }
        EXPECT_TRUE( changes ) << c.Label << " changes nothing from either base: a dead widget";
    }
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
