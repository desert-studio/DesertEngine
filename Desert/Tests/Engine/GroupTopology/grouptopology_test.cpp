// FGroupTopology (ported from UE 5.8 GroupTopology.cpp) on meshes whose UE answer is known by construction:
// a polygrouped cube (6 groups, 8 corners, 12 group edges), a capped cylinder (3 groups, NO corners - every
// ring vertex meets exactly two group edges - and two closed loops), an open tube, and a cube whose top is
// its own group (one corner-free loop). The invariants below hold for any mesh: every group boundary closes,
// and every mesh edge between two groups belongs to exactly one group edge.
#include <gtest/gtest.h>

#include <Engine/Geometry/UECore/DynamicMesh/DynamicMesh3.hpp>
#include <Engine/Geometry/UECore/DynamicMesh/GroupTopology.hpp>
#include <Engine/Geometry/DynamicMeshSelection.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <map>

using namespace Desert::Geometry;

namespace
{
    constexpr double Side = 100.0; // centimetres

    // Quads wound counter-clockwise seen from outside; face f gets group GroupOf[f].
    FDynamicMesh3 MakeCube( const int ( &GroupOf )[6] )
    {
        FDynamicMesh3 Mesh;
        Mesh.EnableTriangleGroups();
        for ( int i = 0; i < 8; ++i )
            Mesh.AppendVertex(
                 glm::dvec3( ( i & 1 ) * Side, ( ( i >> 1 ) & 1 ) * Side, ( ( i >> 2 ) & 1 ) * Side ) );
        const int Quads[6][4] = { { 0, 2, 3, 1 }, { 4, 5, 7, 6 }, { 0, 1, 5, 4 },
                                  { 2, 6, 7, 3 }, { 0, 4, 6, 2 }, { 1, 3, 7, 5 } };
        for ( int f = 0; f < 6; ++f )
        {
            const auto& Q = Quads[f];
            EXPECT_GE( Mesh.AppendTriangle( Q[0], Q[1], Q[2], GroupOf[f] ), 0 );
            EXPECT_GE( Mesh.AppendTriangle( Q[0], Q[2], Q[3], GroupOf[f] ), 0 );
        }
        return Mesh;
    }

    // N-sided cylinder: side = group 1, top cap = group 2, bottom cap = group 3 (caps are centre fans).
    FDynamicMesh3 MakeCylinder( int N, bool bCaps )
    {
        FDynamicMesh3 Mesh;
        Mesh.EnableTriangleGroups();
        for ( int Ring = 0; Ring < 2; ++Ring )
            for ( int i = 0; i < N; ++i )
            {
                const double A = 2.0 * 3.14159265358979 * i / N;
                Mesh.AppendVertex( glm::dvec3( 50.0 * std::cos( A ), 50.0 * std::sin( A ), Ring * 200.0 ) );
            }
        auto B = [N]( int i ) { return i % N; };
        auto T = [N]( int i ) { return N + i % N; };
        for ( int i = 0; i < N; ++i )
        {
            EXPECT_GE( Mesh.AppendTriangle( B( i ), B( i + 1 ), T( i + 1 ), 1 ), 0 );
            EXPECT_GE( Mesh.AppendTriangle( B( i ), T( i + 1 ), T( i ), 1 ), 0 );
        }
        if ( bCaps )
        {
            const int Bc = Mesh.AppendVertex( glm::dvec3( 0.0, 0.0, 0.0 ) );
            const int Tc = Mesh.AppendVertex( glm::dvec3( 0.0, 0.0, 200.0 ) );
            for ( int i = 0; i < N; ++i )
            {
                EXPECT_GE( Mesh.AppendTriangle( Tc, T( i ), T( i + 1 ), 2 ), 0 );
                EXPECT_GE( Mesh.AppendTriangle( Bc, B( i + 1 ), B( i ), 3 ), 0 );
            }
        }
        return Mesh;
    }

    // Every boundary of every group closes: a corner-free edge is a loop whose span repeats its first vertex;
    // otherwise each endpoint of the boundary's spans is shared by exactly two of them.
    void ExpectBoundariesClose( const FGroupTopology& Topo )
    {
        for ( const auto& Group : Topo.Groups )
            for ( const auto& Boundary : Group.Boundaries )
            {
                ASSERT_GT( static_cast<int32_t>( Boundary.GroupEdges.size() ), 0 );
                std::map<int, int> EndpointUses;
                for ( int E : Boundary.GroupEdges )
                {
                    const auto& V = Topo.GetGroupEdgeVertices( E );
                    ASSERT_GE( static_cast<int32_t>( V.size() ), 2 );
                    if ( Topo.IsIsolatedLoop( E ) )
                    {
                        EXPECT_EQ( static_cast<int32_t>( Boundary.GroupEdges.size() ), 1 )
                             << "group " << Group.GroupID;
                        EXPECT_EQ( V[0], V.back() ) << "loop edge " << E << " does not close";
                        continue;
                    }
                    ++EndpointUses[V[0]];
                    ++EndpointUses[V.back()];
                }
                for ( const auto& [Vid, Uses] : EndpointUses )
                    EXPECT_EQ( Uses, 2 ) << "group " << Group.GroupID << " boundary open at vertex " << Vid;
            }
    }

    // Every mesh edge between two groups (or on the mesh border) is in exactly one group edge, and
    // FindGroupEdgeID finds that one; no other mesh edge is in any.
    void ExpectGroupEdgesPartition( const FDynamicMesh3& Mesh, const FGroupTopology& Topo )
    {
        std::map<int, int> Owner;
        for ( int G = 0; G < static_cast<int32_t>( Topo.Edges.size() ); ++G )
            for ( int Eid : Topo.GetGroupEdgeEdges( G ) )
            {
                EXPECT_EQ( Owner.count( Eid ), 0u ) << "mesh edge " << Eid << " in two group edges";
                Owner[Eid] = G;
            }
        for ( int Eid : Mesh.EdgeIndicesItr() )
        {
            const FIndex2i Et = Mesh.GetEdgeT( Eid );
            const bool     bOnG =
                 Et.B == FDynamicMesh3::InvalidID || Topo.GetGroupID( Et.A ) != Topo.GetGroupID( Et.B );
            EXPECT_EQ( Owner.count( Eid ) == 1, bOnG ) << "mesh edge " << Eid;
            if ( bOnG )
                EXPECT_EQ( Topo.FindGroupEdgeID( Eid ), Owner[Eid] );
        }
    }
} // namespace

TEST( GroupTopology, CubeHasSixGroupsEightCornersTwelveEdges )
{
    const int           Groups[6] = { 0, 1, 2, 3, 4, 5 };
    const FDynamicMesh3 Mesh      = MakeCube( Groups );
    FGroupTopology      Topo( &Mesh, false );
    ASSERT_TRUE( Topo.RebuildTopology() ) << Topo.Failure();
    EXPECT_EQ( static_cast<int32_t>( Topo.Groups.size() ), 6 );
    EXPECT_EQ( static_cast<int32_t>( Topo.Corners.size() ), 8 );
    EXPECT_EQ( static_cast<int32_t>( Topo.Edges.size() ), 12 );
    for ( const auto& G : Topo.Groups )
    {
        ASSERT_EQ( static_cast<int32_t>( G.Boundaries.size() ), 1 );
        EXPECT_EQ( static_cast<int32_t>( G.Boundaries[0].GroupEdges.size() ), 4 );
        EXPECT_FALSE( G.Boundaries[0].bIsOnBoundary );
        EXPECT_EQ( static_cast<int32_t>( G.NeighbourGroupIDs.size() ), 4 );
        EXPECT_EQ( static_cast<int32_t>( G.Triangles.size() ), 2 );
    }
    for ( int E = 0; E < static_cast<int32_t>( Topo.Edges.size() ); ++E )
    {
        EXPECT_TRUE( Topo.IsSimpleGroupEdge( E ) );
        EXPECT_FALSE( Topo.IsIsolatedLoop( E ) );
        EXPECT_NE( Topo.Edges[E].Groups.A, Topo.Edges[E].Groups.B );
    }
    for ( int C = 0; C < static_cast<int32_t>( Topo.Corners.size() ); ++C )
    {
        std::vector<int> NbrEdges, NbrCorners, NbrGroups;
        Topo.FindCornerNbrEdges( C, NbrEdges );
        Topo.FindCornerNbrCorners( C, NbrCorners );
        Topo.FindCornerNbrGroups( C, NbrGroups );
        EXPECT_EQ( static_cast<int32_t>( NbrEdges.size() ), 3 ) << "corner " << C;
        EXPECT_EQ( static_cast<int32_t>( NbrCorners.size() ), 3 ) << "corner " << C;
        EXPECT_EQ( static_cast<int32_t>( NbrGroups.size() ), 3 ) << "corner " << C;
        EXPECT_EQ( Topo.GetCornerIDFromVertexID( Topo.GetCornerVertexID( C ) ), C );
    }
    std::vector<int> EdgeNbrs;
    Topo.FindEdgeNbrEdges( 0, EdgeNbrs );
    EXPECT_EQ( static_cast<int32_t>( EdgeNbrs.size() ),
               6 ); // three at each endpoint corner, the edge itself counted at both
    ExpectBoundariesClose( Topo );
    ExpectGroupEdgesPartition( Mesh, Topo );
}

TEST( GroupTopology, CappedCylinderHasNoCornersAndTwoClosedLoops )
{
    constexpr int       N    = 12;
    const FDynamicMesh3 Mesh = MakeCylinder( N, true );
    FGroupTopology      Topo( &Mesh, true );
    EXPECT_TRUE( Topo.Failure().empty() ) << Topo.Failure();
    EXPECT_EQ( static_cast<int32_t>( Topo.Groups.size() ), 3 );
    EXPECT_EQ( static_cast<int32_t>( Topo.Corners.size() ), 0 );
    ASSERT_EQ( static_cast<int32_t>( Topo.Edges.size() ), 2 );
    for ( int E = 0; E < 2; ++E )
    {
        EXPECT_TRUE( Topo.IsIsolatedLoop( E ) );
        EXPECT_EQ( static_cast<int32_t>( Topo.GetGroupEdgeEdges( E ).size() ), N );
        EXPECT_EQ( static_cast<int32_t>( Topo.GetGroupEdgeVertices( E ).size() ), N + 1 );
        EXPECT_EQ( Topo.Edges[E].Groups.A, 1 ); // the side (1) is the lower group of both loops
    }
    ASSERT_NE( Topo.FindGroupByID( 1 ), nullptr );
    EXPECT_EQ( static_cast<int32_t>( Topo.FindGroupByID( 1 )->Boundaries.size() ), 2 );
    EXPECT_EQ( static_cast<int32_t>( Topo.GetGroupNbrGroups( 1 ).size() ), 2 );
    EXPECT_EQ( static_cast<int32_t>( Topo.FindGroupByID( 2 )->Boundaries.size() ), 1 );
    EXPECT_EQ( static_cast<int32_t>( Topo.GetGroupTriangles( 1 ).size() ), 2 * N );
    EXPECT_EQ( Topo.FindGroupByID( 0 ), nullptr );
    ExpectBoundariesClose( Topo );
    ExpectGroupEdgesPartition( Mesh, Topo );
}

TEST( GroupTopology, OpenTubeLoopsLieOnTheMeshBorder )
{
    const FDynamicMesh3 Mesh = MakeCylinder( 8, false );
    FGroupTopology      Topo( &Mesh, true );
    ASSERT_EQ( static_cast<int32_t>( Topo.Groups.size() ), 1 );
    EXPECT_EQ( static_cast<int32_t>( Topo.Corners.size() ), 0 );
    ASSERT_EQ( static_cast<int32_t>( Topo.Edges.size() ), 2 );
    for ( int E = 0; E < 2; ++E )
    {
        EXPECT_TRUE( Topo.IsBoundaryEdge( E ) );
        EXPECT_EQ( Topo.Edges[E].Groups.B, FDynamicMesh3::InvalidID );
    }
    for ( const auto& B : Topo.Groups[0].Boundaries )
        EXPECT_TRUE( B.bIsOnBoundary );
    EXPECT_EQ( static_cast<int32_t>( Topo.Groups[0].NeighbourGroupIDs.size() ), 0 );
    ExpectBoundariesClose( Topo );
    ExpectGroupEdgesPartition( Mesh, Topo );
}

TEST( GroupTopology, CubeTopAsOwnGroupIsOneCornerFreeLoop )
{
    const int           Groups[6] = { 0, 7, 0, 0, 0, 0 }; // face 1 (z = Side) is group 7
    const FDynamicMesh3 Mesh      = MakeCube( Groups );
    FGroupTopology      Topo( &Mesh, true );
    EXPECT_EQ( static_cast<int32_t>( Topo.Groups.size() ), 2 );
    EXPECT_EQ( static_cast<int32_t>( Topo.Corners.size() ), 0 );
    ASSERT_EQ( static_cast<int32_t>( Topo.Edges.size() ), 1 );
    EXPECT_TRUE( Topo.IsIsolatedLoop( 0 ) );
    EXPECT_EQ( static_cast<int32_t>( Topo.GetGroupEdgeEdges( 0 ).size() ), 4 );
    std::unordered_set<int> Verts;
    Topo.CollectGroupBoundaryVertices( 7, Verts );
    EXPECT_EQ( static_cast<int32_t>( Verts.size() ), 4 );
    FGroupTopologySelection Sel;
    Sel.SelectedGroupIDs.insert( 7 );
    std::vector<int32_t> Tris;
    Topo.GetSelectedTriangles( Sel, Tris );
    EXPECT_EQ( static_cast<int32_t>( Tris.size() ), 2 );
    ExpectBoundariesClose( Topo );
    ExpectGroupEdgesPartition( Mesh, Topo );
}

TEST( GroupTopology, TriangleTopologyIsTheMeshItself )
{
    const int              Groups[6] = { 0, 1, 2, 3, 4, 5 };
    const FDynamicMesh3    Mesh      = MakeCube( Groups );
    FTriangleGroupTopology Topo( &Mesh, true );
    EXPECT_EQ( static_cast<int32_t>( Topo.Groups.size() ), 12 );
    EXPECT_EQ( static_cast<int32_t>( Topo.Corners.size() ), 8 );
    EXPECT_EQ( static_cast<int32_t>( Topo.Edges.size() ), 18 );
    for ( const auto& G : Topo.Groups )
        EXPECT_EQ( static_cast<int32_t>( G.NeighbourGroupIDs.size() ), 3 );
    ExpectBoundariesClose( Topo );
    ExpectGroupEdgesPartition( Mesh, Topo );
}

namespace
{
    // The view ElementSelectTool builds for "pick at the viewport centre": the eye, a unit ray towards the
    // target, and the view-projection the pixel tolerance is measured in.
    PickView CentreView( const glm::vec3& eye, const glm::vec3& target )
    {
        const glm::vec3 dir = glm::normalize( target - eye );
        const glm::vec3 up  = std::abs( dir.y ) > 0.99f ? glm::vec3( 0, 0, -1 ) : glm::vec3( 0, 1, 0 );
        PickView        view;
        view.ViewProj =
             glm::perspective( glm::radians( 60.0f ), 1.0f, 1.0f, 10000.0f ) * glm::lookAt( eye, target, up );
        view.ViewportSize    = glm::vec2( 800.0f );
        view.Cursor          = glm::vec2( 400.0f );
        view.RayOrigin       = eye;
        view.RayDirection    = dir;
        view.TolerancePixels = 8.0f;
        return view;
    }

    std::vector<int> TriEditEdgePick( const FDynamicMesh3& mesh, const glm::vec3& eye, const glm::vec3& target )
    {
        const FGroupTopology topology( &mesh, true );
        const ElementHit     hit =
             PickElement( mesh, topology, ElementMode::Edge, CentreView( eye, target ), TopologyLevel::Triangle );
        return HitElements( topology, ElementMode::Edge, TopologyLevel::Triangle, hit );
    }
} // namespace

// TriEdit picks the diagonal inside a group. Face 1 (z = Side) is triangles (4,5,7) + (4,7,6): its diagonal is
// 4-7. Looking straight down -z at the diagonal's middle, the back face's diagonal 0-3 lies exactly behind it
// on the same ray: the front one must win, the hidden one must not turn the pick into a miss.
TEST( GroupTopology, TriEditPicksTheFaceDiagonalHeadOn )
{
    const int           Groups[6] = { 0, 1, 2, 3, 4, 5 };
    const FDynamicMesh3 Mesh      = MakeCube( Groups );
    const glm::vec3     mid( 50.0f, 50.0f, 100.0f );
    EXPECT_EQ( TriEditEdgePick( Mesh, mid + glm::vec3( 0, 0, 300 ), mid ),
               std::vector<int>{ Mesh.FindEdge( 4, 7 ) } );
}

TEST( GroupTopology, TriEditPicksTheFaceDiagonalObliquely )
{
    const int           Groups[6] = { 0, 1, 2, 3, 4, 5 };
    const FDynamicMesh3 Mesh      = MakeCube( Groups );
    const glm::vec3     mid( 50.0f, 50.0f, 100.0f );
    EXPECT_EQ( TriEditEdgePick( Mesh, mid + glm::vec3( 120, -80, 300 ), mid ),
               std::vector<int>{ Mesh.FindEdge( 4, 7 ) } );
}

// The middle of a cube edge (4-5, between faces 1 and 2) selects that one triangle edge.
TEST( GroupTopology, TriEditPicksOneCubeEdgeAtItsMiddle )
{
    const int           Groups[6] = { 0, 1, 2, 3, 4, 5 };
    const FDynamicMesh3 Mesh      = MakeCube( Groups );
    const glm::vec3     mid( 50.0f, 0.0f, 100.0f );
    EXPECT_EQ( TriEditEdgePick( Mesh, mid + glm::vec3( 60, -200, 250 ), mid ),
               std::vector<int>{ Mesh.FindEdge( 4, 5 ) } );
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
