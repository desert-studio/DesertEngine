// LS-8 and LS-7: the landscape as a surface, from two sides.
//
//   1. The ray (LandscapeRaycast) against the landscape's one surface — two triangles per cell on Jolt's
//      diagonal, the one SampleLandscapeHeight answers and the shader draws. Its hit must lie on that
//      surface to 1e-3 cm, and it must be the FIRST crossing: every ray is checked against an independent
//      march along it, so a skipped cell cannot pass.
//   2. Jolt (LandscapeCollision -> PhysicsWorld heightfield) against the same samples: on the samples, the
//      grid lines and (5) everywhere inside the cells of a steep hill, it is the ray's surface.
//   3. A ball rolled across the tile seams neither sinks through nor hops.
//   4. Edits reach the body in place; the body goes with its tile.

#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/System/LandscapeCollision.hpp>
#include <Engine/Physics/PhysicsWorld.hpp>
#include <Engine/World/Landscape/LandscapeData.hpp>
#include <Engine/World/Landscape/LandscapeLayout.hpp>
#include <Engine/World/Landscape/LandscapeRaycast.hpp>

#include <Common/Core/GlslAsCpp.hpp>

#include <glm/geometric.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <functional>
#include <optional>
#include <random>
#include <vector>

using namespace Desert;
using namespace Desert::World::Landscape;

namespace
{
    // The shader's surface functions, compiled here as they are in LandscapeData.cpp, so the drawn-chord
    // measurement below evaluates the tessellation stage's own arithmetic.
    using glm::floor;
    using glm::min;
    DESERT_GLSL_AS_CPP_BEGIN
#include <Common/LandscapeHeight.glslh>
    DESERT_GLSL_AS_CPP_END
} // namespace

namespace
{
    constexpr uint32_t kQuads   = 31u; // 32 samples per side: a multiple of Jolt's block
    constexpr uint32_t kSamples = kQuads + 1u;
    constexpr float    kSpacing = 100.0f;

    using HeightFn = std::function<float( float gx, float gz )>; // global sample coords -> cm

    // Rolling hills with a short-wavelength ripple on top, so cells carry real twist (h00 - h10 - h01 + h11),
    // the quantity that separates a bilinear reading of the samples from the triangles.
    float Hills( float gx, float gz )
    {
        return 800.0f * std::sin( gx * 0.21f ) * std::cos( gz * 0.17f ) +
               300.0f * std::sin( gx * 0.9f + gz * 0.7f );
    }

    // A tilted plane: both triangles of every cell are one plane, so a ball's contact distance is exact too.
    // Both slopes are whole multiples of the encoding step (100 / 128 = 0.78125 cm): any other slope is
    // rounded per sample and the stored surface is a plane plus up to ±0.39 cm of stair noise.
    float Tilted( float gx, float gz )
    {
        return -16.0f * 0.78125f * gx + 6.0f * 0.78125f * gz;
    }

    // A 2 x 2 landscape. Tiles are built from ONE global function so their shared edge samples are equal,
    // as tiles on disk are.
    struct Landscape2x2
    {
        LandscapeRoot                    Root;
        std::array<LandscapeTileData, 4> Tiles;
        std::array<LandscapeFrame, 4>    Frames;
        std::array<glm::ivec2, 4>        Coords{ glm::ivec2( 0, 0 ), glm::ivec2( 1, 0 ), glm::ivec2( 0, 1 ),
                                          glm::ivec2( 1, 1 ) };

        explicit Landscape2x2( const HeightFn& fn )
        {
            Root.Origin       = glm::vec3( -1234.5f, 50.0f, 777.0f );
            Root.QuadsPerTile = kQuads;
            Root.SpacingCm    = kSpacing;
            Root.ZScale       = kLandscapeDefaultZScale;
            for ( size_t i = 0; i < 4; ++i )
            {
                std::vector<uint16_t> samples( kSamples * kSamples );
                for ( uint32_t z = 0; z < kSamples; ++z )
                    for ( uint32_t x = 0; x < kSamples; ++x )
                    {
                        const float gx            = static_cast<float>( Coords[i].x * kQuads + x );
                        const float gz            = static_cast<float>( Coords[i].y * kQuads + z );
                        samples[z * kSamples + x] = LandscapeSampleFromLocal( fn( gx, gz ) / Root.ZScale );
                    }
                auto tile = LandscapeTileData::FromSamples( kSamples, kSamples, std::move( samples ) );
                EXPECT_TRUE( tile.IsSuccess() ) << tile.GetError();
                Tiles[i]  = tile.ExtractValue();
                Frames[i] = LandscapeTileFrame( Root, Coords[i].x, Coords[i].y );
            }
        }

        std::vector<LandscapeRayTile> RayTiles() const
        {
            std::vector<LandscapeRayTile> out;
            for ( size_t i = 0; i < 4; ++i )
                out.push_back( { &Tiles[i], Frames[i] } );
            return out;
        }

        // The reference surface: SampleLandscapeHeight, from whichever tile covers the point.
        std::optional<float> Height( float x, float z ) const
        {
            for ( size_t i = 0; i < 4; ++i )
                if ( const auto h = SampleLandscapeHeight( Tiles[i], Frames[i], x, z ) )
                    return h;
            return std::nullopt;
        }

        float MinX() const
        {
            return Root.Origin.x;
        }
        float MinZ() const
        {
            return Root.Origin.z;
        }
        float Extent() const
        {
            return 2.0f * kQuads * kSpacing;
        }
        float Seam() const
        {
            return kQuads * kSpacing;
        }

        // Largest quarter-twist over every cell: how far a bilinear reading would part from the triangles.
        float MaxQuarterTwist() const
        {
            float worst = 0.0f;
            for ( const auto& tile : Tiles )
                for ( uint32_t z = 0; z + 1 < kSamples; ++z )
                    for ( uint32_t x = 0; x + 1 < kSamples; ++x )
                    {
                        const auto h = [&]( uint32_t a, uint32_t b )
                        { return LandscapeHeightCm( tile.Sample( a, b ), Root.ZScale ); };
                        worst = std::max( worst, 0.25f * std::abs( h( x, z ) - h( x + 1, z ) - h( x, z + 1 ) +
                                                                   h( x + 1, z + 1 ) ) );
                    }
            return worst;
        }
    };

    // Independent oracle: the first t at which the ray goes from above the surface to below it (or the
    // reverse), found by marching in small steps. nullopt when the march never crosses.
    std::optional<float> MarchFirstCrossing( const Landscape2x2& land, const glm::vec3& o, const glm::vec3& d,
                                             float maxT, float step )
    {
        std::optional<float> prev;
        for ( float t = 0.0f; t <= maxT; t += step )
        {
            const glm::vec3 p = o + d * t;
            const auto      h = land.Height( p.x, p.z );
            if ( !h )
            {
                prev.reset();
                continue;
            }
            const float side = p.y - *h;
            if ( prev && ( ( *prev > 0.0f ) != ( side > 0.0f ) ) )
                return t;
            prev = side;
        }
        return std::nullopt;
    }

    // On the surface to 1e-3 cm, plus what storing the point in floats costs: x and z are rounded to their
    // float grid (half an ulp each — 0.00024 cm at 4000 cm), and on a slope of 4 that moves the surface
    // under them by 0.001 cm on its own. Without the term the check would measure float, not the ray.
    void ExpectOnSurface( const Landscape2x2& land, const LandscapeRayHit& hit, const char* what )
    {
        const auto h = land.Height( hit.Point.x, hit.Point.z );
        ASSERT_TRUE( h.has_value() ) << what << ": hit (" << hit.Point.x << ", " << hit.Point.z
                                     << ") is off the landscape";
        const auto halfUlp = []( float v )
        { return 0.5f * ( std::nextafter( std::abs( v ), INFINITY ) - std::abs( v ) ); };
        const float slopeX = std::abs( hit.Normal.x / hit.Normal.y );
        const float slopeZ = std::abs( hit.Normal.z / hit.Normal.y );
        const float storage =
             slopeX * halfUlp( hit.Point.x ) + slopeZ * halfUlp( hit.Point.z ) + halfUlp( hit.Point.y );
        EXPECT_NEAR( hit.Point.y, *h, 1e-3f + storage )
             << what << " at (" << hit.Point.x << ", " << hit.Point.z << ")";
    }
} // namespace

// ── 1. The ray against the triangulated surface ──────────────────────────────────────────────────────

TEST( LandscapeRaycast, RandomRaysHitTheFirstCrossingOnTheSurface )
{
    const Landscape2x2                    land( Hills );
    const auto                            tiles = land.RayTiles();
    std::mt19937                          rng( 1234u );
    std::uniform_real_distribution<float> across( -1500.0f, land.Extent() + 1500.0f );
    std::uniform_real_distribution<float> unit( -1.0f, 1.0f );
    std::uniform_real_distribution<float> down( -1.0f, -0.03f ); // grazing rays included

    constexpr float kMaxT = 20000.0f;
    constexpr float kStep = 2.0f;
    int             hits = 0, misses = 0;
    for ( int i = 0; i < 400; ++i )
    {
        const glm::vec3 o( land.MinX() + across( rng ), 2500.0f, land.MinZ() + across( rng ) );
        const glm::vec3 d      = glm::normalize( glm::vec3( unit( rng ), down( rng ), unit( rng ) ) );
        const auto      hit    = RaycastLandscape( tiles, o, d, kMaxT );
        const auto      oracle = MarchFirstCrossing( land, o, d, kMaxT, kStep );
        if ( hit )
        {
            ++hits;
            ExpectOnSurface( land, *hit, "random ray" );
            ASSERT_TRUE( oracle.has_value() )
                 << "ray " << i << " hit at t=" << hit->Distance << " where the march never crossed";
            // The march reports the first step PAST the crossing: the true first crossing is within a step
            // before it. A hit further than that skipped an earlier crossing.
            EXPECT_LE( hit->Distance, *oracle + 1e-3f ) << "ray " << i << " skipped an earlier crossing";
            EXPECT_GE( hit->Distance, *oracle - kStep - 1e-3f ) << "ray " << i;
            EXPECT_NEAR( glm::length( hit->Normal ), 1.0f, 1e-5f );
            EXPECT_GT( hit->Normal.y, 0.0f );
            EXPECT_GE( hit->Uv.x, 0.0f );
            EXPECT_LE( hit->Uv.x, 1.0f );
        }
        else
        {
            ++misses;
            EXPECT_FALSE( oracle.has_value() )
                 << "ray " << i << " missed, but crosses the surface at t≈" << *oracle;
        }
    }
    EXPECT_GT( hits, 100 ) << "the sample must exercise hits";
    EXPECT_GT( misses, 20 ) << "and misses";
}

TEST( LandscapeRaycast, SeamCornerAndEdgeRaysLandOnTheSurface )
{
    const Landscape2x2 land( Hills );
    const auto         tiles = land.RayTiles();
    const glm::vec3    down( 0.0f, -1.0f, 0.0f );
    const float        seamX = land.MinX() + land.Seam();
    const float        seamZ = land.MinZ() + land.Seam();
    const float        maxX  = land.MinX() + land.Extent();
    const float        maxZ  = land.MinZ() + land.Extent();

    struct Case
    {
        const char* What;
        float       X, Z;
    };
    const Case cases[] = {
         { "vertical on the X seam", seamX, land.MinZ() + 1234.5f },
         { "vertical on the Z seam", land.MinX() + 4321.0f, seamZ },
         { "the four-tile corner", seamX, seamZ },
         { "the near outer edge", land.MinX(), land.MinZ() + 777.0f },
         { "the far outer edge", maxX, land.MinZ() + 3000.0f },
         { "the far outer corner", maxX, maxZ },
         { "a sample on the seam", seamX, land.MinZ() + 5.0f * kSpacing },
    };
    for ( const auto& c : cases )
    {
        const auto hit = RaycastLandscape( tiles, glm::vec3( c.X, 5000.0f, c.Z ), down, 10000.0f );
        ASSERT_TRUE( hit.has_value() ) << c.What;
        ExpectOnSurface( land, *hit, c.What );
        EXPECT_FLOAT_EQ( hit->Point.x, c.X ) << c.What;
    }

    // Rays running ALONG a seam and along the outer edge, descending: the line both tiles own.
    const glm::vec3 alongZ = glm::normalize( glm::vec3( 0.0f, -0.6f, 1.0f ) );
    const glm::vec3 alongX = glm::normalize( glm::vec3( 1.0f, -0.6f, 0.0f ) );
    for ( const auto& [what, o, d] :
          { std::tuple{ "along the X seam", glm::vec3( seamX, 2500.0f, land.MinZ() ), alongZ },
            std::tuple{ "along the Z seam", glm::vec3( land.MinX(), 2500.0f, seamZ ), alongX },
            std::tuple{ "along the near edge", glm::vec3( land.MinX(), 2500.0f, land.MinZ() ), alongZ } } )
    {
        const auto hit    = RaycastLandscape( tiles, o, d, 20000.0f );
        const auto oracle = MarchFirstCrossing( land, o, d, 20000.0f, 1.0f );
        ASSERT_TRUE( hit.has_value() ) << what;
        ASSERT_TRUE( oracle.has_value() ) << what;
        ExpectOnSurface( land, *hit, what );
        EXPECT_LE( hit->Distance, *oracle + 1e-3f ) << what;
        EXPECT_GE( hit->Distance, *oracle - 1.0f - 1e-3f ) << what;
    }
}

// A ray that enters and leaves the surface inside ONE cell: the random rays almost never do (a mutation
// returning the far root of the cell passed them all). The cell under sample (4, 4) = +400 cm is, along
// its anti-diagonal, a tent over the cell's diagonal: 400·s on the upper triangle (s < 1/2), 400·(1 - s)
// on the lower. At y = 50 cm the ray meets it at s = 1/8 and leaves at s = 7/8; the hit is the first.
TEST( LandscapeRaycast, TheNearerOfTwoCrossingsInOneCellIsTheHit )
{
    auto made = LandscapeTileData::Create( 8u, 8u );
    ASSERT_TRUE( made.IsSuccess() );
    LandscapeTileData tile = made.ExtractValue();
    tile.SetSample( 4u, 4u, LandscapeSampleFromLocal( 400.0f / kLandscapeDefaultZScale ) );
    LandscapeFrame frame;
    frame.OriginX = 0.0f;
    frame.OriginZ = 0.0f;
    const std::vector<LandscapeRayTile> tiles{ { &tile, frame } };

    // From above sample (2, 5), level at 50 cm, along +x -z: through the corners (3, 4) and (4, 3).
    const glm::vec3 origin( 2.0f * kSpacing, 50.0f, 5.0f * kSpacing );
    const glm::vec3 dir = glm::normalize( glm::vec3( 1.0f, 0.0f, -1.0f ) );
    const auto      hit = RaycastLandscape( tiles, origin, dir, 1e4f );
    ASSERT_TRUE( hit.has_value() );

    const float s = 0.125f;
    EXPECT_NEAR( hit->Point.x, ( 3.0f + s ) * kSpacing, 1e-3f );
    EXPECT_NEAR( hit->Point.z, ( 4.0f - s ) * kSpacing, 1e-3f );
    EXPECT_NEAR( hit->Point.y, 50.0f, 1e-3f );
    EXPECT_NEAR( *SampleLandscapeHeight( tile, frame, hit->Point.x, hit->Point.z ), 50.0f, 1e-3f );
}

TEST( LandscapeRaycast, OutsideTheTilesIsAMiss )
{
    const Landscape2x2 land( Hills );
    const auto         tiles = land.RayTiles();
    const glm::vec3    down( 0.0f, -1.0f, 0.0f );
    const float        maxX = land.MinX() + land.Extent();

    EXPECT_FALSE(
         RaycastLandscape( tiles, glm::vec3( land.MinX() - 0.01f, 5000.0f, land.MinZ() + 100.0f ), down, 1e4f ) )
         << "a hair outside the near edge";
    EXPECT_FALSE( RaycastLandscape( tiles, glm::vec3( maxX + 0.01f, 5000.0f, land.MinZ() + 100.0f ), down, 1e4f ) )
         << "a hair outside the far edge";
    EXPECT_FALSE( RaycastLandscape( tiles, glm::vec3( land.MinX() + 100.0f, 5000.0f, land.MinZ() + 100.0f ),
                                    glm::vec3( 0.0f, 1.0f, 0.0f ), 1e4f ) )
         << "pointing up from above";
    EXPECT_FALSE( RaycastLandscape( tiles, glm::vec3( land.MinX() + 100.0f, 5000.0f, land.MinZ() + 100.0f ), down,
                                    100.0f ) )
         << "the surface lies beyond maxDistance";
    EXPECT_FALSE( RaycastLandscape( {}, glm::vec3( 0.0f, 5000.0f, 0.0f ), down, 1e4f ) ) << "no tiles";

    // A ray passing over the whole landscape, above its highest sample, misses too.
    EXPECT_FALSE( RaycastLandscape( tiles, glm::vec3( land.MinX() - 500.0f, 1400.0f, land.MinZ() + 300.0f ),
                                    glm::vec3( 1.0f, 0.0f, 0.0f ), 1e5f ) );
}

// ── 2. The ray and Jolt: one surface on the samples and the grid lines ──────────────────────────────

namespace
{
    struct JoltLandscape
    {
        Physics::PhysicsWorld                    World;
        entt::registry                           Registry;
        std::array<entt::entity, 4>              Entities{};
        std::unique_ptr<ECS::LandscapeCollision> Collision;

        explicit JoltLandscape( const Landscape2x2& land )
        {
            EXPECT_TRUE( World.Init( 981.0f ) );
            Collision = std::make_unique<ECS::LandscapeCollision>( World );
            Collision->Attach( Registry );
            for ( size_t i = 0; i < 4; ++i )
            {
                Entities[i]       = Registry.create();
                auto& component   = Registry.emplace<ECS::LandscapeTileComponent>( Entities[i] );
                component.TileX   = land.Coords[i].x;
                component.TileZ   = land.Coords[i].y;
                component.Heights = land.Tiles[i];
            }
            Sync( land );
        }

        ~JoltLandscape()
        {
            Collision.reset(); // releases into World, which must still exist
        }

        void Sync( const Landscape2x2& land )
        {
            std::vector<ECS::LandscapeTileRef> refs;
            for ( size_t i = 0; i < 4; ++i )
                if ( Registry.valid( Entities[i] ) &&
                     Registry.get<ECS::LandscapeTileComponent>( Entities[i] ).Heights.has_value() )
                    refs.push_back( { Entities[i], &Registry.get<ECS::LandscapeTileComponent>( Entities[i] ),
                                      land.Root, land.Frames[i] } );
            Collision->Sync( refs );
        }

        std::optional<float> JoltHeight( float x, float z ) const
        {
            const auto hit = World.CastRay( glm::vec3( x, 5000.0f, z ), glm::vec3( 0.0f, -1.0f, 0.0f ), 10000.0f );
            return hit ? std::optional<float>( hit->Point.y ) : std::nullopt;
        }
    };

    // Jolt quantises the encodable range to 16 bits: (tile range + 2 x 1000 cm headroom) / 65534 per step.
    constexpr float kJoltQuantisationCm = 0.1f;
} // namespace

TEST( LandscapeCollision, JoltEqualsTheRayOnSamplesAndGridLines )
{
    const Landscape2x2 land( Hills );
    JoltLandscape      jolt( land );
    const auto         tiles = land.RayTiles();
    ASSERT_EQ( jolt.World.GetBodyCount(), 4u );

    const auto both = [&]( float x, float z )
    {
        const auto ray =
             RaycastLandscape( tiles, glm::vec3( x, 5000.0f, z ), glm::vec3( 0.0f, -1.0f, 0.0f ), 1e4f );
        const auto j = jolt.JoltHeight( x, z );
        EXPECT_TRUE( ray.has_value() && j.has_value() ) << "(" << x << ", " << z << ")";
        return std::pair{ ray ? ray->Point.y : 0.0f, j.value_or( 0.0f ) };
    };

    float worst = 0.0f;
    for ( uint32_t gz = 0; gz <= 2 * kQuads; ++gz )
        for ( uint32_t gx = 0; gx <= 2 * kQuads; ++gx )
        {
            const auto [r, j] = both( land.MinX() + gx * kSpacing, land.MinZ() + gz * kSpacing );
            worst             = std::max( worst, std::abs( r - j ) );
        }
    EXPECT_LE( worst, kJoltQuantisationCm ) << "on the samples";

    // Along the grid lines each triangle's edge is linear between its two samples, in Jolt as here.
    std::mt19937                          rng( 99u );
    std::uniform_real_distribution<float> along( 0.0f, land.Extent() );
    std::uniform_int_distribution<int>    line( 0, 2 * kQuads );
    worst = 0.0f;
    for ( int i = 0; i < 1000; ++i )
    {
        const float onLine  = land.MinX() + line( rng ) * kSpacing;
        const float free    = along( rng );
        const auto [r1, j1] = both( onLine, land.MinZ() + free );
        const auto [r2, j2] = both( land.MinX() + free, land.MinZ() + ( onLine - land.MinX() ) );
        worst               = std::max( { worst, std::abs( r1 - j1 ), std::abs( r2 - j2 ) } );
    }
    EXPECT_LE( worst, kJoltQuantisationCm ) << "on the grid lines";
}

// ── 3. A ball across the seams ────────────────────────────────────────────────────────────────────────

TEST( LandscapeCollision, ABallRollsAcrossTheSeamsWithoutSinkingOrHopping )
{
    const Landscape2x2 land( Tilted );
    JoltLandscape      jolt( land );

    constexpr float kRadius = 40.0f;
    // Plane h = BaseY - 12.5·gx + 4.6875·gz (gx, gz in samples): unit normal and the distance of a point from it.
    const glm::vec3 n                 = glm::normalize( glm::vec3( 12.5f / kSpacing, 1.0f, -4.6875f / kSpacing ) );
    const auto      distanceFromPlane = [&]( const glm::vec3& p )
    {
        const float gx = ( p.x - land.MinX() ) / kSpacing;
        const float gz = ( p.z - land.MinZ() ) / kSpacing;
        return ( p.y - ( land.Root.Origin.y + Tilted( gx, gz ) ) ) * n.y;
    };

    // Start in tile (0, 1), heading downhill towards +x and -z: through the X seam, then the Z seam.
    const float       startGx = 24.0f, startGz = 36.0f;
    const float       startX = land.MinX() + startGx * kSpacing;
    const float       startZ = land.MinZ() + startGz * kSpacing;
    const float       startY = land.Root.Origin.y + Tilted( startGx, startGz ) + kRadius / n.y + 0.5f;
    Physics::BodyDesc ball;
    ball.Shape       = Physics::ShapeType::Sphere;
    ball.Radius      = kRadius;
    ball.Type        = Physics::BodyType::Dynamic;
    ball.Mass        = 10.0f;
    ball.Restitution = 0.0f;
    ball.Position    = glm::vec3( startX, startY, startZ );
    const auto body  = jolt.World.CreateBody( ball );
    ASSERT_NE( body, Physics::kInvalidBody );
    jolt.World.SetLinearVelocity( body, glm::vec3( 250.0f, 0.0f, -150.0f ) );

    const float seamX    = land.MinX() + land.Seam();
    const float seamZ    = land.MinZ() + land.Seam();
    bool        crossedX = false, crossedZ = false;
    float       lowest = 1e9f, highest = -1e9f;
    for ( int step = 0; step < 6 * 60; ++step )
    {
        jolt.World.Step( 1.0f / 60.0f );
        const glm::vec3 p = jolt.World.GetPosition( body );
        crossedX          = crossedX || p.x > seamX + 2.0f * kRadius;
        crossedZ          = crossedZ || p.z < seamZ - 2.0f * kRadius;
        if ( step < 10 )
            continue; // settling the half centimetre it was dropped from
        const float d = distanceFromPlane( p ) - kRadius;
        lowest        = std::min( lowest, d );
        highest       = std::max( highest, d );
        if ( crossedX && crossedZ && p.x > seamX + 600.0f )
            break;
    }
    EXPECT_TRUE( crossedX ) << "the ball never reached the X seam";
    EXPECT_TRUE( crossedZ ) << "the ball never reached the Z seam";
    // Rolling, Jolt holds the ball at a steady depth of about 0.2 cm; at the seams, without internal-edge
    // removal, it hopped 0.24 cm and then sank to 0.38 cm (PhysicsWorld.cpp, CreateBody).
    EXPECT_GE( lowest, -0.3f ) << "the ball sank into the surface";
    EXPECT_LE( highest, 0.1f ) << "the ball left the surface (a hop at a seam)";
}

TEST( LandscapeCollision, ABallOverTheFourTileCornerOfHillsStaysAbove )
{
    const Landscape2x2 land( Hills );
    JoltLandscape      jolt( land );
    // One surface (section 5 measures it to 0.006 cm), so what is left is Jolt's contact resolution: the
    // ball lands from 200 cm and rolls at ~5 m/s over creases, and sinks 2.5 cm at worst while it does.
    // The bound was the quarter-twist + 1 cm (53 cm) while the query was bilinear, and hid this entirely.
    const float slack = 3.0f;

    constexpr float   kRadius = 30.0f;
    const float       seamX   = land.MinX() + land.Seam();
    const float       seamZ   = land.MinZ() + land.Seam();
    Physics::BodyDesc ball;
    ball.Shape  = Physics::ShapeType::Sphere;
    ball.Radius = kRadius;
    ball.Type   = Physics::BodyType::Dynamic;
    ball.Position =
         glm::vec3( seamX - 500.0f, *land.Height( seamX - 500.0f, seamZ - 450.0f ) + 200.0f, seamZ - 450.0f );
    const auto body = jolt.World.CreateBody( ball );
    jolt.World.SetLinearVelocity( body, glm::vec3( 400.0f, 0.0f, 360.0f ) );

    float worstSink = 0.0f;
    for ( int step = 0; step < 5 * 60; ++step )
    {
        jolt.World.Step( 1.0f / 60.0f );
        const glm::vec3 p = jolt.World.GetPosition( body );
        const auto      h = land.Height( p.x, p.z );
        if ( !h )
            break; // rolled off the landscape
        worstSink = std::max( worstSink, *h - ( p.y - kRadius ) );
    }
    EXPECT_LE( worstSink, slack ) << "the ball's bottom went below the surface by more than contact slop";
}

// ── 4. Edits and lifetime ─────────────────────────────────────────────────────────────────────────────

TEST( LandscapeCollision, AnEditPatchesTheBodyInPlaceAndItsLifetimeFollowsTheTile )
{
    Landscape2x2  land( Hills );
    JoltLandscape jolt( land );
    auto&         component = jolt.Registry.get<ECS::LandscapeTileComponent>( jolt.Entities[0] );
    const auto    body      = jolt.Collision->BodyOf( jolt.Entities[0] );
    ASSERT_NE( body, Physics::kInvalidBody );

    const float x     = land.Frames[0].OriginX + 10.0f * kSpacing;
    const float z     = land.Frames[0].OriginZ + 10.0f * kSpacing;
    const auto  raise = [&]( float cm )
    {
        const LandscapeRect   rect{ 8u, 8u, 13u, 13u };
        std::vector<uint16_t> values( rect.Area() );
        for ( uint32_t zz = rect.Z0; zz < rect.Z1; ++zz )
            for ( uint32_t xx = rect.X0; xx < rect.X1; ++xx )
                values[( zz - rect.Z0 ) * 5u + ( xx - rect.X0 )] = LandscapeSampleFromLocal(
                     ( LandscapeHeightCm( component.Heights->Sample( xx, zz ), 100.0f ) + cm ) / 100.0f );
        ASSERT_TRUE( component.Heights->WriteRegion( rect, values ).IsSuccess() );
        jolt.Sync( land );
    };

    // Within the headroom: patched in place.
    raise( 200.0f );
    EXPECT_EQ( jolt.Collision->BodyOf( jolt.Entities[0] ), body ) << "a brush stroke must not re-create the body";
    const auto expected = SampleLandscapeHeight( *component.Heights, land.Frames[0], x, z );
    ASSERT_TRUE( expected.has_value() );
    EXPECT_NEAR( *jolt.JoltHeight( x, z ), *expected, kJoltQuantisationCm ) << "the patched height";
    EXPECT_FALSE( component.Heights->DirtyRects( LandscapeDirtyConsumer::Gpu ).empty() )
         << "physics taking its rectangles must leave the GPU's";

    // Far beyond the headroom: Jolt would clamp, so the shape is rebuilt — still the same body.
    raise( 3000.0f );
    EXPECT_EQ( jolt.Collision->BodyOf( jolt.Entities[0] ), body );
    const auto raised = SampleLandscapeHeight( *component.Heights, land.Frames[0], x, z );
    EXPECT_NEAR( *jolt.JoltHeight( x, z ), *raised, kJoltQuantisationCm ) << "clamped at the old range?";

    // Unloading the heights drops the body at the next pass; destroying the entity drops it at once.
    jolt.Registry.get<ECS::LandscapeTileComponent>( jolt.Entities[1] ).Heights.reset();
    jolt.Sync( land );
    EXPECT_EQ( jolt.Collision->BodyOf( jolt.Entities[1] ), Physics::kInvalidBody );
    EXPECT_EQ( jolt.World.GetBodyCount(), 3u );

    jolt.Registry.destroy( jolt.Entities[0] );
    EXPECT_EQ( jolt.Collision->BodyOf( jolt.Entities[0] ), Physics::kInvalidBody );
    EXPECT_EQ( jolt.World.GetBodyCount(), 2u );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}

// ── 5. LS-7b: one surface — the ray, the height query and Jolt answer the same triangles ─────────────

namespace
{
    // A steep procedural hill: a 15 m Gaussian mound with a ripple across it, so its flanks run at up to
    // ~60 degrees and its cells carry real twist — where a bilinear and a triangulated reading of the
    // same samples part by centimetres.
    float SteepHill( float gx, float gz )
    {
        const float dx = gx - 31.0f;
        const float dz = gz - 31.0f;
        return 1500.0f * std::exp( -( dx * dx + dz * dz ) / 72.0f ) +
               250.0f * std::sin( gx * 0.8f ) * std::sin( gz * 0.7f );
    }

    // What Jolt's 16-bit quantisation alone may move a height by: half of one global step over the
    // encodable range (the tile's range plus PhysicsWorld's 1000 cm headroom each side), worst tile.
    float JoltHalfStepCm( const Landscape2x2& land )
    {
        float worst = 0.0f;
        for ( const auto& tile : land.Tiles )
        {
            const auto [lo, hi] = std::minmax_element( tile.Samples().begin(), tile.Samples().end() );
            const float range   = LandscapeHeightCm( *hi, land.Root.ZScale ) -
                                LandscapeHeightCm( *lo, land.Root.ZScale ) + 2.0f * 1000.0f;
            worst = std::max( worst, 0.5f * range / 65534.0f );
        }
        return worst;
    }
} // namespace

// The relation, not the parts: at the same (x, z) on the steep hill, the ray (LS-8, what a click picks),
// SampleLandscapeHeight (what placement asks, and LandscapeHeight.glslh — the function the terrain shader
// displaces every vertex with) and Jolt's CastRay (what bodies rest on) must name one height. Before LS-7b
// the first two were bilinear and Jolt triangulated; this test measured the gap at the numbers it prints.
TEST( LandscapeOneSurface, RayHeightQueryAndJoltAreOneSurfaceOnASteepHill )
{
    const Landscape2x2 land( SteepHill );
    JoltLandscape      jolt( land );
    const auto         tiles = land.RayTiles();

    std::mt19937                          rng( 2026u );
    std::uniform_real_distribution<float> across( 0.0f, land.Extent() );
    float                                 rayVsJolt   = 0.0f;
    float                                 queryVsJolt = 0.0f;
    float                                 rayVsQuery  = 0.0f;
    for ( int i = 0; i < 4000; ++i )
    {
        const float x = land.MinX() + across( rng );
        const float z = land.MinZ() + across( rng );
        const auto  ray =
             RaycastLandscape( tiles, glm::vec3( x, 5000.0f, z ), glm::vec3( 0.0f, -1.0f, 0.0f ), 1e4f );
        const auto j = jolt.JoltHeight( x, z );
        const auto q = land.Height( x, z );
        ASSERT_TRUE( ray && j && q ) << "(" << x << ", " << z << ")";
        rayVsJolt   = std::max( rayVsJolt, std::abs( ray->Point.y - *j ) );
        queryVsJolt = std::max( queryVsJolt, std::abs( *q - *j ) );
        // The ray is solved in double at the exact (x, z); the query is float — 0.01 cm covers the float.
        rayVsQuery = std::max( rayVsQuery, std::abs( ray->Point.y - *q ) );
    }
    const float quant = JoltHalfStepCm( land );
    std::printf( "[ LS7b ] steep hill: worst |ray - Jolt| %.4f cm, |query - Jolt| %.4f cm, |ray - query| %.4f cm "
                 "(Jolt quantisation half-step %.4f cm, max quarter-twist %.3f cm)\n",
                 rayVsJolt, queryVsJolt, rayVsQuery, quant, land.MaxQuarterTwist() );
    EXPECT_LE( rayVsQuery, 0.01f ) << "the ray and the height query are two surfaces";
    EXPECT_LE( rayVsJolt, quant + 0.01f ) << "the ray is not the surface Jolt collides with";
    EXPECT_LE( queryVsJolt, quant + 0.01f ) << "the height query (and the shader) is not Jolt's surface";
}

namespace
{
    // What the GPU draws between tessellated vertices, measured against the surface Jolt collides with.
    // A patch of `quads` cells per side cut into `level` equal segments (equal_spacing, integer level),
    // each vertex displaced by `vertexHeight` (the TES), each micro-quad drawn as two triangles split on
    // (0,0)-(1,1) when `joltDiagonal`, else on the other diagonal — the tessellator picks that split and
    // Vulkan leaves the choice to the implementation, so both are measured.
    using VertexHeightFn = float ( * )( float, float, float, float, float, float );

    float WorstDrawnChordCm( const Landscape2x2& land, uint32_t quads, float level, VertexHeightFn vertexHeight,
                             bool joltDiagonal )
    {
        const LandscapeTileData& tile  = land.Tiles[0];
        const float              zs    = land.Root.ZScale;
        const uint32_t           whole = ( kQuads / quads ) * quads; // cells covered by whole patches
        const auto               at    = [&]( float gx, float gz )
        {
            const float cx = LandscapeCellOf( gx, static_cast<float>( kSamples ) );
            const float cz = LandscapeCellOf( gz, static_cast<float>( kSamples ) );
            const auto  h  = [&]( float x, float z ) {
                return LandscapeHeightCm( tile.Sample( static_cast<uint32_t>( x ), static_cast<uint32_t>( z ) ),
                                            zs );
            };
            return vertexHeight( h( cx, cz ), h( cx + 1, cz ), h( cx, cz + 1 ), h( cx + 1, cz + 1 ), gx - cx,
                                 gz - cz );
        };
        std::mt19937                          rng( 31u );
        std::uniform_real_distribution<float> across( 0.0f, static_cast<float>( whole ) );
        float                                 worst = 0.0f;
        for ( int i = 0; i < 20000; ++i )
        {
            const float gx = across( rng );
            const float gz = across( rng );
            // Micro-quad of the tessellated patch holding (gx, gz), and the fraction inside it.
            const float step = static_cast<float>( quads ) / level;
            const float ux   = std::floor( gx / step );
            const float uz   = std::floor( gz / step );
            const float x0 = ux * step, z0 = uz * step;
            const float fx = ( gx - x0 ) / step, fz = ( gz - z0 ) / step;
            const float v00 = at( x0, z0 ), v10 = at( x0 + step, z0 ), v01 = at( x0, z0 + step ),
                        v11 = at( x0 + step, z0 + step );
            // The other diagonal is LandscapeTriangle of the mirrored micro-quad.
            const float drawn = joltDiagonal ? LandscapeTriangle( v00, v10, v01, v11, fx, fz )
                                             : LandscapeTriangle( v10, v00, v11, v01, 1.0f - fx, fz );
            const float truth =
                 *SampleLandscapeHeight( tile, land.Frames[0], land.Frames[0].OriginX + gx * kSpacing,
                                         land.Frames[0].OriginZ + gz * kSpacing ) -
                 land.Frames[0].BaseY;
            worst = std::max( worst, std::abs( drawn - truth ) );
        }
        return worst;
    }
} // namespace

// The drawn surface. The tessellation stage places every vertex with LandscapeTriangle, and a near patch's
// level is its own quads per side (TerrainRenderer, LandscapeInstance), so near the camera the vertices are
// the samples and the drawn triangles are the cells' — the surface itself when the tessellator splits
// on Jolt's diagonal. The split is the implementation's, so the other one is measured and printed as the
// residual; so is the old fixed level of 16 with bilinear vertices, for the record of what changed.
TEST( LandscapeOneSurface, AtFullDetailTheDrawnTrianglesAreTheCells )
{
    const Landscape2x2 land( SteepHill );
    const float        aligned     = WorstDrawnChordCm( land, 9u, 9.0f, &LandscapeTriangle, true );
    const float        mirrored    = WorstDrawnChordCm( land, 9u, 9.0f, &LandscapeTriangle, false );
    const float        before      = WorstDrawnChordCm( land, 9u, 16.0f, &LandscapeBilinear, true );
    const float        beforeOther = WorstDrawnChordCm( land, 9u, 16.0f, &LandscapeBilinear, false );
    std::printf( "[ LS7b ] drawn chord vs surface, 9-quad patch: level 9 %.4f cm (Jolt split) / %.4f cm (other "
                 "split); before, level 16 bilinear: %.3f / %.3f cm\n",
                 aligned, mirrored, before, beforeOther );
    EXPECT_LE( aligned, 0.01f ) << "at one segment per cell with Jolt's split the drawn triangles must be the "
                                   "surface; a vertex off it means the TES and the query evaluate two surfaces";
}
