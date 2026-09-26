// The UE primitives FMeshBevel's chamfer needs (P13b): TLine3, FDistLine3Line3d, the shared inset-line solve in
// PolyEditingEdgeUtil and the 3D ear clip in PolygonTriangulation. Each test pins a value UE's code produces, so a
// port slip (a sign, an average, a winding) turns it red; FInsetMeshRegion's own tests cover the solve in context.
#include "Engine/Geometry/UECore/CompGeom/PolygonTriangulation.hpp"
#include "Engine/Geometry/UECore/Distance/DistLine3Line3.hpp"
#include "Engine/Geometry/UECore/DynamicMesh/Operations/PolyEditingEdgeUtil.hpp"
#include "Engine/Geometry/UECore/VectorUtil.hpp"

#include <gtest/gtest.h>

#include <cmath>

using namespace Desert::Geometry;

namespace
{
    constexpr double Tol = 1e-9;

    void ExpectNear( const glm::dvec3& Actual, const glm::dvec3& Expected, const char* What )
    {
        EXPECT_NEAR( Actual.x, Expected.x, Tol ) << What;
        EXPECT_NEAR( Actual.y, Expected.y, Tol ) << What;
        EXPECT_NEAR( Actual.z, Expected.z, Tol ) << What;
    }

    // A 100 x 100 cm square in the XY plane, two triangles, counter-clockwise corners 0..3 from the origin.
    FDynamicMesh3 Square( TArray<int32_t>& CornersOut )
    {
        FDynamicMesh3 Mesh;
        CornersOut.Reset();
        CornersOut.Add( Mesh.AppendVertex( glm::dvec3( 0, 0, 0 ) ) );
        CornersOut.Add( Mesh.AppendVertex( glm::dvec3( 100, 0, 0 ) ) );
        CornersOut.Add( Mesh.AppendVertex( glm::dvec3( 100, 100, 0 ) ) );
        CornersOut.Add( Mesh.AppendVertex( glm::dvec3( 0, 100, 0 ) ) );
        Mesh.AppendTriangle( CornersOut[0], CornersOut[1], CornersOut[2] );
        Mesh.AppendTriangle( CornersOut[0], CornersOut[2], CornersOut[3] );
        return Mesh;
    }

    // Rotates about X by 30 degrees and shifts, so the polygon tests do not run in an axis plane.
    glm::dvec3 Tilt( double X, double Y )
    {
        const double c = std::cos( 0.5235987755982988 );
        const double s = std::sin( 0.5235987755982988 );
        return { X + 7.0, Y * c - 3.0, Y * s + 11.0 };
    }

    glm::dvec3 TriangleCross( const TArray<glm::dvec3>& P, const FIndex3i& T )
    {
        return glm::cross( ( P[T.B] - P[T.A] ), P[T.C] - P[T.A] );
    }
} // namespace

TEST( BevelPrimitives, Line3FromPointsIsUnitAndProjects )
{
    const FLine3d Line = FLine3d::FromPoints( glm::dvec3( 1, 2, 3 ), glm::dvec3( 1, 2, 13 ) );
    ExpectNear( Line.Direction, glm::dvec3( 0, 0, 1 ), "direction" );
    EXPECT_NEAR( Line.Project( glm::dvec3( 5, 5, 8 ) ), 5.0, Tol );
    ExpectNear( Line.PointAt( 5.0 ), glm::dvec3( 1, 2, 8 ), "point at" );
    EXPECT_NEAR( Line.DistanceSquared( glm::dvec3( 4, 6, 0 ) ), 25.0, Tol );
}

TEST( BevelPrimitives, DistLine3Line3SkewLines )
{
    // X axis, and a Y-parallel line 5 cm above it through (3, 2, 5): closest pair (3,0,0)-(3,0,5).
    FDistLine3Line3d Distance( FLine3d( glm::dvec3( 0, 0, 0 ), glm::dvec3( 1, 0, 0 ) ),
                               FLine3d( glm::dvec3( 3, 2, 5 ), glm::dvec3( 0, 1, 0 ) ) );
    EXPECT_NEAR( Distance.Get(), 5.0, Tol );
    EXPECT_FALSE( Distance.bIsParallel );
    ExpectNear( Distance.Line1ClosestPoint, glm::dvec3( 3, 0, 0 ), "line 1 closest" );
    ExpectNear( Distance.Line2ClosestPoint, glm::dvec3( 3, 0, 5 ), "line 2 closest" );
    EXPECT_NEAR( Distance.Line1Parameter, 3.0, Tol );
    EXPECT_NEAR( Distance.Line2Parameter, -2.0, Tol );

    // At 45 degrees the cross term of the solve matters: projections cross at (2, 0), 5 cm apart in Z.
    FDistLine3Line3d Oblique( FLine3d( glm::dvec3( 0, 0, 0 ), glm::dvec3( 1, 0, 0 ) ),
                              FLine3d( glm::dvec3( 3, 1, 5 ), Normalized( glm::dvec3( 1, 1, 0 ) ) ) );
    EXPECT_NEAR( Oblique.Get(), 5.0, Tol );
    ExpectNear( Oblique.Line1ClosestPoint, glm::dvec3( 2, 0, 0 ), "oblique line 1 closest" );
    ExpectNear( Oblique.Line2ClosestPoint, glm::dvec3( 2, 0, 5 ), "oblique line 2 closest" );
    EXPECT_NEAR( Oblique.Line2Parameter, -std::sqrt( 2.0 ), Tol );
}

TEST( BevelPrimitives, DistLine3Line3ParallelLines )
{
    FDistLine3Line3d Distance( FLine3d( glm::dvec3( 4, 0, 0 ), glm::dvec3( 1, 0, 0 ) ),
                               FLine3d( glm::dvec3( -9, 3, 4 ), glm::dvec3( -1, 0, 0 ) ) );
    EXPECT_NEAR( Distance.GetSquared(), 25.0, Tol );
    EXPECT_TRUE( Distance.bIsParallel );
    ExpectNear( Distance.Line1ClosestPoint, glm::dvec3( 4, 0, 0 ), "line 1 origin" );
    ExpectNear( Distance.Line2ClosestPoint, glm::dvec3( 4, 3, 4 ), "projection onto line 2" );
}

TEST( BevelPrimitives, InsetPairIsMidpointOfClosestPoints )
{
    // Skew lines: the solve returns the midpoint of the closest pair, not either point.
    const glm::dvec3 P = SolveInsetVertexPositionFromLinePair(
         glm::dvec3( 50, 50, 50 ), FLine3d( glm::dvec3( 0, 0, 0 ), glm::dvec3( 1, 0, 0 ) ),
         FLine3d( glm::dvec3( 3, 2, 6 ), glm::dvec3( 0, 1, 0 ) ) );
    ExpectNear( P, glm::dvec3( 3, 0, 3 ), "skew midpoint" );

    // Nearly parallel (|dot| > 0.999): nearest point on the first line to the vertex.
    const glm::dvec3 Q = SolveInsetVertexPositionFromLinePair(
         glm::dvec3( 20, 7, 0 ), FLine3d( glm::dvec3( 0, 0, 0 ), glm::dvec3( 1, 0, 0 ) ),
         FLine3d( glm::dvec3( 0, 5, 0 ), Normalized( glm::dvec3( 1, 0.01, 0 ) ) ) );
    ExpectNear( Q, glm::dvec3( 20, 0, 0 ), "parallel fallback" );
}

TEST( BevelPrimitives, InsetLinesOfASquareLoopSolveToTheInnerSquare )
{
    TArray<int32_t>     Corners;
    const FDynamicMesh3 Mesh = Square( Corners );
    TArray<int32_t>     Edges;
    for ( int32_t i = 0; i < 4; ++i )
        Edges.Add( Mesh.FindEdge( Corners[i], Corners[( i + 1 ) % 4] ) );

    TArray<FLine3d> Lines;
    ComputeInsetLineSegmentsFromEdges( Mesh, Edges, 10.0, Lines );
    ASSERT_EQ( Lines.Num(), 4 );
    // Each line runs along its edge, moved 10 cm towards the square's inside.
    EXPECT_NEAR( Lines[0].DistanceSquared( glm::dvec3( 50, 10, 0 ) ), 0.0, Tol );
    EXPECT_NEAR( Lines[1].DistanceSquared( glm::dvec3( 90, 50, 0 ) ), 0.0, Tol );
    EXPECT_NEAR( Lines[2].DistanceSquared( glm::dvec3( 50, 90, 0 ) ), 0.0, Tol );
    EXPECT_NEAR( Lines[3].DistanceSquared( glm::dvec3( 10, 50, 0 ) ), 0.0, Tol );

    TArray<glm::dvec3> Loop;
    SolveInsetVertexPositionsFromInsetLines( Mesh, Lines, Corners, Loop, true );
    ASSERT_EQ( Loop.Num(), 4 );
    ExpectNear( Loop[0], glm::dvec3( 10, 10, 0 ), "loop corner 0" );
    ExpectNear( Loop[1], glm::dvec3( 90, 10, 0 ), "loop corner 1" );
    ExpectNear( Loop[2], glm::dvec3( 90, 90, 0 ), "loop corner 2" );
    ExpectNear( Loop[3], glm::dvec3( 10, 90, 0 ), "loop corner 3" );

    // An open span 0-1-2 over edges 0 and 1: the ends are projected onto their one line.
    const TArray<int32_t> SpanVertices = { Corners[0], Corners[1], Corners[2] };
    const TArray<FLine3d> SpanLines    = { Lines[0], Lines[1] };
    TArray<glm::dvec3>    Span;
    SolveInsetVertexPositionsFromInsetLines( Mesh, SpanLines, SpanVertices, Span, false );
    ASSERT_EQ( Span.Num(), 3 );
    ExpectNear( Span[0], glm::dvec3( 0, 10, 0 ), "span start" );
    ExpectNear( Span[1], glm::dvec3( 90, 10, 0 ), "span middle" );
    ExpectNear( Span[2], glm::dvec3( 90, 100, 0 ), "span end" );
}

TEST( BevelPrimitives, InsetLineOfAMissingEdgeIsTheDefaultLine )
{
    TArray<int32_t>     Corners;
    const FDynamicMesh3 Mesh = Square( Corners );
    TArray<FLine3d>     Lines;
    ComputeInsetLineSegmentsFromEdges( Mesh, TArray<int32_t>{ 12345 }, 10.0, Lines );
    ASSERT_EQ( Lines.Num(), 1 );
    ExpectNear( Lines[0].Origin, glm::dvec3( 0, 0, 0 ), "default origin" );
    ExpectNear( Lines[0].Direction, glm::dvec3( 1, 0, 0 ), "default direction" );
}

TEST( BevelPrimitives, PolygonPlaneIsNewellNormalCentroidAndArea )
{
    const TArray<glm::dvec3> P = { glm::dvec3( 0, 0, 0 ), glm::dvec3( 100, 0, 0 ), glm::dvec3( 100, 100, 0 ),
                                   glm::dvec3( 0, 100, 0 ) };
    glm::dvec3               Normal{};
    glm::dvec3               Centroid{};
    const double            Area = PolygonTriangulation::ComputePolygonPlane( P, Normal, Centroid );
    EXPECT_NEAR( Area, 10000.0, Tol );
    // UE's convention (VectorUtil::Normal): the front of a polygon is the side it looks clockwise from, so a
    // counter-clockwise square seen from +Z faces -Z.
    ExpectNear( Normal, glm::dvec3( 0, 0, -1 ), "counter-clockwise square faces -Z" );
    ExpectNear( Centroid, glm::dvec3( 50, 50, 0 ), "centroid" );
}

TEST( BevelPrimitives, EarClipOfAConcavePolygonCoversItOnce )
{
    // An L of three 10 cm cells, counter-clockwise, tilted out of the axis planes; vertex 3 is the reflex corner.
    const TArray<glm::dvec3> P = { Tilt( 0, 0 ),   Tilt( 20, 0 ),  Tilt( 20, 10 ),
                                   Tilt( 10, 10 ), Tilt( 10, 20 ), Tilt( 0, 20 ) };
    glm::dvec3               PolygonNormal{};
    glm::dvec3               Centroid{};
    PolygonTriangulation::ComputePolygonPlane( P, PolygonNormal, Centroid );

    for ( const bool bHoleFill : { true, false } )
    {
        TArray<FIndex3i> Triangles;
        PolygonTriangulation::TriangulateSimplePolygon( P, Triangles, bHoleFill );
        ASSERT_EQ( Triangles.Num(), 4 ) << "hole fill " << bHoleFill;
        double UnsignedArea = 0;
        for ( const FIndex3i& T : Triangles )
        {
            UnsignedArea += 0.5 * glm::length( TriangleCross( P, T ) );
            // A hole fill is wound against the polygon, the plain triangulation with it (both in UE's normal).
            const double Facing = glm::dot( VectorUtil::Normal( P[T.A], P[T.B], P[T.C] ), PolygonNormal );
            if ( bHoleFill )
                EXPECT_NEAR( Facing, -1.0, 1e-9 ) << T.A << " " << T.B << " " << T.C;
            else
                EXPECT_NEAR( Facing, 1.0, 1e-9 ) << T.A << " " << T.B << " " << T.C;
        }
        // An ear across the reflex corner would cover area outside the L and push this above 300.
        EXPECT_NEAR( UnsignedArea, 300.0, 1e-6 ) << "hole fill " << bHoleFill;
    }
}

TEST( BevelPrimitives, EarClipOfATriangleHonoursHoleFillWinding )
{
    const TArray<glm::dvec3> P = { glm::dvec3( 0, 0, 0 ), glm::dvec3( 1, 0, 0 ), glm::dvec3( 0, 1, 0 ) };
    TArray<FIndex3i>        Triangles;
    PolygonTriangulation::TriangulateSimplePolygon( P, Triangles );
    ASSERT_EQ( Triangles.Num(), 1 );
    EXPECT_EQ( Triangles[0], FIndex3i( 0, 2, 1 ) );
    PolygonTriangulation::TriangulateSimplePolygon( P, Triangles, false );
    ASSERT_EQ( Triangles.Num(), 1 );
    EXPECT_EQ( Triangles[0], FIndex3i( 0, 1, 2 ) );

    PolygonTriangulation::TriangulateSimplePolygon( TArray<glm::dvec3>{ P[0], P[1] }, Triangles );
    EXPECT_EQ( Triangles.Num(), 0 );
}
