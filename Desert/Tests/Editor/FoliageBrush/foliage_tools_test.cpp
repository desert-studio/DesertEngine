// FO-4: the foliage tools beside the paint brush (UE FEdModeFoliage): Remove, Single, Select / Lasso and the
// operations on a selection, Reapply - each against planes, each deterministic from the stroke seed, and each
// one undo step through the FoliageStroke record (instances AND selection before / after).

#include <Editor/Panels/ViewportPanel/Tools/FoliageBrush.hpp>

#include <gtest/gtest.h>

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <vector>

namespace
{
    using namespace Desert::Editor::Tools;
    using Desert::Assets::Serialization::FoliageTypeData;

    // Ground: the plane y = @p y with normal @p n, answering every crossing segment as a landscape.
    FoliageBrushWorld Ground( float y = 0.0f, glm::vec3 n = { 0.0f, 1.0f, 0.0f } )
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
            return FoliageTraceHit{ a + t * ( b - a ), n, FoliageSurface::Landscape, std::nullopt };
        };
        world.LayerWeightAt = []( const glm::vec3& ) { return std::optional<float>(); };
        return world;
    }

    FoliageBrushDab DabAt( glm::vec3 centre, float radius )
    {
        FoliageBrushDab dab;
        dab.Center = centre;
        dab.Radius = radius;
        return dab;
    }

    // A row of upright unit-scale instances on y = 0 at x = 0, 100, ..., 100 * (n - 1).
    std::vector<glm::mat4> Row( int n )
    {
        std::vector<glm::mat4> row;
        for ( int i = 0; i < n; ++i )
            row.push_back( glm::translate( glm::mat4( 1.0f ), { 100.0f * static_cast<float>( i ), 0.0f, 0.0f } ) );
        return row;
    }

    uint64_t Fnv1a( const std::vector<glm::mat4>& field )
    {
        uint64_t h = 1469598103934665603ull;
        for ( const auto& m : field )
            for ( int c = 0; c < 4; ++c )
                for ( int r = 0; r < 4; ++r )
                {
                    uint32_t bits = 0;
                    std::memcpy( &bits, &m[c][r], 4 );
                    for ( int b = 0; b < 4; ++b )
                        h = ( h ^ ( ( bits >> ( 8 * b ) ) & 0xFFu ) ) * 1099511628211ull;
                }
        return h;
    }

    float ScaleOf( const glm::mat4& m )
    {
        return glm::length( glm::vec3( m[1] ) );
    }
} // namespace

TEST( FoliageTools, RemoveTakesTheSphereAndRenumbersTheSelection )
{
    auto             field    = Row( 10 ); // x = 0..900
    FoliageSelection selected = { 1u, 4u, 8u };
    // Sphere at x = 400, r = 250: removes x = 200..600 (indices 2..6).
    EXPECT_EQ( FoliageBrushRemove( field, { 400.0f, 0.0f, 0.0f }, 250.0f, &selected ), 5u );
    ASSERT_EQ( field.size(), 5u );
    EXPECT_FLOAT_EQ( field[2][3].x, 700.0f );
    // Index 4 was removed; 8 (x = 800) is now index 3.
    EXPECT_EQ( selected, ( FoliageSelection{ 1u, 3u } ) );
}

TEST( FoliageTools, RemoveStrokeIsOneUndoStep )
{
    const Common::UUID grass( 11u );
    auto               field  = Row( 20 );
    const auto         before = field;
    FoliageStroke      stroke( 5u );
    for ( float x = 0.0f; x <= 1000.0f; x += 250.0f ) // five dabs
    {
        stroke.Touch( grass, field );
        FoliageBrushRemove( field, { x, 0.0f, 0.0f }, 120.0f );
    }
    const auto entries = stroke.Finish( [&]( const Common::UUID& ) { return &field; } );
    ASSERT_EQ( entries.size(), 1u );
    EXPECT_EQ( entries[0].Before, before ); // undo writes this back
    EXPECT_EQ( entries[0].After, field );
    EXPECT_EQ( field.size(), 8u ); // x = 1200..1900 survive
}

TEST( FoliageTools, SinglePlacesOneInstanceAtTheCentre )
{
    FoliageTypeData type;
    type.ScaleX  = { 2.0f, 2.0f };
    type.Density = 0.001f; // no density limit applies to Single
    FoliageRandom rng( 9u );
    const auto    placed = FoliageBrushSingle( type, DabAt( { 300.0f, 0.0f, -200.0f }, 5000.0f ), rng, Ground() );
    ASSERT_TRUE( placed.has_value() );
    EXPECT_NEAR( ( *placed )[3].x, 300.0f, 1e-3f );
    EXPECT_NEAR( ( *placed )[3].y, 0.0f, 1e-3f );
    EXPECT_NEAR( ( *placed )[3].z, -200.0f, 1e-3f );
    EXPECT_NEAR( ScaleOf( *placed ), 2.0f, 1e-4f );

    // The same seed places the same transform.
    FoliageRandom again( 9u );
    EXPECT_EQ( FoliageBrushSingle( type, DabAt( { 300.0f, 0.0f, -200.0f }, 5000.0f ), again, Ground() ), placed );
}

TEST( FoliageTools, SingleKeepsTheTypeRules )
{
    FoliageTypeData type;
    type.GroundSlopeAngle = { 0.0f, 20.0f };
    FoliageRandom   rng( 1u );
    FoliageBrushDab dab = DabAt( {}, 100.0f );
    dab.Normal          = glm::normalize( glm::vec3( 1.0f, 1.0f, 0.0f ) );
    EXPECT_FALSE( FoliageBrushSingle( type, dab, rng, Ground( 0.0f, dab.Normal ) ).has_value() ); // 45 deg
    type.Height = { 10.0f, 20.0f };
    EXPECT_FALSE( FoliageBrushSingle( type, DabAt( {}, 100.0f ), rng, Ground() ).has_value() ); // y = 0
    type.Height = { -1.0f, 1.0f };
    EXPECT_TRUE( FoliageBrushSingle( type, DabAt( {}, 100.0f ), rng, Ground() ).has_value() );
    FoliageBrushDab noLandscape  = DabAt( {}, 100.0f );
    noLandscape.Filter.Landscape = false;
    EXPECT_FALSE( FoliageBrushSingle( type, noLandscape, rng, Ground() ).has_value() );
}

TEST( FoliageTools, LassoSelectsAndDeselectsTheSphere )
{
    const auto       field = Row( 10 );
    FoliageSelection selected;
    // x = 100..400.
    EXPECT_EQ( FoliageSelectInSphere( field, { 250.0f, 0.0f, 0.0f }, 160.0f, true, selected ), 4u );
    EXPECT_EQ( selected, ( FoliageSelection{ 1u, 2u, 3u, 4u } ) );
    const FoliageSelection first = selected;
    EXPECT_EQ( FoliageSelectInSphere( field, { 800.0f, 0.0f, 0.0f }, 50.0f, true, selected ), 1u );
    EXPECT_EQ( selected.size(), first.size() + 1u );
    EXPECT_EQ( selected.back(), 8u );
    // Shift: the same sphere deselects; selecting twice changes nothing.
    EXPECT_EQ( FoliageSelectInSphere( field, { 800.0f, 0.0f, 0.0f }, 50.0f, false, selected ), 1u );
    EXPECT_EQ( selected, first );
    EXPECT_EQ( FoliageSelectInSphere( field, { 250.0f, 0.0f, 0.0f }, 160.0f, true, selected ), 0u );
}

TEST( FoliageTools, SelectPicksTheFirstInstanceTheRayEnters )
{
    auto field = Row( 5 );
    // A ray along +x at height 20 passes through every instance; the first entered is x = 0 (index 0) from
    // the left, x = 400 (index 4) from the right.
    EXPECT_EQ( FoliagePickInstance( field, { -500.0f, 20.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, 50.0f ), 0u );
    EXPECT_EQ( FoliagePickInstance( field, { 900.0f, 20.0f, 0.0f }, { -1.0f, 0.0f, 0.0f }, 50.0f ), 4u );
    // Straight down onto x = 200.
    EXPECT_EQ( FoliagePickInstance( field, { 210.0f, 500.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }, 50.0f ), 2u );
    // Past every instance, and behind the ray.
    EXPECT_FALSE( FoliagePickInstance( field, { 0.0f, 500.0f, 300.0f }, { 1.0f, 0.0f, 0.0f }, 50.0f ) );
    EXPECT_FALSE( FoliagePickInstance( field, { 900.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, 50.0f ) );
    // The pick sphere grows with the instance's scale.
    field[2] = glm::scale( field[2], glm::vec3( 4.0f ) );
    EXPECT_EQ( FoliagePickInstance( field, { 350.0f, 500.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }, 50.0f ), 2u );
}

TEST( FoliageTools, SelectionMovesAndDeletesAsOneUndoStep )
{
    const Common::UUID grass( 3u );
    auto               field     = Row( 6 );
    FoliageSelection   selected  = { 1u, 3u };
    const auto         before    = field;
    const auto         selBefore = selected;

    FoliageStroke stroke( 1u );
    stroke.Touch( grass, field, selected );
    FoliageMoveSelected( field, selected, { 0.0f, 50.0f, 10.0f } );
    EXPECT_FLOAT_EQ( field[1][3].y, 50.0f );
    EXPECT_FLOAT_EQ( field[3][3].z, 10.0f );
    EXPECT_FLOAT_EQ( field[2][3].y, 0.0f );
    EXPECT_EQ( FoliageRemoveSelected( field, selected ), 2u );
    EXPECT_TRUE( selected.empty() );
    ASSERT_EQ( field.size(), 4u );
    EXPECT_FLOAT_EQ( field[1][3].x, 200.0f );

    const auto entries = stroke.Finish( [&]( const Common::UUID& ) { return &field; },
                                        [&]( const Common::UUID& ) { return selected; } );
    ASSERT_EQ( entries.size(), 1u );
    EXPECT_EQ( entries[0].Before, before );
    EXPECT_EQ( entries[0].SelectedBefore, selBefore );
    EXPECT_TRUE( entries[0].SelectedAfter.empty() );
}

TEST( FoliageTools, ASelectionOnlyStrokeIsAnUndoStep )
{
    const Common::UUID grass( 4u );
    const auto         field = Row( 6 );
    FoliageSelection   selected;
    FoliageStroke      stroke( 2u );
    stroke.Touch( grass, field, selected );
    FoliageSelectInSphere( field, { 0.0f, 0.0f, 0.0f }, 150.0f, true, selected );
    const auto entries = stroke.Finish( [&]( const Common::UUID& ) { return &field; },
                                        [&]( const Common::UUID& ) { return selected; } );
    ASSERT_EQ( entries.size(), 1u );
    EXPECT_EQ( entries[0].Before, entries[0].After );
    EXPECT_EQ( entries[0].SelectedAfter, ( FoliageSelection{ 0u, 1u } ) );
    // Without a selection answer the stroke is about instances only, and nothing changed.
    EXPECT_TRUE( stroke.Finish( [&]( const Common::UUID& ) { return &field; } ).empty() );
}

TEST( FoliageTools, ReapplyRerollsTheSwitchedPropertiesInsideTheSphere )
{
    FoliageTypeData type;
    type.ScaleX    = { 3.0f, 3.0f };
    type.RandomYaw = false;
    // Row at y = 25: each instance stands 25 cm above the ground (a kept Z offset).
    std::vector<glm::mat4> field;
    for ( int i = 0; i < 10; ++i )
        field.push_back( glm::rotate( glm::translate( glm::mat4( 1.0f ), { 100.0f * i, 25.0f, 0.0f } ), 0.5f,
                                      glm::vec3( 0.0f, 1.0f, 0.0f ) ) );
    const auto             original = field;
    std::vector<glm::vec3> readjusted;
    FoliageRandom          rng( 4u );
    FoliageReapplySettings settings; // Scale on, yaw off: the yaw is kept
    const auto r = FoliageBrushReapply( type, settings, DabAt( { 200.0f, 0.0f, 0.0f }, 150.0f ), field, readjusted,
                                        rng, Ground() );
    EXPECT_EQ( r.Updated, 3u ); // x = 100, 200, 300
    EXPECT_EQ( r.Removed, 0u );
    for ( int i = 0; i < 10; ++i )
    {
        const bool inside = i >= 1 && i <= 3;
        EXPECT_NEAR( ScaleOf( field[i] ), inside ? 3.0f : 1.0f, 1e-4f ) << i;
        EXPECT_NEAR( field[i][3].y, 25.0f, 1e-3f ) << i; // offset kept
        const glm::vec3 x = glm::normalize( glm::vec3( field[i][0] ) );
        EXPECT_NEAR( std::atan2( -x.z, x.x ), 0.5f, 1e-4f ) << i; // yaw kept
    }
    // A second dab over the same instances in the same stroke leaves them (UE FOLIAGE_Readjusted).
    type.ScaleX      = { 5.0f, 5.0f };
    const auto again = FoliageBrushReapply( type, settings, DabAt( { 200.0f, 0.0f, 0.0f }, 150.0f ), field,
                                            readjusted, rng, Ground() );
    EXPECT_EQ( again.Updated, 0u );
    EXPECT_NEAR( ScaleOf( field[2] ), 3.0f, 1e-4f );

    // Yaw switched on with a type without random yaw: yaw 0. Z offset switched on: the type's offset.
    readjusted.clear();
    settings.RandomYaw = true;
    settings.ZOffset   = true;
    type.ZOffset       = { -5.0f, -5.0f };
    FoliageBrushReapply( type, settings, DabAt( { 0.0f, 0.0f, 0.0f }, 50.0f ), field, readjusted, rng, Ground() );
    EXPECT_NEAR( field[0][3].y, -5.0f, 1e-3f );
    EXPECT_NEAR( glm::normalize( glm::vec3( field[0][0] ) ).z, 0.0f, 1e-5f );
    EXPECT_NE( field, original );
}

TEST( FoliageTools, ReapplyRemovesWhatTheRecheckedFiltersRefuse )
{
    FoliageTypeData type;
    type.Height                     = { 10.0f, 1000.0f }; // the ground at y = 0 is now out of range
    auto                   field    = Row( 6 );
    FoliageSelection       selected = { 0u, 5u };
    std::vector<glm::vec3> readjusted;
    FoliageRandom          rng( 8u );
    FoliageReapplySettings settings;
    auto r = FoliageBrushReapply( type, settings, DabAt( { 100.0f, 0.0f, 0.0f }, 150.0f ), field, readjusted, rng,
                                  Ground(), &selected );
    EXPECT_EQ( r.Removed, 3u ); // x = 0, 100, 200
    EXPECT_EQ( field.size(), 3u );
    EXPECT_EQ( selected, ( FoliageSelection{ 2u } ) ); // x = 500 renumbered, x = 0 gone

    // Height not re-checked: nothing is removed.
    settings.Height = false;
    field           = Row( 6 );
    readjusted.clear();
    r = FoliageBrushReapply( type, settings, DabAt( { 100.0f, 0.0f, 0.0f }, 150.0f ), field, readjusted, rng,
                             Ground() );
    EXPECT_EQ( r.Removed, 0u );
    EXPECT_EQ( r.Updated, 3u );

    // No ground under the instance: left as it was.
    field = Row( 3 );
    readjusted.clear();
    r = FoliageBrushReapply( type, settings, DabAt( { 100.0f, 0.0f, 0.0f }, 150.0f ), field, readjusted, rng,
                             Ground( -1000.0f ) );
    EXPECT_EQ( r.Skipped, 3u );
    EXPECT_EQ( field, Row( 3 ) );
}

TEST( FoliageTools, ReapplyAlignsToTheGroundAndIsDeterministic )
{
    FoliageTypeData type;
    type.AlignToNormal = true;
    type.RandomYaw     = true;
    type.ScaleX        = { 0.5f, 2.0f };
    const glm::vec3 n  = glm::normalize( glm::vec3( 0.3f, 1.0f, 0.1f ) );

    auto run = []( const FoliageTypeData& t, const glm::vec3& normal )
    {
        std::vector<glm::mat4> field;
        for ( int i = 0; i < 40; ++i )
            field.push_back( glm::translate( glm::mat4( 1.0f ), { 25.0f * ( i % 8 ), 0.0f, 25.0f * ( i / 8 ) } ) );
        FoliageReapplySettings settings;
        settings.RandomYaw = true;
        std::vector<glm::vec3> readjusted;
        FoliageRandom          rng( 0xF04u );
        FoliageBrushReapply( t, settings, DabAt( { 90.0f, 0.0f, 50.0f }, 400.0f ), field, readjusted, rng,
                             Ground( 0.0f, normal ) );
        return field;
    };
    const auto field = run( type, n );
    for ( const auto& m : field )
        EXPECT_NEAR( glm::dot( glm::normalize( glm::vec3( m[1] ) ), n ), 1.0f, 1e-4f );
    EXPECT_EQ( Fnv1a( run( type, n ) ), Fnv1a( field ) );
    std::printf( "[ reapply ] hash %016llx\n", static_cast<unsigned long long>( Fnv1a( field ) ) );
    // Pinned: the same numbers on every platform (PCG32, no <random>).
    EXPECT_EQ( Fnv1a( field ), 0x515801d99d7d7a57ull );
}
