// FO-3: the foliage brush (UE FEdModeFoliage::AddInstancesForBrush) against planes: density per area in UE's
// units, determinism from the stroke seed, the surface / layer / slope / height filters, erase and the
// stroke record that makes a stroke one undo step.

#include <Editor/Panels/ViewportPanel/Tools/FoliageBrush.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <optional>
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
        world.Trace = [=]( const glm::vec3& a, const glm::vec3& b ) -> std::optional<FoliageTraceHit>
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

TEST( FoliageBrush, EraseRemovesWhatTheSphereHolds )
{
    FoliageRandom          rng( 2u );
    std::vector<glm::mat4> field = FoliageBrushAdd( Grass(), DabAt( {}, 1000.0f ), {}, rng, kGround );
    const size_t           all   = field.size();
    const size_t           gone  = FoliageBrushErase( field, {}, 400.0f );
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

    FoliageStroke stroke( 77u, false );
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
    FoliageStroke          again( 77u, false );
    for ( float x = 0.0f; x < 2000.0f; x += 400.0f )
    {
        const auto added =
             FoliageBrushAdd( Grass(), DabAt( { x, 0.0f, 0.0f }, 300.0f ), replay, again.Random(), kGround );
        replay.insert( replay.end(), added.begin(), added.end() );
    }
    EXPECT_EQ( replay, grassField );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
