// Plane Cut and Trim (EditMeshModelOperations.hpp) on the generator cube (200 cm: x and z centred on the origin, y
// from 0 to 200). Plane Cut: the halves are closed, their volumes sum to the cube's, the cap lies in the plane,
// faces out of its half, is one new polygroup and carries UVs. Trim: the removed area is the cutter's footprint in
// closed form, the cut is left open, a mirroring cutter transform trims the same, and every refusal by its words.

#include <gtest/gtest.h>

#include "OperationsTestSupport.hpp"

#include <Engine/Geometry/EditMeshModelOperations.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <map>
#include <string>

using namespace OperationsTest;

namespace
{
    template <typename Range>
    std::vector<int> Ids( const Range& range )
    {
        std::vector<int> out;
        for ( const int id : range )
            out.push_back( id );
        return out;
    }

    double TotalArea( const EditMesh& mesh )
    {
        return Area( mesh, Ids( mesh.TriangleIds() ) );
    }

    EditMesh WithoutFace( EditMesh mesh, const glm::vec3& direction )
    {
        const int group = GroupFacing( mesh, direction );
        for ( const int t : Ids( mesh.TriangleIds() ) )
            if ( mesh.Attributes().GetPolyGroup( t ) == group )
                EXPECT_EQ( mesh.RemoveTriangle( t, true ), EditResult::Ok );
        return mesh;
    }

    PlaneCutOutcome Cut( const EditMesh& mesh, const CutPlane& plane, PlaneCutMode mode, bool fill )
    {
        auto result = PlaneCutMesh( mesh, plane, mode, fill );
        EXPECT_TRUE( result.IsSuccess() ) << result.GetError();
        return result.IsSuccess() ? result.GetValue() : PlaneCutOutcome{};
    }

    template <typename T>
    void ExpectRefused( const Common::ResultStr<T>& result, const std::string& words )
    {
        ASSERT_FALSE( result.IsSuccess() );
        EXPECT_NE( result.GetError().find( words ), std::string::npos ) << result.GetError();
    }

    std::vector<int> CapOf( const PlaneCutOutcome& out )
    {
        return TrianglesOf( out.Kept.Mesh, out.Kept.Selection );
    }

    // The cap: every corner in the plane, every face looking out of the kept half (against the normal), UVs
    // set with area.
    void ExpectFlatCap( const EditMesh& mesh, const std::vector<int>& cap, const CutPlane& plane )
    {
        ASSERT_FALSE( cap.empty() );
        const glm::vec3  n  = glm::normalize( plane.Normal );
        const UVOverlay* uv = mesh.Attributes().UV( 0 );
        for ( const int t : cap )
        {
            const auto& c = mesh.GetTriangle( t );
            for ( const int v : c )
                EXPECT_NEAR( glm::dot( mesh.GetPosition( v ) - plane.Point, n ), 0.0f, 1e-3f ) << "vertex " << v;
            const glm::vec3 face = glm::cross( mesh.GetPosition( c[1] ) - mesh.GetPosition( c[0] ),
                                               mesh.GetPosition( c[2] ) - mesh.GetPosition( c[0] ) );
            EXPECT_LT( glm::dot( face, n ), 0.0f ) << "cap triangle " << t << " faces into its half";
            ASSERT_NE( uv, nullptr );
            ASSERT_TRUE( uv->IsSetTriangle( t ) );
            const auto&     u = uv->GetTriangle( t );
            const glm::vec2 a = uv->GetElement( u[1] ) - uv->GetElement( u[0] );
            const glm::vec2 b = uv->GetElement( u[2] ) - uv->GetElement( u[0] );
            EXPECT_GT( std::abs( a.x * b.y - a.y * b.x ), 0.0f ) << "cap triangle " << t << " has no UV area";
        }
    }
} // namespace

TEST( PlaneCutMesh, CubeInHalvesIsTwoClosedMeshesThatSumToIt )
{
    const EditMesh cube = MakeCube();
    const CutPlane plane{ { 50, 0, 0 }, { 1, 0, 0 } };
    const auto     out = Cut( cube, plane, PlaneCutMode::KeepBothHalves, true );
    ASSERT_TRUE( out.OtherHalf.has_value() );
    EXPECT_TRUE( ClosedAndConsistent( out.Kept.Mesh ) );
    EXPECT_TRUE( ClosedAndConsistent( *out.OtherHalf ) );
    EXPECT_NEAR( SignedVolume( out.Kept.Mesh ), 50.0 * 200.0 * 200.0, 1.0 );
    EXPECT_NEAR( SignedVolume( *out.OtherHalf ), 150.0 * 200.0 * 200.0, 1.0 );
    // Five of the cube's groups reach each half, plus the cap.
    EXPECT_EQ( GroupCount( out.Kept.Mesh ), 6 );
    EXPECT_EQ( GroupCount( *out.OtherHalf ), 6 );
    const std::vector<int> cap = CapOf( out );
    ExpectFlatCap( out.Kept.Mesh, cap, plane );
    EXPECT_NEAR( Area( out.Kept.Mesh, cap ), 200.0 * 200.0, 1e-2 );
}

TEST( PlaneCutMesh, AnObliquePlaneKeepsTheVolume )
{
    const EditMesh cube = MakeCube();
    const CutPlane plane{ { 30, 110, -20 }, { 1.0f, 0.4f, -0.3f } };
    const auto     out = Cut( cube, plane, PlaneCutMode::KeepBothHalves, true );
    ASSERT_TRUE( out.OtherHalf.has_value() );
    EXPECT_TRUE( ClosedAndConsistent( out.Kept.Mesh ) );
    EXPECT_TRUE( ClosedAndConsistent( *out.OtherHalf ) );
    const double kept  = SignedVolume( out.Kept.Mesh );
    const double other = SignedVolume( *out.OtherHalf );
    EXPECT_GT( kept, 0.0 );
    EXPECT_GT( other, kept );
    EXPECT_NEAR( kept + other, 8.0e6, 10.0 );
    ExpectFlatCap( out.Kept.Mesh, CapOf( out ), plane );
    // Every kept vertex is on the positive side (or in the plane).
    const glm::vec3 n = glm::normalize( plane.Normal );
    for ( const int v : out.Kept.Mesh.VertexIds() )
        EXPECT_GT( glm::dot( out.Kept.Mesh.GetPosition( v ) - plane.Point, n ), -1e-3f ) << "vertex " << v;
}

TEST( PlaneCutMesh, DiscardKeepsOnlyThePositiveHalf )
{
    const auto out =
         Cut( MakeCube(), CutPlane{ { 0, 60, 0 }, { 0, -1, 0 } }, PlaneCutMode::DiscardNegativeSide, true );
    EXPECT_FALSE( out.OtherHalf.has_value() );
    EXPECT_TRUE( ClosedAndConsistent( out.Kept.Mesh ) );
    EXPECT_NEAR( SignedVolume( out.Kept.Mesh ), 60.0 * 200.0 * 200.0, 1.0 );
}

TEST( PlaneCutMesh, WithoutFillTheCutStaysOpen )
{
    const auto out =
         Cut( MakeCube(), CutPlane{ { 50, 0, 0 }, { 1, 0, 0 } }, PlaneCutMode::DiscardNegativeSide, false );
    EXPECT_TRUE( out.Kept.Selection.Empty() );
    EXPECT_EQ( GroupCount( out.Kept.Mesh ), 5 );
    EXPECT_FALSE( OpenEdges( out.Kept.Mesh ).empty() );
    EXPECT_NEAR( TotalArea( out.Kept.Mesh ), 200.0 * 200.0 + 4.0 * 50.0 * 200.0, 1e-2 );
}

TEST( PlaneCutMesh, TheOpenBorderAcrossThePlaneCannotBeCapped )
{
    const EditMesh open = WithoutFace( MakeCube(), { -1, 0, 0 } );
    ExpectRefused(
         PlaneCutMesh( open, CutPlane{ { 0, 100, 0 }, { 0, 1, 0 } }, PlaneCutMode::KeepBothHalves, true ),
         "open border crosses the plane" );
    // Without the cap it is an ordinary cut.
    const auto out = Cut( open, CutPlane{ { 0, 100, 0 }, { 0, 1, 0 } }, PlaneCutMode::KeepBothHalves, false );
    ASSERT_TRUE( out.OtherHalf.has_value() );
    EXPECT_NEAR( TotalArea( out.Kept.Mesh ) + TotalArea( *out.OtherHalf ), 5.0 * 200.0 * 200.0, 1e-1 );
    // The open face is on the discarded side: the rim is closed and capped.
    const auto capped = Cut( open, CutPlane{ { 0, 0, 0 }, { 1, 0, 0 } }, PlaneCutMode::DiscardNegativeSide, true );
    EXPECT_TRUE( ClosedAndConsistent( capped.Kept.Mesh ) );
    EXPECT_NEAR( SignedVolume( capped.Kept.Mesh ), 100.0 * 200.0 * 200.0, 1.0 );
}

TEST( PlaneCutMesh, ASectionWithAHoleIsNotCapped )
{
    // A hollow cube: the generator cube plus a half-size cube inside it wound inwards (its cavity). Cut
    // across, the section is a square ring: the inner outline runs clockwise, a hole in the outer one.
    const EditMesh cube = MakeCube();
    EditMesh       hollow;
    for ( const bool inner : { false, true } )
    {
        std::map<int, int> at;
        for ( const int v : cube.VertexIds() )
        {
            const glm::vec3 p = cube.GetPosition( v );
            at[v] =
                 hollow.AppendVertex( inner ? glm::vec3( 0, 100, 0 ) + 0.5f * ( p - glm::vec3( 0, 100, 0 ) ) : p );
        }
        for ( const int t : cube.TriangleIds() )
        {
            const auto& c     = cube.GetTriangle( t );
            int         added = InvalidId;
            EXPECT_EQ( inner ? hollow.AppendTriangle( at[c[0]], at[c[2]], at[c[1]], added )
                             : hollow.AppendTriangle( at[c[0]], at[c[1]], at[c[2]], added ),
                       EditResult::Ok );
        }
    }
    EXPECT_NEAR( SignedVolume( hollow ), 8.0e6 - 1.0e6, 1.0 );
    ExpectRefused(
         PlaneCutMesh( hollow, CutPlane{ { 0, 100, 0 }, { 0, 1, 0 } }, PlaneCutMode::KeepBothHalves, true ),
         "runs clockwise - it is a hole" );
    // Without the cap the ring is simply open.
    const auto open = Cut( hollow, CutPlane{ { 0, 100, 0 }, { 0, 1, 0 } }, PlaneCutMode::KeepBothHalves, false );
    ASSERT_TRUE( open.OtherHalf.has_value() );
    EXPECT_FALSE( OpenEdges( open.Kept.Mesh ).empty() );
}

TEST( PlaneCutMesh, Refusals )
{
    const EditMesh cube = MakeCube();
    ExpectRefused( PlaneCutMesh( cube, CutPlane{ {}, { 0, 0, 0 } }, PlaneCutMode::KeepBothHalves, true ),
                   "zero normal" );
    ExpectRefused(
         PlaneCutMesh( cube, CutPlane{ { 150, 0, 0 }, { 1, 0, 0 } }, PlaneCutMode::KeepBothHalves, true ),
         "does not cross the mesh" );
    ExpectRefused(
         PlaneCutMesh( cube, CutPlane{ { 150, 0, 0 }, { -1, 0, 0 } }, PlaneCutMode::DiscardNegativeSide, true ),
         "does not cross the mesh" );
    // A plane through a face: that face faces along the normal, so it closes the kept side and nothing goes.
    ExpectRefused(
         PlaneCutMesh( cube, CutPlane{ { 100, 0, 0 }, { -1, 0, 0 } }, PlaneCutMode::DiscardNegativeSide, true ),
         "does not cross the mesh" );
    ExpectRefused( PlaneCutMesh( EditMesh{}, CutPlane{}, PlaneCutMode::KeepBothHalves, true ), "no triangle" );
}

TEST( TrimMesh, RemoveInsideCutsTheCutterFootprintOutOpen )
{
    const EditMesh cube = MakeCube();
    // The cutter: the cube halved in size, centred on the +X face - it spans x 50..150, y 50..150, z -50..50.
    const glm::mat4 toMesh = glm::scale( glm::translate( glm::mat4( 1.0f ), { 100, 100, 0 } ), glm::vec3( 0.5f ) );
    auto            inside = TrimMesh( cube, cube, toMesh, TrimSide::RemoveInside );
    ASSERT_TRUE( inside.IsSuccess() ) << inside.GetError();
    const EditMesh& trimmed = inside.GetValue().Mesh;
    // The +X face loses a 100 x 100 square; the cut is left open (UE's Trim is not a solid Boolean).
    EXPECT_NEAR( TotalArea( trimmed ), 6.0 * 200.0 * 200.0 - 100.0 * 100.0, 1e-1 );
    EXPECT_EQ( OpenEdges( trimmed ).empty(), false );
    EXPECT_TRUE( Valid( trimmed ) );

    auto outside = TrimMesh( cube, cube, toMesh, TrimSide::RemoveOutside );
    ASSERT_TRUE( outside.IsSuccess() ) << outside.GetError();
    EXPECT_NEAR( TotalArea( outside.GetValue().Mesh ), 100.0 * 100.0, 1e-1 );
}

TEST( TrimMesh, AMirroringCutterTransformTrimsTheSame )
{
    const EditMesh  cube = MakeCube();
    const glm::mat4 toMesh =
         glm::scale( glm::translate( glm::mat4( 1.0f ), { 100, 100, 0 } ), { -0.5f, 0.5f, 0.5f } );
    auto result = TrimMesh( cube, cube, toMesh, TrimSide::RemoveInside );
    ASSERT_TRUE( result.IsSuccess() ) << result.GetError();
    EXPECT_NEAR( TotalArea( result.GetValue().Mesh ), 6.0 * 200.0 * 200.0 - 100.0 * 100.0, 1e-1 );
}

TEST( TrimMesh, Refusals )
{
    const EditMesh  cube   = MakeCube();
    const glm::mat4 toMesh = glm::scale( glm::translate( glm::mat4( 1.0f ), { 100, 100, 0 } ), glm::vec3( 0.5f ) );
    ExpectRefused( TrimMesh( cube, WithoutFace( cube, { 1, 0, 0 } ), toMesh, TrimSide::RemoveInside ),
                   "the cutter is open at its edge" );
    EditMesh dented = cube;
    dented.SetPosition( Ids( dented.VertexIds() ).front(), glm::vec3( 0.0f, 100.0f, 0.0f ) );
    ExpectRefused( TrimMesh( cube, dented, toMesh, TrimSide::RemoveInside ), "the cutter is not convex" );
    ExpectRefused(
         TrimMesh( cube, cube, glm::translate( glm::mat4( 1.0f ), { 1000, 0, 0 } ), TrimSide::RemoveInside ),
         "does not reach the mesh" );
    ExpectRefused( TrimMesh( cube, cube,
                             glm::scale( glm::translate( glm::mat4( 1.0f ), { 0, -100, 0 } ), glm::vec3( 2.0f ) ),
                             TrimSide::RemoveInside ),
                   "all" );
    ExpectRefused( TrimMesh( cube, EditMesh{}, toMesh, TrimSide::RemoveInside ), "the cutter has no triangle" );
}
