// S1: the procedural foliage simulation, ported from UE's ProceduralFoliage{Tile,Broadphase,Instance,Spawner}.
// A seed grows the same forest every time and another seed another one; in an overlap the stronger plant takes the
// room; tiles stitched with an overlap place every plant once, with no pair overlapping across a seam and no bare
// strip along it. S1a: no two survivors closer than their grown collision radii, shade kills only the intolerant,
// one year per generation with the scale following the age, and a seam independent of the layout around it.

#include <Engine/World/Foliage/Procedural/ProceduralFoliageSpawner.hpp>
#include <Engine/World/Foliage/Procedural/ProceduralFoliageVolume.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <tuple>
#include <vector>

namespace
{
    using namespace Desert::World::Foliage::Procedural;
    using Desert::Assets::Serialization::FoliageTypeData;

    FoliageTypeData Type( float collision, float shade, float priority, float density )
    {
        FoliageTypeData type;
        type.Procedural.CollisionRadius       = collision;
        type.Procedural.ShadeRadius           = shade;
        type.Procedural.OverlapPriority       = priority;
        type.Procedural.InitialSeedDensity    = density;
        type.Procedural.ProceduralScale       = { 1.0f, 1.0f };
        type.Procedural.AverageSpreadDistance = 40.0f;
        type.Procedural.SpreadVariance        = 60.0f;
        return type;
    }

    ProceduralFoliageSpawnerSettings Settings( int32_t seed, int32_t uniqueTiles, float tileSize )
    {
        ProceduralFoliageSpawnerSettings settings;
        settings.RandomSeed     = seed;
        settings.NumUniqueTiles = uniqueTiles;
        settings.TileSize       = tileSize;
        return settings;
    }

    float Distance( glm::vec2 a, glm::vec2 b )
    {
        return std::hypot( a.x - b.x, a.y - b.y );
    }
} // namespace

TEST( ProceduralFoliage, TheRandomStreamDrawsFractionsInTheUnitInterval )
{
    Common::Math::RandomStream stream( 42 );
    Common::Math::RandomStream again( 42 );
    for ( int i = 0; i < 1000; ++i )
    {
        const float f = stream.FRand();
        EXPECT_GE( f, 0.0f );
        EXPECT_LT( f, 1.0f );
        EXPECT_EQ( f, again.FRand() );
    }
}

TEST( ProceduralFoliage, TheSameSeedGrowsTheSameTilesAndAnotherSeedOthers )
{
    const std::vector<FoliageTypeData> types{ Type( 60.0f, 60.0f, 0.0f, 2.0f ), Type( 25.0f, 25.0f, 0.0f, 3.0f ) };
    ProceduralFoliageSpawner           a( Settings( 7, 2, 3000.0f ), types );
    ProceduralFoliageSpawner           b( Settings( 7, 2, 3000.0f ), types );
    ProceduralFoliageSpawner           c( Settings( 8, 2, 3000.0f ), types );
    ASSERT_TRUE( a.Validate() );
    a.Simulate();
    b.Simulate();
    c.Simulate();

    const auto first = a.GetRandomTile( 0, 0 )->PlacedInstances();
    ASSERT_GT( first.size(), 20u ) << "the tile must grow a forest for the comparison to mean anything";
    EXPECT_EQ( first, b.GetRandomTile( 0, 0 )->PlacedInstances() );
    EXPECT_NE( first, c.GetRandomTile( 0, 0 )->PlacedInstances() ) << "RandomSeed must reach the simulation";

    const ProceduralFoliageTileLayout layout{ 0, 0, 2, 2 };
    EXPECT_EQ( GenerateProceduralContent( a, layout, {}, 200.0f ),
               GenerateProceduralContent( b, layout, {}, 200.0f ) );
}

TEST( ProceduralFoliage, InAnOverlapTheHigherPriorityThenTheOlderThenTheLargerSurvives )
{
    const auto                weak   = Type( 50.0f, 50.0f, 0.0f, 1.0f ).Procedural;
    const auto                strong = Type( 50.0f, 50.0f, 1.0f, 1.0f ).Procedural;
    ProceduralFoliageInstance a;
    ProceduralFoliageInstance b;
    EXPECT_EQ( Dominated( a, weak, b, strong, OverlapKind::Collision ), &a );
    EXPECT_EQ( Dominated( a, strong, b, weak, OverlapKind::Collision ), &b );
    b.Age = 2.0f;
    EXPECT_EQ( Dominated( a, weak, b, weak, OverlapKind::Collision ), &a ) << "the older wins at equal priority";
    b.Age   = 0.0f;
    a.Scale = 2.0f;
    EXPECT_EQ( Dominated( a, weak, b, weak, OverlapKind::Collision ), &b ) << "the larger wins at equal age";
    a.Blocker = true;
    EXPECT_EQ( Dominated( a, weak, b, strong, OverlapKind::Collision ), &b ) << "a blocker always survives";
    a.Blocker = false;

    auto shadeTolerant           = weak;
    shadeTolerant.CanGrowInShade = true;
    EXPECT_EQ( Dominated( a, strong, b, shadeTolerant, OverlapKind::Shade ), nullptr );
    EXPECT_EQ( Dominated( a, strong, b, shadeTolerant, OverlapKind::Collision ), &b );
}

TEST( ProceduralFoliage, TheLargeTypeCrowdsTheSmallOneOut )
{
    // Trees (priority 1, 150 cm) and grass (priority 0, 20 cm): no grass survives inside a tree's circle.
    const std::vector<FoliageTypeData> types{ Type( 150.0f, 150.0f, 1.0f, 2.0f ),
                                              Type( 20.0f, 20.0f, 0.0f, 6.0f ) };
    ProceduralFoliageSpawner           spawner( Settings( 3, 1, 4000.0f ), types );
    spawner.Simulate();
    const auto placed = spawner.GetRandomTile( 0, 0 )->PlacedInstances();

    size_t trees = 0;
    size_t grass = 0;
    for ( const auto& tree : placed )
    {
        if ( tree.TypeIndex != 0 )
        {
            ++grass;
            continue;
        }
        ++trees;
        for ( const auto& other : placed )
            if ( other.TypeIndex == 1 )
                EXPECT_GT( Distance( tree.Location, other.Location ), 150.0f + 20.0f )
                     << "grass at (" << other.Location.x << ", " << other.Location.y << ") survived inside a tree";
    }
    EXPECT_GT( trees, 10u );
    EXPECT_GT( grass, 10u );
}

TEST( ProceduralFoliage, StitchedTilesPlaceEveryPlantOnceWithNoOverlapOrBareStripAtTheSeams )
{
    constexpr float                    kSize    = 3000.0f;
    constexpr float                    kRadius  = 50.0f;
    constexpr float                    kOverlap = 2.0f * kRadius * 2.0f; // twice the largest radius, and some
    const std::vector<FoliageTypeData> types{ Type( kRadius, kRadius, 0.0f, 3.0f ) };
    ProceduralFoliageSpawner           spawner( Settings( 11, 3, kSize ), types );
    spawner.Simulate();
    const auto placed =
         GenerateProceduralContent( spawner, ProceduralFoliageTileLayout{ 0, 0, 3, 3 }, {}, kOverlap );
    ASSERT_GT( placed.size(), 200u );

    // No duplicate and no overlap, anywhere: a seam that placed a plant twice or let two neighbours' plants
    // both stand would put two centres closer than two radii.
    for ( size_t i = 0; i < placed.size(); ++i )
        for ( size_t j = i + 1; j < placed.size(); ++j )
            ASSERT_GT( Distance( placed[i].Location, placed[j].Location ), 2.0f * kRadius )
                 << "(" << placed[i].Location.x << ", " << placed[i].Location.y << ") and ("
                 << placed[j].Location.x << ", " << placed[j].Location.y << ")";

    // No bare strip: a band across the seam between the first two columns holds about as many plants as a band
    // of the same size inside a tile.
    const auto countIn = [&]( float x0, float x1 )
    {
        size_t n = 0;
        for ( const auto& p : placed )
            if ( p.Location.x >= x0 && p.Location.x < x1 && p.Location.y >= 500.0f && p.Location.y < 2500.0f )
                ++n;
        return n;
    };
    const size_t seam     = countIn( kSize - 200.0f, kSize + kOverlap + 200.0f );
    const size_t interior = countIn( 1000.0f, 1000.0f + kOverlap + 400.0f );
    ASSERT_GT( interior, 10u );
    EXPECT_GE( static_cast<float>( seam ), 0.6f * static_cast<float>( interior ) )
         << "seam " << seam << " against interior " << interior;
}

// S1a: the rules each surviving plant obeys, checked over a whole grown tile rather than one pair.
namespace
{
    bool LessByLocation( const ProceduralFoliagePlacement& a, const ProceduralFoliagePlacement& b )
    {
        return std::tie( a.Location.x, a.Location.y, a.TypeIndex ) <
               std::tie( b.Location.x, b.Location.y, b.TypeIndex );
    }

    /// The placements whose X lies in [@p x0, @p x1), ordered by location.
    std::vector<ProceduralFoliagePlacement> Band( const std::vector<ProceduralFoliagePlacement>& placed, float x0,
                                                  float x1 )
    {
        std::vector<ProceduralFoliagePlacement> band;
        for ( const auto& p : placed )
            if ( p.Location.x >= x0 && p.Location.x < x1 )
                band.push_back( p );
        std::sort( band.begin(), band.end(), LessByLocation );
        return band;
    }
} // namespace

TEST( ProceduralFoliage, NoTwoSurvivorsStandCloserThanTheirGrownCollisionRadii )
{
    // Two types growing through a scale range, so the radii differ per instance (UE: CollisionRadius * Scale).
    auto large                       = Type( 60.0f, 60.0f, 1.0f, 2.0f );
    large.Procedural.ProceduralScale = { 1.0f, 2.0f };
    large.Procedural.MaxAge          = 3.0f;
    auto small                       = Type( 15.0f, 15.0f, 0.0f, 5.0f );
    small.Procedural.ProceduralScale = { 0.5f, 1.5f };
    small.Procedural.MaxAge          = 2.0f;
    const std::vector<FoliageTypeData> types{ large, small };
    ProceduralFoliageSpawner           spawner( Settings( 21, 1, 4000.0f ), types );
    spawner.Simulate();
    const auto placed = spawner.GetRandomTile( 0, 0 )->PlacedInstances();
    ASSERT_GT( placed.size(), 50u );

    for ( size_t i = 0; i < placed.size(); ++i )
        for ( size_t j = i + 1; j < placed.size(); ++j )
        {
            const float ri = RadiiOf( placed[i], types[placed[i].TypeIndex].Procedural ).Collision;
            const float rj = RadiiOf( placed[j], types[placed[j].TypeIndex].Procedural ).Collision;
            ASSERT_GE( Distance( placed[i].Location, placed[j].Location ), ri + rj - 0.01f )
                 << "(" << placed[i].Location.x << ", " << placed[i].Location.y << ") radius " << ri << " and ("
                 << placed[j].Location.x << ", " << placed[j].Location.y << ") radius " << rj;
        }
}

TEST( ProceduralFoliage, ShadeKillsATypeThatCannotGrowInShadeAndSparesOneThatCan )
{
    // Trees with a small trunk (collision 30) and a wide crown (shade 200) over ground cover (10 / 10).
    const auto tree  = Type( 30.0f, 200.0f, 1.0f, 2.0f );
    auto       cover = Type( 10.0f, 10.0f, 0.0f, 6.0f );

    const auto coverInShade = [&]( bool canGrowInShade, size_t& coverCount )
    {
        cover.Procedural.CanGrowInShade = canGrowInShade;
        ProceduralFoliageSpawner spawner( Settings( 9, 1, 4000.0f ), { tree, cover } );
        spawner.Simulate();
        const auto placed = spawner.GetRandomTile( 0, 0 )->PlacedInstances();
        size_t     shaded = 0;
        coverCount        = 0;
        for ( const auto& c : placed )
        {
            if ( c.TypeIndex != 1 )
                continue;
            ++coverCount;
            for ( const auto& t : placed )
                if ( t.TypeIndex == 0 && Distance( t.Location, c.Location ) < 200.0f + 10.0f )
                {
                    ++shaded;
                    break;
                }
        }
        return shaded;
    };

    size_t intolerant = 0;
    EXPECT_EQ( coverInShade( false, intolerant ), 0u ) << "cover that cannot grow in shade survived under a crown";
    EXPECT_GT( intolerant, 10u );
    size_t tolerant = 0;
    EXPECT_GT( coverInShade( true, tolerant ), 10u ) << "shade-tolerant cover must survive under the crowns";
}

TEST( ProceduralFoliage, AnInstanceAgesOneGenerationPerStepUpToMaxAgeAndItsScaleFollowsItsAge )
{
    auto type                       = Type( 25.0f, 25.0f, 0.0f, 3.0f );
    type.Procedural.ProceduralScale = { 1.0f, 3.0f };
    type.Procedural.MaxAge          = 3.0f;
    type.Procedural.NumSteps        = 5;
    const auto& procedural          = type.Procedural;

    // UE GetScaleForAge / AgeSeeds: never shrinking with age, one year per generation, capped at MaxAge.
    for ( float age = 0.0f; age < 5.0f; age += 0.5f )
        EXPECT_LE( ScaleForAge( procedural, age ), ScaleForAge( procedural, age + 0.5f ) );
    EXPECT_EQ( NextAge( procedural, 0.0f, 1 ), 1.0f );
    EXPECT_EQ( NextAge( procedural, 2.0f, 5 ), 3.0f );
    EXPECT_EQ( ScaleForAge( procedural, 3.0f ), 3.0f );

    ProceduralFoliageSpawner younger( Settings( 13, 1, 3000.0f ), { type } );
    ProceduralFoliageSpawner older( Settings( 13, 1, 3000.0f ), { type } );
    younger.Simulate( 2 );
    older.Simulate( 3 );
    const auto before = younger.GetRandomTile( 0, 0 )->PlacedInstances();
    const auto after  = older.GetRandomTile( 0, 0 )->PlacedInstances();
    ASSERT_GT( before.size(), 20u );

    for ( const auto& instance : after )
    {
        EXPECT_LE( instance.Age, procedural.MaxAge );
        EXPECT_EQ( instance.Scale, ScaleForAge( procedural, instance.Age ) );
    }
    // A plant that lives through the extra generation stands where it stood, a year older and no smaller.
    size_t survivors = 0;
    for ( const auto& b : before )
        for ( const auto& a : after )
            if ( a.Location == b.Location && a.TypeIndex == b.TypeIndex )
            {
                ++survivors;
                EXPECT_EQ( a.Age, NextAge( procedural, b.Age, 1 ) );
                EXPECT_GE( a.Scale, b.Scale );
            }
    EXPECT_GT( survivors, 10u );
}

TEST( ProceduralFoliage, ASeamPlacesTheSamePlantsWhicheverLayoutAroundItIsGenerated )
{
    // The seam between tiles 0 and 1 is composed from those two tiles alone: a layout that also holds tile -1 (to
    // the left) or tile 2 (to the right) places the same plants along it, so regenerating a neighbour never moves
    // a plant at the border.
    constexpr float                    kSize    = 3000.0f;
    constexpr float                    kOverlap = 200.0f;
    const std::vector<FoliageTypeData> types{ Type( 50.0f, 50.0f, 0.0f, 3.0f ), Type( 20.0f, 20.0f, 0.0f, 4.0f ) };
    ProceduralFoliageSpawner           spawner( Settings( 17, 4, kSize ), types );
    spawner.Simulate();

    const auto pair =
         GenerateProceduralContent( spawner, ProceduralFoliageTileLayout{ 0, 0, 2, 1 }, {}, kOverlap );
    const auto wider     = GenerateProceduralContent( spawner, ProceduralFoliageTileLayout{ -1, 0, 4, 1 },
                                                      { -kSize, 0.0f }, kOverlap );
    const auto seamPair  = Band( pair, kSize - 2.0f * kOverlap, kSize + 2.0f * kOverlap );
    const auto seamWider = Band( wider, kSize - 2.0f * kOverlap, kSize + 2.0f * kOverlap );
    ASSERT_GT( seamPair.size(), 10u );
    EXPECT_EQ( seamPair, seamWider );
}

// S1-b: the volume. A resimulation traces every placement inside the volume onto the ground, files it by type and
// cell, and replaces only the fields its own volume owns: painted fields and another volume's stay as they were.
namespace
{
    using Desert::World::Foliage::FoliageCellOf;

    std::optional<ProceduralFoliageGround> GroundAt( const glm::vec3& start, const glm::vec3& end, float height )
    {
        if ( start.y < height || end.y > height )
            return std::nullopt;
        return ProceduralFoliageGround{ { start.x, height, start.z }, { 0.0f, 1.0f, 0.0f } };
    }

    std::optional<glm::mat4> Upright( const ProceduralFoliagePlacement& p, const ProceduralFoliageGround& g )
    {
        glm::mat4 m( p.Scale );
        m[3] = glm::vec4( g.Point, 1.0f );
        return m;
    }
} // namespace

TEST( ProceduralFoliage, AVolumePlacesOnlyInsideItselfOnTheGroundItsTraceFinds )
{
    ProceduralFoliageSpawner spawner( Settings( 7, 3, 1000.0f ), { Type( 20.0f, 40.0f, 0.0f, 2.0f ) } );
    spawner.Simulate();
    const ProceduralFoliageBox volume{ { -700.0f, -500.0f, 300.0f }, { 1500.0f, 800.0f, 2600.0f } };
    const auto                 desired = DesiredInstancesInVolume( spawner, volume, 0.0f );
    ASSERT_FALSE( desired.empty() );
    for ( const auto& d : desired )
    {
        EXPECT_GE( d.Placement.Location.x, volume.Min.x );
        EXPECT_LE( d.Placement.Location.x, volume.Max.x );
        EXPECT_GE( d.Placement.Location.y, volume.Min.z );
        EXPECT_LE( d.Placement.Location.y, volume.Max.z );
        EXPECT_EQ( d.TraceStart.y, volume.Max.y );
        EXPECT_EQ( d.TraceEnd.y, volume.Min.y );
    }

    const auto trace  = []( const glm::vec3& s, const glm::vec3& e ) { return GroundAt( s, e, 120.0f ); };
    const auto fields = PlaceProceduralFoliage( desired, 1, trace, Upright, 1000.0 );
    ASSERT_TRUE( fields ) << fields.GetError();
    size_t placed = 0;
    for ( const auto& field : fields.GetValue() )
        for ( const auto& m : field.Instances )
        {
            ++placed;
            EXPECT_FLOAT_EQ( m[3].y, 120.0f );
            EXPECT_EQ( FoliageCellOf( m, 1000.0 ), field.Cell );
        }
    EXPECT_EQ( placed, desired.size() );

    // A ground below the volume is out of every trace's reach: nothing is placed.
    const auto deep    = []( const glm::vec3& s, const glm::vec3& e ) { return GroundAt( s, e, -900.0f ); };
    const auto nothing = PlaceProceduralFoliage( desired, 1, deep, Upright, 1000.0 );
    ASSERT_TRUE( nothing );
    EXPECT_TRUE( nothing.GetValue().empty() );
}

TEST( ProceduralFoliage, ResimulatingReplacesTheVolumesOwnFieldsAndNoOtherOnes )
{
    const Common::UUID                      volume( 11u );
    const Common::UUID                      other( 22u );
    std::vector<ProceduralFoliageTypeField> fresh{ { 0, { 0, 0 }, {} }, { 0, { 1, 0 }, {} }, { 1, { 0, 0 }, {} } };
    std::vector<ProceduralFoliageExistingField> existing{
         { Common::UUID::Null(), 0, { 0, 0 } }, // painted by hand
         { other, 0, { 0, 0 } },                // another volume's
         { volume, 0, { 0, 0 } },               // ours, still occupied
         { volume, 0, { 5, 5 } },               // ours, nothing lands there any more
         { volume, UINT32_MAX, { 1, 0 } },      // ours, a type the volume dropped
    };
    const auto plan = PlanProceduralFields( existing, volume, fresh );
    EXPECT_EQ( plan.Rewrite, ( std::vector<std::pair<size_t, size_t>>{ { 2, 0 } } ) );
    EXPECT_EQ( plan.Create, ( std::vector<size_t>{ 1, 2 } ) );
    EXPECT_EQ( plan.Remove, ( std::vector<size_t>{ 3, 4 } ) );

    // A second resimulation over the fields the first one left keeps every one of them.
    std::vector<ProceduralFoliageExistingField> after{
         existing[0], existing[1], existing[2], { volume, 0, { 1, 0 } }, { volume, 1, { 0, 0 } } };
    const auto again = PlanProceduralFields( after, volume, fresh );
    EXPECT_TRUE( again.Remove.empty() );
    EXPECT_TRUE( again.Create.empty() );
    EXPECT_EQ( again.Rewrite.size(), 3u );
}

TEST( ProceduralFoliage, TilesGrownInParallelMatchTheSameSeedEveryRun )
{
    for ( int run = 0; run < 3; ++run )
    {
        ProceduralFoliageSpawner a( Settings( 5, 6, 800.0f ), { Type( 15.0f, 30.0f, 0.0f, 3.0f ) } );
        ProceduralFoliageSpawner b( Settings( 5, 6, 800.0f ), { Type( 15.0f, 30.0f, 0.0f, 3.0f ) } );
        a.Simulate();
        b.Simulate();
        for ( int32_t x = 0; x < 6; ++x )
        {
            const auto pa = a.GetRandomTile( x, x * 3 )->PlacedInstances();
            const auto pb = b.GetRandomTile( x, x * 3 )->PlacedInstances();
            ASSERT_EQ( pa.size(), pb.size() );
            for ( size_t i = 0; i < pa.size(); ++i )
                EXPECT_EQ( pa[i].Location, pb[i].Location );
        }
    }
}
