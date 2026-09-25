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

    void ExpectNear( const FVector3d& Actual, const FVector3d& Expected, const char* What )
    {
        EXPECT_NEAR( Actual.X, Expected.X, Tol ) << What;
        EXPECT_NEAR( Actual.Y, Expected.Y, Tol ) << What;
        EXPECT_NEAR( Actual.Z, Expected.Z, Tol ) << What;
    }

    // A 100 x 100 cm square in the XY plane, two triangles, counter-clockwise corners 0..3 from the origin.
    FDynamicMesh3 Square( TArray<int32>& CornersOut )
    {
        FDynamicMesh3 Mesh;
        CornersOut.Reset();
        CornersOut.Add( Mesh.AppendVertex( FVector3d( 0, 0, 0 ) ) );
        CornersOut.Add( Mesh.AppendVertex( FVector3d( 100, 0, 0 ) ) );
        CornersOut.Add( Mesh.AppendVertex( FVector3d( 100, 100, 0 ) ) );
        CornersOut.Add( Mesh.AppendVertex( FVector3d( 0, 100, 0 ) ) );
        Mesh.AppendTriangle( CornersOut[0], CornersOut[1], CornersOut[2] );
        Mesh.AppendTriangle( CornersOut[0], CornersOut[2], CornersOut[3] );
        return Mesh;
    }

    // Rotates about X by 30 degrees and shifts, so the polygon tests do not run in an axis plane.
    FVector3d Tilt( double X, double Y )
    {
        const double c = std::cos( 0.5235987755982988 ), s = std::sin( 0.5235987755982988 );
        return FVector3d( X + 7.0, Y * c - 3.0, Y * s + 11.0 );
    }

    FVector3d TriangleCross( const TArray<FVector3d>& P, const FIndex3i& T )
    {
        return ( P[T.B] - P[T.A] ).Cross( P[T.C] - P[T.A] );
    }
} // namespace

TEST( BevelPrimitives, Line3FromPointsIsUnitAndProjects )
{
    const FLine3d Line = FLine3d::FromPoints( FVector3d( 1, 2, 3 ), FVector3d( 1, 2, 13 ) );
    ExpectNear( Line.Direction, FVector3d( 0, 0, 1 ), "direction" );
    EXPECT_NEAR( Line.Project( FVector3d( 5, 5, 8 ) ), 5.0, Tol );
    ExpectNear( Line.PointAt( 5.0 ), FVector3d( 1, 2, 8 ), "point at" );
    EXPECT_NEAR( Line.DistanceSquared( FVector3d( 4, 6, 0 ) ), 25.0, Tol );
}

TEST( BevelPrimitives, DistLine3Line3SkewLines )
{
    // X axis, and a Y-parallel line 5 cm above it through (3, 2, 5): closest pair (3,0,0)-(3,0,5).
    FDistLine3Line3d Distance( FLine3d( FVector3d( 0, 0, 0 ), FVector3d( 1, 0, 0 ) ),
                               FLine3d( FVector3d( 3, 2, 5 ), FVector3d( 0, 1, 0 ) ) );
    EXPECT_NEAR( Distance.Get(), 5.0, Tol );
    EXPECT_FALSE( Distance.bIsParallel );
    ExpectNear( Distance.Line1ClosestPoint, FVector3d( 3, 0, 0 ), "line 1 closest" );
    ExpectNear( Distance.Line2ClosestPoint, FVector3d( 3, 0, 5 ), "line 2 closest" );
    EXPECT_NEAR( Distance.Line1Parameter, 3.0, Tol );
    EXPECT_NEAR( Distance.Line2Parameter, -2.0, Tol );

    // At 45 degrees the cross term of the solve matters: projections cross at (2, 0), 5 cm apart in Z.
    FDistLine3Line3d Oblique( FLine3d( FVector3d( 0, 0, 0 ), FVector3d( 1, 0, 0 ) ),
                              FLine3d( FVector3d( 3, 1, 5 ), Normalized( FVector3d( 1, 1, 0 ) ) ) );
    EXPECT_NEAR( Oblique.Get(), 5.0, Tol );
    ExpectNear( Oblique.Line1ClosestPoint, FVector3d( 2, 0, 0 ), "oblique line 1 closest" );
    ExpectNear( Oblique.Line2ClosestPoint, FVector3d( 2, 0, 5 ), "oblique line 2 closest" );
    EXPECT_NEAR( Oblique.Line2Parameter, -std::sqrt( 2.0 ), Tol );
}

TEST( BevelPrimitives, DistLine3Line3ParallelLines )
{
    FDistLine3Line3d Distance( FLine3d( FVector3d( 4, 0, 0 ), FVector3d( 1, 0, 0 ) ),
                               FLine3d( FVector3d( -9, 3, 4 ), FVector3d( -1, 0, 0 ) ) );
    EXPECT_NEAR( Distance.GetSquared(), 25.0, Tol );
    EXPECT_TRUE( Distance.bIsParallel );
    ExpectNear( Distance.Line1ClosestPoint, FVector3d( 4, 0, 0 ), "line 1 origin" );
    ExpectNear( Distance.Line2ClosestPoint, FVector3d( 4, 3, 4 ), "projection onto line 2" );
}

TEST( BevelPrimitives, InsetPairIsMidpointOfClosestPoints )
{
    // Skew lines: the solve returns the midpoint of the closest pair, not either point.
    const FVector3d P = SolveInsetVertexPositionFromLinePair(
         FVector3d( 50, 50, 50 ), FLine3d( FVector3d( 0, 0, 0 ), FVector3d( 1, 0, 0 ) ),
         FLine3d( FVector3d( 3, 2, 6 ), FVector3d( 0, 1, 0 ) ) );
    ExpectNear( P, FVector3d( 3, 0, 3 ), "skew midpoint" );

    // Nearly parallel (|dot| > 0.999): nearest point on the first line to the vertex.
    const FVector3d Q = SolveInsetVertexPositionFromLinePair(
         FVector3d( 20, 7, 0 ), FLine3d( FVector3d( 0, 0, 0 ), FVector3d( 1, 0, 0 ) ),
         FLine3d( FVector3d( 0, 5, 0 ), Normalized( FVector3d( 1, 0.01, 0 ) ) ) );
    ExpectNear( Q, FVector3d( 20, 0, 0 ), "parallel fallback" );
}

TEST( BevelPrimitives, InsetLinesOfASquareLoopSolveToTheInnerSquare )
{
    TArray<int32>       Corners;
    const FDynamicMesh3 Mesh = Square( Corners );
    TArray<int32>       Edges;
    for ( int32 i = 0; i < 4; ++i )
        Edges.Add( Mesh.FindEdge( Corners[i], Corners[( i + 1 ) % 4] ) );

    TArray<FLine3d> Lines;
    ComputeInsetLineSegmentsFromEdges( Mesh, Edges, 10.0, Lines );
    ASSERT_EQ( Lines.Num(), 4 );
    // Each line runs along its edge, moved 10 cm towards the square's inside.
    EXPECT_NEAR( Lines[0].DistanceSquared( FVector3d( 50, 10, 0 ) ), 0.0, Tol );
    EXPECT_NEAR( Lines[1].DistanceSquared( FVector3d( 90, 50, 0 ) ), 0.0, Tol );
    EXPECT_NEAR( Lines[2].DistanceSquared( FVector3d( 50, 90, 0 ) ), 0.0, Tol );
    EXPECT_NEAR( Lines[3].DistanceSquared( FVector3d( 10, 50, 0 ) ), 0.0, Tol );

    TArray<FVector3d> Loop;
    SolveInsetVertexPositionsFromInsetLines( Mesh, Lines, Corners, Loop, true );
    ASSERT_EQ( Loop.Num(), 4 );
    ExpectNear( Loop[0], FVector3d( 10, 10, 0 ), "loop corner 0" );
    ExpectNear( Loop[1], FVector3d( 90, 10, 0 ), "loop corner 1" );
    ExpectNear( Loop[2], FVector3d( 90, 90, 0 ), "loop corner 2" );
    ExpectNear( Loop[3], FVector3d( 10, 90, 0 ), "loop corner 3" );

    // An open span 0-1-2 over edges 0 and 1: the ends are projected onto their one line.
    TArray<int32>     SpanVertices = { Corners[0], Corners[1], Corners[2] };
    TArray<FLine3d>   SpanLines    = { Lines[0], Lines[1] };
    TArray<FVector3d> Span;
    SolveInsetVertexPositionsFromInsetLines( Mesh, SpanLines, SpanVertices, Span, false );
    ASSERT_EQ( Span.Num(), 3 );
    ExpectNear( Span[0], FVector3d( 0, 10, 0 ), "span start" );
    ExpectNear( Span[1], FVector3d( 90, 10, 0 ), "span middle" );
    ExpectNear( Span[2], FVector3d( 90, 100, 0 ), "span end" );
}

TEST( BevelPrimitives, InsetLineOfAMissingEdgeIsTheDefaultLine )
{
    TArray<int32>       Corners;
    const FDynamicMesh3 Mesh = Square( Corners );
    TArray<FLine3d>     Lines;
    ComputeInsetLineSegmentsFromEdges( Mesh, TArray<int32>{ 12345 }, 10.0, Lines );
    ASSERT_EQ( Lines.Num(), 1 );
    ExpectNear( Lines[0].Origin, FVector3d( 0, 0, 0 ), "default origin" );
    ExpectNear( Lines[0].Direction, FVector3d( 1, 0, 0 ), "default direction" );
}

TEST( BevelPrimitives, PolygonPlaneIsNewellNormalCentroidAndArea )
{
    const TArray<FVector3d> P = { FVector3d( 0, 0, 0 ), FVector3d( 100, 0, 0 ), FVector3d( 100, 100, 0 ),
                                  FVector3d( 0, 100, 0 ) };
    FVector3d               Normal, Centroid;
    const double            Area = PolygonTriangulation::ComputePolygonPlane( P, Normal, Centroid );
    EXPECT_NEAR( Area, 10000.0, Tol );
    // UE's convention (VectorUtil::Normal): the front of a polygon is the side it looks clockwise from, so a
    // counter-clockwise square seen from +Z faces -Z.
    ExpectNear( Normal, FVector3d( 0, 0, -1 ), "counter-clockwise square faces -Z" );
    ExpectNear( Centroid, FVector3d( 50, 50, 0 ), "centroid" );
}

TEST( BevelPrimitives, EarClipOfAConcavePolygonCoversItOnce )
{
    // An L of three 10 cm cells, counter-clockwise, tilted out of the axis planes; vertex 3 is the reflex corner.
    const TArray<FVector3d> P = { Tilt( 0, 0 ),   Tilt( 20, 0 ),  Tilt( 20, 10 ),
                                  Tilt( 10, 10 ), Tilt( 10, 20 ), Tilt( 0, 20 ) };
    FVector3d               PolygonNormal, Centroid;
    PolygonTriangulation::ComputePolygonPlane( P, PolygonNormal, Centroid );

    for ( const bool bHoleFill : { true, false } )
    {
        TArray<FIndex3i> Triangles;
        PolygonTriangulation::TriangulateSimplePolygon( P, Triangles, bHoleFill );
        ASSERT_EQ( Triangles.Num(), 4 ) << "hole fill " << bHoleFill;
        double UnsignedArea = 0;
        for ( const FIndex3i& T : Triangles )
        {
            UnsignedArea += 0.5 * TriangleCross( P, T ).Length();
            // A hole fill is wound against the polygon, the plain triangulation with it (both in UE's normal).
            const double Facing = VectorUtil::Normal( P[T.A], P[T.B], P[T.C] ).Dot( PolygonNormal );
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
    const TArray<FVector3d> P = { FVector3d( 0, 0, 0 ), FVector3d( 1, 0, 0 ), FVector3d( 0, 1, 0 ) };
    TArray<FIndex3i>        Triangles;
    PolygonTriangulation::TriangulateSimplePolygon( P, Triangles );
    ASSERT_EQ( Triangles.Num(), 1 );
    EXPECT_EQ( Triangles[0], FIndex3i( 0, 2, 1 ) );
    PolygonTriangulation::TriangulateSimplePolygon( P, Triangles, false );
    ASSERT_EQ( Triangles.Num(), 1 );
    EXPECT_EQ( Triangles[0], FIndex3i( 0, 1, 2 ) );

    PolygonTriangulation::TriangulateSimplePolygon( TArray<FVector3d>{ P[0], P[1] }, Triangles );
    EXPECT_EQ( Triangles.Num(), 0 );
}
