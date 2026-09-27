// Whole-mesh shape operations (EditMeshModelOperations.hpp): Subdivide on the generator cube.
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

    double TotalArea( const EditMesh& mesh )
    {
        return Area( mesh, Ids( mesh.TriangleIds() ) );
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

    int UVSeamEdges( const EditMesh& mesh )
    {
        int seams = 0;
        for ( const int e : mesh.EdgeIds() )
            if ( mesh.Attributes().UV( 0 )->IsSeamEdge( mesh, e ) )
                ++seams;
        return seams;
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

TEST( SubdivideMesh, UniformQuadruplesTheCubeAndMovesNothing )
{
    EditMesh cube = MakeCube();
    ASSERT_GE( cube.Attributes().UVLayerCount(), 1 );
    const int top = GroupFacing( cube, { 0, 1, 0 } );
    for ( const int t : cube.TriangleIds() )
        if ( cube.Attributes().GetPolyGroup( t ) == top )
            cube.Attributes().SetMaterialId( t, 3 );
    const int seamsBefore = UVSeamEdges( cube );
    ASSERT_GT( seamsBefore, 0 );

    const EditMesh fine = Succeeded( SubdivideMesh( cube, 1, SubdivideScheme::Uniform ) );
    EXPECT_EQ( fine.TriangleCount(), 48 );
    EXPECT_TRUE( ClosedAndConsistent( fine ) );
    EXPECT_NEAR( SignedVolume( fine ), 8.0e6, 1.0 );
    EXPECT_EQ( GroupCount( fine ), 6 );
    // A seam edge is cut in two, and both halves stay seams; no edge inside a face becomes one.
    EXPECT_EQ( UVSeamEdges( fine ), 2 * seamsBefore );
    std::vector<int> metal;
    for ( const int t : fine.TriangleIds() )
        if ( fine.Attributes().GetMaterialId( t ) == 3 )
        {
            metal.push_back( t );
            EXPECT_EQ( fine.Attributes().GetPolyGroup( t ), top );
        }
    EXPECT_EQ( metal.size(), 8u );
    EXPECT_NEAR( Area( fine, metal ), 200.0 * 200.0, 1e-2 );
}

TEST( SubdivideMesh, LoopShrinksTheCubeMonotonicallyTowardsASphere )
{
    const EditMesh cube      = MakeCube();
    double         volume    = SignedVolume( cube );
    double         roundness = 36.0 * glm::pi<double>() * volume * volume / std::pow( TotalArea( cube ), 3.0 );
    for ( int level = 1; level <= 3; ++level )
    {
        SCOPED_TRACE( level );
        const EditMesh smooth = Succeeded( SubdivideMesh( cube, level, SubdivideScheme::Loop ) );
        EXPECT_EQ( smooth.TriangleCount(), 12 << ( 2 * level ) );
        EXPECT_TRUE( ClosedAndConsistent( smooth ) );
        // Rebuilt smooth: one normal per vertex, no hard edge left where the cube's corners were.
        EXPECT_EQ( smooth.Attributes().Normals()->ElementCount(), smooth.VertexCount() );
        const double v = SignedVolume( smooth );
        // The isoperimetric quotient 36 pi V^2 / A^3 is 1 for a sphere and pi/6 for a cube.
        const double q = 36.0 * glm::pi<double>() * v * v / std::pow( TotalArea( smooth ), 3.0 );
        EXPECT_LT( v, volume );
        EXPECT_GT( q, roundness );
        EXPECT_LT( q, 1.0 );
        volume    = v;
        roundness = q;
    }
    EXPECT_GT( roundness, 0.9 );
}

TEST( SubdivideMesh, LoopKeepsAnOpenBorderWhereItWas )
{
    const EditMesh open = WithoutFace( MakeCube(), { 0, 1, 0 } );
    ASSERT_EQ( OpenEdges( open ).size(), 4u );
    const EditMesh smooth = Succeeded( SubdivideMesh( open, 2, SubdivideScheme::Loop ) );
    EXPECT_TRUE( Valid( smooth ) );
    EXPECT_EQ( OpenEdges( smooth ).size(), 16u );
    int border = 0;
    for ( const int v : smooth.VertexIds() )
    {
        if ( !smooth.IsBoundaryVertex( v ) )
            continue;
        ++border;
        const glm::vec3 p = smooth.GetPosition( v );
        EXPECT_NEAR( p.y, 200.0f, 1e-3f );
        EXPECT_NEAR( std::max( std::abs( p.x ), std::abs( p.z ) ), 100.0f, 1e-3f ) << p.x << ", " << p.z;
    }
    EXPECT_EQ( border, 16 );
    // The rest of the box is pulled in: the corners at the bottom are gone.
    EXPECT_GT( Extent( smooth, false ).y, 0.0f );
}

TEST( SubdivideMesh, RefusesOutOfRangeLevelsAndAnEmptyMesh )
{
    const EditMesh cube = MakeCube();
    ExpectRefused( SubdivideMesh( cube, 0, SubdivideScheme::Loop ), "the level must be 1 .. 5, not 0" );
    ExpectRefused( SubdivideMesh( cube, 6, SubdivideScheme::Uniform ), "not 6" );
    ExpectRefused( SubdivideMesh( EditMesh{}, 1, SubdivideScheme::Uniform ), "no triangles" );
}
