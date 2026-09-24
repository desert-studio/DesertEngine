// The XForm tab (EditMeshXformOperations.hpp) on the generator cube (200 cm: x and z centred on the origin, y from
// 0 to 200). The invariant every test states is the RELATION the operation promises: after Edit Pivot and Bake
// Transform each vertex is where it was in the world (parent * local * v, corner by corner); a negative
// determinant re-winds (the signed volume stays positive, the normals still face out); Merge then Split gives the
// parts back; Pattern's copies are the count and the distances asked for.

#include <gtest/gtest.h>

#include "OperationsTestSupport.hpp"

#include <Engine/Geometry/EditMeshXformOperations.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <vector>

using namespace OperationsTest;

namespace
{
    constexpr float kTolerance = 1e-2f; // cm, at coordinates up to ~1000 cm

    glm::vec3 Apply( const glm::mat4& m, const glm::vec3& p )
    {
        return glm::vec3( m * glm::vec4( p, 1.0f ) );
    }

    // Triangle k of `after` (IDs compacted, created in the source's ID order) must hold the same three world
    // points as the k-th live triangle of `before`, in any corner order.
    ::testing::AssertionResult SameWorld( const EditMesh& before, const glm::mat4& beforeToWorld,
                                          const EditMesh& after, const glm::mat4& afterToWorld )
    {
        if ( before.TriangleCount() != after.TriangleCount() )
            return ::testing::AssertionFailure()
                   << "triangle counts " << before.TriangleCount() << " vs " << after.TriangleCount();
        std::vector<int> a, b;
        for ( const int t : before.TriangleIds() )
            a.push_back( t );
        for ( const int t : after.TriangleIds() )
            b.push_back( t );
        for ( size_t k = 0; k < a.size(); ++k )
            for ( const int v : before.GetTriangle( a[k] ) )
            {
                const glm::vec3 want  = Apply( beforeToWorld, before.GetPosition( v ) );
                float           close = 1e30f;
                for ( const int w : after.GetTriangle( b[k] ) )
                    close = std::min( close, glm::length( Apply( afterToWorld, after.GetPosition( w ) ) - want ) );
                if ( close > kTolerance )
                    return ::testing::AssertionFailure()
                           << "triangle " << k << ": a corner moved by " << close << " cm";
            }
        return ::testing::AssertionSuccess();
    }

    // Every normal element a triangle uses points the way its wound face does.
    ::testing::AssertionResult NormalsFaceOut( const EditMesh& mesh )
    {
        const NormalOverlay* normals = mesh.Attributes().Normals();
        if ( normals == nullptr )
            return ::testing::AssertionFailure() << "no normal layer";
        for ( const int t : mesh.TriangleIds() )
        {
            const glm::vec3 face = TriangleNormal( mesh, t );
            for ( const int e : normals->GetTriangle( t ) )
                if ( glm::dot( glm::normalize( normals->GetElement( e ) ), face ) < 0.999f )
                    return ::testing::AssertionFailure() << "triangle " << t << ": normal against the face";
        }
        return ::testing::AssertionSuccess();
    }

    const glm::mat4 kParent = glm::rotate( glm::translate( glm::mat4( 1.0f ), glm::vec3( 300.0f, -50.0f, 20.0f ) ),
                                           0.7f, glm::vec3( 0.0f, 1.0f, 0.0f ) ) *
                              glm::scale( glm::mat4( 1.0f ), glm::vec3( 1.5f ) );

    Trs SkewedLocal()
    {
        return Trs{ glm::vec3( 10.0f, 20.0f, -30.0f ), glm::vec3( 0.3f, 0.5f, -0.2f ),
                    glm::vec3( 2.0f, 0.5f, 1.25f ) };
    }
} // namespace

// ── Edit Pivot ─────────────────────────────────────────────────────────────────────────────────────────────────

TEST( EditMeshXform, EditPivotKeepsEveryVertexInTheWorld )
{
    const EditMesh cube  = MakeCube();
    const Trs      local = SkewedLocal();
    for ( const PivotLocation where : { PivotLocation::BoundsCenter, PivotLocation::BoundsBase,
                                        PivotLocation::WorldOrigin, PivotLocation::WorldPoint } )
    {
        const glm::mat4 world = kParent * local.Matrix();
        auto            pivot = ResolvePivot( cube, where, world, glm::vec3( 40.0f, 60.0f, -80.0f ) );
        ASSERT_TRUE( pivot.IsSuccess() ) << pivot.GetError();
        auto out = EditPivot( cube, local, pivot.GetValue() );
        ASSERT_TRUE( out.IsSuccess() ) << out.GetError();
        EXPECT_TRUE( Valid( out.GetValue().Mesh ) );
        EXPECT_TRUE( SameWorld( cube, world, out.GetValue().Mesh, kParent * out.GetValue().Transform.Matrix() ) )
             << ToString( where );
        // The entity's origin is now the pivot, in the world.
        const glm::vec3 origin    = Apply( kParent * out.GetValue().Transform.Matrix(), glm::vec3( 0.0f ) );
        const glm::vec3 wantPivot = Apply( world, pivot.GetValue() );
        EXPECT_LT( glm::length( origin - wantPivot ), kTolerance ) << ToString( where );
        EXPECT_EQ( out.GetValue().Transform.Rotation, local.Rotation );
        EXPECT_EQ( out.GetValue().Transform.Scale, local.Scale );
    }
}

TEST( EditMeshXform, WorldPivotsGoThroughTheParentChain )
{
    // The child's pivot at the world origin: its world origin must be (0,0,0), which only the WORLD transform
    // (parent included) gives - the local one alone puts it at the parent-relative zero.
    const EditMesh  cube  = MakeCube();
    const glm::mat4 world = kParent * SkewedLocal().Matrix();
    auto            pivot = ResolvePivot( cube, PivotLocation::WorldOrigin, world, glm::vec3( 0.0f ) );
    ASSERT_TRUE( pivot.IsSuccess() );
    EXPECT_LT( glm::length( Apply( world, pivot.GetValue() ) ), kTolerance );

    auto base = ResolvePivot( cube, PivotLocation::BoundsBase, world, glm::vec3( 0.0f ) );
    ASSERT_TRUE( base.IsSuccess() );
    EXPECT_LT( glm::length( base.GetValue() - glm::vec3( 0.0f ) ), 1e-3f ); // the cube's base centre is its origin
}

// ── Bake Transform ─────────────────────────────────────────────────────────────────────────────────────────────

TEST( EditMeshXform, BakeKeepsTheWorldForEveryCombination )
{
    const EditMesh cube  = MakeCube();
    const Trs      local = SkewedLocal();
    for ( int mask = 1; mask < 8; ++mask )
    {
        const BakeOptions options{ ( mask & 1 ) != 0, ( mask & 2 ) != 0, ( mask & 4 ) != 0 };
        auto              out = BakeTransform( cube, local, options );
        ASSERT_TRUE( out.IsSuccess() ) << out.GetError();
        EXPECT_TRUE( Valid( out.GetValue().Mesh ) );
        EXPECT_TRUE( SameWorld( cube, kParent * local.Matrix(), out.GetValue().Mesh,
                                kParent * out.GetValue().Transform.Matrix() ) )
             << "mask " << mask;
        const Trs& kept = out.GetValue().Transform;
        EXPECT_EQ( kept.Rotation, options.Rotation ? glm::vec3( 0.0f ) : local.Rotation );
        EXPECT_EQ( kept.Scale, options.Scale ? glm::vec3( 1.0f ) : local.Scale );
        EXPECT_EQ( kept.Translation, options.Translation ? glm::vec3( 0.0f ) : local.Translation );
    }
}

TEST( EditMeshXform, BakedNonUniformScaleKeepsNormalsOnTheFaces )
{
    auto out = BakeTransform( MakeCube(), SkewedLocal(), BakeOptions{} );
    ASSERT_TRUE( out.IsSuccess() );
    EXPECT_TRUE( NormalsFaceOut( out.GetValue().Mesh ) );
    // Rotation and scale baked: the volume is the cube's times the scale's product.
    EXPECT_NEAR( SignedVolume( out.GetValue().Mesh ), 8.0e6 * 2.0 * 0.5 * 1.25, 10.0 );

    // Faces NOT aligned with the scale's axes: only the inverse transpose keeps their normals on them (for an
    // axis-aligned face the plain linear map points the same way and would pass).
    auto turned =
         TransformMesh( MakeCube(), glm::rotate( glm::mat4( 1.0f ), 0.6f, glm::vec3( 0.0f, 1.0f, 0.0f ) ) );
    ASSERT_TRUE( turned.IsSuccess() );
    Trs squash;
    squash.Scale = glm::vec3( 3.0f, 1.0f, 0.5f );
    auto skewed  = BakeTransform( turned.GetValue().Mesh, squash, BakeOptions{} );
    ASSERT_TRUE( skewed.IsSuccess() ) << skewed.GetError();
    EXPECT_TRUE( NormalsFaceOut( skewed.GetValue().Mesh ) );
}

TEST( EditMeshXform, NegativeScaleReversesTheWinding )
{
    Trs mirrored;
    mirrored.Scale = glm::vec3( -1.0f, 2.0f, 1.0f );
    auto out       = BakeTransform( MakeCube(), mirrored, BakeOptions{} );
    ASSERT_TRUE( out.IsSuccess() ) << out.GetError();
    const EditMesh& mesh = out.GetValue().Mesh;
    EXPECT_TRUE( Valid( mesh ) );
    // Still wound outwards: positive volume, |det| times the cube's.
    EXPECT_NEAR( SignedVolume( mesh ), 2.0 * 8.0e6, 10.0 );
    EXPECT_TRUE( NormalsFaceOut( mesh ) );
    EXPECT_NE( out.GetValue().Report.find( "winding was reversed" ), std::string::npos );

    auto direct = TransformMesh( MakeCube(), glm::scale( glm::mat4( 1.0f ), glm::vec3( 1.0f, 1.0f, -1.0f ) ) );
    ASSERT_TRUE( direct.IsSuccess() );
    EXPECT_TRUE( direct.GetValue().WindingReversed );
    // Tangent frames flip handedness with the surface.
    if ( const TangentOverlay* tangents = direct.GetValue().Mesh.Attributes().Tangents() )
    {
        const EditMesh        source   = MakeCube();
        const TangentOverlay* original = source.Attributes().Tangents();
        ASSERT_NE( original, nullptr );
        const int first = *source.TriangleIds().begin();
        EXPECT_EQ( tangents->GetElement( tangents->GetTriangle( first )[0] ).w,
                   -original->GetElement( original->GetTriangle( first )[0] ).w );
    }
}

TEST( EditMeshXform, Refusals )
{
    const EditMesh cube = MakeCube();
    EXPECT_FALSE( BakeTransform( cube, Trs{}, BakeOptions{ false, false, false } ).IsSuccess() );
    Trs flat;
    flat.Scale           = glm::vec3( 1.0f, 0.0f, 1.0f );
    const auto flattened = BakeTransform( cube, flat, BakeOptions{} );
    ASSERT_FALSE( flattened.IsSuccess() );
    EXPECT_NE( flattened.GetError().find( "singular" ), std::string::npos ) << flattened.GetError();
    EXPECT_FALSE( EditPivot( cube, flat, glm::vec3( 1.0f ) ).IsSuccess() );
    EXPECT_FALSE( ResolvePivot( EditMesh{}, PivotLocation::BoundsCenter, glm::mat4( 1.0f ), {} ).IsSuccess() );
    EXPECT_FALSE( TransformMesh( cube, glm::mat4( 0.0f ) ).IsSuccess() );
}

// ── Merge / Split ──────────────────────────────────────────────────────────────────────────────────────────────

TEST( EditMeshXform, MergeThenSplitGivesThePartsBack )
{
    const EditMesh  a = MakeCube();
    const EditMesh  b = MakeCylinder12();
    const glm::mat4 bToA =
         glm::rotate( glm::translate( glm::mat4( 1.0f ), glm::vec3( 500.0f, 0.0f, 0.0f ) ), 0.4f,
                      glm::vec3( 0.0f, 0.0f, 1.0f ) ) *
         glm::scale( glm::mat4( 1.0f ), glm::vec3( 1.0f, 1.0f, -1.0f ) ); // a mirrored part is re-wound
    const std::vector<MergePart> parts{ { a, glm::mat4( 1.0f ), {} }, { b, bToA, {} } };
    auto                         merged = MergeMeshes( parts );
    ASSERT_TRUE( merged.IsSuccess() ) << merged.GetError();
    const EditMesh& m = merged.GetValue().Mesh;
    EXPECT_TRUE( Valid( m ) );
    EXPECT_EQ( m.TriangleCount(), a.TriangleCount() + b.TriangleCount() );
    EXPECT_EQ( GroupCount( m ), GroupCount( a ) + GroupCount( b ) ); // polygroups stay distinct
    EXPECT_NEAR( SignedVolume( m ), SignedVolume( a ) + SignedVolume( b ), 50.0 );

    auto split = SplitMesh( m, SplitMethod::ConnectedComponents );
    ASSERT_TRUE( split.IsSuccess() ) << split.GetError();
    ASSERT_EQ( split.GetValue().size(), 2u );
    EXPECT_TRUE( SameWorld( a, glm::mat4( 1.0f ), split.GetValue()[0], glm::mat4( 1.0f ) ) );
    EXPECT_TRUE( SameWorld( b, bToA, split.GetValue()[1], glm::mat4( 1.0f ) ) );
    for ( const EditMesh& part : split.GetValue() )
    {
        EXPECT_TRUE( Valid( part ) );
        EXPECT_TRUE( ClosedAndConsistent( part ) );
    }
    EXPECT_NEAR( SignedVolume( split.GetValue()[1] ), SignedVolume( b ), 50.0 );
}

TEST( EditMeshXform, MergeRemapsMaterialsAndNamesDroppedLayers )
{
    EditMesh a = MakeCube();
    EditMesh b = MakeCube();
    b.Attributes().DisableColors();
    a.Attributes().EnableColors(); // a layer only one part has cannot be carried
    const std::vector<MergePart> parts{
         { a, glm::mat4( 1.0f ), { 3 } },
         { b, glm::translate( glm::mat4( 1.0f ), glm::vec3( 0, 0, 900.0f ) ), { 5 } } };
    auto merged = MergeMeshes( parts );
    ASSERT_TRUE( merged.IsSuccess() ) << merged.GetError();
    int threes = 0, fives = 0;
    for ( const int t : merged.GetValue().Mesh.TriangleIds() )
    {
        threes += merged.GetValue().Mesh.Attributes().GetMaterialId( t ) == 3;
        fives += merged.GetValue().Mesh.Attributes().GetMaterialId( t ) == 5;
    }
    EXPECT_EQ( threes, a.TriangleCount() );
    EXPECT_EQ( fives, b.TriangleCount() );
    EXPECT_EQ( merged.GetValue().Mesh.Attributes().Colors(), nullptr );
    EXPECT_NE( merged.GetValue().Report.find( "colours" ), std::string::npos ) << merged.GetValue().Report;

    const std::vector<MergePart> outside{ { a, glm::mat4( 1.0f ), {} }, { b, glm::mat4( 1.0f ), {} } };
    EXPECT_TRUE( MergeMeshes( outside ).IsSuccess() );
    const std::vector<MergePart> badRemap{ { a, glm::mat4( 1.0f ), {} }, { b, glm::mat4( 1.0f ), { 1, 2 } } };
    b.Attributes().SetMaterialId( *b.TriangleIds().begin(), 7 );
    EXPECT_FALSE( MergeMeshes( badRemap ).IsSuccess() );
}

TEST( EditMeshXform, SplitByPolyGroupsAndRefusesOnePart )
{
    auto groups = SplitMesh( MakeCube(), SplitMethod::PolyGroups );
    ASSERT_TRUE( groups.IsSuccess() ) << groups.GetError();
    ASSERT_EQ( groups.GetValue().size(), 6u );
    for ( const EditMesh& face : groups.GetValue() )
    {
        EXPECT_TRUE( Valid( face ) );
        EXPECT_EQ( face.TriangleCount(), 2 );
    }
    const auto one = SplitMesh( MakeCube(), SplitMethod::ConnectedComponents );
    ASSERT_FALSE( one.IsSuccess() );
    EXPECT_NE( one.GetError().find( "one part" ), std::string::npos );
}

// ── Pattern ────────────────────────────────────────────────────────────────────────────────────────────────────

TEST( EditMeshXform, PatternLineAndGridCountsAndDistances )
{
    PatternSettings line;
    line.Count   = 5;
    line.Spacing = 250.0f;
    auto t       = PatternTransforms( line );
    ASSERT_TRUE( t.IsSuccess() ) << t.GetError();
    ASSERT_EQ( t.GetValue().size(), 5u );
    EXPECT_EQ( t.GetValue()[0], glm::mat4( 1.0f ) );
    for ( size_t i = 1; i < 5; ++i )
        EXPECT_NEAR( glm::length( Apply( t.GetValue()[i], {} ) - Apply( t.GetValue()[i - 1], {} ) ), 250.0f,
                     1e-3f );

    PatternSettings grid;
    grid.Shape    = PatternShape::Grid;
    grid.Count    = 3;
    grid.CountB   = 2;
    grid.AxisB    = 1;
    grid.SpacingB = 400.0f;
    auto g        = PatternTransforms( grid );
    ASSERT_TRUE( g.IsSuccess() ) << g.GetError();
    ASSERT_EQ( g.GetValue().size(), 6u );
    EXPECT_LT( glm::length( Apply( g.GetValue()[5], {} ) - glm::vec3( 400.0f, 400.0f, 0.0f ) ), 1e-3f );

    // The single-output pattern: the copies merged, each a whole cube.
    const EditMesh         cube = MakeCube();
    std::vector<MergePart> parts;
    for ( const glm::mat4& m : t.GetValue() )
        parts.push_back( { cube, m, {} } );
    auto merged = MergeMeshes( parts );
    ASSERT_TRUE( merged.IsSuccess() );
    EXPECT_EQ( merged.GetValue().Mesh.TriangleCount(), 5 * cube.TriangleCount() );
    EXPECT_NEAR( SignedVolume( merged.GetValue().Mesh ), 5 * 8.0e6, 50.0 );
}

TEST( EditMeshXform, PatternCircleIsARingThroughTheSource )
{
    PatternSettings ring;
    ring.Shape  = PatternShape::Circle;
    ring.AxisA  = 1;
    ring.Count  = 6;
    ring.Radius = 300.0f;
    auto t      = PatternTransforms( ring );
    ASSERT_TRUE( t.IsSuccess() ) << t.GetError();
    ASSERT_EQ( t.GetValue().size(), 6u );
    EXPECT_LT( glm::length( Apply( t.GetValue()[0], {} ) ), 1e-3f );
    const glm::vec3 centre( 0.0f, 0.0f, -300.0f ); // Radius behind the source along the axis after Y (Z)
    for ( size_t i = 0; i < 6; ++i )
    {
        const glm::vec3 p = Apply( t.GetValue()[i], {} );
        EXPECT_NEAR( glm::length( p - centre ), 300.0f, 1e-2f );
        const glm::vec3 next = Apply( t.GetValue()[( i + 1 ) % 6], {} );
        EXPECT_NEAR( glm::length( next - p ), 300.0f, 1e-2f ); // chord of 60 degrees = r
    }

    PatternSettings arc = ring;
    arc.SweepDegrees    = 90.0f;
    arc.Count           = 3;
    auto half           = PatternTransforms( arc );
    ASSERT_TRUE( half.IsSuccess() );
    EXPECT_NEAR( glm::length( Apply( half.GetValue()[2], {} ) - centre - glm::vec3( 300.0f, 0.0f, 0.0f ) ), 0.0f,
                 1e-2f );

    PatternSettings bad = ring;
    bad.Count           = 1;
    EXPECT_FALSE( PatternTransforms( bad ).IsSuccess() );
    bad       = ring;
    bad.Count = kMaxPatternCopies + 1;
    EXPECT_FALSE( PatternTransforms( bad ).IsSuccess() );
    PatternSettings flatLine;
    flatLine.Spacing = 0.0f;
    EXPECT_FALSE( PatternTransforms( flatLine ).IsSuccess() );
}
