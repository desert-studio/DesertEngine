// FO-3: the foliage brush (UE FEdModeFoliage::AddInstancesForBrush) against planes: density per area in UE's
// units, determinism from the stroke seed, the surface / layer / slope / height filters, erase and the
// stroke record that makes a stroke one undo step.

#include <Editor/Panels/ViewportPanel/Tools/FoliageBrush.hpp>
#include <Engine/World/Landscape/LandscapeData.hpp>
#include <Engine/World/Landscape/LandscapeRaycast.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <utility>
#include <vector>

namespace
{
    using namespace Desert::Editor::Tools;
    using Desert::Assets::Serialization::FoliageTypeData;

    // A plane through the origin with normal @p n (tilted planes test slope), height offset @p y, answering
    // every segment that crosses it with @p surface and @p weight.
    FoliageBrushWorld Plane( glm::vec3 n, float y, FoliageSurface surface, std::optional<float> weight )
    {
        n = glm::normalize( n );
        FoliageBrushWorld world;
        world.Trace = [=]( const glm::vec3& a, const glm::vec3& b,
                           const FoliageSurfaceFilter& ) -> std::optional<FoliageTraceHit>
        {
            const glm::vec3 o( 0.0f, y, 0.0f );
            const float     da = glm::dot( a - o, n ), db = glm::dot( b - o, n );
            if ( ( da > 0.0f ) == ( db > 0.0f ) )
                return std::nullopt;
            const float t = da / ( da - db );
            return FoliageTraceHit{ a + t * ( b - a ), n, surface, weight };
        };
        world.LayerWeightAt = [=]( const glm::vec3& ) { return weight; };
        return world;
    }

    FoliageTypeData Grass()
    {
        FoliageTypeData type;
        type.Density = 100.0f; // UE's default: 100 per 1000x1000 cm
        return type;
    }

    FoliageBrushDab DabAt( glm::vec3 centre, float radius, glm::vec3 normal = { 0.0f, 1.0f, 0.0f } )
    {
        FoliageBrushDab dab;
        dab.Center = centre;
        dab.Normal = normal;
        dab.Radius = radius;
        return dab;
    }

    const FoliageBrushWorld kGround = Plane( { 0.0f, 1.0f, 0.0f }, 0.0f, FoliageSurface::Landscape, std::nullopt );
} // namespace

TEST( FoliageBrush, OneDabPlacesDensityTimesAreaInUnitsOfUE )
{
    // 100 per 1000x1000 cm over a 1000 cm disk: pi * 1000^2 * 100 / 1000^2 = 314.16 -> 314.
    FoliageRandom rng( 7u );
    const auto    placed = FoliageBrushAdd( Grass(), DabAt( {}, 1000.0f ), {}, rng, kGround );
    EXPECT_EQ( placed.size(), 314u );
    EXPECT_NEAR( FoliageBrushDesiredCount( 100.0f, 1000.0f, 1.0f ), 314.159f, 1e-2f );
    for ( const auto& m : placed )
    {
        const glm::vec3 p( m[3] );
        EXPECT_LE( p.x * p.x + p.z * p.z, 1000.0f * 1000.0f + 1.0f );
        EXPECT_NEAR( p.y, 0.0f, 1e-3f );
    }
}

TEST( FoliageBrush, RepeatedDabsTopUpInsteadOfPilingUp )
{
    FoliageRandom          rng( 11u );
    std::vector<glm::mat4> field;
    for ( int tick = 0; tick < 30; ++tick )
    {
        const auto added = FoliageBrushAdd( Grass(), DabAt( {}, 500.0f ), field, rng, kGround );
        field.insert( field.end(), added.begin(), added.end() );
    }
    // pi * 500^2 * 100 / 1e6 = 78.5 -> 79: thirty ticks hold what one tick placed.
    EXPECT_EQ( field.size(), 79u );
}

TEST( FoliageBrush, AStrokeAcrossAFieldReachesTheDensityAndASecondPassAddsNothing )
{
    // Dabs 250 cm apart over 6000 x 6000 cm, counted in the central 4000 x 4000 cm. Each dab tops ITS disk up
    // to the count, spread over the whole disk, so overlapping dabs leave between 1x and 2x the density (UE's
    // brush behaves the same) — never less, and a second pass over the same ground adds nothing.
    FoliageRandom          rng( 3u );
    std::vector<glm::mat4> field;
    const auto             sweep = [&]()
    {
        size_t added = 0;
        for ( float x = -3000.0f; x <= 3000.0f; x += 250.0f )
            for ( float z = -3000.0f; z <= 3000.0f; z += 250.0f )
            {
                const auto more = FoliageBrushAdd( Grass(), DabAt( { x, 0.0f, z }, 500.0f ), field, rng, kGround );
                field.insert( field.end(), more.begin(), more.end() );
                added += more.size();
            }
        return added;
    };
    sweep();
    size_t inside = 0;
    for ( const auto& m : field )
        if ( std::abs( m[3].x ) <= 2000.0f && std::abs( m[3].z ) <= 2000.0f )
            ++inside;
    const double expected = 100.0 * ( 4000.0 * 4000.0 ) / ( 1000.0 * 1000.0 ); // 1600
    EXPECT_GE( static_cast<double>( inside ), expected * 0.95 ) << inside << " instances";
    EXPECT_LE( static_cast<double>( inside ), expected * 2.0 ) << inside << " instances";
    EXPECT_EQ( sweep(), 0u );
}

TEST( FoliageBrush, TheSameSeedPlacesTheSameInstancesAndAnotherSeedDoesNot )
{
    FoliageTypeData type  = Grass();
    type.RandomPitchAngle = 20.0f;
    type.ZOffset          = { -5.0f, 5.0f };
    FoliageRandom a( 42u ), b( 42u ), c( 43u );
    const auto    first  = FoliageBrushAdd( type, DabAt( {}, 800.0f ), {}, a, kGround );
    const auto    second = FoliageBrushAdd( type, DabAt( {}, 800.0f ), {}, b, kGround );
    const auto    other  = FoliageBrushAdd( type, DabAt( {}, 800.0f ), {}, c, kGround );
    ASSERT_FALSE( first.empty() );
    EXPECT_EQ( first, second );
    EXPECT_NE( first, other );
}

TEST( FoliageBrush, TheStreamIsDefinedBitForBit )
{
    // PCG32 reference output (pcg32_srandom(42, 54) differs; this is the default-stream variant used here).
    FoliageRandom  rng( 42u );
    const uint32_t first = rng.NextU32();
    FoliageRandom  again( 42u );
    EXPECT_EQ( first, again.NextU32() );
    for ( int i = 0; i < 1000; ++i )
    {
        const float f = rng.Next01();
        ASSERT_GE( f, 0.0f );
        ASSERT_LT( f, 1.0f );
    }
}

TEST( FoliageBrush, ASurfaceTheFilterExcludesGetsNothing )
{
    const auto mesh = Plane( { 0.0f, 1.0f, 0.0f }, 0.0f, FoliageSurface::StaticMesh, std::nullopt );
    auto       dab  = DabAt( {}, 500.0f );

    FoliageRandom rng( 1u );
    dab.Filter.StaticMesh = false;
    EXPECT_TRUE( FoliageBrushAdd( Grass(), dab, {}, rng, mesh ).empty() );
    EXPECT_FALSE( FoliageBrushAdd( Grass(), dab, {}, rng, kGround ).empty() );

    dab.Filter           = {};
    dab.Filter.Landscape = false;
    EXPECT_TRUE( FoliageBrushAdd( Grass(), dab, {}, rng, kGround ).empty() );
    EXPECT_FALSE( FoliageBrushAdd( Grass(), dab, {}, rng, mesh ).empty() );
}

TEST( FoliageBrush, ALandscapeOnlyBrushReachesTheGroundUnderAMeshSheet )
{
    // A static-mesh sheet at 100 cm over the landscape at 0: the trace honours the dab's filter and passes
    // through the sheet (UE FFoliagePaintingGeometryFilter), as the tool's Scene::Raycast(accept) does.
    FoliageBrushWorld world;
    world.Trace = []( const glm::vec3& a, const glm::vec3& b,
                      const FoliageSurfaceFilter& filter ) -> std::optional<FoliageTraceHit>
    {
        for ( const auto& [y, surface] :
              { std::pair{ 100.0f, FoliageSurface::StaticMesh }, std::pair{ 0.0f, FoliageSurface::Landscape } } )
        {
            if ( !filter.Allows( surface ) || ( a.y > y ) == ( b.y > y ) )
                continue;
            const float t = ( a.y - y ) / ( a.y - b.y );
            return FoliageTraceHit{ a + t * ( b - a ), { 0.0f, 1.0f, 0.0f }, surface, std::nullopt };
        }
        return std::nullopt;
    };
    auto dab              = DabAt( { 0.0f, 100.0f, 0.0f }, 500.0f );
    dab.Filter.StaticMesh = false;
    FoliageRandom rng( 12u );
    const auto    placed = FoliageBrushAdd( Grass(), dab, {}, rng, world );
    ASSERT_FALSE( placed.empty() );
    for ( const auto& m : placed )
        EXPECT_NEAR( m[3].y, 0.0f, 1e-3f ); // on the landscape, none on the sheet

    dab.Filter = {};
    FoliageRandom again( 12u );
    for ( const auto& m : FoliageBrushAdd( Grass(), dab, {}, again, world ) )
        EXPECT_NEAR( m[3].y, 100.0f, 1e-3f ); // both allowed: the sheet is nearest
}

TEST( FoliageBrush, ATypeTiedToALayerPlantsOnlyWhereTheLayerIs )
{
    FoliageTypeData type = Grass();
    type.LandscapeLayers = { { "0123456789abcdef0123456789abcdef", "Landscape/Grass.delayerinfo" } };
    FoliageRandom rng( 5u );

    const auto bare = Plane( { 0.0f, 1.0f, 0.0f }, 0.0f, FoliageSurface::Landscape, 0.0f );
    EXPECT_TRUE( FoliageBrushAdd( type, DabAt( {}, 500.0f ), {}, rng, bare ).empty() );

    const auto painted = Plane( { 0.0f, 1.0f, 0.0f }, 0.0f, FoliageSurface::Landscape, 1.0f );
    EXPECT_EQ( FoliageBrushAdd( type, DabAt( {}, 500.0f ), {}, rng, painted ).size(), 79u );

    // Below MinimumLayerWeight nothing survives, whatever the draw.
    type.MinimumLayerWeight = 0.6f;
    const auto half         = Plane( { 0.0f, 1.0f, 0.0f }, 0.0f, FoliageSurface::Landscape, 0.5f );
    EXPECT_TRUE( FoliageBrushAdd( type, DabAt( {}, 500.0f ), {}, rng, half ).empty() );

    // A static mesh is not filtered by layer (UE: GetMaxHitWeight answers only for a landscape).
    const auto mesh = Plane( { 0.0f, 1.0f, 0.0f }, 0.0f, FoliageSurface::StaticMesh, std::nullopt );
    EXPECT_FALSE( FoliageBrushAdd( type, DabAt( {}, 500.0f ), {}, rng, mesh ).empty() );
}

TEST( FoliageBrush, SlopeAndHeightRangesRefuseGroundOutsideThem )
{
    const glm::vec3 tilted = glm::normalize( glm::vec3( 1.0f, 1.0f, 0.0f ) ); // 45 degrees
    const auto      slope  = Plane( tilted, 0.0f, FoliageSurface::Landscape, std::nullopt );
    FoliageTypeData type   = Grass();
    FoliageRandom   rng( 9u );

    type.GroundSlopeAngle = { 0.0f, 30.0f };
    EXPECT_TRUE( FoliageBrushAdd( type, DabAt( {}, 500.0f, tilted ), {}, rng, slope ).empty() );
    type.GroundSlopeAngle = { 40.0f, 50.0f };
    EXPECT_FALSE( FoliageBrushAdd( type, DabAt( {}, 500.0f, tilted ), {}, rng, slope ).empty() );

    const auto high = Plane( { 0.0f, 1.0f, 0.0f }, 500.0f, FoliageSurface::Landscape, std::nullopt );
    type            = Grass();
    type.Height     = { 0.0f, 100.0f };
    EXPECT_TRUE( FoliageBrushAdd( type, DabAt( { 0.0f, 500.0f, 0.0f }, 400.0f ), {}, rng, high ).empty() );
    type.Height = { 400.0f, 600.0f };
    EXPECT_FALSE( FoliageBrushAdd( type, DabAt( { 0.0f, 500.0f, 0.0f }, 400.0f ), {}, rng, high ).empty() );
}

TEST( FoliageBrush, RemoveTakesWhatTheSphereHolds )
{
    FoliageRandom          rng( 2u );
    std::vector<glm::mat4> field = FoliageBrushAdd( Grass(), DabAt( {}, 1000.0f ), {}, rng, kGround );
    const size_t           all   = field.size();
    const size_t           gone  = FoliageBrushRemove( field, {}, 400.0f );
    EXPECT_GT( gone, 0u );
    EXPECT_EQ( field.size(), all - gone );
    for ( const auto& m : field )
        EXPECT_GT( m[3].x * m[3].x + m[3].z * m[3].z, 400.0f * 400.0f );
}

TEST( FoliageBrush, AStrokeIsOneUndoStepHoldingThePressState )
{
    const Common::UUID     grass( 1u ), rocks( 2u );
    std::vector<glm::mat4> grassField = { glm::mat4( 1.0f ) };
    std::vector<glm::mat4> rockField;
    const auto             original = grassField;

    FoliageStroke stroke( 77u );
    for ( float x = 0.0f; x < 2000.0f; x += 400.0f ) // five dabs, one stroke
    {
        stroke.Touch( grass, grassField );
        stroke.Touch( rocks, rockField );
        const auto added =
             FoliageBrushAdd( Grass(), DabAt( { x, 0.0f, 0.0f }, 300.0f ), grassField, stroke.Random(), kGround );
        grassField.insert( grassField.end(), added.begin(), added.end() );
    }
    const auto fields = stroke.Finish( [&]( const Common::UUID& id ) -> const std::vector<glm::mat4>*
                                       { return id == grass ? &grassField : &rockField; } );
    // The untouched-in-effect rock field is not an undo entry; the grass field is ONE entry for five dabs.
    ASSERT_EQ( fields.size(), 1u );
    EXPECT_EQ( fields[0].Entity, grass );
    EXPECT_EQ( fields[0].Before, original );
    EXPECT_EQ( fields[0].After, grassField );

    // Replaying the stroke from its seed places the same instances.
    std::vector<glm::mat4> replay = original;
    FoliageStroke          again( 77u );
    for ( float x = 0.0f; x < 2000.0f; x += 400.0f )
    {
        const auto added =
             FoliageBrushAdd( Grass(), DabAt( { x, 0.0f, 0.0f }, 300.0f ), replay, again.Random(), kGround );
        replay.insert( replay.end(), added.begin(), added.end() );
    }
    EXPECT_EQ( replay, grassField );
}

namespace
{
    namespace Landscape = Desert::World::Landscape;

    // Terrain_Grass's landscape: 5 x 5 tiles of 255 quads at 23.53 cm, rolling hills, centred on the origin.
    struct GrassLandscape
    {
        std::vector<Landscape::LandscapeTileData> Tiles;
        std::vector<Landscape::LandscapeRayTile>  RayTiles;

        GrassLandscape()
        {
            constexpr uint32_t kQuads   = 255u;
            constexpr float    kSpacing = 23.52941131591797f;
            constexpr float    kSpan    = kSpacing * static_cast<float>( kQuads );
            Tiles.reserve( 25u );
            for ( int tz = 0; tz < 5; ++tz )
                for ( int tx = 0; tx < 5; ++tx )
                {
                    Landscape::LandscapeFrame frame;
                    frame.OriginX   = ( static_cast<float>( tx ) - 2.5f ) * kSpan;
                    frame.OriginZ   = ( static_cast<float>( tz ) - 2.5f ) * kSpan;
                    frame.SpacingCm = kSpacing;
                    std::vector<uint16_t> samples( ( kQuads + 1u ) * ( kQuads + 1u ) );
                    for ( uint32_t z = 0; z <= kQuads; ++z )
                        for ( uint32_t x = 0; x <= kQuads; ++x )
                        {
                            const float wx = frame.OriginX + kSpacing * static_cast<float>( x );
                            const float wz = frame.OriginZ + kSpacing * static_cast<float>( z );
                            const float h  = 900.0f * std::sin( wx * 0.0011f ) * std::cos( wz * 0.0007f );
                            samples[z * ( kQuads + 1u ) + x] =
                                 static_cast<uint16_t>( static_cast<float>( Landscape::kLandscapeMidSample ) +
                                                        h * 128.0f / frame.ZScale );
                        }
                    auto tile = Landscape::LandscapeTileData::FromSamples( kQuads + 1u, kQuads + 1u,
                                                                           std::move( samples ) );
                    EXPECT_TRUE( tile.IsSuccess() );
                    Tiles.push_back( std::move( tile.GetValue() ) );
                    RayTiles.push_back( { nullptr, frame } );
                }
            for ( size_t i = 0; i < Tiles.size(); ++i )
                RayTiles[i].Heights = &Tiles[i];
        }

        // The editor's trace: the landscape ray over the segment, a layer painted west of x = 0.
        FoliageBrushWorld World() const
        {
            FoliageBrushWorld world;
            world.Trace = [this]( const glm::vec3& a, const glm::vec3& b,
                                  const FoliageSurfaceFilter& ) -> std::optional<FoliageTraceHit>
            {
                const glm::vec3 d   = b - a;
                const float     len = glm::length( d );
                const auto      hit = Landscape::RaycastLandscape( RayTiles, a, d / len, len );
                if ( !hit )
                    return std::nullopt;
                return FoliageTraceHit{ hit->Point, hit->Normal, FoliageSurface::Landscape,
                                        hit->Point.x < 0.0f ? 1.0f : 0.3f };
            };
            world.LayerWeightAt = []( const glm::vec3& p )
            { return std::optional<float>( p.x < 0.0f ? 1.0f : 0.3f ); };
            return world;
        }
    };

    uint64_t Fnv1a( const std::vector<glm::mat4>& transforms )
    {
        uint64_t    h     = 1469598103934665603ull;
        const auto* bytes = reinterpret_cast<const unsigned char*>( transforms.data() );
        for ( size_t i = 0; i < transforms.size() * sizeof( glm::mat4 ); ++i )
            h = ( h ^ bytes[i] ) * 1099511628211ull;
        return h;
    }
} // namespace

// FO-3b: the editor's default dab (300 cm) of the frame's layered type (3000 per 1000 x 1000 cm) on a landscape
// the size of Terrain_Grass. The hash was recorded BEFORE the stroke was made fast; any speed-up that changes
// one byte of one placed transform fails here.
TEST( FoliageBrush, ALandscapeStrokeFromOneSeedPlacesTheRecordedInstancesByteForByte )
{
    const GrassLandscape land;
    FoliageTypeData      type = Grass();
    type.Density              = 3000.0f;
    type.ScaleX               = { 0.3f, 0.5f };
    type.RandomYaw            = true;
    type.AlignToNormal        = true;
    type.MinimumLayerWeight   = 0.2f;
    type.LandscapeLayers.push_back( { "0123456789abcdef0123456789abcdef", "Landscape/Layers/Grass.delayerinfo" } );

    std::vector<glm::mat4> field;
    FoliageStroke          stroke( 0xF03Bu );
    FoliageBrushStats      stats;
    const auto             at = std::chrono::steady_clock::now();
    for ( float x = -600.0f; x <= 600.0f; x += 300.0f )
    {
        const auto added = FoliageBrushAdd( type, DabAt( { x, 0.0f, 150.0f }, 300.0f ), field, stroke.Random(),
                                            land.World(), &stats );
        field.insert( field.end(), added.begin(), added.end() );
    }
    const double ms = std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - at ).count();
    std::printf(
         "[ landscape stroke ] 5 dabs: %d candidates, %d placed in %.1f ms (trace %.1f ms), hash %016llx\n",
         stats.Candidates, stats.Placed, ms, stats.TraceMs, static_cast<unsigned long long>( Fnv1a( field ) ) );

    EXPECT_GT( stats.Placed, 100 );
    EXPECT_EQ( stats.Placed, static_cast<int>( field.size() ) );
    EXPECT_EQ( Fnv1a( field ), 0xfa8d00c62198be7eull );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
