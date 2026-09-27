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

TEST( FoliageTools, SelectPicksTheFirstMeshBoxTheRayEnters )
{
    // A tall thin mesh: 20 x 200 x 20 cm, standing on its origin.
    const glm::vec3 lo( -10.0f, 0.0f, -10.0f ), hi( 10.0f, 200.0f, 10.0f );
    auto            field = Row( 5 );
    const auto      pick  = []( const std::vector<glm::mat4>& f, glm::vec3 o, glm::vec3 d, glm::vec3 a, glm::vec3 b )
    {
        const auto p = FoliagePickInstance( f, o, d, a, b );
        return p ? static_cast<int>( p->Index ) : -1;
    };
    // Along +x at height 150 every box is crossed; the first entered from each side wins, at its face.
    EXPECT_EQ( pick( field, { -500.0f, 150.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, lo, hi ), 0 );
    EXPECT_NEAR( FoliagePickInstance( field, { -500.0f, 150.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, lo, hi )->Distance,
                 490.0f, 1e-3f );
    EXPECT_EQ( pick( field, { 900.0f, 150.0f, 0.0f }, { -1.0f, 0.0f, 0.0f }, lo, hi ), 4 );
    // Above the box tops (y 250), and 15 cm beside a box: a sphere of the old kind would have taken both.
    EXPECT_EQ( pick( field, { -500.0f, 250.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, lo, hi ), -1 );
    EXPECT_EQ( pick( field, { 215.0f, 500.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }, lo, hi ), -1 );
    EXPECT_EQ( pick( field, { 205.0f, 500.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }, lo, hi ), 2 );
    // The box follows the instance: scaled x4 it is 40 cm wide each way, rotated 90 deg about z it lies along x.
    field[2] = glm::scale( field[2], glm::vec3( 4.0f ) );
    EXPECT_EQ( pick( field, { 235.0f, 500.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }, lo, hi ), 2 );
    field[2] = glm::rotate( glm::translate( glm::mat4( 1.0f ), { 200.0f, 0.0f, 0.0f } ), glm::radians( -90.0f ),
                            glm::vec3( 0.0f, 0.0f, 1.0f ) );
    EXPECT_EQ( pick( field, { 300.0f, 500.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }, lo, hi ), 3 ); // x=300 stands taller
    EXPECT_EQ( pick( field, { 350.0f, 500.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }, lo, hi ), 2 ); // the lying box
}

namespace
{
    // A 1000 x 1000 cm square (two triangles, normal +y) at height y, and a vertical 1000 x 1000 wall.
    std::vector<FoliageFillTriangle> Square( float y )
    {
        const glm::vec3 a( 0.0f, y, 0.0f ), b( 0.0f, y, 1000.0f ), c( 1000.0f, y, 1000.0f ), d( 1000.0f, y, 0.0f );
        return { { a, b, c, FoliageSurface::StaticMesh }, { a, c, d, FoliageSurface::StaticMesh } };
    }
} // namespace

TEST( FoliageTools, FillCoversTheMeshByArea )
{
    FoliageTypeData type;
    type.Density = 400.0f; // per 1000 x 1000 cm
    FoliageRandom rng( 21u );
    const auto    placed = FoliageFill( type, Square( 50.0f ), 1.0f, {}, rng );
    EXPECT_EQ( placed.size(), 400u ); // two triangles of 500 000 cm2, 200 each
    for ( const auto& m : placed )
    {
        EXPECT_NEAR( m[3].y, 50.0f, 1e-3f );
        EXPECT_GE( m[3].x, 0.0f );
        EXPECT_LE( m[3].x, 1000.0f );
        EXPECT_GE( m[3].z, 0.0f );
        EXPECT_LE( m[3].z, 1000.0f );
    }
    // Area-weighted: a triangle of a quarter of the area gets a quarter of the instances.
    std::vector<FoliageFillTriangle> small = { { { 0, 0, 0 }, { 0, 0, 500 }, { 500, 0, 500 } } };
    FoliageRandom                    rng2( 21u );
    EXPECT_EQ( FoliageFill( type, small, 1.0f, {}, rng2 ).size(), 50u );
    // Paint density scales it; the same seed places the same instances.
    FoliageRandom again( 21u );
    EXPECT_EQ( FoliageFill( type, Square( 50.0f ), 1.0f, {}, again ), placed );
    FoliageRandom half( 21u );
    EXPECT_EQ( FoliageFill( type, Square( 50.0f ), 0.5f, {}, half ).size(), 200u );
    std::printf( "[ fill ] hash %016llx\n", static_cast<unsigned long long>( Fnv1a( placed ) ) );
    EXPECT_EQ( Fnv1a( placed ), 0x8268f45bd2b2a98bull );
}

TEST( FoliageTools, FillKeepsTheFiltersAndTheTypeRules )
{
    FoliageTypeData type;
    type.Density          = 400.0f;
    type.GroundSlopeAngle = { 0.0f, 30.0f };
    FoliageRandom rng( 3u );
    // A wall (normal along x) is outside the slope range.
    std::vector<FoliageFillTriangle> wall = { { { 0, 0, 0 }, { 0, 1000, 0 }, { 0, 0, 1000 } } };
    EXPECT_TRUE( FoliageFill( type, wall, 1.0f, {}, rng ).empty() );
    // The static-mesh filter off: nothing on a static mesh.
    FoliageSurfaceFilter noMeshes;
    noMeshes.StaticMesh = false;
    EXPECT_TRUE( FoliageFill( type, Square( 0.0f ), 1.0f, noMeshes, rng ).empty() );
    // Height range above the square.
    type.Height = { 100.0f, 200.0f };
    EXPECT_TRUE( FoliageFill( type, Square( 0.0f ), 1.0f, {}, rng ).empty() );
    EXPECT_EQ( FoliageFill( type, Square( 150.0f ), 1.0f, {}, rng ).size(), 400u );
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

TEST( FoliageTools, ReapplyDensityThinsAndTopsUpToTheType )
{
    // 100 instances packed in a 300 cm sphere; the type asks for Density 100 per 1000x1000 -> round(pi*300^2*1e-4)
    // = 28 in the brush disk.
    std::vector<glm::mat4> field;
    for ( int i = 0; i < 100; ++i )
        field.push_back( glm::translate( glm::mat4( 1.0f ), { 20.0f * ( i % 10 ) - 90.0f, 0.0f, 20.0f * ( i / 10 ) - 90.0f } ) );
    field.push_back( glm::translate( glm::mat4( 1.0f ), { 5000.0f, 0.0f, 0.0f } ) ); // outside, untouched
    FoliageTypeData type;
    type.Density = 100.0f;
    FoliageReapplySettings settings;
    settings.Density = true;
    settings.Scale   = false;
    FoliageSelection       selected = { 100u };
    std::vector<glm::vec3> readjusted;
    FoliageRandom          rng( 0xDE5u );
    const auto r = FoliageBrushReapply( type, settings, DabAt( {}, 300.0f ), field, readjusted, rng, Ground(),
                                        &selected );
    EXPECT_EQ( r.Thinned, 72u );
    EXPECT_EQ( field.size(), 29u );
    EXPECT_EQ( selected, ( FoliageSelection{ 28u } ) ); // the outside instance, renumbered
    EXPECT_FLOAT_EQ( field.back()[3].x, 5000.0f );
    const uint64_t thinned = Fnv1a( field );

    // Density raised: the same sphere is topped up by the brush rule, never past the type's count.
    type.Density = 1000.0f; // 283 in the disk
    readjusted.clear();
    const auto up = FoliageBrushReapply( type, settings, DabAt( {}, 300.0f ), field, readjusted, rng, Ground() );
    EXPECT_GT( up.Added, 200u );
    EXPECT_LE( up.Added, 283u - 28u );
    EXPECT_EQ( up.Thinned, 0u );

    // Deterministic from the seed.
    std::vector<glm::mat4> again;
    for ( int i = 0; i < 100; ++i )
        again.push_back( glm::translate( glm::mat4( 1.0f ), { 20.0f * ( i % 10 ) - 90.0f, 0.0f, 20.0f * ( i / 10 ) - 90.0f } ) );
    again.push_back( glm::translate( glm::mat4( 1.0f ), { 5000.0f, 0.0f, 0.0f } ) );
    type.Density = 100.0f;
    readjusted.clear();
    FoliageRandom rng2( 0xDE5u );
    FoliageBrushReapply( type, settings, DabAt( {}, 300.0f ), again, readjusted, rng2, Ground() );
    EXPECT_EQ( Fnv1a( again ), thinned );
    std::printf( "[ reapply density ] hash %016llx\n", static_cast<unsigned long long>( thinned ) );
    EXPECT_EQ( thinned, 0xf971428d32960e49ull );
}
