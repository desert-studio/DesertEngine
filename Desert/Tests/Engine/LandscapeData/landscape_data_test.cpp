// THE LANDSCAPE HEIGHT CORE: ENCODING, SAMPLING, DIRTY RECTANGLES AND THE TILE BLOB.
//
// WHAT IS ASSERTED, in the order the sections appear:
//
//   1. THE ENCODING IS UE'S, NUMBER FOR NUMBER. The boundary samples decode to the heights UE states
//      (-256 m .. +255.99 m at Z scale 100), halves round the way FMath::RoundToInt rounds, out-of-range
//      and NaN clamp the way FMath::Clamp clamps, and every one of the 65 536 samples survives
//      decode -> encode. The reference here is UE's two formulas TRANSCRIBED into this file with their
//      own literals, not a call back into the code under test — an encoder checked against itself agrees
//      with any constant.
//   2. SAMPLING AGREES WITH ANALYTIC SURFACES. A plane is reproduced exactly (bilinear is exact on it),
//      its normal exactly; a sine is reproduced within the bound bilinear interpolation and quantisation
//      together allow, and that bound is shown to be TIGHT enough to fail a wrong interpolation.
//      Placement (origin, base, spacing) moves the answer; both tile edges are inclusive, off the tile is
//      nullopt, and two tiles sharing an edge answer the seam identically.
//   3. DIRTY RECTANGLES ARE EXACTLY THE CHANGED REGION. A new tile is wholly dirty; a no-op write dirties
//      nothing; touching rectangles merge, far ones do not, and a bridge merges a chain. Refused writes
//      write nothing. A snapshot taken by ReadRegion restores the tile exactly (the undo contract).
//   4. THE BLOB ROUND-TRIPS BYTE FOR BYTE, its layout is pinned byte by byte on a tiny tile, and every
//      way a blob can be wrong is refused with the number that was wrong — including a dimension swap
//      that keeps every length consistent, which only the header-covering checksum can see.

#include <Engine/World/Landscape/LandscapeData.hpp>

#include <Common/Utilities/Crc32c.hpp>

#include <glm/geometric.hpp>
#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <limits>
#include <numbers>
#include <random>
#include <string>
#include <vector>

using namespace Desert::World::Landscape;

namespace
{
    // ── UE, transcribed ───────────────────────────────────────────────────────────────────────────────
    // UE:Engine/Source/Runtime/Landscape/Public/LandscapeDataAccess.h:13-14, 26-37. Literals copied, not
    // taken from the header under test.
    float UeGetLocalHeight( uint16_t height )
    {
        return ( static_cast<float>( height ) - 32768.f ) * ( 1.0f / 128.0f );
    }

    uint16_t UeGetTexHeight( float height )
    {
        const float x   = height * 128.0f + 32768.f;
        const float lo  = 0.f;
        const float hi  = 65535.f;
        const float cl  = x < lo ? lo : ( x < hi ? x : hi );           // FMath::Clamp
        const int   rnd = static_cast<int>( std::floor( cl + 0.5f ) ); // FMath::RoundToInt
        return static_cast<uint16_t>( rnd );
    }

    LandscapeTileData MakeTile( uint32_t sx, uint32_t sz )
    {
        auto r = LandscapeTileData::Create( sx, sz );
        EXPECT_TRUE( r.IsSuccess() ) << r.GetError();
        return r.ExtractValue();
    }

    // A tile whose samples are f(x, z) encoded at the frame's Z scale.
    template <typename F>
    LandscapeTileData TileFrom( uint32_t sx, uint32_t sz, const LandscapeFrame& frame, F&& heightCm )
    {
        std::vector<uint16_t> samples( static_cast<size_t>( sx ) * sz );
        for ( uint32_t z = 0; z < sz; ++z )
            for ( uint32_t x = 0; x < sx; ++x )
                samples[static_cast<size_t>( z ) * sx + x] =
                     LandscapeSampleFromHeightCm( heightCm( static_cast<float>( x ) * frame.SpacingCm,
                                                            static_cast<float>( z ) * frame.SpacingCm ),
                                                  frame.ZScale );
        auto r = LandscapeTileData::FromSamples( sx, sz, std::move( samples ) );
        EXPECT_TRUE( r.IsSuccess() ) << r.GetError();
        return r.ExtractValue();
    }
} // namespace

// ── 1. Encoding ───────────────────────────────────────────────────────────────────────────────────────

TEST( LandscapeEncoding, BoundarySamplesAreUesNumbers )
{
    EXPECT_EQ( kLandscapeMidSample, 32768u );
    EXPECT_EQ( kLandscapeMaxSample, 65535u );

    EXPECT_FLOAT_EQ( LandscapeLocalHeight( 32768u ), 0.0f );
    EXPECT_FLOAT_EQ( LandscapeLocalHeight( 0u ), -256.0f );
    EXPECT_FLOAT_EQ( LandscapeLocalHeight( 65535u ), 255.9921875f );

    // Z scale 100 (UE's default actor scale): the documented ±256 m range, in centimetres.
    EXPECT_FLOAT_EQ( LandscapeHeightCm( 0u, 100.0f ), -25600.0f );
    EXPECT_FLOAT_EQ( LandscapeHeightCm( 65535u, 100.0f ), 25599.21875f );
    EXPECT_FLOAT_EQ( LandscapeHeightCm( 32769u, 100.0f ), 0.78125f ) << "one step at Z scale 100 is 100/128 cm";

    EXPECT_EQ( LandscapeSampleFromHeightCm( -25600.0f, 100.0f ), 0u );
    EXPECT_EQ( LandscapeSampleFromHeightCm( 0.0f, 100.0f ), 32768u );
    EXPECT_EQ( LandscapeSampleFromHeightCm( 25599.21875f, 100.0f ), 65535u );
    EXPECT_EQ( LandscapeSampleFromHeightCm( 25600.0f, 100.0f ), 65535u )
         << "+256 m is one step past the top: clamps";
    EXPECT_EQ( LandscapeSampleFromHeightCm( 1.0e9f, 100.0f ), 65535u );
    EXPECT_EQ( LandscapeSampleFromHeightCm( -1.0e9f, 100.0f ), 0u );

    // Another scale moves the range with it: Z scale 400 → ±1024 m.
    EXPECT_FLOAT_EQ( LandscapeHeightCm( 0u, 400.0f ), -102400.0f );
    EXPECT_EQ( LandscapeSampleFromHeightCm( 102396.875f, 400.0f ), 65535u );
}

TEST( LandscapeEncoding, RoundingAndClampAreUes )
{
    // Exact half steps: 32768.5 → 32769 and 32767.5 → 32768, i.e. floor(x + 0.5), not banker's rounding
    // and not round-away-from-zero-about-the-midpoint.
    EXPECT_EQ( LandscapeSampleFromLocal( 1.0f / 256.0f ), 32769u );
    EXPECT_EQ( LandscapeSampleFromLocal( -1.0f / 256.0f ), 32768u );
    EXPECT_EQ( LandscapeSampleFromLocal( 3.0f / 256.0f ), 32770u );
    EXPECT_EQ( LandscapeSampleFromLocal( -3.0f / 256.0f ), 32767u );

    // NaN: FMath::Clamp's comparison order sends it to the maximum.
    EXPECT_EQ( LandscapeSampleFromLocal( std::numeric_limits<float>::quiet_NaN() ), 65535u );
    EXPECT_EQ( LandscapeSampleFromLocal( std::numeric_limits<float>::infinity() ), 65535u );
    EXPECT_EQ( LandscapeSampleFromLocal( -std::numeric_limits<float>::infinity() ), 0u );
}

TEST( LandscapeEncoding, AgreesWithTranscribedUeOverEverySampleAndASweep )
{
    for ( uint32_t s = 0; s <= 65535u; ++s )
    {
        const auto sample = static_cast<uint16_t>( s );
        ASSERT_EQ( LandscapeLocalHeight( sample ), UeGetLocalHeight( sample ) ) << "sample " << s;
        ASSERT_EQ( LandscapeSampleFromLocal( LandscapeLocalHeight( sample ) ), sample ) << "sample " << s;
    }
    // Arbitrary heights, including quarter and three-quarter steps and values past both ends.
    std::mt19937                          rng( 1234u );
    std::uniform_real_distribution<float> dist( -300.0f, 300.0f );
    for ( int i = 0; i < 200000; ++i )
    {
        const float h = dist( rng );
        ASSERT_EQ( LandscapeSampleFromLocal( h ), UeGetTexHeight( h ) ) << "local height " << h;
    }
}

// ── 2. Sampling ───────────────────────────────────────────────────────────────────────────────────────

TEST( LandscapeSampling, PlaneIsExactHeightAndNormal )
{
    // Z scale 128 makes one step exactly 1 cm, so a plane with integer-cm heights at the samples is
    // stored without quantisation and bilinear interpolation must reproduce it to float precision.
    LandscapeFrame frame;
    frame.OriginX   = -350.0f;
    frame.OriginZ   = 1200.0f;
    frame.BaseY     = 40.0f;
    frame.SpacingCm = 50.0f;
    frame.ZScale    = 128.0f;
    ASSERT_TRUE( ValidateLandscapeFrame( frame ).IsSuccess() );

    // 6 cm per sample in X, -4 cm per sample in Z, 10 cm at the origin: slopes 0.12 and -0.08 per cm.
    const auto plane = []( float lx, float lz ) { return 10.0f + 0.12f * lx - 0.08f * lz; };
    const auto tile  = TileFrom( 17u, 9u, frame, plane );

    const glm::vec3                       expectedNormal = glm::normalize( glm::vec3( -0.12f, 1.0f, 0.08f ) );
    std::mt19937                          rng( 7u );
    std::uniform_real_distribution<float> ux( 0.0f, 16.0f * frame.SpacingCm );
    std::uniform_real_distribution<float> uz( 0.0f, 8.0f * frame.SpacingCm );
    for ( int i = 0; i < 2000; ++i )
    {
        const float lx = ux( rng );
        const float lz = uz( rng );
        const auto  h  = SampleLandscapeHeight( tile, frame, frame.OriginX + lx, frame.OriginZ + lz );
        ASSERT_TRUE( h.has_value() );
        ASSERT_NEAR( *h, frame.BaseY + plane( lx, lz ), 1e-3f ) << "at local (" << lx << ", " << lz << ")";
        const auto n = SampleLandscapeNormal( tile, frame, frame.OriginX + lx, frame.OriginZ + lz );
        ASSERT_TRUE( n.has_value() );
        ASSERT_NEAR( n->x, expectedNormal.x, 1e-5f );
        ASSERT_NEAR( n->y, expectedNormal.y, 1e-5f );
        ASSERT_NEAR( n->z, expectedNormal.z, 1e-5f );
    }
}

TEST( LandscapeSampling, InteriorOfACellIsBilinearNotNearestOrTriangle )
{
    // One cell whose four corners are 0, 0, 0, 128 cm (a saddle-free twist). Bilinear gives 32 at the
    // centre; either triangle split gives 0 or 64, nearest-sample gives one of the corners. This is the
    // case the plane cannot tell apart, because every scheme is exact on a plane.
    LandscapeFrame frame;
    frame.ZScale = 128.0f;
    auto tile    = MakeTile( 2u, 2u );
    tile.SetSample( 1u, 1u, static_cast<uint16_t>( kLandscapeMidSample + 128u ) );
    const auto h = SampleLandscapeHeight( tile, frame, 50.0f, 50.0f );
    ASSERT_TRUE( h.has_value() );
    EXPECT_FLOAT_EQ( *h, 32.0f );
    // The raised corner at (1, 1) weighs fx·fz, which is symmetric in the two fractions; a raised corner
    // at (1, 0) weighs fx·(1 - fz), which is not — so this is the probe that sees X and Z swapped.
    auto skew = MakeTile( 2u, 2u );
    skew.SetSample( 1u, 0u, static_cast<uint16_t>( kLandscapeMidSample + 128u ) );
    const auto q = SampleLandscapeHeight( skew, frame, 25.0f, 75.0f );
    ASSERT_TRUE( q.has_value() );
    EXPECT_FLOAT_EQ( *q, 128.0f * 0.25f * 0.25f ) << "the X and Z fractions must not be swapped or shared";
}

TEST( LandscapeSampling, SineIsWithinTheInterpolationAndQuantisationBound )
{
    LandscapeFrame frame; // UE defaults: 1 m spacing, Z scale 100 (step 0.78125 cm)
    const float    amplitude = 3000.0f;
    const float    k         = 2.0f * std::numbers::pi_v<float> / 1600.0f; // 16 samples per wavelength
    const auto     wave      = [&]( float lx, float ) { return amplitude * std::sin( k * lx ); };
    const auto     tile      = TileFrom( 65u, 5u, frame, wave );

    // Bilinear along X on a sine: |error| <= A k^2 s^2 / 8. Quantisation adds at most half a step.
    const float s         = frame.SpacingCm;
    const float step      = 100.0f / 128.0f;
    const float heightTol = amplitude * k * k * s * s / 8.0f + 0.5f * step + 1e-2f;
    // Central difference of a sine: slope error <= A k^3 s^2 / 6, plus quantisation (step / s), then the
    // bilinear blend of those slopes adds its own A k^3 s^2 / 8.
    const float slopeTol = amplitude * k * k * k * s * s * ( 1.0f / 6.0f + 1.0f / 8.0f ) + step / s + 1e-4f;

    float worstH = 0.0f;
    for ( int i = 0; i <= 6400; ++i )
    {
        const float lx = 64.0f * s * static_cast<float>( i ) / 6400.0f;
        // Interior samples only for the normal: the border uses a one-sided difference, whose error is
        // first order and is checked separately below.
        const auto h = SampleLandscapeHeight( tile, frame, lx, 2.0f * s );
        ASSERT_TRUE( h.has_value() );
        worstH = std::max( worstH, std::fabs( *h - wave( lx, 0.0f ) ) );
        ASSERT_NEAR( *h, wave( lx, 0.0f ), heightTol ) << "x = " << lx;
        if ( lx >= s && lx <= 63.0f * s )
        {
            const auto      n        = SampleLandscapeNormal( tile, frame, lx, 2.0f * s );
            const float     slope    = amplitude * k * std::cos( k * lx );
            const glm::vec3 expected = glm::normalize( glm::vec3( -slope, 1.0f, 0.0f ) );
            ASSERT_TRUE( n.has_value() );
            ASSERT_NEAR( -n->x / n->y, slope, slopeTol ) << "x = " << lx;
            ASSERT_NEAR( n->z, expected.z, 1e-4f );
        }
    }
    // The bound is not vacuous: the interpolation error actually reaches a good fraction of it, so a
    // scheme with a larger error (nearest-sample, for one) could not hide under it.
    EXPECT_GT( worstH, 0.5f * heightTol );
}

TEST( LandscapeSampling, EdgesAreInclusiveOffTileIsNulloptAndSeamsAgree )
{
    LandscapeFrame a;
    a.ZScale      = 128.0f;
    const auto hA = []( float lx, float lz ) { return 3.0f * lx / 100.0f + 7.0f * lz / 100.0f; };
    const auto tA = TileFrom( 5u, 5u, a, hA );

    // Tile B is A's right-hand neighbour: its first column IS A's last column.
    LandscapeFrame b = a;
    b.OriginX        = 4.0f * a.SpacingCm;
    const auto tB    = TileFrom( 5u, 5u, b, [&]( float lx, float lz ) { return hA( lx + b.OriginX, lz ); } );

    const float seam = 4.0f * a.SpacingCm;
    for ( float z = 0.0f; z <= 400.0f; z += 12.5f )
    {
        const auto fromA = SampleLandscapeHeight( tA, a, seam, z );
        const auto fromB = SampleLandscapeHeight( tB, b, seam, z );
        ASSERT_TRUE( fromA.has_value() && fromB.has_value() ) << "the seam belongs to both tiles, z = " << z;
        EXPECT_FLOAT_EQ( *fromA, *fromB ) << "z = " << z;
    }

    // Far corners are on the tile and equal the corner samples.
    ASSERT_TRUE( SampleLandscapeHeight( tA, a, 400.0f, 400.0f ).has_value() );
    EXPECT_FLOAT_EQ( *SampleLandscapeHeight( tA, a, 400.0f, 400.0f ), hA( 400.0f, 400.0f ) );
    EXPECT_FLOAT_EQ( *SampleLandscapeHeight( tA, a, 0.0f, 0.0f ), 0.0f );

    // Just off any edge, and NaN: nullopt, never the edge height.
    EXPECT_FALSE( SampleLandscapeHeight( tA, a, 400.01f, 10.0f ).has_value() );
    EXPECT_FALSE( SampleLandscapeHeight( tA, a, -0.01f, 10.0f ).has_value() );
    EXPECT_FALSE( SampleLandscapeHeight( tA, a, 10.0f, 400.01f ).has_value() );
    EXPECT_FALSE( SampleLandscapeHeight( tA, a, 10.0f, -0.01f ).has_value() );
    EXPECT_FALSE( SampleLandscapeHeight( tA, a, std::nanf( "" ), 10.0f ).has_value() );
    EXPECT_FALSE( SampleLandscapeNormal( tA, a, 400.01f, 10.0f ).has_value() );
}

TEST( LandscapeSampling, FrameValidationNamesTheNumber )
{
    LandscapeFrame f;
    f.SpacingCm = 0.0f;
    auto r      = ValidateLandscapeFrame( f );
    ASSERT_FALSE( r.IsSuccess() );
    EXPECT_NE( r.GetError().find( "spacing" ), std::string::npos ) << r.GetError();

    f        = {};
    f.ZScale = -1.0f;
    r        = ValidateLandscapeFrame( f );
    ASSERT_FALSE( r.IsSuccess() );
    EXPECT_NE( r.GetError().find( "-1" ), std::string::npos ) << r.GetError();

    f         = {};
    f.OriginX = std::numeric_limits<float>::infinity();
    EXPECT_FALSE( ValidateLandscapeFrame( f ).IsSuccess() );
}

// ── 3. Dirty rectangles ───────────────────────────────────────────────────────────────────────────────

TEST( LandscapeDirty, NewTileIsWhollyDirtyAndTakeClears )
{
    auto tile = MakeTile( 33u, 17u );
    ASSERT_EQ( tile.DirtyRects().size(), 1u );
    EXPECT_EQ( tile.DirtyRects()[0], ( LandscapeRect{ 0u, 0u, 33u, 17u } ) );
    const auto taken = tile.TakeDirtyRects();
    EXPECT_EQ( taken.size(), 1u );
    EXPECT_TRUE( tile.DirtyRects().empty() );
}

TEST( LandscapeDirty, NoOpWritesDirtyNothing )
{
    auto tile = MakeTile( 8u, 8u );
    tile.TakeDirtyRects();
    tile.SetSample( 3u, 3u, kLandscapeMidSample );
    EXPECT_TRUE( tile.DirtyRects().empty() ) << "writing the value already there is not a change";
    const std::vector<uint16_t> same( 4u, kLandscapeMidSample );
    ASSERT_TRUE( tile.WriteRegion( { 1u, 1u, 3u, 3u }, same ).IsSuccess() );
    EXPECT_TRUE( tile.DirtyRects().empty() );
}

TEST( LandscapeDirty, TouchingMergesFarStaysApartBridgeJoinsTheChain )
{
    auto tile = MakeTile( 64u, 64u );
    tile.TakeDirtyRects();

    tile.SetSample( 5u, 5u, 1u );
    ASSERT_EQ( tile.DirtyRects().size(), 1u );
    EXPECT_EQ( tile.DirtyRects()[0], ( LandscapeRect{ 5u, 5u, 6u, 6u } ) );

    tile.SetSample( 6u, 5u, 1u ); // shares an edge
    ASSERT_EQ( tile.DirtyRects().size(), 1u );
    EXPECT_EQ( tile.DirtyRects()[0], ( LandscapeRect{ 5u, 5u, 7u, 6u } ) );

    tile.SetSample( 40u, 40u, 1u ); // far away
    ASSERT_EQ( tile.DirtyRects().size(), 2u );

    // A region spanning from the first box to the second absorbs both into one.
    std::vector<uint16_t> fill( 36u * 36u, 2u );
    ASSERT_TRUE( tile.WriteRegion( { 6u, 5u, 42u, 41u }, fill ).IsSuccess() );
    ASSERT_EQ( tile.DirtyRects().size(), 1u );
    EXPECT_EQ( tile.DirtyRects()[0], ( LandscapeRect{ 5u, 5u, 42u, 41u } ) );

    // Chain: B, then A, then R, where R touches only A and only A ∪ R reaches B. B sits EARLIER in the
    // list than A, so a merge that does not rescan after growing leaves two rectangles that touch.
    tile.TakeDirtyRects();
    tile.SetSample( 2u, 8u, 9u );                                                                         // B
    ASSERT_TRUE( tile.WriteRegion( { 0u, 0u, 1u, 10u }, std::vector<uint16_t>( 10u, 9u ) ).IsSuccess() ); // A
    ASSERT_EQ( tile.DirtyRects().size(), 2u );
    tile.SetSample( 1u, 0u, 9u ); // R
    ASSERT_EQ( tile.DirtyRects().size(), 1u );
    EXPECT_EQ( tile.DirtyRects()[0], ( LandscapeRect{ 0u, 0u, 3u, 10u } ) );
}

TEST( LandscapeDirty, RefusedWritesWriteNothingAndNameTheNumbers )
{
    auto tile = MakeTile( 8u, 8u );
    tile.TakeDirtyRects();
    const std::vector<uint16_t> before = tile.Samples();

    const std::vector<uint16_t> four( 4u, 1u );
    auto                        r = tile.WriteRegion( { 7u, 7u, 9u, 9u }, four );
    ASSERT_FALSE( r.IsSuccess() );
    EXPECT_NE( r.GetError().find( "9" ), std::string::npos ) << r.GetError();

    r = tile.WriteRegion( { 0u, 0u, 2u, 2u }, std::vector<uint16_t>( 3u, 1u ) );
    ASSERT_FALSE( r.IsSuccess() );
    EXPECT_NE( r.GetError().find( "got 3" ), std::string::npos ) << r.GetError();

    r = tile.WriteRegion( { 3u, 3u, 3u, 5u }, {} );
    EXPECT_FALSE( r.IsSuccess() ) << "an empty region is a caller error, not a successful nothing";

    EXPECT_FALSE( tile.ReadRegion( { 0u, 0u, 9u, 1u } ).IsSuccess() );

    EXPECT_EQ( tile.Samples(), before );
    EXPECT_TRUE( tile.DirtyRects().empty() );
}

TEST( LandscapeDirty, SnapshotRestoresExactlyTheUndoContract )
{
    LandscapeFrame frame;
    auto           tile = TileFrom( 20u, 20u, frame, []( float x, float z ) { return 0.5f * x - 0.25f * z; } );
    tile.TakeDirtyRects();
    const std::vector<uint16_t> original = tile.Samples();

    const LandscapeRect brush{ 4u, 6u, 11u, 9u };
    auto                snapshot = tile.ReadRegion( brush );
    ASSERT_TRUE( snapshot.IsSuccess() );
    const std::vector<uint16_t> saved = snapshot.ExtractValue();
    ASSERT_EQ( saved.size(), brush.Area() );
    EXPECT_EQ( saved[0], tile.Sample( 4u, 6u ) );
    EXPECT_EQ( saved[brush.Width()], tile.Sample( 4u, 7u ) ) << "row-major, X fastest";

    std::vector<uint16_t> stroke( saved.size() );
    for ( size_t i = 0; i < stroke.size(); ++i )
        stroke[i] = static_cast<uint16_t>( 1000u + i );
    ASSERT_TRUE( tile.WriteRegion( brush, stroke ).IsSuccess() );
    ASSERT_EQ( tile.DirtyRects().size(), 1u );
    EXPECT_EQ( tile.DirtyRects()[0], brush );
    EXPECT_EQ( tile.Sample( 10u, 8u ), static_cast<uint16_t>( 1000u + stroke.size() - 1u ) );
    // Outside the brush nothing moved.
    EXPECT_EQ( tile.Sample( 3u, 6u ), original[6u * 20u + 3u] );
    EXPECT_EQ( tile.Sample( 11u, 8u ), original[8u * 20u + 11u] );

    tile.TakeDirtyRects();
    ASSERT_TRUE( tile.WriteRegion( brush, saved ).IsSuccess() );
    EXPECT_EQ( tile.Samples(), original );
    ASSERT_EQ( tile.DirtyRects().size(), 1u );
    EXPECT_EQ( tile.DirtyRects()[0], brush ) << "the undo is itself a change the GPU must receive";
}

TEST( LandscapeDirty, DimensionsAreValidated )
{
    EXPECT_FALSE( LandscapeTileData::Create( 1u, 8u ).IsSuccess() );
    EXPECT_FALSE( LandscapeTileData::Create( 8u, kLandscapeMaxTileSamples + 1u ).IsSuccess() );
    EXPECT_TRUE( LandscapeTileData::Create( 2u, 2u ).IsSuccess() );
    // The empty tile a failed unwrap hands back is inert, not plausible.
    const LandscapeTileData empty;
    EXPECT_FALSE( SampleLandscapeHeight( empty, LandscapeFrame{}, 0.0f, 0.0f ).has_value() );
    EXPECT_FALSE( SampleLandscapeNormal( empty, LandscapeFrame{}, 0.0f, 0.0f ).has_value() );
    EXPECT_FALSE( empty.ReadRegion( { 0u, 0u, 1u, 1u } ).IsSuccess() );
    EXPECT_TRUE( empty.DirtyRects().empty() );

    auto r = LandscapeTileData::FromSamples( 4u, 4u, std::vector<uint16_t>( 15u ) );
    ASSERT_FALSE( r.IsSuccess() );
    EXPECT_NE( r.GetError().find( "got 15" ), std::string::npos ) << r.GetError();
}

// ── 4. The blob ───────────────────────────────────────────────────────────────────────────────────────

namespace
{
    std::vector<unsigned char> PatchU32( std::vector<unsigned char> b, size_t at, uint32_t v )
    {
        for ( size_t i = 0; i < 4u; ++i )
            b[at + i] = static_cast<unsigned char>( ( v >> ( 8u * i ) ) & 0xFFu );
        return b;
    }

    // Rewrites the trailer so a deliberately wrong FIELD reaches the field's own check rather than being
    // refused by the checksum first. Uses Common's Crc32c directly, not the encoder.
    std::vector<unsigned char> Reseal( std::vector<unsigned char> b )
    {
        const size_t covered = b.size() - kLandscapeTileTrailerSize;
        return PatchU32( b, covered, Common::Utils::Crc32c( b.data(), covered ) );
    }
} // namespace

TEST( LandscapeBlob, LayoutIsPinnedByteByByte )
{
    auto tile = MakeTile( 2u, 3u );
    tile.SetSample( 1u, 0u, 0x1234u );
    tile.SetSample( 0u, 2u, 0xABCDu );
    const auto bytes = EncodeLandscapeTile( tile );
    ASSERT_EQ( kLandscapeTileHeaderSize, 28u );
    ASSERT_EQ( kLandscapeTileTrailerSize, 4u );
    ASSERT_EQ( bytes.size(), 28u + 2u * 6u + 4u );

    const unsigned char header[28] = {
         'D', 'L', 'H', 'T',             //
         1,   0,   0,   0,               // container version
         2,   0,   0,   0,               // samplesX
         3,   0,   0,   0,               // samplesZ
         0,   0,   0,   0,               // edit layers
         12,  0,   0,   0,   0, 0, 0, 0, // payload bytes (u64)
    };
    EXPECT_EQ( std::memcmp( bytes.data(), header, sizeof( header ) ), 0 );

    // Payload: little-endian, row-major, X fastest. Mid is 0x8000.
    const unsigned char payload[12] = { 0x00, 0x80, 0x34, 0x12, 0x00, 0x80, 0x00, 0x80, 0xCD, 0xAB, 0x00, 0x80 };
    EXPECT_EQ( std::memcmp( bytes.data() + 28, payload, sizeof( payload ) ), 0 );

    // Trailer: CRC-32C over header AND payload, little-endian.
    const uint32_t crc = Common::Utils::Crc32c( bytes.data(), 40u );
    for ( size_t i = 0; i < 4u; ++i )
        EXPECT_EQ( bytes[40u + i], static_cast<unsigned char>( ( crc >> ( 8u * i ) ) & 0xFFu ) ) << "byte " << i;
}

TEST( LandscapeBlob, RoundTripIsByteIdentical )
{
    std::mt19937 rng( 99u );
    for ( const auto& [sx, sz] : { std::pair{ 2u, 2u }, std::pair{ 65u, 33u }, std::pair{ 257u, 257u } } )
    {
        std::vector<uint16_t>                   samples( static_cast<size_t>( sx ) * sz );
        std::uniform_int_distribution<uint32_t> any( 0u, 65535u );
        for ( auto& s : samples )
            s = static_cast<uint16_t>( any( rng ) );
        auto made = LandscapeTileData::FromSamples( sx, sz, samples );
        ASSERT_TRUE( made.IsSuccess() );
        const auto tile  = made.ExtractValue();
        const auto first = EncodeLandscapeTile( tile );
        ASSERT_EQ( first.size(), kLandscapeTileHeaderSize + 2u * samples.size() + kLandscapeTileTrailerSize );

        auto decoded = DecodeLandscapeTile( first );
        ASSERT_TRUE( decoded.IsSuccess() ) << decoded.GetError();
        auto back = decoded.ExtractValue();
        EXPECT_EQ( back.SamplesX(), sx );
        EXPECT_EQ( back.SamplesZ(), sz );
        EXPECT_EQ( back.Samples(), samples );
        ASSERT_EQ( back.DirtyRects().size(), 1u ) << "a decoded tile has never been uploaded";
        EXPECT_EQ( back.DirtyRects()[0], back.Bounds() );

        EXPECT_EQ( EncodeLandscapeTile( back ), first ) << sx << " x " << sz;
        // Dirty state is not in the blob.
        back.TakeDirtyRects();
        EXPECT_EQ( EncodeLandscapeTile( back ), first );
    }
}

TEST( LandscapeBlob, EveryWrongBlobIsRefusedWithItsNumber )
{
    auto tile = MakeTile( 4u, 3u );
    tile.SetSample( 2u, 1u, 777u );
    const auto good = EncodeLandscapeTile( tile );
    ASSERT_EQ( good.size(), 56u );
    ASSERT_TRUE( DecodeLandscapeTile( good ).IsSuccess() );
    ASSERT_TRUE( DecodeLandscapeTile( Reseal( good ) ).IsSuccess() ) << "Reseal must be a no-op on a good blob";

    const auto refuse = [&]( const std::vector<unsigned char>& bytes, const char* needle, const char* what )
    {
        auto r = DecodeLandscapeTile( bytes );
        ASSERT_FALSE( r.IsSuccess() ) << what;
        EXPECT_NE( r.GetError().find( needle ), std::string::npos ) << what << ": " << r.GetError();
    };

    refuse( std::vector<unsigned char>( good.begin(), good.begin() + 31 ), "31 bytes", "shorter than header" );
    {
        auto b = good;
        b[0]   = 'X';
        refuse( b, "58", "bad magic" ); // 'X' = 0x58
    }
    refuse( Reseal( PatchU32( good, 4, 2u ) ), "version 2", "future version" );

    // Field checks, each reached past a valid checksum.
    refuse( Reseal( PatchU32( good, 8, 1u ) ), "1 x 3", "one-sample side" );
    refuse( Reseal( PatchU32( good, 12, kLandscapeMaxTileSamples + 1u ) ), "8194", "oversized side" );
    refuse( Reseal( PatchU32( good, 16, 1u ) ), "1 edit layers", "edit layers present" );
    refuse( Reseal( PatchU32( good, 20, 25u ) ), "25 payload", "stated length disagrees with dimensions" );
    {
        std::vector<unsigned char> b( good.begin(), good.end() - 4 );
        b.pop_back(); // one payload byte short
        b.insert( b.end(), 4u, 0u );
        refuse( Reseal( b ), "55 bytes", "truncated payload" );
    }
    {
        std::vector<unsigned char> b( good.begin(), good.end() - 4 );
        b.push_back( 0u ); // one payload byte over
        b.insert( b.end(), 4u, 0u );
        refuse( Reseal( b ), "57 bytes", "trailing byte" );
    }

    // Corruption: refused by the checksum, wherever it lands.
    {
        auto b = good;
        b[kLandscapeTileHeaderSize + 5u] ^= 0x01u;
        refuse( b, "checksum", "flipped payload bit" );
    }
    // 4 x 3 read as 3 x 4: every length check agrees, and the rows would be sheared. Only a checksum that
    // covers the header can see it.
    refuse( PatchU32( PatchU32( good, 8, 3u ), 12, 4u ), "checksum", "dimensions swapped" );
    refuse( std::vector<unsigned char>( good.begin(), good.end() - 1 ), "checksum", "trailer cut" );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
