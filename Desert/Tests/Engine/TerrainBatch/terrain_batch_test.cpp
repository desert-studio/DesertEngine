// The terrain batching contract (Engine/Graphic/Systems/Scene/Terrain/TerrainBatch.hpp), tested as the
// RELATION it is: N terrains with different per-draw data must end up in N different rows / materials,
// and terrains that are the same must SHARE — both directions, because the defect this guards against
// ("first Apply of the frame wins, every later terrain silently draws with somebody else's resources")
// was invisible from either side alone. The census of what is per-draw versus per-material lives in the
// ShaderCacheKey suite, which reflects the real Terrain.shader SPIR-V; this suite drives the pure C++
// half that decides which terrain gets which material and which row.

#include <gtest/gtest.h>

#include <cmath>
#include <optional>

#include <Engine/Graphic/Systems/Scene/Terrain/TerrainBatch.hpp>

using Desert::Graphic::MaterialOverrides;
using Desert::Graphic::System::LandscapeLodFromScreenSize;
using Desert::Graphic::System::LandscapeLodSettings;
using Desert::Graphic::System::LandscapeLodVertexCount;
using Desert::Graphic::System::LandscapeMaxLod;
using Desert::Graphic::System::LandscapeScreenRadiusSquared;
using Desert::Graphic::System::LandscapeTileLod;
using Desert::Graphic::System::LandscapeTileLods;
using Desert::Graphic::System::MakeLandscapeLodSettings;
using Desert::Graphic::System::TerrainInstance;
using Desert::Graphic::System::TerrainTextureKey;

namespace
{
    MaterialOverrides WithTextures( std::vector<std::pair<std::string, uint64_t>> textures )
    {
        MaterialOverrides overrides;
        overrides.Textures = std::move( textures );
        return overrides;
    }
} // namespace

// ---- The key separates what must not share ---------------------------------------------------------

TEST( TerrainTextureKey, DifferentTexturesGetDifferentMaterials )
{
    const auto checker = TerrainTextureKey( WithTextures( { { "u_RockTex", 42 } } ), nullptr );
    const auto plain   = TerrainTextureKey( WithTextures( {} ), nullptr );
    const auto other   = TerrainTextureKey( WithTextures( { { "u_RockTex", 43 } } ), nullptr );

    EXPECT_NE( checker, plain ) << "a terrain with a texture override shared the textureless material";
    EXPECT_NE( checker, other ) << "two different textures in one sampler collapsed into one material";
}

TEST( TerrainTextureKey, TheSamplerNameIsPartOfTheIdentity )
{
    // The same handle in a different slot is a different picture on screen: grass everywhere versus
    // rock everywhere. A key of handles alone would batch them together.
    const auto rock  = TerrainTextureKey( WithTextures( { { "u_RockTex", 42 } } ), nullptr );
    const auto grass = TerrainTextureKey( WithTextures( { { "u_GrassTex", 42 } } ), nullptr );
    EXPECT_NE( rock, grass );
}

// ---- ...and only what must not share ---------------------------------------------------------------

TEST( TerrainTextureKey, TheSameTexturesInAnotherOrderShareOneMaterial )
{
    // Two terrains naming the same set in a different order are the same texture set. Failing this
    // direction is quieter than the other — it only allocates a redundant material — but it is the
    // exact drift MeshRenderer's GenericTextureKey sorts against, and the two keys follow one rule.
    const auto ab = TerrainTextureKey( WithTextures( { { "u_GrassTex", 7 }, { "u_RockTex", 9 } } ), nullptr );
    const auto ba = TerrainTextureKey( WithTextures( { { "u_RockTex", 9 }, { "u_GrassTex", 7 } } ), nullptr );
    EXPECT_EQ( ab, ba );
}

TEST( TerrainTextureKey, AnUnsetSlotIsNoSlot )
{
    // Handle 0 means "keep the fallback", which every terrain shares; it must not split the batch.
    const auto explicitZero = TerrainTextureKey( WithTextures( { { "u_RockTex", 0 } } ), nullptr );
    const auto absent       = TerrainTextureKey( WithTextures( {} ), nullptr );
    EXPECT_EQ( explicitZero, absent );
}

// ---- The instance row layout is the shader's -------------------------------------------------------

TEST( TerrainInstanceRow, TheRowIsSevenSixteenByteSlots )
{
    // The GLSL `TerrainInstance` in Common/TerrainInstance.glslh is the second statement of this layout; the
    // static_asserts in TerrainBatch.hpp hold the offsets and this holds the stride the GPU indexes by.
    // The SPIR-V side of the same relation is asserted in the ShaderCacheKey suite.
    EXPECT_EQ( sizeof( TerrainInstance ), 112u );
}

// ── Landscape tiles: the LOD (UE's FLandscapeRenderSystem, ported) ──────────────────────────────────
//
// UE's numbers, not ours: with its default LOD0 screen size 0.5 and distributions 1.25 / 3, LOD 0 holds
// down to a screen size of 0.5, LOD 1 is reached at 0.4, and every further LOD at a third of the squared
// size. The settings for a 127-quad section end at LOD 6 (stride 64, two cells).
TEST( LandscapeLod, UEScreenSizeRatiosAndTheLastLod )
{
    EXPECT_EQ( LandscapeMaxLod( 127u ), 6u );
    EXPECT_EQ( LandscapeMaxLod( 63u ), 5u );
    EXPECT_EQ( LandscapeMaxLod( 7u ), 2u );
    const LandscapeLodSettings s = MakeLandscapeLodSettings( 127u );
    EXPECT_FLOAT_EQ( s.LOD0ScreenSizeSquared, 0.25f );
    EXPECT_FLOAT_EQ( s.LOD1ScreenSizeSquared, 0.16f );
    EXPECT_FLOAT_EQ( s.LODOnePlusDistributionScalarSquared, 9.0f );
    EXPECT_FLOAT_EQ( s.LastLODScreenSizeSquared, 0.16f / std::pow( 9.0f, 5.0f ) );
    EXPECT_FLOAT_EQ( s.LastLODIndex, 6.0f );

    EXPECT_FLOAT_EQ( LandscapeLodFromScreenSize( s, 1.0f ), 0.0f );
    EXPECT_FLOAT_EQ( LandscapeLodFromScreenSize( s, 0.25f ), 0.0f );
    EXPECT_FLOAT_EQ( LandscapeLodFromScreenSize( s, 0.16f ), 1.0f );
    EXPECT_NEAR( LandscapeLodFromScreenSize( s, 0.16f / 9.0f ), 2.0f, 1e-5f );
    EXPECT_FLOAT_EQ( LandscapeLodFromScreenSize( s, 1e-12f ), 6.0f );
    // Continuous and monotonic: farther is never finer.
    float previous = 0.0f;
    for ( float d = 100.0f; d < 1.0e7f; d *= 1.02f )
    {
        const float lod = LandscapeLodFromScreenSize(
             s, LandscapeScreenRadiusSquared( glm::vec3( 0.0f ), 9000.0f, glm::vec3( 0.0f, 0.0f, d ), glm::mat4( 1.3f ) ) );
        ASSERT_GE( lod, previous ) << d;
        ASSERT_LE( lod, previous + 0.25f ) << "a jump, not a blend, at " << d;
        previous = lod;
    }
    EXPECT_FLOAT_EQ( previous, 6.0f );
}

// A border's LOD is one number for both tiles of it, and a corner's for all four — the relation the seams
// rest on (LandscapeLod.glslh). Checked on a 3 x 3 block with every LOD different.
TEST( LandscapeLod, BordersAndCornersAreOneValueForEveryTileSharingThem )
{
    const float lods[3][3] = { { 0.2f, 1.7f, 3.1f }, { 2.4f, 0.0f, 5.9f }, { 1.1f, 4.4f, 0.6f } };
    const auto  tile       = [&]( int x, int z )
    {
        return LandscapeTileLods( lods[z][x],
                                  [&]( int dx, int dz ) -> std::optional<float>
                                  {
                                      const int nx = x + dx, nz = z + dz;
                                      if ( nx < 0 || nz < 0 || nx > 2 || nz > 2 )
                                          return std::nullopt;
                                      return lods[nz][nx];
                                  } );
    };
    for ( int z = 0; z < 3; ++z )
        for ( int x = 0; x < 3; ++x )
        {
            const LandscapeTileLod t = tile( x, z );
            EXPECT_GE( t.Edges.x, t.Center );
            if ( x < 2 )
                EXPECT_EQ( t.Edges.y, tile( x + 1, z ).Edges.x ) << x << "," << z;
            if ( z < 2 )
                EXPECT_EQ( t.Edges.w, tile( x, z + 1 ).Edges.z ) << x << "," << z;
            if ( x < 2 && z < 2 )
            {
                const float c = t.Corners.w;
                EXPECT_EQ( c, tile( x + 1, z ).Corners.z );
                EXPECT_EQ( c, tile( x, z + 1 ).Corners.y );
                EXPECT_EQ( c, tile( x + 1, z + 1 ).Corners.x );
            }
        }
    EXPECT_EQ( tile( 1, 1 ).Corners.w, 5.9f ) << "the max of the four";
}

TEST( LandscapeLod, EachGridLodDrawsSixVerticesPerCell )
{
    EXPECT_EQ( LandscapeLodVertexCount( 127u, 0u ), 127u * 127u * 6u );
    EXPECT_EQ( LandscapeLodVertexCount( 127u, 1u ), 64u * 64u * 6u );
    EXPECT_EQ( LandscapeLodVertexCount( 127u, 6u ), 2u * 2u * 6u );
    EXPECT_EQ( LandscapeLodVertexCount( 63u, 5u ), 2u * 2u * 6u );
}

// Each landscape tile binds its own heightmap, so two tiles must never share a material — which is what
// the key decides.
TEST( TerrainTextureKey, EveryHeightmapIsItsOwnMaterial )
{
    int        a     = 0;
    int        b     = 0;
    const auto withA = TerrainTextureKey( WithTextures( {} ), &a );
    const auto withB = TerrainTextureKey( WithTextures( {} ), &b );
    EXPECT_NE( withA, withB );
    EXPECT_NE( withA, TerrainTextureKey( WithTextures( {} ), nullptr ) );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
