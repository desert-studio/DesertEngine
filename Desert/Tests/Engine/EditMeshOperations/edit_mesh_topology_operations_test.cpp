// Operations that re-cut the surface (EditMeshTopologyOperations.hpp): Bevel, Insert Edge Loop, Cut and
// Clean on the generator cube and cylinder. Volume is checked against the closed-form value of the cut,
// topology by count, closedness and orientation by ClosedAndConsistent, and every refusal by its words.

#include <gtest/gtest.h>

#include "OperationsTestSupport.hpp"

#include <glm/mat4x4.hpp>

#include <string>

using namespace OperationsTest;
using Desert::Index;

namespace
{
    // The edge whose two triangles face `a` and `b`.
    int EdgeBetween( const EditMesh& mesh, const glm::vec3& a, const glm::vec3& b )
    {
        for ( const int e : mesh.EdgeIds() )
        {
            const auto& t = mesh.GetEdgeTriangles( e );
            if ( t[1] == InvalidId )
                continue;
            const glm::vec3 n0 = TriangleNormal( mesh, t[0] );
            const glm::vec3 n1 = TriangleNormal( mesh, t[1] );
            if ( ( glm::dot( n0, a ) > 0.99f && glm::dot( n1, b ) > 0.99f ) ||
                 ( glm::dot( n0, b ) > 0.99f && glm::dot( n1, a ) > 0.99f ) )
                return e;
        }
        ADD_FAILURE() << "no edge between the two directions";
        return InvalidId;
    }

    int VertexAt( const EditMesh& mesh, const glm::vec3& p )
    {
        for ( const int v : mesh.VertexIds() )
            if ( glm::length( mesh.GetPosition( v ) - p ) < 1e-3f )
                return v;
        ADD_FAILURE() << "no vertex at the point";
        return InvalidId;
    }

    std::vector<int> GroupTriangles( const EditMesh& mesh, int group )
    {
        std::vector<int> out;
        for ( const int t : mesh.TriangleIds() )
            if ( mesh.Attributes().GetPolyGroup( t ) == group )
                out.push_back( t );
        return out;
    }

    ::testing::AssertionResult RefusedWith( const Common::ResultStr<MeshEditOutcome>& r,
                                            const std::string&                        needle )
    {
        if ( r.IsSuccess() )
            return ::testing::AssertionFailure() << "succeeded; expected a refusal naming \"" << needle << "\"";
        if ( r.GetError().find( needle ) == std::string::npos )
            return ::testing::AssertionFailure() << "refused, but with: " << r.GetError();
        return ::testing::AssertionSuccess();
    }

    constexpr float kWidth = 20.0f;
} // namespace

// ── Bevel ───────────────────────────────────────────────────────────────────────────────────────────────

TEST( Bevel, CubeEdgeLosesItsPrismAndGainsOneGroup )
{
    const EditMesh cube = MakeCube();
    const int      e    = EdgeBetween( cube, { 0, 1, 0 }, { 0, 0, 1 } );
    auto           r    = BevelSelection( cube, Select( cube, ElementMode::Edge, { e } ), kWidth );
    ASSERT_TRUE( r.IsSuccess() ) << r.GetError();
    const EditMesh& out = r.GetValue().Mesh;
    EXPECT_TRUE( ClosedAndConsistent( out ) );
    EXPECT_EQ( GroupCount( out ), 7 );
    // The removed prism: a right triangle with legs = width, the edge's full 200 cm long.
    EXPECT_NEAR( SignedVolume( out ), 8.0e6 - kWidth * kWidth / 2.0 * 200.0, 1.0 );

    ASSERT_EQ( r.GetValue().Selection.Mode(), ElementMode::PolyGroup );
    ASSERT_EQ( r.GetValue().Selection.Size(), 1 );
    const std::vector<int> cap = GroupTriangles( out, r.GetValue().Selection.Ids()[0] );
    EXPECT_NEAR( Area( out, cap ), std::sqrt( 2.0 ) * kWidth * 200.0, 1e-2 );
    const glm::vec3 diagonal = glm::normalize( glm::vec3( 0, 1, 1 ) );
    for ( const int t : cap )
    {
        EXPECT_GT( glm::dot( TriangleNormal( out, t ), diagonal ), 0.9999f ) << "cap triangle " << t;
        // A hard edge: the cap's own normal, not an average with the top or the front.
        ASSERT_TRUE( out.Attributes().Normals()->IsSetTriangle( t ) ) << "cap triangle " << t;
        for ( const int n : out.Attributes().Normals()->GetTriangle( t ) )
            EXPECT_GT( glm::dot( out.Attributes().Normals()->GetElement( n ), diagonal ), 0.9999f );
        EXPECT_TRUE( out.Attributes().UV( 0 )->IsSetTriangle( t ) );
    }
    // The cap's UVs at the faces' texel density: its UV area over its world area equals theirs.
    auto density = [&]( const std::vector<int>& tris )
    {
        const UVOverlay& uv  = *out.Attributes().UV( 0 );
        double           tex = 0.0;
        for ( const int t : tris )
        {
            const auto&     u = uv.GetTriangle( t );
            const glm::vec2 a = uv.GetElement( u[1] ) - uv.GetElement( u[0] );
            const glm::vec2 b = uv.GetElement( u[2] ) - uv.GetElement( u[0] );
            tex += 0.5 * std::abs( a.x * b.y - a.y * b.x );
        }
        return tex / Area( out, tris );
    };
    const std::vector<int> top = GroupTriangles( out, GroupFacing( out, { 0, 1, 0 } ) );
    EXPECT_NEAR( density( cap ) / density( top ), 1.0, 1e-3 );
}

TEST( Bevel, CubeCornerLosesItsTetrahedron )
{
    const EditMesh cube   = MakeCube();
    const int      corner = VertexAt( cube, { 100, 200, 100 } );
    auto           r      = BevelSelection( cube, Select( cube, ElementMode::Vertex, { corner } ), kWidth );
    ASSERT_TRUE( r.IsSuccess() ) << r.GetError();
    EXPECT_TRUE( ClosedAndConsistent( r.GetValue().Mesh ) );
    EXPECT_EQ( GroupCount( r.GetValue().Mesh ), 7 );
    EXPECT_NEAR( SignedVolume( r.GetValue().Mesh ), 8.0e6 - kWidth * kWidth * kWidth / 6.0, 1.0 );
    EXPECT_FALSE( r.GetValue().Mesh.IsVertex( corner ) );
}

TEST( Bevel, TwoOppositeEdgesAreTwoCaps )
{
    const EditMesh cube = MakeCube();
    const int      a    = EdgeBetween( cube, { 0, 1, 0 }, { 0, 0, 1 } );
    const int      b    = EdgeBetween( cube, { 0, -1, 0 }, { 0, 0, -1 } );
    auto           r    = BevelSelection( cube, Select( cube, ElementMode::Edge, { a, b } ), kWidth );
    ASSERT_TRUE( r.IsSuccess() ) << r.GetError();
    EXPECT_TRUE( ClosedAndConsistent( r.GetValue().Mesh ) );
    EXPECT_EQ( GroupCount( r.GetValue().Mesh ), 8 );
    EXPECT_NEAR( SignedVolume( r.GetValue().Mesh ), 8.0e6 - 2.0 * kWidth * kWidth / 2.0 * 200.0, 1.0 );
}

TEST( Bevel, Refusals )
{
    const EditMesh cube = MakeCube();
    const int      edge = EdgeBetween( cube, { 0, 1, 0 }, { 0, 0, 1 } );
    const int      side = EdgeBetween( cube, { 0, 1, 0 }, { 1, 0, 0 } ); // shares the corner (100, 200, 100)
    int            flat = InvalidId;
    for ( const int e : cube.EdgeIds() )
        if ( cube.Attributes().GetPolyGroup( cube.GetEdgeTriangles( e )[0] ) ==
             cube.Attributes().GetPolyGroup( cube.GetEdgeTriangles( e )[1] ) )
            flat = e;
    ASSERT_NE( flat, InvalidId );
    EXPECT_TRUE( RefusedWith( BevelSelection( cube, Select( cube, ElementMode::Edge, { edge, side } ), kWidth ),
                              "share vertex" ) );
    EXPECT_TRUE( RefusedWith( BevelSelection( cube, Select( cube, ElementMode::Edge, { edge } ), 250.0f ),
                              "reaches vertex" ) );
    EXPECT_TRUE(
         RefusedWith( BevelSelection( cube, Select( cube, ElementMode::Edge, { flat } ), kWidth ), "is flat" ) );
    EXPECT_TRUE(
         RefusedWith( BevelSelection( cube, Select( cube, ElementMode::Edge, { edge } ), 0.0f ), "above 0" ) );
    EXPECT_TRUE( RefusedWith( BevelSelection( cube, Select( cube, ElementMode::Triangle, { 0 } ), kWidth ),
                              "Edge or Vertex" ) );
}

// ── Insert Edge Loop ────────────────────────────────────────────────────────────────────────────────────

TEST( InsertEdgeLoop, CubeRingIsClosedAndLevel )
{
    const EditMesh cube = MakeCube();
    const int      e    = EdgeBetween( cube, { 1, 0, 0 }, { 0, 0, 1 } ); // a vertical edge
    auto           r    = InsertEdgeLoop( cube, Select( cube, ElementMode::Edge, { e } ), 0.3f );
    ASSERT_TRUE( r.IsSuccess() ) << r.GetError();
    const EditMesh& out = r.GetValue().Mesh;
    EXPECT_TRUE( ClosedAndConsistent( out ) );
    EXPECT_EQ( out.VertexCount(), cube.VertexCount() + 4 );
    EXPECT_EQ( out.TriangleCount(), cube.TriangleCount() + 8 );
    EXPECT_EQ( GroupCount( out ), 6 );
    EXPECT_NEAR( SignedVolume( out ), 8.0e6, 1.0 );
    EXPECT_NE( r.GetValue().Report.find( "closed ring of 4" ), std::string::npos ) << r.GetValue().Report;
    // The loop is level: every new vertex at the same height, 30 % of the way from the selected edge's
    // lower-ID end - the orientation carried round the ring, not re-guessed per edge.
    const float from = cube.GetPosition( cube.GetEdgeVertices( e )[0] ).y;
    const float y    = from + 0.3f * ( 200.0f - 2.0f * from );
    ASSERT_EQ( r.GetValue().Selection.Size(), 4 );
    for ( const int loopEdge : r.GetValue().Selection.Ids() )
        for ( const int v : out.GetEdgeVertices( loopEdge ) )
            EXPECT_NEAR( out.GetPosition( v ).y, y, 1e-3f ) << "loop vertex " << v;
}

TEST( InsertEdgeLoop, CylinderWallRingAndARingStoppedByTheCaps )
{
    const EditMesh cylinder = MakeCylinder12();
    int            vertical = InvalidId;
    int            rim      = InvalidId;
    const int      top      = GroupFacing( cylinder, { 0, 1, 0 } );
    const int      bottom   = GroupFacing( cylinder, { 0, -1, 0 } );
    int            wall     = InvalidId;
    for ( const int t : cylinder.TriangleIds() )
        if ( const int g = cylinder.Attributes().GetPolyGroup( t ); g != top && g != bottom )
            wall = g;
    for ( const int e : cylinder.EdgeIds() )
    {
        const auto&     t  = cylinder.GetEdgeTriangles( e );
        const auto&     v  = cylinder.GetEdgeVertices( e );
        const float     dy = std::abs( cylinder.GetPosition( v[0] ).y - cylinder.GetPosition( v[1] ).y );
        const bool      w0 = cylinder.Attributes().GetPolyGroup( t[0] ) == wall;
        const bool      w1 = cylinder.Attributes().GetPolyGroup( t[1] ) == wall;
        const glm::vec3 d  = cylinder.GetPosition( v[0] ) - cylinder.GetPosition( v[1] );
        if ( w0 && w1 && dy > 199.0f && std::abs( d.x ) + std::abs( d.z ) < 1e-3f ) // not a quad diagonal
            vertical = e;
        if ( w0 != w1 )
            rim = e;
    }
    ASSERT_NE( vertical, InvalidId );
    ASSERT_NE( rim, InvalidId );

    auto ring = InsertEdgeLoop( cylinder, Select( cylinder, ElementMode::Edge, { vertical } ), 0.5f );
    ASSERT_TRUE( ring.IsSuccess() ) << ring.GetError();
    EXPECT_TRUE( ClosedAndConsistent( ring.GetValue().Mesh ) );
    EXPECT_EQ( ring.GetValue().Mesh.VertexCount(), cylinder.VertexCount() + 12 );
    EXPECT_NEAR( SignedVolume( ring.GetValue().Mesh ), 6.0e6, 1.0 );

    // Across the wall from a rim edge: one quad, both ends in the caps' fan triangles.
    auto stopped = InsertEdgeLoop( cylinder, Select( cylinder, ElementMode::Edge, { rim } ), 0.5f );
    ASSERT_TRUE( stopped.IsSuccess() ) << stopped.GetError();
    EXPECT_TRUE( ClosedAndConsistent( stopped.GetValue().Mesh ) );
    EXPECT_EQ( stopped.GetValue().Mesh.VertexCount(), cylinder.VertexCount() + 2 );
    EXPECT_NE( stopped.GetValue().Report.find( "open ring of 1 quads, ends at a triangle" ), std::string::npos )
         << stopped.GetValue().Report;
}

TEST( InsertEdgeLoop, Refusals )
{
    const EditMesh cylinder = MakeCylinder12();
    const EditMesh cube     = MakeCube();
    const int      e        = EdgeBetween( cube, { 1, 0, 0 }, { 0, 0, 1 } );
    const int      f        = EdgeBetween( cube, { -1, 0, 0 }, { 0, 0, 1 } );
    int            spoke    = InvalidId; // a cap spoke: fan triangles on both sides
    const int      top      = GroupFacing( cylinder, { 0, 1, 0 } );
    for ( const int s : cylinder.EdgeIds() )
        if ( cylinder.Attributes().GetPolyGroup( cylinder.GetEdgeTriangles( s )[0] ) == top &&
             cylinder.GetEdgeTriangles( s )[1] != InvalidId &&
             cylinder.Attributes().GetPolyGroup( cylinder.GetEdgeTriangles( s )[1] ) == top )
            spoke = s;
    ASSERT_NE( spoke, InvalidId );
    EXPECT_TRUE( RefusedWith( InsertEdgeLoop( cube, Select( cube, ElementMode::Edge, { e } ), 0.0f ),
                              "between 0 and 1" ) );
    EXPECT_TRUE( RefusedWith( InsertEdgeLoop( cube, Select( cube, ElementMode::Edge, { e } ), 1.0f ),
                              "between 0 and 1" ) );
    EXPECT_TRUE( RefusedWith( InsertEdgeLoop( cube, Select( cube, ElementMode::Edge, { e, f } ), 0.5f ),
                              "exactly one edge" ) );
    EXPECT_TRUE( RefusedWith( InsertEdgeLoop( cylinder, Select( cylinder, ElementMode::Edge, { spoke } ), 0.5f ),
                              "no quad on either side" ) );
}

// ── Cut ─────────────────────────────────────────────────────────────────────────────────────────────────

TEST( Cut, OneGroupAndAllGroups )
{
    const EditMesh cube = MakeCube();
    const int      top  = GroupFacing( cube, { 0, 1, 0 } );
    const CutPlane x10{ { 10, 0, 0 }, { 1, 0, 0 } };

    auto one = CutSelection( cube, Select( cube, ElementMode::PolyGroup, { top } ), x10 );
    ASSERT_TRUE( one.IsSuccess() ) << one.GetError();
    EXPECT_TRUE( ClosedAndConsistent( one.GetValue().Mesh ) );
    EXPECT_EQ( GroupCount( one.GetValue().Mesh ), 7 );
    EXPECT_NEAR( SignedVolume( one.GetValue().Mesh ), 8.0e6, 1.0 );
    EXPECT_EQ( one.GetValue().Selection.Size(), 2 );
    // The positive half is exactly the part of the top past x = 10.
    const int newGroup = one.GetValue().Selection.Ids()[1];
    EXPECT_NEAR( Area( one.GetValue().Mesh, GroupTriangles( one.GetValue().Mesh, newGroup ) ), 90.0 * 200.0,
                 1e-2 );

    ElementSelection all( ElementMode::PolyGroup );
    for ( const int t : cube.TriangleIds() )
        if ( !all.Contains( cube.Attributes().GetPolyGroup( t ) ) )
            ASSERT_TRUE( all.Add( cube, cube.Attributes().GetPolyGroup( t ) ).IsSuccess() );
    auto four = CutSelection( cube, all, x10 );
    ASSERT_TRUE( four.IsSuccess() ) << four.GetError();
    EXPECT_TRUE( ClosedAndConsistent( four.GetValue().Mesh ) );
    EXPECT_EQ( GroupCount( four.GetValue().Mesh ), 10 ); // top, bottom, front, back cut; the x faces not

    EXPECT_TRUE(
         RefusedWith( CutSelection( cube, all, CutPlane{ { 500, 0, 0 }, { 1, 0, 0 } } ), "crosses none" ) );
}

TEST( Cut, ScreenLinePlaneContainsTheViewRays )
{
    // Identity view: NDC x = world x, the line x = 0.2 top to bottom is the plane x = 0.2.
    auto plane = CutPlaneFromScreenLine( glm::mat4( 1.0f ), { 0.2f, -1.0f }, { 0.2f, 1.0f } );
    ASSERT_TRUE( plane.IsSuccess() ) << plane.GetError();
    EXPECT_NEAR( std::abs( plane.GetValue().Normal.x ), 1.0f, 1e-5f );
    EXPECT_NEAR( glm::dot( glm::vec3( 0.2f, 0.7f, -0.3f ) - plane.GetValue().Point, plane.GetValue().Normal ),
                 0.0f, 1e-5f );
    EXPECT_FALSE( CutPlaneFromScreenLine( glm::mat4( 1.0f ), { 0.2f, 0.1f }, { 0.2f, 0.1f } ).IsSuccess() );
    EXPECT_FALSE( CutPlaneFromScreenLine( glm::mat4( 0.0f ), { 0.0f, 0.0f }, { 1.0f, 0.0f } ).IsSuccess() );
}

// ── Clean ───────────────────────────────────────────────────────────────────────────────────────────────

TEST( Clean, WeldsASplitCubeAndDropsTheDebris )
{
    // The box as a render buffer imports it WITHOUT a weld: 24 vertices, six loose faces.
    const ShapeMesh box = MakeBox( glm::vec3( 200.0f ) );
    EditMesh        split;
    for ( const auto& vertex : box.Vertices )
        (void)split.AppendVertex( vertex.Position );
    for ( const Index& index : box.Indices )
    {
        int t = InvalidId;
        ASSERT_EQ( split.AppendTriangle( static_cast<int>( index.V1 ), static_cast<int>( index.V2 ),
                                         static_cast<int>( index.V3 ), t ),
                   EditResult::Ok );
    }
    // Debris: one isolated vertex, and a loose triangle whose two corners are 0.005 cm apart.
    (void)split.AppendVertex( { 500, 0, 0 } );
    const int a      = split.AppendVertex( { 600, 0, 0 } );
    const int b      = split.AppendVertex( { 600.005f, 0, 0 } );
    const int c      = split.AppendVertex( { 600, 50, 0 } );
    int       sliver = InvalidId;
    ASSERT_EQ( split.AppendTriangle( a, b, c, sliver ), EditResult::Ok );
    ASSERT_EQ( split.VertexCount(), 28 );
    ASSERT_EQ( OpenEdges( split ).size(), 27u ); // 24 face edges + the loose triangle's 3

    auto r = CleanMesh( split, 0.01f );
    ASSERT_TRUE( r.IsSuccess() ) << r.GetError();
    const EditMesh&    out = r.GetValue().Edit.Mesh;
    const CleanCounts& n   = r.GetValue().Counts;
    EXPECT_TRUE( Valid( out ) );
    EXPECT_EQ( out.VertexCount(), 8 );
    EXPECT_EQ( out.TriangleCount(), 12 );
    EXPECT_TRUE( OpenEdges( out ).empty() );
    EXPECT_NEAR( SignedVolume( out ), 8.0e6, 1.0 );
    EXPECT_EQ( n.WeldedVertices, 16 + 1 );
    EXPECT_EQ( n.CollapsedRemoved, 1 );
    EXPECT_EQ( n.IsolatedRemoved, 1 + 2 ); // the lone vertex, then a and c once their triangle is gone
    EXPECT_EQ( n.ZeroAreaFlipped + n.ZeroAreaKept, 0 );

    // Nothing within the tolerance: the same mesh, minus nothing but the isolated vertex.
    auto none = CleanMesh( out, 0.01f );
    ASSERT_TRUE( none.IsSuccess() ) << none.GetError();
    EXPECT_EQ( none.GetValue().Counts.WeldedVertices, 0 );
    EXPECT_EQ( none.GetValue().Edit.Mesh.VertexCount(), 8 );
    EXPECT_FALSE( CleanMesh( out, -1.0f ).IsSuccess() );
}
