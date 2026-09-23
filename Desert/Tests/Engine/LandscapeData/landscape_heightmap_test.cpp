// The landscape's GPU heightmap path (LS-4), tested through the SAME TEXT the terrain shader compiles:
// Shaders/Common/LandscapeHeight.glslh, included here as C++. Two questions:
//
//   1. Does the tessellation-evaluation stage compute the height the CPU reports? The GPU starts from an
//      R16_UNORM texel (s / 65535 as a float), not from the uint16 the CPU holds, so the integer has to
//      come back exactly — and then the decode and the bilinear must be the CPU's own.
//   2. Do two tiles agree bit for bit on their shared edge? The shader places an edge vertex from GLOBAL
//      sample indices and evaluates the far column of one tile at fraction 1 and the near column of the
//      next at fraction 0; these tests run that evaluation from both sides of every seam of a 2x2 layout
//      at every tessellation level the pass can emit and compare the bits.
//
// The shader-side glue that is not in the .glslh — the texelFetch clamp and the patch bilerp order — is
// restated below as TesHeight; it mirrors LandscapeMain/TileHeightCm in Terrain.shader line for line.

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

    // One tile as the shader sees it: the texel grid, the tile's first global sample, the frame numbers.
    struct GpuTile
    {
        const LandscapeTileData* Tile   = nullptr;
        float                    FirstX = 0.0f;
        float                    FirstZ = 0.0f;
        float                    Quads  = 0.0f;
        float                    ZScale = 100.0f;
    };

    // Terrain.shader, TileHeightCm: clamp into the tile, fetch, recover the integer, decode.
    float TileHeightCm( const GpuTile& t, int x, int z )
    {
        const int   last  = static_cast<int>( t.Quads );
        const int   cx    = std::clamp( x, 0, last );
        const int   cz    = std::clamp( z, 0, last );
        const float unorm = UnormOf( t.Tile->Sample( static_cast<uint32_t>( cx ), static_cast<uint32_t>( cz ) ) );
        return LandscapeHeightCmFromSample( LandscapeSampleFromUnorm( unorm ), t.ZScale );
    }

    // Terrain.shader, LandscapeMain: the height at global sample coordinate (gx, gz).
    float TesHeight( const GpuTile& t, float gx, float gz )
    {
        const float samples = t.Quads + 1.0f;
        const float lx      = gx - t.FirstX;
        const float lz      = gz - t.FirstZ;
        const float cellX   = LandscapeCellOf( lx, samples );
        const float cellZ   = LandscapeCellOf( lz, samples );
        const int   cx      = static_cast<int>( cellX );
        const int   cz      = static_cast<int>( cellZ );
        return LandscapeBilinear( TileHeightCm( t, cx, cz ), TileHeightCm( t, cx + 1, cz ),
                                  TileHeightCm( t, cx, cz + 1 ), TileHeightCm( t, cx + 1, cz + 1 ), lx - cellX,
                                  lz - cellZ );
    }

    // Terrain.shader, LandscapeMain's patch point: X from the u edge, Z from the v edge.
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
    const GpuTile gpu{ &tile, 0.0f, 0.0f, static_cast<float>( quads ), frame.ZScale };

    for ( uint32_t iz = 0; iz <= quads * 4u; ++iz )
        for ( uint32_t ix = 0; ix <= quads * 4u; ++ix )
        {
            const float gx  = static_cast<float>( ix ) * 0.25f;
            const float gz  = static_cast<float>( iz ) * 0.25f;
            const auto  cpu = SampleLandscapeHeight( tile, frame, gx, gz );
            ASSERT_TRUE( cpu.has_value() );
            ASSERT_EQ( Bits( TesHeight( gpu, gx, gz ) ), Bits( *cpu ) ) << "at (" << gx << ", " << gz << ")";
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
        return GpuTile{ &tiles[tz * 2u + tx], static_cast<float>( tx * quads ), static_cast<float>( tz * quads ),
                        static_cast<float>( quads ), 100.0f };
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
