// FMeshSurfacePath::EmbedSimplePath: the GeometryCore piece UE's FGroupEdgeInserter uses to cut a plane path into
// the mesh (EmbedPlaneCutPath). A path given as surface points must come out as a chain of mesh edges, with the
// closed mesh still closed.
#include "Engine/Geometry/UECore/Distance/DistPoint3Triangle3.hpp"
#include "Engine/Geometry/UECore/DynamicMesh/DynamicMesh3.hpp"
#include "Engine/Geometry/UECore/Operations/EmbedSurfacePath.hpp"

#include <gtest/gtest.h>

#include <vector>

using namespace Desert::Geometry;

namespace
{
    constexpr double Size = 100.0; // cm

    // Closed 100 cm cube. The top quad (z = 100) is triangles 2 = (4,5,6) and 3 = (4,6,7), diagonal 4-6.
    FDynamicMesh3 Cube()
    {
        FDynamicMesh3 mesh;
        for ( int z = 0; z < 2; ++z )
        {
            mesh.AppendVertex( FVector3d( 0, 0, z * Size ) );
            mesh.AppendVertex( FVector3d( Size, 0, z * Size ) );
            mesh.AppendVertex( FVector3d( Size, Size, z * Size ) );
            mesh.AppendVertex( FVector3d( 0, Size, z * Size ) );
        }
        const int tris[12][3] = { { 0, 2, 1 }, { 0, 3, 2 }, { 4, 5, 6 }, { 4, 6, 7 }, { 0, 1, 5 }, { 0, 5, 4 },
                                  { 3, 7, 6 }, { 3, 6, 2 }, { 0, 4, 7 }, { 0, 7, 3 }, { 1, 2, 6 }, { 1, 6, 5 } };
        for ( int t = 0; t < 12; ++t )
        {
            EXPECT_EQ( mesh.AppendTriangle( tris[t][0], tris[t][1], tris[t][2] ), t );
        }
        EXPECT_TRUE( mesh.IsClosed() );
        return mesh;
    }

    // Surface point on the edge a-b at world position p, with the lerp parameter in the edge's own vertex order.
    FMeshSurfacePoint EdgePoint( const FDynamicMesh3& mesh, int a, int b, const FVector3d& p )
    {
        const int eid = mesh.FindEdge( a, b );
        EXPECT_NE( eid, FDynamicMesh3::InvalidID );
        FVector3d ea;
        FVector3d eb;
        mesh.GetEdgeV( eid, ea, eb );
        return FMeshSurfacePoint::MakeEdgePoint( eid, Distance( ea, p ) / Distance( ea, eb ) );
    }

    void ExpectEmbedded( const FDynamicMesh3& mesh, const TArray<int>& pathVertices,
                         const std::vector<FVector3d>& expected )
    {
        ASSERT_EQ( pathVertices.Num(), (int)expected.size() );
        for ( int i = 0; i < pathVertices.Num(); ++i )
        {
            EXPECT_LT( Distance( mesh.GetVertex( pathVertices[i] ), expected[i] ), 1e-9 ) << "path vertex " << i;
            if ( i + 1 < pathVertices.Num() )
            {
                EXPECT_NE( mesh.FindEdge( pathVertices[i], pathVertices[i + 1] ), FDynamicMesh3::InvalidID )
                     << "no mesh edge between path vertices " << i << " and " << i + 1;
            }
        }
        EXPECT_TRUE( mesh.IsClosed() );
        EXPECT_TRUE( mesh.CheckValidity( FDynamicMesh3::FValidityOptions(), EValidityCheckFailMode::ReturnOnly ) );
    }
} // namespace

TEST( EmbedSurfacePath, StraightEdgePathAcrossAQuadBecomesMeshEdges )
{
    FDynamicMesh3    mesh = Cube();
    FMeshSurfacePath path( &mesh );
    const FVector3d  p0( 50, 0, Size );
    const FVector3d  p1( 50, 50, Size );
    const FVector3d  p2( 50, Size, Size );
    path.Path.Emplace( EdgePoint( mesh, 4, 5, p0 ), 2 );
    path.Path.Emplace( EdgePoint( mesh, 4, 6, p1 ), 3 );
    path.Path.Emplace( EdgePoint( mesh, 7, 6, p2 ), FDynamicMesh3::InvalidID );
    ASSERT_TRUE( path.IsConnected() );
    EXPECT_LT( Distance( path.Path[1].Key.Pos( &mesh ), p1 ), 1e-9 );

    TArray<int> pathVertices;
    ASSERT_TRUE( path.EmbedSimplePath( pathVertices ) );
    // Three edge splits, each of an interior edge: two triangles more per split.
    EXPECT_EQ( mesh.TriangleCount(), 12 + 3 * 2 );
    ExpectEmbedded( mesh, pathVertices, { p0, p1, p2 } );
}

TEST( EmbedSurfacePath, TriangleEndPointsArePokedAndTheEndIsRelocatedAfterTheSplit )
{
    FDynamicMesh3    mesh = Cube();
    FMeshSurfacePath path( &mesh );
    // (60,20) inside triangle 2 = (4,5,6); (40,80) inside triangle 3 = (4,6,7); the line crosses the diagonal at
    // (50,50). Splitting the diagonal cuts triangle 3 in two, so the end point must be found again before the
    // poke.
    const FVector3d start( 60, 20, Size );
    const FVector3d cross( 50, 50, Size );
    const FVector3d end( 40, 80, Size );
    path.Path.Emplace( FMeshSurfacePoint( 2, FVector3d( 0.4, 0.4, 0.2 ) ), 2 );
    path.Path.Emplace( EdgePoint( mesh, 4, 6, cross ), 3 );
    path.Path.Emplace( FMeshSurfacePoint( 3, FVector3d( 0.2, 0.4, 0.4 ) ), FDynamicMesh3::InvalidID );
    ASSERT_TRUE( path.IsConnected() );
    ASSERT_LT( Distance( path.Path[0].Key.Pos( &mesh ), start ), 1e-9 );
    ASSERT_LT( Distance( path.Path[2].Key.Pos( &mesh ), end ), 1e-9 );

    TArray<int> pathVertices;
    ASSERT_TRUE( path.EmbedSimplePath( pathVertices ) );
    EXPECT_EQ( mesh.TriangleCount(), 12 + 2 + 2 + 2 );
    ExpectEmbedded( mesh, pathVertices, { start, cross, end } );
}

TEST( EmbedSurfacePath, PathStartingInsideATriangleGetsItsFirstVertexAtThatExactPoint )
{
    FDynamicMesh3    mesh = Cube();
    FMeshSurfacePath path( &mesh );
    // Triangle 2 = (4,5,6) = (0,0), (100,0), (100,100) at z = 100. Three unequal weights, so the point is neither
    // a vertex, nor on an edge, nor the centroid a poke without its barycentric coordinate would land on.
    const FVector3d start( 50, 20, Size );
    const FVector3d corner( Size, 0, Size ); // vertex 5
    path.Path.Emplace( FMeshSurfacePoint( 2, FVector3d( 0.5, 0.3, 0.2 ) ), 2 );
    path.Path.Emplace( FMeshSurfacePoint( 5 ), FDynamicMesh3::InvalidID );
    ASSERT_TRUE( path.IsConnected() );
    ASSERT_LT( Distance( path.Path[0].Key.Pos( &mesh ), start ), 1e-9 );

    TArray<int> pathVertices;
    ASSERT_TRUE( path.EmbedSimplePath( pathVertices ) );
    // One poke: one triangle becomes three.
    EXPECT_EQ( mesh.TriangleCount(), 12 + 2 );
    ASSERT_EQ( pathVertices.Num(), 2 );
    EXPECT_EQ( pathVertices[0], 8 ) << "the poke appends the path's first vertex";
    EXPECT_EQ( pathVertices[1], 5 );
    ExpectEmbedded( mesh, pathVertices, { start, corner } );
}

TEST( EmbedSurfacePath, AppendedPathDoesNotRepeatTheSharedVertexUnlessAsked )
{
    for ( const bool dedupe : { true, false } )
    {
        FDynamicMesh3    mesh = Cube();
        TArray<int>      pathVertices;
        FMeshSurfacePath first( &mesh );
        first.Path.Emplace( FMeshSurfacePoint( 5 ), 2 );
        first.Path.Emplace( FMeshSurfacePoint( 6 ), FDynamicMesh3::InvalidID );
        ASSERT_TRUE( first.EmbedSimplePath( pathVertices, dedupe ) );
        FMeshSurfacePath second( &mesh );
        second.Path.Emplace( FMeshSurfacePoint( 6 ), 3 );
        second.Path.Emplace( FMeshSurfacePoint( 7 ), FDynamicMesh3::InvalidID );
        ASSERT_TRUE( second.EmbedSimplePath( pathVertices, dedupe ) );
        const std::vector<int> expected = dedupe ? std::vector<int>{ 5, 6, 7 } : std::vector<int>{ 5, 6, 6, 7 };
        EXPECT_EQ( std::vector<int>( pathVertices.begin(), pathVertices.end() ), expected ) << "dedupe " << dedupe;
        EXPECT_EQ( mesh.TriangleCount(), 12 );
    }
}

TEST( EmbedSurfacePath, PathThatLeavesItsWalkingTriangleIsNotConnected )
{
    FDynamicMesh3    mesh = Cube();
    FMeshSurfacePath path( &mesh );
    path.Path.Emplace( EdgePoint( mesh, 4, 5, FVector3d( 50, 0, Size ) ), 0 ); // triangle 0 is the bottom face
    path.Path.Emplace( EdgePoint( mesh, 4, 6, FVector3d( 50, 50, Size ) ), FDynamicMesh3::InvalidID );
    EXPECT_FALSE( path.IsConnected() );
}

TEST( DistPoint3Triangle3, BarycentricCoordinatesOfTheClosestPoint )
{
    const FTriangle3d     tri( FVector3d( 0, 0, 0 ), FVector3d( 100, 0, 0 ), FVector3d( 0, 100, 0 ) );
    FDistPoint3Triangle3d above( FVector3d( 20, 30, 7 ), tri );
    EXPECT_NEAR( above.GetSquared(), 49.0, 1e-9 );
    EXPECT_NEAR( above.TriangleBaryCoords[0], 0.5, 1e-12 );
    EXPECT_NEAR( above.TriangleBaryCoords[1], 0.2, 1e-12 );
    EXPECT_NEAR( above.TriangleBaryCoords[2], 0.3, 1e-12 );
    // Outside across the hypotenuse: the closest point is (50,50,0) on edge 1-2.
    FDistPoint3Triangle3d outside( FVector3d( 60, 60, 0 ), tri );
    EXPECT_NEAR( outside.GetSquared(), 200.0, 1e-9 );
    EXPECT_NEAR( outside.TriangleBaryCoords[0], 0.0, 1e-12 );
    EXPECT_NEAR( outside.TriangleBaryCoords[1], 0.5, 1e-12 );
    EXPECT_NEAR( outside.TriangleBaryCoords[2], 0.5, 1e-12 );
}
