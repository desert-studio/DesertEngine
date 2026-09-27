// Trim (EditMeshModelOperations.hpp) on the generator cube (200 cm: x and z centred on the origin, y from 0 to
// 200): the removed area is the cutter's footprint in closed form, the cut is left open, a mirroring cutter
// transform trims the same, and every refusal by its words. Plane Cut runs on the DynamicMesh3: MeshPlaneOperation
// suite.

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

    template <typename T>
    void ExpectRefused( const Common::ResultStr<T>& result, const std::string& words )
    {
        ASSERT_FALSE( result.IsSuccess() );
        EXPECT_NE( result.GetError().find( words ), std::string::npos ) << result.GetError();
    }
} // namespace

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
