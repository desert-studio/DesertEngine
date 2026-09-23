#include <gtest/gtest.h>

#include "EditMeshTestSupport.hpp"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

using Desert::Geometry::CollapseEdgeInfo;
using Desert::Geometry::EditMesh;
using Desert::Geometry::EditResult;
using Desert::Geometry::FlipEdgeInfo;
using Desert::Geometry::InvalidId;
using Desert::Geometry::SplitEdgeInfo;
using EditMeshTest::MakeGrid;
using EditMeshTest::MakeOctahedron;
using EditMeshTest::Tri;
using EditMeshTest::Valid;

namespace
{
    glm::vec3 Normal( const EditMesh& mesh, int t )
    {
        const auto& c = mesh.GetTriangle( t );
        return glm::cross( mesh.GetPosition( c[1] ) - mesh.GetPosition( c[0] ),
                           mesh.GetPosition( c[2] ) - mesh.GetPosition( c[0] ) );
    }

    int BoundaryEdgeCount( const EditMesh& mesh )
    {
        int count = 0;
        for ( const int e : mesh.EdgeIds() )
            count += mesh.IsBoundaryEdge( e ) ? 1 : 0;
        return count;
    }

    int EulerCharacteristic( const EditMesh& mesh )
    {
        return mesh.VertexCount() - mesh.EdgeCount() + mesh.TriangleCount();
    }

    // Everything observable through the public API, so "a refused edit left the mesh untouched" is a
    // comparison and not a belief.
    struct Snapshot
    {
        std::vector<std::array<int, 3>> Triangles;
        std::vector<std::array<int, 2>> Edges;
        std::vector<std::array<int, 2>> EdgeTriangles;
        std::vector<glm::vec3>          Positions;
        std::array<int, 6>              Sizes{};
        // Attribute layers: per live triangle its polygroup, material and every layer's element IDs and
        // values, then each layer's element counts - so a refusal that touched only a layer is caught too.
        std::vector<float> Layers;

        bool operator==( const Snapshot& ) const = default;
    };

    Snapshot Take( const EditMesh& mesh )
    {
        Snapshot s;
        for ( const int t : mesh.TriangleIds() )
            s.Triangles.push_back( mesh.GetTriangle( t ) );
        for ( const int e : mesh.EdgeIds() )
        {
            s.Edges.push_back( mesh.GetEdgeVertices( e ) );
            s.EdgeTriangles.push_back( mesh.GetEdgeTriangles( e ) );
        }
        for ( const int v : mesh.VertexIds() )
            s.Positions.push_back( mesh.GetPosition( v ) );
        s.Sizes = { mesh.VertexCount(), mesh.TriangleCount(), mesh.EdgeCount(),
                    mesh.MaxVertexId(), mesh.MaxTriangleId(), mesh.MaxEdgeId() };

        const auto& attributes = mesh.Attributes();
        const auto  layer      = [&]( const auto* overlay )
        {
            if ( overlay == nullptr )
                return;
            for ( const int t : mesh.TriangleIds() )
                for ( const int e : overlay->GetTriangle( t ) )
                {
                    s.Layers.push_back( static_cast<float>( e ) );
                    if ( e != InvalidId )
                    {
                        const auto& value = overlay->GetElement( e );
                        for ( int i = 0; i < value.length(); ++i )
                            s.Layers.push_back( value[i] );
                    }
                }
            s.Layers.push_back( static_cast<float>( overlay->ElementCount() ) );
            s.Layers.push_back( static_cast<float>( overlay->MaxElementId() ) );
        };
        for ( const int t : mesh.TriangleIds() )
        {
            s.Layers.push_back( static_cast<float>( attributes.GetPolyGroup( t ) ) );
            s.Layers.push_back( static_cast<float>( attributes.GetMaterialId( t ) ) );
        }
        layer( attributes.Normals() );
        layer( attributes.Tangents() );
        layer( attributes.Colors() );
        for ( int i = 0; i < attributes.UVLayerCount(); ++i )
            layer( attributes.UV( i ) );
        return s;
    }

    template <typename Range>
    std::vector<int> Collect( const Range& range )
    {
        std::vector<int> ids;
        for ( const int id : range )
            ids.push_back( id );
        return ids;
    }
} // namespace

// ── construction ─────────────────────────────────────────────────────────────────────────────────────

TEST( EditMesh, QuadHasFiveEdgesOneOfThemInterior )
{
    EditMesh mesh = MakeGrid( 1 );
    EXPECT_EQ( mesh.VertexCount(), 4 );
    EXPECT_EQ( mesh.TriangleCount(), 2 );
    EXPECT_EQ( mesh.EdgeCount(), 5 );
    EXPECT_EQ( BoundaryEdgeCount( mesh ), 4 );

    const int diagonal = mesh.FindEdge( 0, 3 );
    ASSERT_NE( diagonal, InvalidId );
    EXPECT_FALSE( mesh.IsBoundaryEdge( diagonal ) );
    EXPECT_EQ( mesh.FindEdge( 3, 0 ), diagonal );
    EXPECT_EQ( mesh.FindEdge( 1, 2 ), InvalidId );
    EXPECT_EQ( mesh.FindTriangle( 3, 1, 0 ), 0 ); // any winding finds it
    EXPECT_TRUE( mesh.IsManifold() );
    EXPECT_TRUE( Valid( mesh ) );
}

TEST( EditMesh, TriangleEdgeSlotsFollowTheWinding )
{
    EditMesh mesh = MakeGrid( 2 );
    for ( const int t : mesh.TriangleIds() )
        for ( int j = 0; j < 3; ++j )
        {
            const auto& corners = mesh.GetTriangle( t );
            const auto& ends    = mesh.GetEdgeVertices( mesh.GetTriangleEdges( t )[j] );
            const int   lo      = std::min( corners[j], corners[( j + 1 ) % 3] );
            const int   hi      = std::max( corners[j], corners[( j + 1 ) % 3] );
            EXPECT_EQ( ends[0], lo );
            EXPECT_EQ( ends[1], hi );
        }
}

TEST( EditMesh, ClosedOctahedronHasNoBoundaryAndEulerTwo )
{
    const EditMesh mesh = MakeOctahedron();
    EXPECT_EQ( mesh.EdgeCount(), 12 );
    EXPECT_EQ( BoundaryEdgeCount( mesh ), 0 );
    EXPECT_EQ( EulerCharacteristic( mesh ), 2 );
    for ( const int v : mesh.VertexIds() )
    {
        EXPECT_FALSE( mesh.IsBoundaryVertex( v ) );
        EXPECT_EQ( mesh.GetVertexNeighbours( v ).size(), 4u );
        EXPECT_EQ( mesh.GetVertexTriangles( v ).size(), 4u );
    }
    EXPECT_TRUE( Valid( mesh ) );
}

TEST( EditMesh, AppendTriangleRefusesAndLeavesTheMeshUntouched )
{
    EditMesh  mesh = MakeGrid( 1 ); // (0,1,3) and (0,3,2)
    const int farVertex  = mesh.AppendVertex( { 500, 500, 500 } );
    const int far2 = mesh.AppendVertex( { 600, 500, 500 } );
    const auto before = Take( mesh );

    int t = 12345;
    EXPECT_EQ( mesh.AppendTriangle( 0, 1, 99, t ), EditResult::InvalidVertex );
    EXPECT_EQ( mesh.AppendTriangle( 0, 1, -1, t ), EditResult::InvalidVertex );
    EXPECT_EQ( mesh.AppendTriangle( 0, 1, 1, t ), EditResult::DegenerateTriangle );
    EXPECT_EQ( mesh.AppendTriangle( 0, 1, 3, t ), EditResult::DuplicateTriangle );
    EXPECT_EQ( mesh.AppendTriangle( 3, 1, 0, t ), EditResult::DuplicateTriangle ); // other winding too
    // (0,1) is run 0 -> 1 by the first triangle; a second one running 0 -> 1 disagrees about the side.
    EXPECT_EQ( mesh.AppendTriangle( 0, 1, farVertex, t ), EditResult::InconsistentOrientation );
    // The diagonal (0,3) already carries two triangles.
    EXPECT_EQ( mesh.AppendTriangle( 3, 0, farVertex, t ), EditResult::NonManifoldEdge );
    EXPECT_EQ( t, 12345 );
    EXPECT_TRUE( Take( mesh ) == before );

    // The correctly wound neighbour is accepted, and a free-standing triangle too.
    EXPECT_NE( Tri( mesh, 1, 0, farVertex ), InvalidId );
    EXPECT_NE( Tri( mesh, farVertex, far2, 2 ), InvalidId );
    EXPECT_TRUE( Valid( mesh ) );
}

TEST( EditMesh, RemoveTriangleLeavesHolesAndReusesFreedIds )
{
    EditMesh  mesh     = MakeGrid( 2 );
    const int loner    = mesh.AppendVertex( { -1000, 0, 0 } ); // isolated BEFORE: never removed
    const int triangle = 3;
    const auto corners = mesh.GetTriangle( triangle );
    const auto others  = Collect( mesh.TriangleIds() );

    ASSERT_EQ( mesh.RemoveTriangle( triangle ), EditResult::Ok );
    EXPECT_FALSE( mesh.IsTriangle( triangle ) );
    EXPECT_EQ( mesh.TriangleCount(), 7 );
    EXPECT_EQ( mesh.MaxTriangleId(), 8 ); // a hole, not a renumbering
    for ( const int t : others )
        if ( t != triangle )
            EXPECT_TRUE( mesh.IsTriangle( t ) ) << t;
    EXPECT_TRUE( mesh.IsVertex( loner ) );
    EXPECT_EQ( Collect( mesh.TriangleIds() ).size(), 7u );
    EXPECT_TRUE( Valid( mesh ) );

    EXPECT_EQ( mesh.RemoveTriangle( triangle ), EditResult::InvalidTriangle );
    EXPECT_EQ( Tri( mesh, corners[0], corners[1], corners[2] ), triangle ); // the freed ID comes back
    EXPECT_TRUE( Valid( mesh ) );
}

TEST( EditMesh, RemoveTriangleDropsOrphanedVerticesOnlyWhenAsked )
{
    EditMesh mesh = MakeGrid( 1 );
    ASSERT_EQ( mesh.RemoveTriangle( 0, false ), EditResult::Ok ); // (0,1,3): 1 is orphaned
    EXPECT_TRUE( mesh.IsVertex( 1 ) );
    EXPECT_TRUE( mesh.GetVertexEdges( 1 ).empty() );
    EXPECT_TRUE( Valid( mesh ) );

    ASSERT_EQ( mesh.RemoveTriangle( 1, true ), EditResult::Ok );
    EXPECT_EQ( mesh.VertexCount(), 1 ); // 0, 2, 3 go; 1 was already isolated and stays
    EXPECT_TRUE( mesh.IsVertex( 1 ) );
    EXPECT_EQ( mesh.EdgeCount(), 0 );
    EXPECT_TRUE( Valid( mesh ) );
}

// ── split ────────────────────────────────────────────────────────────────────────────────────────────

TEST( EditMesh, SplitInteriorEdgeKeepsEveryIdAndWinding )
{
    EditMesh  mesh     = MakeGrid( 1 );
    const int diagonal = mesh.FindEdge( 0, 3 );
    const auto tris    = mesh.GetEdgeTriangles( diagonal );

    SplitEdgeInfo info;
    ASSERT_EQ( mesh.SplitEdge( diagonal, 0.25f, info ), EditResult::Ok );
    EXPECT_EQ( info.OriginalEdge, diagonal );
    EXPECT_EQ( mesh.VertexCount(), 5 );
    EXPECT_EQ( mesh.TriangleCount(), 4 );
    EXPECT_EQ( mesh.EdgeCount(), 8 );
    EXPECT_NE( info.NewTriangles[1], InvalidId );

    EXPECT_EQ( mesh.GetPosition( info.NewVertex ), glm::vec3( 25, 25, 0 ) );
    EXPECT_EQ( mesh.GetEdgeVertices( diagonal ), ( std::array<int, 2>{ 0, info.NewVertex } ) );
    EXPECT_EQ( mesh.GetEdgeVertices( info.NewEdge ), ( std::array<int, 2>{ 3, info.NewVertex } ) );
    EXPECT_TRUE( mesh.IsTriangle( tris[0] ) );
    EXPECT_TRUE( mesh.IsTriangle( tris[1] ) );
    for ( const int t : mesh.TriangleIds() )
        EXPECT_GT( Normal( mesh, t ).z, 0.0f ) << "triangle " << t << " flipped";
    EXPECT_EQ( mesh.GetVertexNeighbours( info.NewVertex ).size(), 4u );
    EXPECT_TRUE( Valid( mesh ) );
}

TEST( EditMesh, SplitBoundaryEdgeAddsOneTriangle )
{
    EditMesh  mesh = MakeGrid( 1 );
    const int edge = mesh.FindEdge( 0, 1 );

    SplitEdgeInfo info;
    ASSERT_EQ( mesh.SplitEdge( edge, 0.5f, info ), EditResult::Ok );
    EXPECT_EQ( mesh.TriangleCount(), 3 );
    EXPECT_EQ( mesh.EdgeCount(), 7 );
    EXPECT_EQ( info.NewTriangles[1], InvalidId );
    EXPECT_EQ( info.NewSpokes[1], InvalidId );
    EXPECT_TRUE( mesh.IsBoundaryVertex( info.NewVertex ) );
    EXPECT_EQ( BoundaryEdgeCount( mesh ), 5 );
    for ( const int t : mesh.TriangleIds() )
        EXPECT_GT( Normal( mesh, t ).z, 0.0f );
    EXPECT_TRUE( Valid( mesh ) );

    EXPECT_EQ( mesh.SplitEdge( 999, 0.5f, info ), EditResult::InvalidEdge );
}

// ── flip ─────────────────────────────────────────────────────────────────────────────────────────────

TEST( EditMesh, FlipSwapsTheQuadDiagonal )
{
    EditMesh  mesh     = MakeGrid( 1 );
    const int diagonal = mesh.FindEdge( 0, 3 );

    FlipEdgeInfo info;
    ASSERT_EQ( mesh.FlipEdge( diagonal, info ), EditResult::Ok );
    EXPECT_EQ( mesh.GetEdgeVertices( diagonal ), ( std::array<int, 2>{ 1, 2 } ) );
    EXPECT_EQ( mesh.FindEdge( 0, 3 ), InvalidId );
    EXPECT_EQ( mesh.FindEdge( 1, 2 ), diagonal );
    EXPECT_EQ( info.OldVertices, ( std::array<int, 2>{ 0, 3 } ) );
    EXPECT_EQ( info.NewVertices, ( std::array<int, 2>{ 1, 2 } ) );
    EXPECT_EQ( mesh.EdgeCount(), 5 );
    for ( const int t : mesh.TriangleIds() )
        EXPECT_GT( Normal( mesh, t ).z, 0.0f );
    EXPECT_TRUE( Valid( mesh ) );

    // Flipping twice restores the original diagonal.
    ASSERT_EQ( mesh.FlipEdge( diagonal, info ), EditResult::Ok );
    EXPECT_EQ( mesh.GetEdgeVertices( diagonal ), ( std::array<int, 2>{ 0, 3 } ) );
    EXPECT_TRUE( Valid( mesh ) );
}

TEST( EditMesh, FlipRefusesBoundaryAndExistingDiagonal )
{
    EditMesh     mesh = MakeGrid( 1 );
    FlipEdgeInfo info;
    const auto   before = Take( mesh );
    EXPECT_EQ( mesh.FlipEdge( mesh.FindEdge( 0, 1 ), info ), EditResult::BoundaryEdge );
    EXPECT_EQ( mesh.FlipEdge( -1, info ), EditResult::InvalidEdge );
    EXPECT_TRUE( Take( mesh ) == before );

    // Tetrahedron: the other diagonal of every edge's quad is itself an edge.
    EditMesh  tet;
    const int a = tet.AppendVertex( { 0, 0, 0 } );
    const int b = tet.AppendVertex( { 100, 0, 0 } );
    const int c = tet.AppendVertex( { 0, 100, 0 } );
    const int d = tet.AppendVertex( { 0, 0, 100 } );
    Tri( tet, a, c, b );
    Tri( tet, a, b, d );
    Tri( tet, b, c, d );
    Tri( tet, c, a, d );
    ASSERT_TRUE( Valid( tet ) );
    const auto tetBefore = Take( tet );
    for ( const int e : Collect( tet.EdgeIds() ) )
        EXPECT_EQ( tet.FlipEdge( e, info ), EditResult::FlipCreatesExistingEdge );
    EXPECT_TRUE( Take( tet ) == tetBefore );
}

// ── collapse ─────────────────────────────────────────────────────────────────────────────────────────

TEST( EditMesh, CollapseInteriorEdgeRemovesTwoTrianglesAndThreeEdges )
{
    EditMesh  mesh   = MakeGrid( 4 );
    const int centre = 12; // (2,2) of a 5x5 vertex grid: interior
    const int east   = 13;
    ASSERT_NE( mesh.FindEdge( centre, east ), InvalidId );
    const int euler = EulerCharacteristic( mesh );
    const int v = mesh.VertexCount(), e = mesh.EdgeCount(), t = mesh.TriangleCount();

    CollapseEdgeInfo info;
    ASSERT_EQ( mesh.CollapseEdge( centre, east, 0.5f, info ), EditResult::Ok );
    EXPECT_EQ( mesh.VertexCount(), v - 1 );
    EXPECT_EQ( mesh.EdgeCount(), e - 3 );
    EXPECT_EQ( mesh.TriangleCount(), t - 2 );
    EXPECT_EQ( EulerCharacteristic( mesh ), euler );
    EXPECT_FALSE( mesh.IsVertex( east ) );
    EXPECT_TRUE( mesh.IsVertex( centre ) );
    EXPECT_EQ( mesh.GetPosition( centre ), glm::vec3( 250, 200, 0 ) );
    for ( const int r : info.RemovedTriangles )
        EXPECT_FALSE( mesh.IsTriangle( r ) );
    for ( const int k : info.KeptEdges )
        EXPECT_TRUE( mesh.IsEdge( k ) );
    for ( const int tri : mesh.TriangleIds() )
        EXPECT_GT( Normal( mesh, tri ).z, 0.0f );
    EXPECT_TRUE( mesh.IsManifold() );
    EXPECT_TRUE( Valid( mesh ) );
}

TEST( EditMesh, CollapseBoundaryEdge )
{
    EditMesh mesh = MakeGrid( 3 );
    const int v = mesh.VertexCount(), e = mesh.EdgeCount(), t = mesh.TriangleCount();

    CollapseEdgeInfo info;
    ASSERT_EQ( mesh.CollapseEdge( 1, 2, 0.0f, info ), EditResult::Ok ); // on the y = 0 border
    EXPECT_EQ( mesh.VertexCount(), v - 1 );
    EXPECT_EQ( mesh.EdgeCount(), e - 2 );
    EXPECT_EQ( mesh.TriangleCount(), t - 1 );
    EXPECT_EQ( info.RemovedTriangles[1], InvalidId );
    EXPECT_EQ( mesh.GetPosition( 1 ), glm::vec3( 100, 0, 0 ) );
    EXPECT_TRUE( Valid( mesh ) );
}

TEST( EditMesh, CollapseRefusalsLeaveTheMeshUntouched )
{
    CollapseEdgeInfo info;

    // Pinch: an interior edge between two boundary vertices (the diagonal of a lone quad).
    {
        EditMesh   mesh   = MakeGrid( 1 );
        const auto before = Take( mesh );
        EXPECT_EQ( mesh.CollapseEdge( 0, 3, 0.5f, info ), EditResult::CollapseBreaksTopology );
        EXPECT_EQ( mesh.CollapseEdge( 3, 0, 0.5f, info ), EditResult::CollapseBreaksTopology );
        EXPECT_TRUE( Take( mesh ) == before );
    }
    // Ear: a single triangle.
    {
        EditMesh  mesh;
        const int a = mesh.AppendVertex( { 0, 0, 0 } );
        const int b = mesh.AppendVertex( { 100, 0, 0 } );
        const int c = mesh.AppendVertex( { 0, 100, 0 } );
        Tri( mesh, a, b, c );
        const auto before = Take( mesh );
        EXPECT_EQ( mesh.CollapseEdge( a, b, 0.5f, info ), EditResult::CollapseBreaksTopology );
        EXPECT_TRUE( Take( mesh ) == before );
    }
    // Link condition / tetrahedron: every pair shares a third neighbour.
    {
        EditMesh  tet;
        const int a = tet.AppendVertex( { 0, 0, 0 } );
        const int b = tet.AppendVertex( { 100, 0, 0 } );
        const int c = tet.AppendVertex( { 0, 100, 0 } );
        const int d = tet.AppendVertex( { 0, 0, 100 } );
        Tri( tet, a, c, b );
        Tri( tet, a, b, d );
        Tri( tet, b, c, d );
        Tri( tet, c, a, d );
        const auto before = Take( tet );
        EXPECT_EQ( tet.CollapseEdge( a, b, 0.5f, info ), EditResult::CollapseBreaksTopology );
        EXPECT_TRUE( Take( tet ) == before );
    }
    // Arguments that name no edge.
    {
        EditMesh mesh = MakeGrid( 2 );
        EXPECT_EQ( mesh.CollapseEdge( 0, 2, 0.5f, info ), EditResult::InvalidEdge );
        EXPECT_EQ( mesh.CollapseEdge( 0, 0, 0.5f, info ), EditResult::InvalidEdge );
        EXPECT_EQ( mesh.CollapseEdge( 0, 77, 0.5f, info ), EditResult::InvalidVertex );
    }
}

// ── bowtie / manifold ────────────────────────────────────────────────────────────────────────────────

TEST( EditMesh, TwoFansOnOneVertexAreABowtie )
{
    EditMesh  mesh = MakeGrid( 1 );
    const int x    = mesh.AppendVertex( { -100, -50, 0 } );
    const int y    = mesh.AppendVertex( { -100, -150, 0 } );
    EXPECT_TRUE( mesh.IsManifold() );
    Tri( mesh, 0, x, y ); // touches the quad at vertex 0 only
    EXPECT_TRUE( mesh.IsBowtieVertex( 0 ) );
    EXPECT_FALSE( mesh.IsBowtieVertex( 3 ) );
    EXPECT_FALSE( mesh.IsManifold() );
    EXPECT_TRUE( Valid( mesh ) ); // legal, only reported
}

// ── compact / iteration ──────────────────────────────────────────────────────────────────────────────

TEST( EditMesh, CompactRenumbersDenselyAndReturnsTheMaps )
{
    EditMesh mesh = MakeGrid( 3 );
    ASSERT_EQ( mesh.RemoveTriangle( 0 ), EditResult::Ok );
    ASSERT_EQ( mesh.RemoveTriangle( 7 ), EditResult::Ok );
    CollapseEdgeInfo collapse;
    ASSERT_EQ( mesh.CollapseEdge( 5, 6, 0.5f, collapse ), EditResult::Ok );

    std::vector<std::array<glm::vec3, 3>> geometry;
    for ( const int t : mesh.TriangleIds() )
    {
        const auto& c = mesh.GetTriangle( t );
        geometry.push_back( { mesh.GetPosition( c[0] ), mesh.GetPosition( c[1] ), mesh.GetPosition( c[2] ) } );
    }
    const auto oldTriangles = Collect( mesh.TriangleIds() );
    const int  vertices     = mesh.VertexCount();

    const auto maps = mesh.Compact();
    EXPECT_TRUE( Valid( mesh ) );
    EXPECT_EQ( mesh.MaxVertexId(), vertices );
    EXPECT_EQ( mesh.MaxTriangleId(), mesh.TriangleCount() );
    EXPECT_EQ( mesh.MaxEdgeId(), mesh.EdgeCount() );
    EXPECT_EQ( maps.Vertices[6], InvalidId );
    EXPECT_EQ( maps.Triangles[0], InvalidId );

    // The same triangles, same corners in the same winding, under new names.
    for ( size_t i = 0; i < oldTriangles.size(); ++i )
    {
        const int   nt = maps.Triangles[oldTriangles[i]];
        const auto& c  = mesh.GetTriangle( nt );
        EXPECT_EQ( mesh.GetPosition( c[0] ), geometry[i][0] );
        EXPECT_EQ( mesh.GetPosition( c[1] ), geometry[i][1] );
        EXPECT_EQ( mesh.GetPosition( c[2] ), geometry[i][2] );
    }
}

TEST( EditMesh, IteratorsSkipHoles )
{
    EditMesh mesh = MakeGrid( 2 );
    ASSERT_EQ( mesh.RemoveTriangle( 0, false ), EditResult::Ok );
    ASSERT_EQ( mesh.RemoveTriangle( 7, false ), EditResult::Ok );
    EXPECT_EQ( Collect( mesh.TriangleIds() ), ( std::vector<int>{ 1, 2, 3, 4, 5, 6 } ) );
    EXPECT_EQ( static_cast<int>( Collect( mesh.EdgeIds() ).size() ), mesh.EdgeCount() );
    EXPECT_EQ( static_cast<int>( Collect( mesh.VertexIds() ).size() ), mesh.VertexCount() );

    const EditMesh empty;
    EXPECT_TRUE( Collect( empty.VertexIds() ).empty() );
    EXPECT_TRUE( Valid( empty ) );
}

// ── fuzz ─────────────────────────────────────────────────────────────────────────────────────────────
//
// Random sequences of every edit, CheckValidity after EACH one. Two relations are checked beyond the
// invariants, because a structure can be internally consistent and still wrong:
//   * split, flip and collapse preserve the Euler characteristic V - E + F, and a manifold mesh stays
//     manifold (these are the operations that promise not to change the surface's topology);
//   * a refused edit leaves the mesh byte-identical through the public API.
// Fixed seeds, so a red run reproduces.

namespace
{
    enum class Op
    {
        Split,
        Flip,
        Collapse,
        Remove,
        Append,
        Compact
    };

    struct FuzzStats
    {
        int Applied[6]{};
        int Refused[6]{};
        int SeamRefused[6]{}; // the share of Refused that was EditResult::AttributeSeam
        int MaxTriangles = 0;
    };

    // ── the attribute oracle ─────────────────────────────────────────────────────────────────────────
    // A flat grid cut in two by a seam line x = kSeamX. Every layer is a KNOWN function of (side, position),
    // with side = the triangle's polygroup, so after any edit each set corner can be checked against the
    // value it must have: linear functions survive split and collapse interpolation exactly (up to float
    // rounding), and constants survive renormalisation. The layers differ across the line, so it is a seam
    // in every overlay at once; UV layer 1 is set on side 0 only, a set/unset boundary on the same line.
    constexpr float kSeamX = 300.0f;

    glm::vec3 SideNormal( int side )
    {
        return side == 0 ? glm::vec3( 0, 0, 1 ) : glm::vec3( 0, 0.6f, 0.8f );
    }
    glm::vec4 SideTangent( int side )
    {
        return side == 0 ? glm::vec4( 1, 0, 0, 1 ) : glm::vec4( 1, 0, 0, -1 );
    }
    glm::vec4 SideColor( int side, const glm::vec3& p )
    {
        return { p.x * 0.001f, p.y * 0.001f, static_cast<float>( side ), 1.0f };
    }
    glm::vec2 SideUV0( int side, const glm::vec3& p )
    {
        return glm::vec2( p.x, p.y ) * 0.01f + ( side == 0 ? glm::vec2( 0.0f ) : glm::vec2( 10.0f, 0.0f ) );
    }
    glm::vec2 SideUV1( const glm::vec3& p )
    {
        return glm::vec2( p.y, p.x ) * 0.02f;
    }

    void PaintSides( EditMesh& mesh )
    {
        auto& attributes = mesh.Attributes();
        attributes.EnableNormals();
        attributes.EnableTangents();
        attributes.EnableColors();
        ASSERT_TRUE( attributes.SetUVLayerCount( 2 ) );

        // One element per (vertex, side) in each layer: shared inside a side, split across the line.
        std::vector<std::array<std::array<int, 5>, 2>> elements(
             static_cast<size_t>( mesh.MaxVertexId() ),
             { { { InvalidId, InvalidId, InvalidId, InvalidId, InvalidId },
                 { InvalidId, InvalidId, InvalidId, InvalidId, InvalidId } } } );
        for ( const int t : mesh.TriangleIds() )
        {
            const auto& c = mesh.GetTriangle( t );
            const float centroid =
                 ( mesh.GetPosition( c[0] ).x + mesh.GetPosition( c[1] ).x + mesh.GetPosition( c[2] ).x ) / 3.0f;
            const int side = centroid < kSeamX ? 0 : 1;
            attributes.SetPolyGroup( t, side );
            std::array<std::array<int, 3>, 5> tri{};
            for ( int j = 0; j < 3; ++j )
            {
                auto&            el = elements[c[j]][side];
                const glm::vec3& p  = mesh.GetPosition( c[j] );
                if ( el[0] == InvalidId )
                {
                    el[0] = attributes.Normals()->AppendElement( SideNormal( side ) );
                    el[1] = attributes.Tangents()->AppendElement( SideTangent( side ) );
                    el[2] = attributes.Colors()->AppendElement( SideColor( side, p ) );
                    el[3] = attributes.UV( 0 )->AppendElement( SideUV0( side, p ) );
                    if ( side == 0 )
                        el[4] = attributes.UV( 1 )->AppendElement( SideUV1( p ) );
                }
                for ( int k = 0; k < 5; ++k )
                    tri[k][j] = el[k];
            }
            ASSERT_EQ( attributes.Normals()->SetTriangle( mesh, t, tri[0] ), EditResult::Ok );
            ASSERT_EQ( attributes.Tangents()->SetTriangle( mesh, t, tri[1] ), EditResult::Ok );
            ASSERT_EQ( attributes.Colors()->SetTriangle( mesh, t, tri[2] ), EditResult::Ok );
            ASSERT_EQ( attributes.UV( 0 )->SetTriangle( mesh, t, tri[3] ), EditResult::Ok );
            if ( side == 0 )
                ASSERT_EQ( attributes.UV( 1 )->SetTriangle( mesh, t, tri[4] ), EditResult::Ok );
        }
    }

    template <typename V>
    bool Near( const V& a, const V& b )
    {
        for ( int i = 0; i < a.length(); ++i )
            if ( std::abs( a[i] - b[i] ) > 1e-3f * ( 1.0f + std::abs( b[i] ) ) )
                return false;
        return true;
    }

    ::testing::AssertionResult LayersFollowSides( const EditMesh& mesh )
    {
        const auto& attributes = mesh.Attributes();
        for ( const int t : mesh.TriangleIds() )
        {
            const int side = attributes.GetPolyGroup( t );
            if ( side != 0 && side != 1 )
                return ::testing::AssertionFailure() << "triangle " << t << " is in group " << side;
            if ( !attributes.Normals()->IsSetTriangle( t ) || !attributes.UV( 0 )->IsSetTriangle( t ) ||
                 attributes.UV( 1 )->IsSetTriangle( t ) != ( side == 0 ) )
                return ::testing::AssertionFailure()
                       << "triangle " << t << " (side " << side << ") has the wrong set/unset layers";
            const auto& c = mesh.GetTriangle( t );
            for ( int j = 0; j < 3; ++j )
            {
                const glm::vec3& p  = mesh.GetPosition( c[j] );
                const auto       at = [&]( const auto* overlay )
                { return overlay->GetElement( overlay->GetTriangle( t )[j] ); };
                const bool ok = Near( at( attributes.Normals() ), SideNormal( side ) ) &&
                                Near( at( attributes.Tangents() ), SideTangent( side ) ) &&
                                Near( at( attributes.Colors() ), SideColor( side, p ) ) &&
                                Near( at( attributes.UV( 0 ) ), SideUV0( side, p ) ) &&
                                ( side == 1 || Near( at( attributes.UV( 1 ) ), SideUV1( p ) ) );
                if ( !ok )
                    return ::testing::AssertionFailure()
                           << "triangle " << t << " corner " << j << " (vertex " << c[j] << " at " << p.x << ", "
                           << p.y << ", side " << side
                           << "): a layer value is not the side's function of the position";
            }
        }
        return ::testing::AssertionSuccess();
    }

    bool OnSeam( const EditMesh& mesh, int v )
    {
        int first = InvalidId;
        for ( const int t : mesh.GetVertexTriangles( v ) )
        {
            const int g = mesh.Attributes().GetPolyGroup( t );
            if ( first != InvalidId && g != first )
                return true;
            first = g;
        }
        return false;
    }

    template <typename Rng>
    int Pick( const std::vector<int>& ids, Rng& rng )
    {
        return ids[std::uniform_int_distribution<size_t>( 0, ids.size() - 1 )( rng )];
    }

    // layers: the mesh was painted by PaintSides; collapse positions are chosen so the oracle stays exact,
    // Append is replaced by Remove (a fresh triangle is unset, which the oracle does not model), and the
    // layers are checked against it after every step.
    ::testing::AssertionResult Fuzz( EditMesh& mesh, uint32_t seed, int steps, bool topologyOnly, FuzzStats& stats,
                                     bool layers = false )
    {
        std::mt19937 rng( seed );
        bool         manifold = mesh.IsManifold(); // carried across steps: recomputed only after a change
        for ( int step = 0; step < steps; ++step )
        {
            const auto edges     = Collect( mesh.EdgeIds() );
            const auto vertices  = Collect( mesh.VertexIds() );
            const auto triangles = Collect( mesh.TriangleIds() );
            if ( edges.empty() || triangles.empty() )
                return ::testing::AssertionSuccess(); // the fuzz ate the mesh; the run is still meaningful

            // Keep the mesh in a useful size band (every step re-verifies the whole mesh, so size is run
            // time): grow when small, shrink when large. The remaining share goes to remove/append, or to
            // compact/flip when only topology-preserving edits are wanted.
            const int tris      = mesh.TriangleCount();
            stats.MaxTriangles  = std::max( stats.MaxTriangles, tris );
            const int roll      = std::uniform_int_distribution<int>( 0, 99 )( rng );
            const int splitW    = tris < 40 ? 50 : tris > 160 ? 5 : 25;
            const int collapseW = tris < 40 ? 10 : tris > 160 ? 50 : 25;
            Op        op        = Op::Flip;
            if ( roll < splitW )
                op = Op::Split;
            else if ( roll < splitW + collapseW )
                op = Op::Collapse;
            else if ( roll < splitW + collapseW + 30 )
                op = Op::Flip;
            else if ( topologyOnly )
                op = roll % 5 == 0 ? Op::Compact : Op::Flip;
            else
                op = roll % 2 == 0 || layers ? Op::Remove : Op::Append;

            const Snapshot before     = Take( mesh );
            const int      euler      = EulerCharacteristic( mesh );
            EditResult     result      = EditResult::Ok;

            switch ( op )
            {
                case Op::Split:
                {
                    SplitEdgeInfo info;
                    const float   t = std::uniform_real_distribution<float>( 0.1f, 0.9f )( rng );
                    result          = mesh.SplitEdge( Pick( edges, rng ), t, info );
                    break;
                }
                case Op::Flip:
                {
                    FlipEdgeInfo info;
                    result = mesh.FlipEdge( Pick( edges, rng ), info );
                    break;
                }
                case Op::Collapse:
                {
                    const int        edge = Pick( edges, rng );
                    const auto&      ends = mesh.GetEdgeVertices( edge );
                    const bool       swap = ( rng() & 1u ) != 0;
                    const int        keep = ends[swap ? 1 : 0];
                    const int        gone = ends[swap ? 0 : 1];
                    float            t    = 0.5f;
                    if ( layers )
                    {
                        // Where the merged vertex may land without leaving any element's value behind: a seam
                        // vertex's elements are pinned to its position, so it must not move (or the other end
                        // must move onto it); a collapse ALONG the seam blends both sides' pairs and may land
                        // anywhere on it. Two seam vertices joined across a side have no such position.
                        const bool keepSeam = OnSeam( mesh, keep );
                        const bool goneSeam = OnSeam( mesh, gone );
                        t                   = std::uniform_real_distribution<float>( 0.0f, 1.0f )( rng );
                        if ( keepSeam && !goneSeam )
                            t = 0.0f;
                        else if ( !keepSeam && goneSeam )
                            t = 1.0f;
                        else if ( keepSeam && goneSeam && !mesh.Attributes().IsAttributeSeamEdge( mesh, edge ) )
                            continue;
                    }
                    CollapseEdgeInfo info;
                    result = mesh.CollapseEdge( keep, gone, t, info );
                    break;
                }
                case Op::Remove:
                    result = mesh.RemoveTriangle( Pick( triangles, rng ), ( rng() & 1u ) != 0 );
                    break;
                case Op::Append:
                {
                    int t = InvalidId;
                    result =
                        mesh.AppendTriangle( Pick( vertices, rng ), Pick( vertices, rng ), Pick( vertices, rng ), t );
                    break;
                }
                case Op::Compact:
                    (void)mesh.Compact();
                    break;
            }

            const int kind = static_cast<int>( op );
            ( result == EditResult::Ok ? stats.Applied : stats.Refused )[kind]++;
            if ( result == EditResult::AttributeSeam )
                stats.SeamRefused[kind]++;

            if ( const auto valid = mesh.CheckValidity(); !valid.IsSuccess() )
                return ::testing::AssertionFailure() << "seed " << seed << " step " << step << " op " << kind
                                                     << " (" << ToString( result ) << "): " << valid.GetError();
            if ( result != EditResult::Ok && !( Take( mesh ) == before ) )
                return ::testing::AssertionFailure() << "seed " << seed << " step " << step << " op " << kind
                                                     << " was refused (" << ToString( result )
                                                     << ") but changed the mesh";
            if ( layers )
                if ( auto follows = LayersFollowSides( mesh ); !follows )
                    return ::testing::AssertionFailure()
                           << "seed " << seed << " step " << step << " op " << kind << " (" << ToString( result )
                           << "): " << follows.message();
            const bool preserving = op == Op::Split || op == Op::Flip || op == Op::Collapse || op == Op::Compact;
            if ( result == EditResult::Ok && preserving )
            {
                if ( EulerCharacteristic( mesh ) != euler )
                    return ::testing::AssertionFailure() << "seed " << seed << " step " << step << " op " << kind
                                                         << " changed V-E+F from " << euler << " to "
                                                         << EulerCharacteristic( mesh );
            }
            if ( result == EditResult::Ok )
            {
                const bool now = mesh.IsManifold();
                if ( preserving && manifold && !now )
                    return ::testing::AssertionFailure() << "seed " << seed << " step " << step << " op " << kind
                                                         << " made a manifold mesh non-manifold";
                manifold = now;
            }
        }
        return ::testing::AssertionSuccess();
    }

    // Printed, not asserted exactly: the counts are the evidence that the run exercised what it claims.
    void Report( const char* name, const FuzzStats& stats )
    {
        std::printf( "[ fuzz ] %s: split %d/%d flip %d/%d collapse %d/%d remove %d/%d append %d/%d "
                     "compact %d (applied/refused), max %d triangles\n",
                     name, stats.Applied[0], stats.Refused[0], stats.Applied[1], stats.Refused[1], stats.Applied[2],
                     stats.Refused[2], stats.Applied[3], stats.Refused[3], stats.Applied[4], stats.Refused[4],
                     stats.Applied[5], stats.MaxTriangles );
    }
} // namespace

TEST( EditMeshFuzz, OpenGridTopologyOperations )
{
    FuzzStats stats;
    for ( uint32_t seed = 1; seed <= 6; ++seed )
    {
        EditMesh mesh = MakeGrid( 6 );
        EXPECT_TRUE( Fuzz( mesh, seed, 1200, true, stats ) );
    }
    Report( "open grid", stats );
    EXPECT_LT( stats.MaxTriangles, 400 );
    // The run is only evidence if every operation both happened and was refused along the way.
    for ( const Op op : { Op::Split, Op::Flip, Op::Collapse } )
        EXPECT_GT( stats.Applied[static_cast<int>( op )], 500 ) << static_cast<int>( op );
    EXPECT_GT( stats.Refused[static_cast<int>( Op::Flip )], 50 );
    EXPECT_GT( stats.Refused[static_cast<int>( Op::Collapse )], 50 );
}

TEST( EditMeshFuzz, ClosedSurfaceStaysClosed )
{
    FuzzStats stats;
    for ( uint32_t seed = 100; seed <= 105; ++seed )
    {
        EditMesh mesh = MakeOctahedron();
        EXPECT_TRUE( Fuzz( mesh, seed, 1200, true, stats ) );
        EXPECT_EQ( BoundaryEdgeCount( mesh ), 0 ) << "seed " << seed;
        EXPECT_EQ( EulerCharacteristic( mesh ), 2 ) << "seed " << seed;
    }
    Report( "closed octahedron", stats );
    EXPECT_LT( stats.MaxTriangles, 400 );
    EXPECT_GT( stats.Applied[static_cast<int>( Op::Collapse )], 500 );
    EXPECT_GT( stats.Refused[static_cast<int>( Op::Collapse )], 50 );
}

TEST( EditMeshFuzz, EveryOperationIncludingRemoveAndAppend )
{
    FuzzStats stats;
    for ( uint32_t seed = 1000; seed <= 1007; ++seed )
    {
        EditMesh mesh = MakeGrid( 5 );
        EXPECT_TRUE( Fuzz( mesh, seed, 1200, false, stats ) );
    }
    Report( "all operations", stats );
    EXPECT_LT( stats.MaxTriangles, 400 );
    EXPECT_GT( stats.Applied[static_cast<int>( Op::Remove )], 100 );
    EXPECT_GT( stats.Applied[static_cast<int>( Op::Append )], 20 );
    EXPECT_GT( stats.Refused[static_cast<int>( Op::Append )], 100 );
}

// The same fuzz with every layer painted: after each step the structure (CheckValidity now covers the
// layers: parents, reference counts, set/unset, slot counts) AND every value against the oracle.
TEST( EditMeshFuzz, AttributeLayersFollowEveryOperation )
{
    FuzzStats stats;
    for ( uint32_t seed = 2000; seed <= 2007; ++seed )
    {
        EditMesh mesh = MakeGrid( 6 );
        PaintSides( mesh );
        ASSERT_TRUE( Valid( mesh ) );
        ASSERT_TRUE( LayersFollowSides( mesh ) );
        EXPECT_TRUE( Fuzz( mesh, seed, 1200, seed % 2 == 0, stats, true ) );
    }
    Report( "attribute layers", stats );
    std::printf( "[ fuzz ] attribute layers: AttributeSeam refusals flip %d collapse %d\n",
                 stats.SeamRefused[static_cast<int>( Op::Flip )],
                 stats.SeamRefused[static_cast<int>( Op::Collapse )] );
    for ( const Op op : { Op::Split, Op::Flip, Op::Collapse } )
        EXPECT_GT( stats.Applied[static_cast<int>( op )], 500 ) << static_cast<int>( op );
    EXPECT_GT( stats.Applied[static_cast<int>( Op::Remove )], 50 );
    EXPECT_GT( stats.Applied[static_cast<int>( Op::Compact )], 20 );
    // Evidence the flip rule was reached, not just the easy interior. (This seam crosses the whole grid, so
    // it has no interior END and the collapse refusal cannot fire here; SeamEndCollapseIsRefused covers it.)
    EXPECT_GT( stats.SeamRefused[static_cast<int>( Op::Flip )], 20 );
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
