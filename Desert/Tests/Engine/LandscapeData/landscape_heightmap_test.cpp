// The landscape's GPU heightmap path (LS-4), tested through the SAME TEXT the terrain shader compiles:
// Shaders/Common/LandscapeHeight.glslh, included here as C++. Three questions:
//
//   1. Does the tessellation-evaluation stage compute the height the CPU reports? The GPU starts from an
//      R16_UNORM texel (s / 65535 as a float), not from the uint16 the CPU holds, so the integer has to
//      come back exactly — and then the decode and the cell's triangle must be the CPU's own.
//   2. Do two tiles agree bit for bit on their shared edge? The shader places an edge vertex from GLOBAL
//      sample indices and evaluates the far column of one tile at fraction 1 and the near column of the
//      next at fraction 0; these tests run that evaluation from both sides of every seam of a 2x2 layout
//      at every tessellation level the pass can emit and compare the bits.
//   3. Is the NORMAL on a shared edge one value (LS-5)? The gradient at a border sample is a central
//      difference through the neighbour's ring row, so both tiles must give the same bits — and the CPU's
//      SampleLandscapeNormal must give the GPU's.
//
// The shader-side glue that is not in the .glslh — the ring texel addressing, the neighbour mask and the
// patch bilerp order — is restated below (TileHeightCm, TileGradient, TesHeight, TesNormal); it mirrors
// Programs/Terrain/TerrainTessEval.glslh line for line.

#include <Engine/World/Landscape/LandscapeData.hpp>
#include <Common/Core/GlslAsCpp.hpp>

#include <glm/glm.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

using namespace Desert::World::Landscape;

namespace
{
    using glm::floor;
    using glm::min;

    DESERT_GLSL_AS_CPP_BEGIN
#include <Common/LandscapeHeight.glslh>
    DESERT_GLSL_AS_CPP_END

    // What the sampler hands the shader for an R16_UNORM texel: c / (2^16 - 1) (Vulkan spec, "Conversion
    // from Normalized Fixed-Point to Floating-Point").
    float UnormOf( uint16_t s )
    {
        return static_cast<float>( s ) / 65535.0f;
    }

    uint32_t Bits( float f )
    {
        uint32_t b = 0u;
        std::memcpy( &b, &f, sizeof( b ) );
        return b;
    }

    // One tile as the shader sees it: the texel grid as LandscapeECSSystem uploads it (the samples with the
    // neighbours' ring, LandscapeBorderedSamples), the neighbour mask, the tile's first global sample, the
    // frame numbers.
    struct GpuTile
    {
        std::vector<uint16_t> Texels;
        int                   Width  = 0; // samples + 2
        float                 Mask   = 0.0f;
        float                 FirstX = 0.0f;
        float                 FirstZ = 0.0f;
        float                 Quads  = 0.0f;
        float                 ZScale = 100.0f;
    };

    GpuTile MakeGpuTile( const LandscapeTileData& tile, const LandscapeTileNeighbours& neighbours, float firstX,
                         float firstZ, float quads, float zScale )
    {
        GpuTile t;
        t.Texels = LandscapeBorderedSamples( tile, neighbours );
        t.Width  = static_cast<int>( tile.SamplesX() ) + 2;
        t.Mask   = static_cast<float>( LandscapeNeighbourMask( neighbours ) );
        t.FirstX = firstX;
        t.FirstZ = firstZ;
        t.Quads  = quads;
        t.ZScale = zScale;
        return t;
    }

    // TerrainTessEval.glslh, TileHeightCm: texel (x + 1, z + 1), recover the integer, decode. The index
    // is asserted in range rather than clamped, as the shader relies on it being.
    float TileHeightCm( const GpuTile& t, int x, int z )
    {
        EXPECT_TRUE( x >= -1 && z >= -1 && x + 1 < t.Width && z + 1 < t.Width ) << x << ", " << z;
        const float unorm = UnormOf( t.Texels[static_cast<size_t>( z + 1 ) * static_cast<size_t>( t.Width ) +
                                              static_cast<size_t>( x + 1 )] );
        return LandscapeHeightCmFromSample( LandscapeSampleFromUnorm( unorm ), t.ZScale );
    }

    // TerrainTessEval.glslh, NeighbourPresent.
    float NeighbourPresent( const GpuTile& t, int bit )
    {
        return static_cast<float>( ( static_cast<int>( std::lround( t.Mask ) ) >> bit ) & 1 );
    }

    // TerrainTessEval.glslh, TileGradient.
    glm::vec2 TileGradient( const GpuTile& t, int x, int z, float spacing )
    {
        const float last = t.Quads;
        const float xa   = LandscapeGradientLow( static_cast<float>( x ), NeighbourPresent( t, 0 ) );
        const float xb   = LandscapeGradientHigh( static_cast<float>( x ), last, NeighbourPresent( t, 1 ) );
        const float za   = LandscapeGradientLow( static_cast<float>( z ), NeighbourPresent( t, 2 ) );
        const float zb   = LandscapeGradientHigh( static_cast<float>( z ), last, NeighbourPresent( t, 3 ) );
        return { LandscapeGradient( TileHeightCm( t, static_cast<int>( xa ), z ),
                                    TileHeightCm( t, static_cast<int>( xb ), z ), xb - xa, spacing ),
                 LandscapeGradient( TileHeightCm( t, x, static_cast<int>( za ) ),
                                    TileHeightCm( t, x, static_cast<int>( zb ) ), zb - za, spacing ) };
    }

    // TerrainTessEval.glslh, LandscapeMain's normal at global sample coordinate (gx, gz).
    glm::vec3 TesNormal( const GpuTile& t, float gx, float gz, float spacing )
    {
        const float     samples = t.Quads + 1.0f;
        const float     lx      = gx - t.FirstX;
        const float     lz      = gz - t.FirstZ;
        const float     cellX   = LandscapeCellOf( lx, samples );
        const float     cellZ   = LandscapeCellOf( lz, samples );
        const int       cx      = static_cast<int>( cellX );
        const int       cz      = static_cast<int>( cellZ );
        const float     fx      = lx - cellX;
        const float     fz      = lz - cellZ;
        const glm::vec2 d00     = TileGradient( t, cx, cz, spacing );
        const glm::vec2 d10     = TileGradient( t, cx + 1, cz, spacing );
        const glm::vec2 d01     = TileGradient( t, cx, cz + 1, spacing );
        const glm::vec2 d11     = TileGradient( t, cx + 1, cz + 1, spacing );
        const float     dx      = LandscapeBilinear( d00.x, d10.x, d01.x, d11.x, fx, fz );
        const float     dz      = LandscapeBilinear( d00.y, d10.y, d01.y, d11.y, fx, fz );
        return glm::normalize( glm::vec3( -dx, 1.0f, -dz ) );
    }

    // TerrainTessEval.glslh, LandscapeMain: the height at global sample coordinate (gx, gz).
    float TesHeight( const GpuTile& t, float gx, float gz )
    {
        const float samples = t.Quads + 1.0f;
        const float lx      = gx - t.FirstX;
        const float lz      = gz - t.FirstZ;
        const float cellX   = LandscapeCellOf( lx, samples );
        const float cellZ   = LandscapeCellOf( lz, samples );
        const int   cx      = static_cast<int>( cellX );
        const int   cz      = static_cast<int>( cellZ );
        return LandscapeTriangle( TileHeightCm( t, cx, cz ), TileHeightCm( t, cx + 1, cz ),
                                  TileHeightCm( t, cx, cz + 1 ), TileHeightCm( t, cx + 1, cz + 1 ), lx - cellX,
                                  lz - cellZ );
    }

    // TerrainTessEval.glslh, LandscapeMain's patch point: X from the u edge, Z from the v edge.
    glm::vec2 PatchPoint( glm::vec2 g0, glm::vec2 g1, glm::vec2 /*g2*/, glm::vec2 g3, float u, float v )
    {
        return { LandscapeLerp( g0.x, g1.x, u ), LandscapeLerp( g0.y, g3.y, v ) };
    }

    // A deterministic rough field — steep enough that neighbouring samples differ by thousands of steps,
    // so an off-by-one sample or a rounding in the lerp shows as a different height.
    uint16_t FieldSample( uint32_t x, uint32_t z )
    {
        uint32_t h = x * 73856093u ^ z * 19349663u;
        h ^= h >> 13;
        h *= 0x5bd1e995u;
        h ^= h >> 15;
        return static_cast<uint16_t>( 20000u + h % 25000u );
    }

    // Tile (tx, tz) of a landscape of `quads` quads per tile, cut from the global field: its last row and
    // column ARE the neighbour's first, as the tile files store them.
    LandscapeTileData CutTile( uint32_t quads, uint32_t tx, uint32_t tz )
    {
        const uint32_t        n = quads + 1u;
        std::vector<uint16_t> samples( static_cast<size_t>( n ) * n );
        for ( uint32_t z = 0; z < n; ++z )
            for ( uint32_t x = 0; x < n; ++x )
                samples[static_cast<size_t>( z ) * n + x] = FieldSample( tx * quads + x, tz * quads + z );
        auto r = LandscapeTileData::FromSamples( n, n, std::move( samples ) );
        EXPECT_TRUE( r.IsSuccess() );
        return r.GetValue();
    }
} // namespace

// ── 1. The shader's decode is the CPU's ──────────────────────────────────────────────────────────────

TEST( LandscapeHeightmap, EveryR16TexelDecodesToTheIntegerItWasUploadedFrom )
{
    for ( uint32_t s = 0; s <= 65535u; ++s )
    {
        const float unorm = UnormOf( static_cast<uint16_t>( s ) );
        ASSERT_EQ( LandscapeSampleFromUnorm( unorm ), static_cast<float>( s ) ) << "sample " << s;
        // The conversion is allowed a little slack by the spec; one ulp either way must not move it.
        ASSERT_EQ( LandscapeSampleFromUnorm( std::nextafter( unorm, 2.0f ) ), static_cast<float>( s ) ) << s;
        ASSERT_EQ( LandscapeSampleFromUnorm( std::nextafter( unorm, -1.0f ) ), static_cast<float>( s ) ) << s;
    }
}

TEST( LandscapeHeightmap, TheShaderDecodeIsTheCpuDecodeBitForBit )
{
    for ( const float zScale : { 100.0f, 37.5f, 256.3f, 1.0f } )
        for ( uint32_t s = 0; s <= 65535u; ++s )
        {
            const auto  sample = static_cast<uint16_t>( s );
            const float gpu = LandscapeHeightCmFromSample( LandscapeSampleFromUnorm( UnormOf( sample ) ), zScale );
            ASSERT_EQ( Bits( gpu ), Bits( LandscapeHeightCm( sample, zScale ) ) )
                 << "sample " << s << " scale " << zScale;
        }
}

TEST( LandscapeHeightmap, TheGlslConstantsAreTheHeadersConstants )
{
    EXPECT_EQ( LANDSCAPE_MID_SAMPLE, static_cast<float>( kLandscapeMidSample ) );
    EXPECT_EQ( LANDSCAPE_LOCAL_PER_STEP, kLandscapeLocalPerStep );
    EXPECT_EQ( LANDSCAPE_MAX_SAMPLE, static_cast<float>( kLandscapeMaxSample ) );
}

// The tessellated surface between samples is the CPU's SampleLandscapeHeight, bit for bit, at every
// point where both sides can name the same grid coordinate exactly (quarter-sample steps on a unit
// spacing, so world -> grid is exact on the CPU side too).
TEST( LandscapeHeightmap, TheTessellatedSurfaceIsTheCpuSampling )
{
    const uint32_t          quads = 15u;
    const LandscapeTileData tile  = CutTile( quads, 0u, 0u );
    LandscapeFrame          frame;
    frame.SpacingCm = 1.0f;
    frame.ZScale    = 100.0f;
    frame.BaseY     = 0.0f;
    const GpuTile gpu = MakeGpuTile( tile, {}, 0.0f, 0.0f, static_cast<float>( quads ), frame.ZScale );

    for ( uint32_t iz = 0; iz <= quads * 4u; ++iz )
        for ( uint32_t ix = 0; ix <= quads * 4u; ++ix )
        {
            const float gx  = static_cast<float>( ix ) * 0.25f;
            const float gz  = static_cast<float>( iz ) * 0.25f;
            const auto  cpu = SampleLandscapeHeight( tile, frame, gx, gz );
            ASSERT_TRUE( cpu.has_value() );
            ASSERT_EQ( Bits( TesHeight( gpu, gx, gz ) ),
                       Bits( cpu.value() ) ) // NOLINT(bugprone-unchecked-optional-access)
                 << "at (" << gx << ", " << gz << ")";
        }
}

// The last sample belongs to the last CELL, at fraction 1. No height can show the difference — a cell one
// further on is weighted by fraction 0 — so this is pinned directly: that cell's far corner is a sample the
// tile does not have, and the CPU would read past its vector to fetch it.
TEST( LandscapeHeightmap, TheLastSampleIsTheFarCornerOfTheLastCell )
{
    for ( const float samples : { 2.0f, 16.0f, 64.0f, 256.0f } )
    {
        EXPECT_EQ( LandscapeCellOf( samples - 1.0f, samples ), samples - 2.0f ) << samples;
        EXPECT_EQ( LandscapeCellOf( samples - 1.5f, samples ), samples - 2.0f ) << samples;
        EXPECT_EQ( LandscapeCellOf( 0.0f, samples ), 0.0f ) << samples;
    }
}

// ── 2. Seams ────────────────────────────────────────────────────────────────────────────────────────

// Every vertex the tessellator can place on a shared edge of a 2x2 landscape, evaluated from both tiles:
// the world position (X, Y, Z) must be the same bits. The corner positions — the TCS's only input to the
// edge's tessellation level — are the endpoints of the same evaluation, so equal corners here also mean
// an equal level on both sides.
TEST( LandscapeHeightmap, TwoByTwoTilesAgreeBitForBitOnEverySharedEdge )
{
    const uint32_t quads    = 63u; // the default tile, 7 patches of 9 quads
    const uint32_t perPatch = 9u;
    const float    spacing  = 100.0f;
    const float    originX  = -3150.0f;
    const float    originZ  = 1234.5f;
    const float    baseY    = -250.0f;

    std::array<LandscapeTileData, 4> tiles = { CutTile( quads, 0, 0 ), CutTile( quads, 1, 0 ),
                                               CutTile( quads, 0, 1 ), CutTile( quads, 1, 1 ) };
    auto                             gpuOf = [&]( uint32_t tx, uint32_t tz )
    {
        return MakeGpuTile( tiles[tz * 2u + tx], {}, static_cast<float>( tx * quads ),
                            static_cast<float>( tz * quads ), static_cast<float>( quads ), 100.0f );
    };
    auto world = [&]( const GpuTile& t, glm::vec2 g )
    { return glm::vec3( originX + g.x * spacing, baseY + TesHeight( t, g.x, g.y ), originZ + g.y * spacing ); };

    uint64_t compared = 0u;
    for ( uint32_t level = 1u; level <= 16u; ++level )
        for ( uint32_t patch = 0u; patch < 2u * quads / perPatch; ++patch )
            for ( uint32_t i = 0u; i <= level; ++i )
            {
                const float t = static_cast<float>( i ) / static_cast<float>( level );
                const auto  a = static_cast<float>( patch * perPatch );
                const auto  b = static_cast<float>( ( patch + 1u ) * perPatch );
                const auto  q = static_cast<float>( quads );

                // Vertical seam x = quads: the left tile's patch at u = 1, the right tile's at u = 0.
                {
                    const uint32_t  tz = patch < quads / perPatch ? 0u : 1u;
                    const glm::vec2 left =
                         PatchPoint( { q - perPatch, a }, { q, a }, { q, b }, { q - perPatch, b }, 1.0f, t );
                    const glm::vec2 right =
                         PatchPoint( { q, a }, { q + perPatch, a }, { q + perPatch, b }, { q, b }, 0.0f, t );
                    const glm::vec3 wl = world( gpuOf( 0u, tz ), left );
                    const glm::vec3 wr = world( gpuOf( 1u, tz ), right );
                    ASSERT_EQ( Bits( wl.x ), Bits( wr.x ) ) << "level " << level << " z " << left.y;
                    ASSERT_EQ( Bits( wl.y ), Bits( wr.y ) ) << "level " << level << " z " << left.y;
                    ASSERT_EQ( Bits( wl.z ), Bits( wr.z ) ) << "level " << level << " z " << left.y;
                    ++compared;
                }
                // Horizontal seam z = quads: the lower tile's patch at v = 1, the upper tile's at v = 0.
                {
                    const uint32_t  tx = patch < quads / perPatch ? 0u : 1u;
                    const glm::vec2 lower =
                         PatchPoint( { a, q - perPatch }, { b, q - perPatch }, { b, q }, { a, q }, t, 1.0f );
                    const glm::vec2 upper =
                         PatchPoint( { a, q }, { b, q }, { b, q + perPatch }, { a, q + perPatch }, t, 0.0f );
                    const glm::vec3 wl = world( gpuOf( tx, 0u ), lower );
                    const glm::vec3 wu = world( gpuOf( tx, 1u ), upper );
                    ASSERT_EQ( Bits( wl.x ), Bits( wu.x ) ) << "level " << level << " x " << lower.x;
                    ASSERT_EQ( Bits( wl.y ), Bits( wu.y ) ) << "level " << level << " x " << lower.x;
                    ASSERT_EQ( Bits( wl.z ), Bits( wu.z ) ) << "level " << level << " x " << lower.x;
                    ++compared;
                }
            }
    EXPECT_EQ( compared, 2u * 14u * ( 16u * 17u / 2u + 16u ) ) << "every level, every patch, every vertex";
}

// Why LandscapeLerp is spelled a*(1-t) + b*t: the other common form, a + (b-a)*t, does NOT return b at
// t = 1 for every pair of heights this landscape can hold — and the seam evaluates the far column at
// exactly t = 1. Found by search over real decoded heights, so the argument is a fact about this data.
TEST( LandscapeHeightmap, TheDifferenceFormOfLerpWouldOpenASeam )
{
    bool found = false;
    for ( uint32_t s = 0; s <= 65535u && !found; s += 7u )
    {
        const float a = LandscapeHeightCmFromSample( static_cast<float>( s ), 37.3f );
        for ( uint32_t r = 1; r <= 65535u && !found; r += 4099u )
        {
            const float b = LandscapeHeightCmFromSample( static_cast<float>( r ), 37.3f );
            found         = a + ( b - a ) * 1.0f != b;
        }
    }
    EXPECT_TRUE( found ) << "no counterexample: the lerp form would be a free choice, not a seam guarantee";
    for ( const float a : { -25600.0f, 0.78125f, 1234.5f } )
        for ( const float b : { 25599.2f, -3.3f, 0.1f } )
        {
            EXPECT_EQ( Bits( LandscapeLerp( a, b, 1.0f ) ), Bits( b ) );
            EXPECT_EQ( Bits( LandscapeLerp( a, b, 0.0f ) ), Bits( a ) );
        }
}

// ── 3. The normal on a seam (LS-5) ─────────────────────────────────────────────────────────────────────

namespace
{
    // A 2x2 landscape cut from the global field, each tile knowing its loaded neighbours.
    struct TwoByTwo
    {
        static constexpr uint32_t kQuads = 15u;

        std::array<LandscapeTileData, 4> Tiles = { CutTile( kQuads, 0, 0 ), CutTile( kQuads, 1, 0 ),
                                                   CutTile( kQuads, 0, 1 ), CutTile( kQuads, 1, 1 ) };

        [[nodiscard]] const LandscapeTileData& At( uint32_t tx, uint32_t tz ) const
        {
            return Tiles[tz * 2u + tx];
        }

        [[nodiscard]] LandscapeTileNeighbours NeighboursOf( uint32_t tx, uint32_t tz ) const
        {
            LandscapeTileNeighbours n;
            n.West  = tx == 1u ? &At( 0u, tz ) : nullptr;
            n.East  = tx == 0u ? &At( 1u, tz ) : nullptr;
            n.South = tz == 1u ? &At( tx, 0u ) : nullptr;
            n.North = tz == 0u ? &At( tx, 1u ) : nullptr;
            return n;
        }

        // Tile frames on a unit spacing at the origin, so world -> grid is exact on the CPU side.
        [[nodiscard]] static LandscapeFrame FrameOf( uint32_t tx, uint32_t tz )
        {
            LandscapeFrame f;
            f.OriginX   = static_cast<float>( tx * kQuads );
            f.OriginZ   = static_cast<float>( tz * kQuads );
            f.SpacingCm = 1.0f;
            f.ZScale    = 100.0f;
            return f;
        }

        [[nodiscard]] GpuTile Gpu( uint32_t tx, uint32_t tz, bool withNeighbours ) const
        {
            return MakeGpuTile( At( tx, tz ), withNeighbours ? NeighboursOf( tx, tz ) : LandscapeTileNeighbours{},
                                static_cast<float>( tx * kQuads ), static_cast<float>( tz * kQuads ),
                                static_cast<float>( kQuads ), 100.0f );
        }
    };

    bool SameBits( const glm::vec3& a, const glm::vec3& b )
    {
        return Bits( a.x ) == Bits( b.x ) && Bits( a.y ) == Bits( b.y ) && Bits( a.z ) == Bits( b.z );
    }

    // Every point of the seams of a 2x2 layout, in quarter samples, as (tile A, tile B, global g): the
    // vertical seam x = kQuads between (0, tz) and (1, tz), the horizontal z = kQuads between (tx, 0), (tx, 1).
    struct SeamPoint
    {
        uint32_t  ax, az, bx, bz;
        glm::vec2 G;
    };

    std::vector<SeamPoint> SeamPoints()
    {
        const auto             q = static_cast<float>( TwoByTwo::kQuads );
        std::vector<SeamPoint> points;
        for ( uint32_t i = 0; i <= 2u * TwoByTwo::kQuads * 4u; ++i )
        {
            const float    along = static_cast<float>( i ) * 0.25f;
            const uint32_t side  = along < q ? 0u : 1u;
            points.push_back( { 0u, side, 1u, side, { q, along } } );
            points.push_back( { side, 0u, side, 1u, { along, q } } );
            // A point exactly on the crossing belongs to all four; the other pair of tiles says it too.
            if ( along == q )
            {
                points.push_back( { 0u, 1u, 1u, 1u, { q, along } } );
                points.push_back( { 1u, 0u, 1u, 1u, { along, q } } );
            }
        }
        return points;
    }
} // namespace

TEST( LandscapeHeightmap, TheBorderedSamplesCarryTheNeighboursNextRow )
{
    const TwoByTwo                land;
    const LandscapeTileNeighbours n = land.NeighboursOf( 0u, 0u ); // east and north present
    EXPECT_EQ( LandscapeNeighbourMask( n ), 2u | 8u );

    const auto&                 tile  = land.At( 0u, 0u );
    const std::vector<uint16_t> ring  = LandscapeBorderedSamples( tile, n );
    const uint32_t              s     = tile.SamplesX();
    const uint32_t              width = s + 2u;
    ASSERT_EQ( ring.size(), static_cast<size_t>( width ) * ( s + 2u ) );
    auto at = [&]( int x, int z )
    { return ring[static_cast<size_t>( z + 1 ) * width + static_cast<size_t>( x + 1 )]; };

    for ( uint32_t i = 0; i < s; ++i )
    {
        // Interior: the tile's own samples.
        EXPECT_EQ( at( static_cast<int>( i ), 3 ), tile.Sample( i, 3u ) );
        // East ring: the east neighbour's SECOND column (its first is this tile's last) = global x = s.
        EXPECT_EQ( at( static_cast<int>( s ), static_cast<int>( i ) ), land.At( 1u, 0u ).Sample( 1u, i ) );
        EXPECT_EQ( at( static_cast<int>( s ), static_cast<int>( i ) ), FieldSample( s, i ) );
        // North ring: the north neighbour's second row = global z = s.
        EXPECT_EQ( at( static_cast<int>( i ), static_cast<int>( s ) ), FieldSample( i, s ) );
        // Absent west / south: the nearest own sample, never differenced (the mask bit is 0).
        EXPECT_EQ( at( -1, static_cast<int>( i ) ), tile.Sample( 0u, i ) );
        EXPECT_EQ( at( static_cast<int>( i ), -1 ), tile.Sample( i, 0u ) );
    }
}

// THE acceptance relation: on every seam point of a 2x2 layout the two tiles give the normal the same bits
// — on the GPU (the TES mirror over the uploaded texels) and on the CPU (SampleLandscapeNormal) — and the
// CPU's normal is the GPU's.
TEST( LandscapeHeightmap, TheNormalOnASeamIsOneValueFromBothTilesCpuAndGpu )
{
    const TwoByTwo land;
    uint64_t       compared = 0u;
    for ( const SeamPoint& p : SeamPoints() )
    {
        const glm::vec3 gpuA = TesNormal( land.Gpu( p.ax, p.az, true ), p.G.x, p.G.y, 1.0f );
        const glm::vec3 gpuB = TesNormal( land.Gpu( p.bx, p.bz, true ), p.G.x, p.G.y, 1.0f );
        const auto      cpuA = SampleLandscapeNormal( land.At( p.ax, p.az ), TwoByTwo::FrameOf( p.ax, p.az ),
                                                      land.NeighboursOf( p.ax, p.az ), p.G.x, p.G.y );
        const auto      cpuB = SampleLandscapeNormal( land.At( p.bx, p.bz ), TwoByTwo::FrameOf( p.bx, p.bz ),
                                                      land.NeighboursOf( p.bx, p.bz ), p.G.x, p.G.y );
        ASSERT_TRUE( cpuA.has_value() && cpuB.has_value() ) << p.G.x << ", " << p.G.y;
        ASSERT_TRUE( SameBits( gpuA, gpuB ) ) << "GPU seam normal differs at (" << p.G.x << ", " << p.G.y << ")";
        ASSERT_TRUE( SameBits( cpuA.value(), cpuB.value() ) ) // NOLINT(bugprone-unchecked-optional-access)
             << "CPU seam normal differs at (" << p.G.x << ", " << p.G.y << ")";
        // NOLINTBEGIN(bugprone-unchecked-optional-access)
        ASSERT_TRUE( SameBits( cpuA.value(), gpuA ) )
             << "CPU != GPU at (" << p.G.x << ", " << p.G.y << ")"; // NOLINT(bugprone-unchecked-optional-access)
        // NOLINTEND(bugprone-unchecked-optional-access)
        ++compared;
    }
    EXPECT_EQ( compared, 2u * ( 2u * TwoByTwo::kQuads * 4u + 1u ) + 2u );
}

// Everywhere, not only on the seam: the CPU normal is the GPU's at every quarter-sample of every tile, so
// the ring changes nothing inside a tile and the CPU follows the GPU onto the ring.
TEST( LandscapeHeightmap, TheCpuNormalIsTheGpuNormalOverEveryTile )
{
    const TwoByTwo land;
    for ( uint32_t tz = 0; tz < 2u; ++tz )
        for ( uint32_t tx = 0; tx < 2u; ++tx )
        {
            const GpuTile gpu = land.Gpu( tx, tz, true );
            for ( uint32_t iz = 0; iz <= TwoByTwo::kQuads * 4u; ++iz )
                for ( uint32_t ix = 0; ix <= TwoByTwo::kQuads * 4u; ++ix )
                {
                    const float gx =
                         static_cast<float>( tx * TwoByTwo::kQuads ) + static_cast<float>( ix ) * 0.25f;
                    const float gz =
                         static_cast<float>( tz * TwoByTwo::kQuads ) + static_cast<float>( iz ) * 0.25f;
                    const auto cpu = SampleLandscapeNormal( land.At( tx, tz ), TwoByTwo::FrameOf( tx, tz ),
                                                            land.NeighboursOf( tx, tz ), gx, gz );
                    ASSERT_TRUE( cpu.has_value() );
                    // NOLINTBEGIN(bugprone-unchecked-optional-access)
                    ASSERT_TRUE( SameBits( cpu.value(), TesNormal( gpu, gx, gz, 1.0f ) ) )
                         << gx << ", " << gz; // NOLINT(bugprone-unchecked-optional-access)
                    // NOLINTEND(bugprone-unchecked-optional-access)
                }
        }
}

// The negative control: WITHOUT the ring (each tile one-sided on its border, as LS-4 shipped) the two
// tiles disagree on the seam of this field — which is the line of light, and what proves the test above
// can see it.
TEST( LandscapeHeightmap, WithoutTheRingTheSeamNormalsDisagree )
{
    const TwoByTwo land;
    uint32_t       differing = 0u;
    for ( const SeamPoint& p : SeamPoints() )
        if ( !SameBits( TesNormal( land.Gpu( p.ax, p.az, false ), p.G.x, p.G.y, 1.0f ),
                        TesNormal( land.Gpu( p.bx, p.bz, false ), p.G.x, p.G.y, 1.0f ) ) )
            ++differing;
    EXPECT_GT( differing, 50u ) << "the one-sided border no longer shows a seam: the positive test proves nothing";
}

// On the landscape's own edge nothing lies beyond, and the gradient stays one-sided: a plane's normal is
// the plane's there too, not a slope halved against a repeated edge sample.
TEST( LandscapeHeightmap, OnTheLandscapesOwnEdgeTheGradientIsOneSided )
{
    const uint32_t        n = 8u;
    std::vector<uint16_t> ramp( static_cast<size_t>( n ) * n );
    for ( uint32_t z = 0; z < n; ++z )
        for ( uint32_t x = 0; x < n; ++x )
            ramp[static_cast<size_t>( z ) * n + x] =
                 static_cast<uint16_t>( 30000u + 128u * x ); // 1 cm/cm at ZScale 100
    const auto tile = LandscapeTileData::FromSamples( n, n, std::move( ramp ) );
    ASSERT_TRUE( tile.IsSuccess() );
    LandscapeFrame frame;
    frame.SpacingCm   = 100.0f;
    frame.ZScale      = 100.0f;
    const GpuTile gpu = MakeGpuTile( tile.GetValue(), {}, 0.0f, 0.0f, static_cast<float>( n - 1u ), 100.0f );
    for ( const float g : { 0.0f, 3.5f, 7.0f } )
    {
        const glm::vec3 expected = glm::normalize( glm::vec3( -1.0f, 1.0f, 0.0f ) );
        const glm::vec3 gpuN     = TesNormal( gpu, g, g, frame.SpacingCm );
        const auto      cpuN     = SampleLandscapeNormal( tile.GetValue(), frame, {}, g * 100.0f, g * 100.0f );
        ASSERT_TRUE( cpuN.has_value() );
        EXPECT_NEAR( gpuN.x, expected.x, 1e-6f ) << g;
        EXPECT_NEAR( gpuN.y, expected.y, 1e-6f ) << g;
        EXPECT_TRUE( SameBits( cpuN.value(), gpuN ) ) << g; // NOLINT(bugprone-unchecked-optional-access)
    }
}
