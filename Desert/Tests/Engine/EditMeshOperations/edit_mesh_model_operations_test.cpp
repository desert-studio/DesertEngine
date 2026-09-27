// Whole-mesh shape operations (EditMeshModelOperations.hpp): Mirror on the generator cube.
// Shape by closed-form volume, topology by count, closedness and orientation by ClosedAndConsistent, the seam
// by the absence of coincident vertices, and every refusal by its words.

#include <gtest/gtest.h>

#include "OperationsTestSupport.hpp"

#include <Engine/Geometry/EditMeshModelOperations.hpp>

#include <glm/gtc/constants.hpp>

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

    // Removes every triangle of the polygroup facing `direction`: the cube with that face open.
    EditMesh WithoutFace( EditMesh mesh, const glm::vec3& direction )
    {
        const int group = GroupFacing( mesh, direction );
        for ( const int t : Ids( mesh.TriangleIds() ) )
            if ( mesh.Attributes().GetPolyGroup( t ) == group )
                EXPECT_EQ( mesh.RemoveTriangle( t, true ), EditResult::Ok );
        return mesh;
    }

    // Pairs of distinct vertices closer than 1e-3 cm: a seam that was not welded leaves one per seam vertex.
    int CoincidentPairs( const EditMesh& mesh )
    {
        const std::vector<int> ids   = Ids( mesh.VertexIds() );
        int                    pairs = 0;
        for ( size_t i = 0; i < ids.size(); ++i )
            for ( size_t j = i + 1; j < ids.size(); ++j )
                if ( glm::length( mesh.GetPosition( ids[i] ) - mesh.GetPosition( ids[j] ) ) < 1e-3f )
                    ++pairs;
        return pairs;
    }

    glm::vec3 Extent( const EditMesh& mesh, bool upper )
    {
        glm::vec3 r( upper ? -1e30f : 1e30f );
        for ( const int v : mesh.VertexIds() )
            r = upper ? glm::max( r, mesh.GetPosition( v ) ) : glm::min( r, mesh.GetPosition( v ) );
        return r;
    }

    EditMesh Succeeded( const Common::ResultStr<MeshEditOutcome>& result )
    {
        EXPECT_TRUE( result.IsSuccess() ) << result.GetError();
        return result.IsSuccess() ? result.GetValue().Mesh : EditMesh{};
    }

    void ExpectRefused( const Common::ResultStr<MeshEditOutcome>& result, const std::string& words )
    {
        ASSERT_FALSE( result.IsSuccess() );
        EXPECT_NE( result.GetError().find( words ), std::string::npos ) << result.GetError();
    }
} // namespace

TEST( MirrorMesh, AddingTheReflectionClosesAnOpenHalf )
{
    const EditMesh half = WithoutFace( MakeCube(), { -1, 0, 0 } );
    ASSERT_EQ( OpenEdges( half ).size(), 4u );
    const EditMesh whole = Succeeded(
         MirrorMesh( half, CutPlane{ { -100, 0, 0 }, { 1, 0, 0 } }, MirrorMode::AddMirroredCopy, 0.01f ) );
    EXPECT_EQ( whole.TriangleCount(), 20 );
    EXPECT_EQ( whole.VertexCount(), 12 );
    EXPECT_EQ( CoincidentPairs( whole ), 0 );
    EXPECT_TRUE( ClosedAndConsistent( whole ) );
    EXPECT_NEAR( SignedVolume( whole ), 1.6e7, 1.0 );
    EXPECT_NEAR( Extent( whole, false ).x, -300.0f, 1e-3f );
    EXPECT_NEAR( Extent( whole, true ).x, 100.0f, 1e-3f );
}

TEST( MirrorMesh, AFaceLyingInThePlaneIsDroppedNotDoubled )
{
    const auto result =
         MirrorMesh( MakeCube(), CutPlane{ { 100, 0, 0 }, { 1, 0, 0 } }, MirrorMode::AddMirroredCopy, 0.01f );
    const EditMesh whole = Succeeded( result );
    EXPECT_EQ( whole.TriangleCount(), 20 );
    EXPECT_EQ( CoincidentPairs( whole ), 0 );
    EXPECT_TRUE( ClosedAndConsistent( whole ) );
    EXPECT_NEAR( SignedVolume( whole ), 1.6e7, 1.0 );
    EXPECT_NE( result.GetValue().Report.find( "2 triangles lying in the plane dropped" ), std::string::npos )
         << result.GetValue().Report;
}

TEST( MirrorMesh, CutAndMirrorKeepsTheSideTheNormalPointsTo )
{
    const EditMesh cube = MakeCube();
    const EditMesh right =
         Succeeded( MirrorMesh( cube, CutPlane{ { 50, 0, 0 }, { 1, 0, 0 } }, MirrorMode::CutAndMirror, 0.01f ) );
    EXPECT_TRUE( ClosedAndConsistent( right ) );
    EXPECT_EQ( CoincidentPairs( right ), 0 );
    EXPECT_NEAR( SignedVolume( right ), 100.0 * 200.0 * 200.0, 1.0 );
    EXPECT_NEAR( Extent( right, false ).x, 0.0f, 1e-3f );
    EXPECT_NEAR( Extent( right, true ).x, 100.0f, 1e-3f );
    // Symmetric: every vertex has its reflection in the mesh.
    for ( const int v : right.VertexIds() )
    {
        glm::vec3 p = right.GetPosition( v );
        p.x         = 100.0f - p.x;
        bool found  = false;
        for ( const int o : right.VertexIds() )
            found = found || glm::length( right.GetPosition( o ) - p ) < 1e-3f;
        EXPECT_TRUE( found ) << "vertex " << v;
    }

    const EditMesh left =
         Succeeded( MirrorMesh( cube, CutPlane{ { 50, 0, 0 }, { -1, 0, 0 } }, MirrorMode::CutAndMirror, 0.01f ) );
    EXPECT_TRUE( ClosedAndConsistent( left ) );
    EXPECT_EQ( CoincidentPairs( left ), 0 );
    EXPECT_NEAR( SignedVolume( left ), 300.0 * 200.0 * 200.0, 1.0 );
}

TEST( MirrorMesh, TheReflectionCarriesMaterialAndUVs )
{
    EditMesh  half = WithoutFace( MakeCube(), { -1, 0, 0 } );
    const int top  = GroupFacing( half, { 0, 1, 0 } );
    for ( const int t : half.TriangleIds() )
        if ( half.Attributes().GetPolyGroup( t ) == top )
            half.Attributes().SetMaterialId( t, 7 );
    const EditMesh whole = Succeeded(
         MirrorMesh( half, CutPlane{ { -100, 0, 0 }, { 1, 0, 0 } }, MirrorMode::AddMirroredCopy, 0.01f ) );
    std::vector<int> metal;
    for ( const int t : whole.TriangleIds() )
        if ( whole.Attributes().GetMaterialId( t ) == 7 )
            metal.push_back( t );
    EXPECT_EQ( metal.size(), 4u );
    EXPECT_NEAR( Area( whole, metal ), 2.0 * 200.0 * 200.0, 1e-2 );
    const UVOverlay* uv = whole.Attributes().UV( 0 );
    for ( const int t : whole.TriangleIds() )
        EXPECT_TRUE( uv->IsSetTriangle( t ) ) << "triangle " << t;
}

TEST( MirrorMesh, RefusesWhatItCannotMirror )
{
    const EditMesh cube = MakeCube();
    ExpectRefused( MirrorMesh( cube, CutPlane{ {}, { 0, 0, 0 } }, MirrorMode::AddMirroredCopy, 0.01f ),
                   "zero normal" );
    ExpectRefused( MirrorMesh( cube, CutPlane{ {}, { 1, 0, 0 } }, MirrorMode::AddMirroredCopy, -1.0f ),
                   "must be >= 0 cm, not -1" );
    ExpectRefused( MirrorMesh( cube, CutPlane{ { 200, 0, 0 }, { 1, 0, 0 } }, MirrorMode::CutAndMirror, 0.01f ),
                   "nothing is left to mirror - 0 triangles lie in the plane, 12 on its far side" );
    // A fin: two triangles hinged on an edge that lies in the plane - the reflection would put a third and a
    // fourth triangle on that edge.
    EditMesh  fin;
    const int a = fin.AppendVertex( { 0, 0, 0 } );
    const int b = fin.AppendVertex( { 0, 100, 0 } );
    const int c = fin.AppendVertex( { 100, 50, 50 } );
    const int d = fin.AppendVertex( { 100, 50, -50 } );
    int       t = InvalidId;
    ASSERT_EQ( fin.AppendTriangle( a, b, c, t ), EditResult::Ok );
    ASSERT_EQ( fin.AppendTriangle( b, a, d, t ), EditResult::Ok );
    const auto r = MirrorMesh( fin, CutPlane{ {}, { 1, 0, 0 } }, MirrorMode::AddMirroredCopy, 0.01f );
    ExpectRefused( r, "cannot join the surface" );
}
