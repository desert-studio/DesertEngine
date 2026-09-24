// L1: the landscape circle brush, ported from UE's FLandscapeBrushCircle (LandscapeEdModeBrushes.cpp).
//
//   1. Each falloff equals UE's formula at the control points (centre, inner circle, middle and quarter of
//      the falloff band, outer edge) — the expected values are written out here from UE's source, not
//      computed by the function under test — and is monotone and within [0, 1] everywhere.
//   2. The weight map is that falloff of each sample's distance to the centre, symmetric in the eight
//      octants, zero outside the total radius, scaled by strength, and the larger of two overlapping stamps.
//   3. Across a tile seam the two stored copies of the shared row receive the same weight, and a tile that
//      only shares an edge row with the brush rectangle is still reported.

#include <Engine/World/Landscape/LandscapeBrush.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <map>
#include <utility>

using namespace Desert::World::Landscape;

namespace
{
    constexpr std::array<LandscapeBrushFalloff, 4> kShapes = {
         LandscapeBrushFalloff::Linear, LandscapeBrushFalloff::Smooth, LandscapeBrushFalloff::Spherical,
         LandscapeBrushFalloff::Tip };

    LandscapeRoot MakeRoot( uint32_t quads, float spacing = 100.0f )
    {
        LandscapeRoot root;
        root.QuadsPerTile = quads;
        root.SpacingCm    = spacing;
        return root;
    }

    LandscapeBrushWeights Brush( const LandscapeRoot& root, const LandscapeBrushSettings& s,
                                 std::vector<glm::vec2> positions )
    {
        auto result = ComputeLandscapeBrush( root, s, positions );
        EXPECT_TRUE( result.IsSuccess() ) << ( result.IsSuccess() ? "" : result.GetError() );
        return result.IsSuccess() ? result.GetValue() : LandscapeBrushWeights{};
    }
} // namespace

// UE :1020-1024, :1048-1052, :1076-1087, :1112-1123 at R = 4, F = 8: the band is d in (4, 12].
TEST( LandscapeBrush, FalloffMatchesUeAtControlPoints )
{
    const float r = 4.0f, f = 8.0f;
    struct Row
    {
        LandscapeBrushFalloff Shape;
        float                 Quarter; // d = R + F/4
        float                 Middle;  // d = R + F/2
    };
    const std::array<Row, 4> rows = { {
         { LandscapeBrushFalloff::Linear, 0.75f, 0.5f },
         { LandscapeBrushFalloff::Smooth, 0.75f * 0.75f * ( 3.0f - 1.5f ), 0.5f },
         { LandscapeBrushFalloff::Spherical, std::sqrt( 1.0f - 1.0f / 16.0f ), std::sqrt( 0.75f ) },
         { LandscapeBrushFalloff::Tip, 1.0f - std::sqrt( 1.0f - 9.0f / 16.0f ), 1.0f - std::sqrt( 0.75f ) },
    } };
    for ( const Row& row : rows )
    {
        SCOPED_TRACE( static_cast<int>( row.Shape ) );
        EXPECT_FLOAT_EQ( LandscapeBrushFalloffWeight( row.Shape, 0.0f, r, f ), 1.0f );
        EXPECT_FLOAT_EQ( LandscapeBrushFalloffWeight( row.Shape, 3.99f, r, f ), 1.0f );
        EXPECT_FLOAT_EQ( LandscapeBrushFalloffWeight( row.Shape, r + f / 4.0f, r, f ), row.Quarter );
        EXPECT_FLOAT_EQ( LandscapeBrushFalloffWeight( row.Shape, r + f / 2.0f, r, f ), row.Middle );
        EXPECT_NEAR( LandscapeBrushFalloffWeight( row.Shape, r + f, r, f ), 0.0f, 1e-6f );
        EXPECT_EQ( LandscapeBrushFalloffWeight( row.Shape, r + f + 0.01f, r, f ), 0.0f );
    }
}

// UE's comparisons to the letter: Linear is 1 for d < R, Spherical and Tip for d <= R. With no falloff band a
// sample exactly on the circle is therefore out for Linear/Smooth and in for Spherical/Tip.
TEST( LandscapeBrush, InnerCircleComparisonIsUes )
{
    EXPECT_EQ( LandscapeBrushFalloffWeight( LandscapeBrushFalloff::Linear, 5.0f, 5.0f, 0.0f ), 0.0f );
    EXPECT_EQ( LandscapeBrushFalloffWeight( LandscapeBrushFalloff::Smooth, 5.0f, 5.0f, 0.0f ), 0.0f );
    EXPECT_EQ( LandscapeBrushFalloffWeight( LandscapeBrushFalloff::Spherical, 5.0f, 5.0f, 0.0f ), 1.0f );
    EXPECT_EQ( LandscapeBrushFalloffWeight( LandscapeBrushFalloff::Tip, 5.0f, 5.0f, 0.0f ), 1.0f );
    for ( const LandscapeBrushFalloff shape : kShapes )
        EXPECT_EQ( LandscapeBrushFalloffWeight( shape, 5.01f, 5.0f, 0.0f ), 0.0f );
}

TEST( LandscapeBrush, FalloffIsMonotoneAndBounded )
{
    for ( const LandscapeBrushFalloff shape : kShapes )
        for ( const float r : { 0.0f, 2.5f, 10.0f } )
            for ( const float f : { 0.5f, 3.0f, 17.0f } )
            {
                float prev = 1.0f;
                for ( int i = 0; i <= 4000; ++i )
                {
                    const float d = static_cast<float>( i ) * ( r + f + 2.0f ) / 4000.0f;
                    const float w = LandscapeBrushFalloffWeight( shape, d, r, f );
                    ASSERT_GE( w, 0.0f ) << "shape " << static_cast<int>( shape ) << " d " << d;
                    ASSERT_LE( w, prev + 1e-6f ) << "shape " << static_cast<int>( shape ) << " d " << d;
                    prev = w;
                }
            }
}

// The map is UE's ApplyBrush: every sample weighs CalculateFalloff(distance to centre), in lattice units.
TEST( LandscapeBrush, MapIsFalloffOfSampleDistanceAndSymmetric )
{
    const LandscapeRoot root = MakeRoot( 63u, 50.0f );
    for ( const LandscapeBrushFalloff shape : kShapes )
    {
        SCOPED_TRACE( static_cast<int>( shape ) );
        LandscapeBrushSettings s;
        s.RadiusCm        = 500.0f; // 10 samples
        s.FalloffFraction = 0.6f;   // inner 4, band 6
        s.Shape           = shape;
        // Centre on global sample (20, 30).
        const LandscapeBrushWeights w = Brush( root, s, { glm::vec2( 20.0f * 50.0f, 30.0f * 50.0f ) } );
        ASSERT_FALSE( w.Empty() );
        EXPECT_EQ( w.X0, 10 );
        EXPECT_EQ( w.Z0, 20 );
        EXPECT_EQ( w.Width, 21u ); // floor(c - 10) .. ceil(c + 10) + 1, exclusive
        EXPECT_FLOAT_EQ( w.At( 20, 30 ), 1.0f );
        for ( int dz = -11; dz <= 11; ++dz )
            for ( int dx = -11; dx <= 11; ++dx )
            {
                const float d      = std::sqrt( static_cast<float>( dx * dx + dz * dz ) );
                const float expect = LandscapeBrushFalloffWeight( shape, d, 4.0f, 6.0f );
                EXPECT_NEAR( w.At( 20 + dx, 30 + dz ), expect, 1e-5f ) << dx << "," << dz;
                EXPECT_EQ( w.At( 20 + dx, 30 + dz ), w.At( 20 - dx, 30 + dz ) );
                EXPECT_EQ( w.At( 20 + dx, 30 + dz ), w.At( 20 + dx, 30 - dz ) );
                EXPECT_EQ( w.At( 20 + dx, 30 + dz ), w.At( 20 + dz, 30 + dx ) );
                if ( d >= 10.0f )
                    EXPECT_NEAR( w.At( 20 + dx, 30 + dz ), 0.0f, 1e-6f ) << dx << "," << dz;
            }
    }
}

TEST( LandscapeBrush, StrengthScalesAndOverlapTakesTheLarger )
{
    const LandscapeRoot    root = MakeRoot( 31u, 100.0f );
    LandscapeBrushSettings s;
    s.RadiusCm        = 800.0f;
    s.FalloffFraction = 1.0f;
    s.Shape           = LandscapeBrushFalloff::Linear;

    const LandscapeBrushWeights a = Brush( root, s, { glm::vec2( 1000.0f, 1000.0f ) } );
    const LandscapeBrushWeights b = Brush( root, s, { glm::vec2( 1500.0f, 1000.0f ) } );
    const LandscapeBrushWeights both =
         Brush( root, s, { glm::vec2( 1000.0f, 1000.0f ), glm::vec2( 1500.0f, 1000.0f ) } );
    s.Strength                       = 0.25f;
    const LandscapeBrushWeights weak = Brush( root, s, { glm::vec2( 1000.0f, 1000.0f ) } );

    EXPECT_EQ( both.X0, a.X0 );
    EXPECT_EQ( both.X0 + static_cast<int32_t>( both.Width ), b.X0 + static_cast<int32_t>( b.Width ) );
    for ( int z = -2; z < 22; ++z )
        for ( int x = -2; x < 28; ++x )
        {
            EXPECT_EQ( both.At( x, z ), std::max( a.At( x, z ), b.At( x, z ) ) ) << x << "," << z;
            EXPECT_FLOAT_EQ( weak.At( x, z ), 0.25f * a.At( x, z ) ) << x << "," << z;
        }
}

TEST( LandscapeBrush, MoreThanTenPositionsKeepFirstAndLast )
{
    const LandscapeRoot    root = MakeRoot( 63u, 100.0f );
    LandscapeBrushSettings s;
    s.RadiusCm        = 150.0f;
    s.FalloffFraction = 0.0f;
    std::vector<glm::vec2> line;
    for ( int i = 0; i <= 30; ++i )
        line.emplace_back( static_cast<float>( i ) * 100.0f, 0.0f );
    const LandscapeBrushWeights w = Brush( root, s, line );
    EXPECT_EQ( w.At( 0, 0 ), 1.0f );
    EXPECT_EQ( w.At( 30, 0 ), 1.0f );
    // Ten of 31 kept: i * 30 / 9 = 0, 3, 6, 10, 13, 16, 20, 23, 26, 30 — sample 8 is 2 from both 6 and 10.
    EXPECT_EQ( w.At( 8, 0 ), 0.0f );
}

// Tile 0 holds global samples 0..7 and tile 1 holds 7..14 (Q = 7); sample 7 is stored twice.
TEST( LandscapeBrush, SeamSamplesWeighTheSameInBothTiles )
{
    const LandscapeRoot root = MakeRoot( 7u, 100.0f );
    for ( const LandscapeBrushFalloff shape : kShapes )
    {
        SCOPED_TRACE( static_cast<int>( shape ) );
        LandscapeBrushSettings s;
        s.RadiusCm        = 450.0f;
        s.FalloffFraction = 0.7f;
        s.Shape           = shape;
        // Straddles the X seam at 700 cm and the Z seam at 0 (tile -1 below it).
        const LandscapeBrushWeights w = Brush( root, s, { glm::vec2( 730.0f, 120.0f ) } );

        const std::vector<LandscapeBrushTile>                tiles = LandscapeBrushTiles( root, w );
        std::map<std::pair<int32_t, int32_t>, LandscapeRect> byTile;
        for ( const LandscapeBrushTile& t : tiles )
            byTile[{ t.TileX, t.TileZ }] = t.Samples;
        for ( const auto& key : { std::pair{ 0, 0 }, std::pair{ 1, 0 }, std::pair{ 0, -1 }, std::pair{ 1, -1 } } )
            ASSERT_TRUE( byTile.count( key ) ) << key.first << "," << key.second;

        // Every covered local sample reads the global lattice; a sample on the X seam reads the same value from
        // tile 0 (local 7) and tile 1 (local 0), and it is not a trivially zero one.
        std::map<std::pair<int32_t, int32_t>, float> seen;
        for ( const LandscapeBrushTile& t : tiles )
            for ( uint32_t z = t.Samples.Z0; z < t.Samples.Z1; ++z )
                for ( uint32_t x = t.Samples.X0; x < t.Samples.X1; ++x )
                {
                    const int32_t gx = t.TileX * 7 + static_cast<int32_t>( x );
                    const int32_t gz = t.TileZ * 7 + static_cast<int32_t>( z );
                    const float   v  = LandscapeBrushTileWeight( root, w, t.TileX, t.TileZ, x, z );
                    EXPECT_EQ( v, w.At( gx, gz ) );
                    auto [it, fresh] = seen.emplace( std::pair{ gx, gz }, v );
                    if ( !fresh )
                        EXPECT_EQ( it->second, v ) << "global " << gx << "," << gz;
                }
        EXPECT_GT( LandscapeBrushTileWeight( root, w, 0, 0, 7u, 1u ), 0.5f );
        EXPECT_EQ( LandscapeBrushTileWeight( root, w, 0, 0, 7u, 1u ),
                   LandscapeBrushTileWeight( root, w, 1, 0, 0u, 1u ) );
        EXPECT_EQ( LandscapeBrushTileWeight( root, w, 0, 0, 7u, 0u ),
                   LandscapeBrushTileWeight( root, w, 1, -1, 0u, 7u ) );
    }
}

// A rectangle that ENDS on a seam row still reaches the next tile: that row is stored there too.
TEST( LandscapeBrush, TilesIncludeANeighbourSharingOnlyTheEdgeRow )
{
    const LandscapeRoot   root = MakeRoot( 7u, 100.0f );
    LandscapeBrushWeights w;
    w.X0    = 0; // global 0..7 in X: tile 0 whole row, tile -1's last column, tile 1's first column
    w.Z0    = 2;
    w.Width = 8u;
    w.Depth = 3u; // global 2..4 in Z: tile 0 only
    w.Values.assign( 24u, 1.0f );

    std::map<std::pair<int32_t, int32_t>, LandscapeRect> byTile;
    for ( const LandscapeBrushTile& t : LandscapeBrushTiles( root, w ) )
        byTile[{ t.TileX, t.TileZ }] = t.Samples;
    ASSERT_EQ( byTile.size(), 3u );
    EXPECT_EQ( byTile.at( { -1, 0 } ), ( LandscapeRect{ 7u, 2u, 8u, 5u } ) );
    EXPECT_EQ( byTile.at( { 0, 0 } ), ( LandscapeRect{ 0u, 2u, 8u, 5u } ) );
    EXPECT_EQ( byTile.at( { 1, 0 } ), ( LandscapeRect{ 0u, 2u, 1u, 5u } ) );
}

TEST( LandscapeBrush, NegativeSideOfTheRoot )
{
    const LandscapeRoot    root = MakeRoot( 15u, 100.0f );
    LandscapeBrushSettings s;
    s.RadiusCm                    = 300.0f;
    const LandscapeBrushWeights w = Brush( root, s, { glm::vec2( -1500.0f, -3000.0f ) } );
    EXPECT_EQ( w.At( -15, -30 ), 1.0f );
    // Global -15 is tile -1's first column and tile -2's last; -30 is tile -2's first row and tile -3's last.
    EXPECT_EQ( LandscapeBrushTileWeight( root, w, -1, -2, 0u, 0u ), 1.0f );
    EXPECT_EQ( LandscapeBrushTileWeight( root, w, -2, -3, 15u, 15u ), 1.0f );
}

TEST( LandscapeBrush, RefusesSettingsItCannotHonour )
{
    const LandscapeRoot    root = MakeRoot( 63u );
    const glm::vec2        p( 0.0f );
    LandscapeBrushSettings s;
    s.RadiusCm = 0.0f;
    EXPECT_FALSE( ComputeLandscapeBrush( root, s, { &p, 1 } ).IsSuccess() );
    s                 = {};
    s.FalloffFraction = 1.5f;
    EXPECT_FALSE( ComputeLandscapeBrush( root, s, { &p, 1 } ).IsSuccess() );
    s          = {};
    s.Strength = -1.0f;
    EXPECT_FALSE( ComputeLandscapeBrush( root, s, { &p, 1 } ).IsSuccess() );
    s = {};
    EXPECT_FALSE( ComputeLandscapeBrush( MakeRoot( 64u ), s, { &p, 1 } ).IsSuccess() );
    const glm::vec2 bad( NAN, 0.0f );
    EXPECT_FALSE( ComputeLandscapeBrush( root, s, { &bad, 1 } ).IsSuccess() );
    const auto none = ComputeLandscapeBrush( root, s, {} );
    ASSERT_TRUE( none.IsSuccess() );
    EXPECT_TRUE( none.GetValue().Empty() );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
