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
