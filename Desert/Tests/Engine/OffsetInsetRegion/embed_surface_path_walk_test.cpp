// MeshSurfacePath::AddViaPlanarWalk and the EmbedSimplePathSettings branches of EmbedSimplePath: the pieces the
// Boolean's cut path (MeshMeshCut) needs on top of GroupEdgeInserter's simple embedding. A planar walk over a flat
// grid must cross exactly the edges the plane cuts; the three simplifications must each take their shortcut, and
// leave the mesh alone when off.
#include "Engine/Geometry/MeshCore/DynamicMesh/DynamicMesh3.hpp"
#include "Engine/Geometry/MeshCore/Operations/EmbedSurfacePath.hpp"

#include <gtest/gtest.h>

#include <vector>

using namespace Desert::Geometry;

namespace
{
    constexpr double Cell = 100.0; // cm

    int GridV( int i, int j )
    {
        return j * 4 + i;
    }

    // 3x3 quads of 100 cm in z = 0, vertex (i, j) at (100 i, 100 j). Quad (i, j) is triangles
    // 2 (3 j + i) = (i,j)(i+1,j)(i+1,j+1) below its diagonal and 2 (3 j + i) + 1 = (i,j)(i+1,j+1)(i,j+1) above it.
    DynamicMesh3 Grid()
    {
        DynamicMesh3 mesh;
        for ( int j = 0; j < 4; ++j )
        {
            for ( int i = 0; i < 4; ++i )
            {
                mesh.AppendVertex( glm::dvec3( i * Cell, j * Cell, 0 ) );
            }
        }
        for ( int j = 0; j < 3; ++j )
        {
            for ( int i = 0; i < 3; ++i )
            {
                mesh.AppendTriangle( GridV( i, j ), GridV( i + 1, j ), GridV( i + 1, j + 1 ) );
                mesh.AppendTriangle( GridV( i, j ), GridV( i + 1, j + 1 ), GridV( i, j + 1 ) );
            }
        }
        return mesh;
    }

    int QuadTri( int i, int j, bool bAboveDiagonal )
    {
        return 2 * ( 3 * j + i ) + ( bAboveDiagonal ? 1 : 0 );
    }

    MeshSurfacePoint EdgePointAt( const DynamicMesh3& mesh, int a, int b, const glm::dvec3& p )
    {
        const int eid = mesh.FindEdge( a, b );
        EXPECT_NE( eid, DynamicMesh3::InvalidID );
        glm::dvec3 ea{};
        glm::dvec3 eb{};
        mesh.GetEdgeV( eid, ea, eb );
        return MeshSurfacePoint::MakeEdgePoint( eid, Distance( ea, p ) / Distance( ea, eb ) );
    }

    void ExpectChain( const DynamicMesh3& mesh, const std::vector<int>& pathVertices )
    {
        for ( size_t i = 0; i + 1 < pathVertices.size(); ++i )
        {
            EXPECT_NE( mesh.FindEdge( pathVertices[i], pathVertices[i + 1] ), DynamicMesh3::InvalidID )
                 << "no mesh edge between path vertices " << i << " and " << i + 1;
        }
    }
} // namespace

// Along y = 50 from (10,50) to (290,50) the plane crosses the three diagonals and the two interior verticals, in
// that order; both ends sit inside triangles. Embedding that walk yields seven vertices on the line, chained.
TEST( EmbedSurfacePathWalk, PlanarWalkCrossesTheCutEdgesInOrder )
{
    DynamicMesh3     mesh = Grid();
    MeshSurfacePath  path( &mesh );
    const glm::dvec3 start( 10, 50, 0 );
    const glm::dvec3 end( 290, 50, 0 );
    ASSERT_TRUE( path.AddViaPlanarWalk( QuadTri( 0, 0, true ), -1, start, QuadTri( 2, 0, false ), -1, end,
                                        glm::dvec3( 0, 1, 0 ) ) );
    ASSERT_EQ( path.m_Path.size(), 7u );
    EXPECT_TRUE( path.IsConnected() );
    const std::vector<double> xs{ 10, 50, 100, 150, 200, 250, 290 };
    for ( size_t i = 0; i < xs.size(); ++i )
    {
        const SurfacePointType expected =
             ( i == 0 || i + 1 == xs.size() ) ? SurfacePointType::Triangle : SurfacePointType::Edge;
        EXPECT_EQ( path.m_Path[i].first.PointType, expected ) << "point " << i;
        EXPECT_LT( Distance( path.m_Path[i].first.Pos( &mesh ), glm::dvec3( xs[i], 50, 0 ) ), 1e-9 )
             << "point " << i;
    }
    EXPECT_EQ( path.m_Path[1].first.ElementID, mesh.FindEdge( GridV( 0, 0 ), GridV( 1, 1 ) ) );
    EXPECT_EQ( path.m_Path[2].first.ElementID, mesh.FindEdge( GridV( 1, 0 ), GridV( 1, 1 ) ) );

    std::vector<int> pathVertices;
    ASSERT_TRUE( path.EmbedSimplePath( pathVertices ) );
    ASSERT_EQ( pathVertices.size(), 7u );
    for ( size_t i = 0; i < xs.size(); ++i )
    {
        EXPECT_LT( Distance( mesh.GetVertex( pathVertices[i] ), glm::dvec3( xs[i], 50, 0 ) ), 1e-9 )
             << "vertex " << i;
    }
    ExpectChain( mesh, pathVertices );
    EXPECT_TRUE( mesh.CheckValidity( DynamicMesh3::ValidityOptions(), ValidityCheckFailMode::ReturnOnly ) );
}

// A walk whose end vertex is known stops on the first triangle touching it, and a start on an exact vertex stays
// that vertex: the path along the diagonal line y = x from vertex (0,0) to vertex (2,2) crosses only vertex (1,1).
TEST( EmbedSurfacePathWalk, PlanarWalkBetweenVerticesThroughAVertex )
{
    DynamicMesh3    mesh = Grid();
    MeshSurfacePath path( &mesh );
    ASSERT_TRUE( path.AddViaPlanarWalk( QuadTri( 0, 0, false ), GridV( 0, 0 ), glm::dvec3( 0 ), -1, GridV( 2, 2 ),
                                        glm::dvec3( 200, 200, 0 ), glm::normalize( glm::dvec3( 1, -1, 0 ) ) ) );
    ASSERT_EQ( path.m_Path.size(), 3u );
    const std::vector<int> expected{ GridV( 0, 0 ), GridV( 1, 1 ), GridV( 2, 2 ) };
    for ( size_t i = 0; i < expected.size(); ++i )
    {
        EXPECT_EQ( path.m_Path[i].first.PointType, SurfacePointType::Vertex ) << "point " << i;
        EXPECT_EQ( path.m_Path[i].first.ElementID, expected[i] ) << "point " << i;
    }
}

// An edge point 0.001 cm from vertex (1,0) snaps onto it: no vertex is added and the path runs through (1,0).
// Without the setting the same path splits the edge.
TEST( EmbedSurfacePathWalk, SnappingReplacesANearVertexCrossing )
{
    for ( const bool bSnap : { false, true } )
    {
        DynamicMesh3    mesh = Grid();
        MeshSurfacePath path( &mesh );
        path.m_Path.emplace_back( MeshSurfacePoint( GridV( 0, 0 ) ), QuadTri( 0, 0, false ) );
        path.m_Path.emplace_back( EdgePointAt( mesh, GridV( 1, 0 ), GridV( 1, 1 ), glm::dvec3( Cell, 0.001, 0 ) ),
                                  QuadTri( 1, 0, true ) );
        path.m_Path.emplace_back( MeshSurfacePoint( GridV( 2, 1 ) ), -1 );
        ASSERT_TRUE( path.IsConnected() );
        const int               vertsBefore = mesh.VertexCount();
        EmbedSimplePathSettings settings;
        settings.bSimplifyPathBySnapping = bSnap;
        std::vector<int> pathVertices;
        ASSERT_TRUE( path.EmbedSimplePath( pathVertices, true, ZeroTolerance<float> * 100, settings ) );
        ASSERT_EQ( pathVertices.size(), 3u ) << "snap " << bSnap;
        EXPECT_EQ( mesh.VertexCount(), vertsBefore + ( bSnap ? 0 : 1 ) ) << "snap " << bSnap;
        EXPECT_EQ( pathVertices[1] == GridV( 1, 0 ), bSnap );
        ExpectChain( mesh, pathVertices );
    }
}

// A B C B E becomes A B E: the loop through C is dropped; without the setting all five vertices stay.
TEST( EmbedSurfacePathWalk, LoopRemovalDropsARevisit )
{
    for ( const bool bRemove : { false, true } )
    {
        DynamicMesh3    mesh = Grid();
        MeshSurfacePath path( &mesh );
        for ( const int v : { GridV( 0, 0 ), GridV( 1, 0 ), GridV( 1, 1 ), GridV( 1, 0 ), GridV( 2, 1 ) } )
        {
            path.m_Path.emplace_back( MeshSurfacePoint( v ), -1 );
        }
        EmbedSimplePathSettings settings;
        settings.bRemovePathLoops = bRemove;
        std::vector<int> pathVertices;
        ASSERT_TRUE( path.EmbedSimplePath( pathVertices, true, ZeroTolerance<float> * 100, settings ) );
        const std::vector<int> expected = bRemove ? std::vector<int>{ GridV( 0, 0 ), GridV( 1, 0 ), GridV( 2, 1 ) }
                                                  : std::vector<int>{ GridV( 0, 0 ), GridV( 1, 0 ), GridV( 1, 1 ),
                                                                      GridV( 1, 0 ), GridV( 2, 1 ) };
        EXPECT_EQ( pathVertices, expected ) << "remove loops " << bRemove;
    }
}

// Vertex P sits 0.001 cm above edge A-C; crossing A-C right under P would split off a 0.001 cm edge. With the
// setting the edge flips to P-Q instead: no new vertex, and P joins Q directly.
TEST( EmbedSurfacePathWalk, TinySplitBecomesAFlip )
{
    for ( const bool bFlip : { false, true } )
    {
        DynamicMesh3 mesh;
        const int    a = mesh.AppendVertex( glm::dvec3( 0, 0, 0 ) );
        const int    c = mesh.AppendVertex( glm::dvec3( Cell, 0, 0 ) );
        const int    p = mesh.AppendVertex( glm::dvec3( Cell / 2, 0.001, 0 ) );
        const int    q = mesh.AppendVertex( glm::dvec3( Cell / 2, -Cell, 0 ) );
        mesh.AppendTriangle( a, c, p );
        mesh.AppendTriangle( c, a, q );
        const int tinyTri = 0;

        MeshSurfacePath path( &mesh );
        path.m_Path.emplace_back( MeshSurfacePoint( p ), tinyTri );
        path.m_Path.emplace_back( EdgePointAt( mesh, a, c, glm::dvec3( Cell / 2, 0, 0 ) ), 1 );
        path.m_Path.emplace_back( MeshSurfacePoint( q ), -1 );
        ASSERT_TRUE( path.IsConnected() );
        EmbedSimplePathSettings settings;
        settings.bAllowEdgeFlipToCollapseTinyEdges = bFlip;
        std::vector<int> pathVertices;
        ASSERT_TRUE( path.EmbedSimplePath( pathVertices, true, ZeroTolerance<float> * 100, settings ) );
        if ( bFlip )
        {
            EXPECT_EQ( pathVertices, ( std::vector<int>{ p, q } ) );
            EXPECT_EQ( mesh.VertexCount(), 4 );
            EXPECT_EQ( mesh.FindEdge( a, c ), DynamicMesh3::InvalidID );
        }
        else
        {
            ASSERT_EQ( pathVertices.size(), 3u );
            EXPECT_EQ( mesh.VertexCount(), 5 );
        }
        ExpectChain( mesh, pathVertices );
        EXPECT_TRUE( mesh.CheckValidity( DynamicMesh3::ValidityOptions(), ValidityCheckFailMode::ReturnOnly ) );
    }
}

// On a closed 100 cm cube the plane x = 60 loops all the way round, so the walk from the top face to the front
// face has two routes: 100 cm over the near top edge, or 300 cm over the back and the bottom. Shortest-first must
// take the near one.
TEST( EmbedSurfacePathWalk, PlanarWalkTakesTheShorterWayRound )
{
    DynamicMesh3 mesh;
    for ( int z = 0; z < 2; ++z )
    {
        mesh.AppendVertex( glm::dvec3( 0, 0, z * Cell ) );
        mesh.AppendVertex( glm::dvec3( Cell, 0, z * Cell ) );
        mesh.AppendVertex( glm::dvec3( Cell, Cell, z * Cell ) );
        mesh.AppendVertex( glm::dvec3( 0, Cell, z * Cell ) );
    }
    const int tris[12][3] = { { 0, 2, 1 }, { 0, 3, 2 }, { 4, 5, 6 }, { 4, 6, 7 }, { 0, 1, 5 }, { 0, 5, 4 },
                              { 3, 7, 6 }, { 3, 6, 2 }, { 0, 4, 7 }, { 0, 7, 3 }, { 1, 2, 6 }, { 1, 6, 5 } };
    for ( const auto& t : tris )
    {
        mesh.AppendTriangle( t[0], t[1], t[2] );
    }
    ASSERT_TRUE( mesh.IsClosed() );

    MeshSurfacePath  path( &mesh );
    const glm::dvec3 start( 60, 40, Cell ); // top face, triangle (4,5,6)
    const glm::dvec3 end( 60, 0, 40 );      // front face, triangle (0,1,5)
    ASSERT_TRUE( path.AddViaPlanarWalk( 2, -1, start, 4, -1, end, glm::dvec3( 1, 0, 0 ) ) );
    EXPECT_TRUE( path.IsConnected() );
    double length = 0;
    for ( size_t i = 0; i + 1 < path.m_Path.size(); ++i )
    {
        length += Distance( path.m_Path[i].first.Pos( &mesh ), path.m_Path[i + 1].first.Pos( &mesh ) );
    }
    EXPECT_NEAR( length, 100.0, 1e-9 );
    ASSERT_EQ( path.m_Path.size(), 4u );
    EXPECT_LT( Distance( path.m_Path[1].first.Pos( &mesh ), glm::dvec3( 60, 0, Cell ) ), 1e-9 );
    EXPECT_LT( Distance( path.m_Path[2].first.Pos( &mesh ), glm::dvec3( 60, 0, 60 ) ), 1e-9 );
}
