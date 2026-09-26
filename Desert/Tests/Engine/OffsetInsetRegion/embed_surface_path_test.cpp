// MeshSurfacePath::EmbedSimplePath: the GeometryCore piece UE's GroupEdgeInserter uses to cut a plane path into
// the mesh (EmbedPlaneCutPath). A path given as surface points must come out as a chain of mesh edges, with the
// closed mesh still closed.
#include "Engine/Geometry/MeshCore/Distance/DistPoint3Triangle3.hpp"
#include "Engine/Geometry/MeshCore/DynamicMesh/DynamicMesh3.hpp"
#include "Engine/Geometry/MeshCore/Operations/EmbedSurfacePath.hpp"

#include <gtest/gtest.h>

#include <vector>

using namespace Desert::Geometry;

namespace
{
    constexpr double Size = 100.0; // cm

    // Closed 100 cm cube. The top quad (z = 100) is triangles 2 = (4,5,6) and 3 = (4,6,7), diagonal 4-6.
    DynamicMesh3 Cube()
    {
        DynamicMesh3 mesh;
        for ( int z = 0; z < 2; ++z )
        {
            mesh.AppendVertex( glm::dvec3( 0, 0, z * Size ) );
            mesh.AppendVertex( glm::dvec3( Size, 0, z * Size ) );
            mesh.AppendVertex( glm::dvec3( Size, Size, z * Size ) );
            mesh.AppendVertex( glm::dvec3( 0, Size, z * Size ) );
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
    MeshSurfacePoint EdgePoint( const DynamicMesh3& mesh, int a, int b, const glm::dvec3& p )
    {
        const int eid = mesh.FindEdge( a, b );
        EXPECT_NE( eid, DynamicMesh3::InvalidID );
        glm::dvec3 ea{};
        glm::dvec3 eb{};
        mesh.GetEdgeV( eid, ea, eb );
        return MeshSurfacePoint::MakeEdgePoint( eid, Distance( ea, p ) / Distance( ea, eb ) );
    }

    void ExpectEmbedded( const DynamicMesh3& mesh, const std::vector<int>& pathVertices,
                         const std::vector<glm::dvec3>& expected )
    {
        ASSERT_EQ( static_cast<int32_t>( pathVertices.size() ), (int)expected.size() );
        for ( int i = 0; i < static_cast<int32_t>( pathVertices.size() ); ++i )
        {
            EXPECT_LT( Distance( mesh.GetVertex( pathVertices[i] ), expected[i] ), 1e-9 ) << "path vertex " << i;
            if ( i + 1 < static_cast<int32_t>( pathVertices.size() ) )
            {
                EXPECT_NE( mesh.FindEdge( pathVertices[i], pathVertices[i + 1] ), DynamicMesh3::InvalidID )
                     << "no mesh edge between path vertices " << i << " and " << i + 1;
            }
        }
        EXPECT_TRUE( mesh.IsClosed() );
        EXPECT_TRUE( mesh.CheckValidity( DynamicMesh3::ValidityOptions(), ValidityCheckFailMode::ReturnOnly ) );
    }
} // namespace

TEST( EmbedSurfacePath, StraightEdgePathAcrossAQuadBecomesMeshEdges )
{
    DynamicMesh3     mesh = Cube();
    MeshSurfacePath  path( &mesh );
    const glm::dvec3 p0( 50, 0, Size );
    const glm::dvec3 p1( 50, 50, Size );
    const glm::dvec3 p2( 50, Size, Size );
    path.m_Path.emplace_back( EdgePoint( mesh, 4, 5, p0 ), 2 );
    path.m_Path.emplace_back( EdgePoint( mesh, 4, 6, p1 ), 3 );
    path.m_Path.emplace_back( EdgePoint( mesh, 7, 6, p2 ), DynamicMesh3::InvalidID );
    ASSERT_TRUE( path.IsConnected() );
    EXPECT_LT( Distance( path.m_Path[1].first.Pos( &mesh ), p1 ), 1e-9 );

    std::vector<int> pathVertices;
    ASSERT_TRUE( path.EmbedSimplePath( pathVertices ) );
    // Three edge splits, each of an interior edge: two triangles more per split.
    EXPECT_EQ( mesh.TriangleCount(), 12 + 3 * 2 );
    ExpectEmbedded( mesh, pathVertices, { p0, p1, p2 } );
}

TEST( EmbedSurfacePath, TriangleEndPointsArePokedAndTheEndIsRelocatedAfterTheSplit )
{
    DynamicMesh3    mesh = Cube();
    MeshSurfacePath path( &mesh );
    // (60,20) inside triangle 2 = (4,5,6); (40,80) inside triangle 3 = (4,6,7); the line crosses the diagonal at
    // (50,50). Splitting the diagonal cuts triangle 3 in two, so the end point must be found again before the
    // poke.
    const glm::dvec3 start( 60, 20, Size );
    const glm::dvec3 cross( 50, 50, Size );
    const glm::dvec3 end( 40, 80, Size );
    path.m_Path.emplace_back( MeshSurfacePoint( 2, glm::dvec3( 0.4, 0.4, 0.2 ) ), 2 );
    path.m_Path.emplace_back( EdgePoint( mesh, 4, 6, cross ), 3 );
    path.m_Path.emplace_back( MeshSurfacePoint( 3, glm::dvec3( 0.2, 0.4, 0.4 ) ), DynamicMesh3::InvalidID );
    ASSERT_TRUE( path.IsConnected() );
    ASSERT_LT( Distance( path.m_Path[0].first.Pos( &mesh ), start ), 1e-9 );
    ASSERT_LT( Distance( path.m_Path[2].first.Pos( &mesh ), end ), 1e-9 );

    std::vector<int> pathVertices;
    ASSERT_TRUE( path.EmbedSimplePath( pathVertices ) );
    EXPECT_EQ( mesh.TriangleCount(), 12 + 2 + 2 + 2 );
    ExpectEmbedded( mesh, pathVertices, { start, cross, end } );
}

TEST( EmbedSurfacePath, PathStartingInsideATriangleGetsItsFirstVertexAtThatExactPoint )
{
    DynamicMesh3    mesh = Cube();
    MeshSurfacePath path( &mesh );
    // Triangle 2 = (4,5,6) = (0,0), (100,0), (100,100) at z = 100. Three unequal weights, so the point is neither
    // a vertex, nor on an edge, nor the centroid a poke without its barycentric coordinate would land on.
    const glm::dvec3 start( 50, 20, Size );
    const glm::dvec3 corner( Size, 0, Size ); // vertex 5
    path.m_Path.emplace_back( MeshSurfacePoint( 2, glm::dvec3( 0.5, 0.3, 0.2 ) ), 2 );
    path.m_Path.emplace_back( MeshSurfacePoint( 5 ), DynamicMesh3::InvalidID );
    ASSERT_TRUE( path.IsConnected() );
    ASSERT_LT( Distance( path.m_Path[0].first.Pos( &mesh ), start ), 1e-9 );

    std::vector<int> pathVertices;
    ASSERT_TRUE( path.EmbedSimplePath( pathVertices ) );
    // One poke: one triangle becomes three.
    EXPECT_EQ( mesh.TriangleCount(), 12 + 2 );
    ASSERT_EQ( static_cast<int32_t>( pathVertices.size() ), 2 );
    EXPECT_EQ( pathVertices[0], 8 ) << "the poke appends the path's first vertex";
    EXPECT_EQ( pathVertices[1], 5 );
    ExpectEmbedded( mesh, pathVertices, { start, corner } );
}

TEST( EmbedSurfacePath, AppendedPathDoesNotRepeatTheSharedVertexUnlessAsked )
{
    for ( const bool dedupe : { true, false } )
    {
        DynamicMesh3     mesh = Cube();
        std::vector<int> pathVertices;
        MeshSurfacePath  first( &mesh );
        first.m_Path.emplace_back( MeshSurfacePoint( 5 ), 2 );
        first.m_Path.emplace_back( MeshSurfacePoint( 6 ), DynamicMesh3::InvalidID );
        ASSERT_TRUE( first.EmbedSimplePath( pathVertices, dedupe ) );
        MeshSurfacePath second( &mesh );
        second.m_Path.emplace_back( MeshSurfacePoint( 6 ), 3 );
        second.m_Path.emplace_back( MeshSurfacePoint( 7 ), DynamicMesh3::InvalidID );
        ASSERT_TRUE( second.EmbedSimplePath( pathVertices, dedupe ) );
        const std::vector<int> expected = dedupe ? std::vector<int>{ 5, 6, 7 } : std::vector<int>{ 5, 6, 6, 7 };
        EXPECT_EQ( std::vector<int>( pathVertices.begin(), pathVertices.end() ), expected ) << "dedupe " << dedupe;
        EXPECT_EQ( mesh.TriangleCount(), 12 );
    }
}

TEST( EmbedSurfacePath, PathThatLeavesItsWalkingTriangleIsNotConnected )
{
    DynamicMesh3    mesh = Cube();
    MeshSurfacePath path( &mesh );
    path.m_Path.emplace_back( EdgePoint( mesh, 4, 5, glm::dvec3( 50, 0, Size ) ),
                              0 ); // triangle 0 is the bottom face
    path.m_Path.emplace_back( EdgePoint( mesh, 4, 6, glm::dvec3( 50, 50, Size ) ), DynamicMesh3::InvalidID );
    EXPECT_FALSE( path.IsConnected() );
}

TEST( DistPoint3Triangle3, BarycentricCoordinatesOfTheClosestPoint )
{
    const Triangle3d     tri( glm::dvec3( 0, 0, 0 ), glm::dvec3( 100, 0, 0 ), glm::dvec3( 0, 100, 0 ) );
    DistPoint3Triangle3d above( glm::dvec3( 20, 30, 7 ), tri );
    EXPECT_NEAR( above.GetSquared(), 49.0, 1e-9 );
    EXPECT_NEAR( above.m_TriangleBaryCoords[0], 0.5, 1e-12 );
    EXPECT_NEAR( above.m_TriangleBaryCoords[1], 0.2, 1e-12 );
    EXPECT_NEAR( above.m_TriangleBaryCoords[2], 0.3, 1e-12 );
    // Outside across the hypotenuse: the closest point is (50,50,0) on edge 1-2.
    DistPoint3Triangle3d outside( glm::dvec3( 60, 60, 0 ), tri );
    EXPECT_NEAR( outside.GetSquared(), 200.0, 1e-9 );
    EXPECT_NEAR( outside.m_TriangleBaryCoords[0], 0.0, 1e-12 );
    EXPECT_NEAR( outside.m_TriangleBaryCoords[1], 0.5, 1e-12 );
    EXPECT_NEAR( outside.m_TriangleBaryCoords[2], 0.5, 1e-12 );
}
