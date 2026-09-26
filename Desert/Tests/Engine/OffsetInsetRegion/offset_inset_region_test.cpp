// OffsetMeshRegion / InsetMeshRegion (ported from UE 5.8, P11a) on a PolyGroup cube: the topology UE's
// algorithm produces for one face, the new face's position, the wall / ring groups, and a closed result.
#include "Engine/Geometry/UECore/DynamicMesh/DynamicMesh3.hpp"
#include "Engine/Geometry/UECore/DynamicMesh/DynamicMeshAttributeSet.hpp"
#include "Engine/Geometry/UECore/DynamicMesh/Operations/InsetMeshRegion.hpp"
#include "Engine/Geometry/UECore/DynamicMesh/Operations/OffsetMeshRegion.hpp"

#include <gtest/gtest.h>

#include <set>

using namespace Desert::Geometry;

namespace
{
    // A 100 cm cube, one polygroup per face (two triangles each), outward winding, attributes on.
    DynamicMesh3 MakeCube()
    {
        DynamicMesh3 M;
        M.EnableTriangleGroups();
        M.EnableAttributes();
        const double S = 100;
        for ( int i = 0; i < 8; ++i )
            M.AppendVertex( glm::dvec3( ( ( i & 1 ) != 0 ) ? S : 0, ( ( i & 2 ) != 0 ) ? S : 0,
                                        ( ( i & 4 ) != 0 ) ? S : 0 ) );
        // Quads a,b,c,d counter-clockwise seen from outside (the render winding); DynamicMesh3 keeps UE's
        // clockwise front (see DynamicMeshRenderConversion.hpp), so each triangle takes corners 0, 2, 1.
        const int Quads[6][4] = { { 0, 2, 3, 1 }, { 4, 5, 7, 6 }, { 0, 1, 5, 4 },
                                  { 2, 6, 7, 3 }, { 0, 4, 6, 2 }, { 1, 3, 7, 5 } };
        for ( auto Quad : Quads )
        {
            const int g = M.AllocateTriangleGroup();
            M.AppendTriangle( Quad[0], Quad[2], Quad[1], g );
            M.AppendTriangle( Quad[0], Quad[3], Quad[2], g );
        }
        return M;
    }

    int CountGroups( const DynamicMesh3& M )
    {
        std::set<int> G;
        for ( int const t : M.TriangleIndicesItr() )
            G.insert( M.GetTriangleGroup( t ) );
        return static_cast<int>( G.size() );
    }

    int CountBoundaryEdges( const DynamicMesh3& M )
    {
        int N = 0;
        for ( int const e : M.EdgeIndicesItr() )
            N += M.IsBoundaryEdge( e ) ? 1 : 0;
        return N;
    }

    // The top face (+Z, z = 100): triangles 2 and 3.
    std::vector<int32_t> TopFace()
    {
        std::vector<int32_t> T;
        T.push_back( 2 );
        T.push_back( 3 );
        return T;
    }
} // namespace

TEST( OffsetMeshRegion, CubeFaceExtrudeMatchesUETopology )
{
    DynamicMesh3 M = MakeCube();
    ASSERT_EQ( M.GetTriNormal( 2 ).z, 1.0 );
    OffsetMeshRegion Op( &M );
    Op.m_Triangles = TopFace();
    Op.m_ExtrusionVectorType =
         OffsetMeshRegion::VertexExtrusionVectorType::SelectionTriNormalsAngleWeightedAdjusted;
    Op.m_DefaultOffsetDistance = 50.0;
    ASSERT_TRUE( Op.Apply() ) << Op.m_FailureReason;
    // UE: +Vb vertices, +2*Eb triangles for a 4-vertex boundary; the face gets one new group, the walls four.
    EXPECT_EQ( M.VertexCount(), 12 );
    EXPECT_EQ( M.TriangleCount(), 20 );
    EXPECT_EQ( CountGroups( M ), 10 );
    EXPECT_EQ( CountBoundaryEdges( M ), 0 ) << "the extruded cube must stay closed";
    ASSERT_EQ( static_cast<int32_t>( Op.m_OffsetRegions.size() ), 1 );
    EXPECT_FALSE( Op.m_OffsetRegions[0].bIsSolid );
    for ( int32_t const t : Op.m_OffsetRegions[0].OffsetTids )
    {
        Index3i Tri = M.GetTriangle( t );
        for ( int j = 0; j < 3; ++j )
            EXPECT_DOUBLE_EQ( M.GetVertex( Tri[j] ).z, 150.0 );
        EXPECT_NEAR( M.GetTriNormal( t ).z, 1.0, 1e-9 );
    }
    // Four walls, two triangles each, each its own new group, facing outwards (horizontal normals).
    ASSERT_EQ( static_cast<int32_t>( Op.m_OffsetRegions[0].StitchTriangles.size() ), 1 );
    EXPECT_EQ( static_cast<int32_t>( Op.m_OffsetRegions[0].StitchTriangles[0].size() ), 8 );
    EXPECT_EQ( static_cast<int32_t>( Op.m_OffsetRegions[0].StitchPolygonIDs[0].size() ), 4 );
    for ( int32_t const t : Op.m_OffsetRegions[0].StitchTriangles[0] )
    {
        const glm::dvec3 N = M.GetTriNormal( t );
        const glm::dvec3 C = M.GetTriCentroid( t ) - glm::dvec3( 50, 50, 125 );
        EXPECT_NEAR( N.z, 0.0, 1e-9 );
        EXPECT_GT( glm::dot( N, C ), 0.0 ) << "wall triangle " << t << " faces inwards";
        EXPECT_TRUE( M.Attributes()->PrimaryNormals()->IsSetTriangle( t ) );
        EXPECT_TRUE( M.Attributes()->PrimaryUV()->IsSetTriangle( t ) );
    }
}

TEST( OffsetMeshRegion, WholeCubeOffsetsIntoASolid )
{
    DynamicMesh3     M = MakeCube();
    OffsetMeshRegion Op( &M );
    for ( int t = 0; t < 12; ++t )
        Op.m_Triangles.push_back( t );
    Op.m_ExtrusionVectorType =
         OffsetMeshRegion::VertexExtrusionVectorType::SelectionTriNormalsAngleWeightedAdjusted;
    Op.m_DefaultOffsetDistance = 10.0;
    ASSERT_TRUE( Op.Apply() ) << Op.m_FailureReason;
    EXPECT_TRUE( Op.m_OffsetRegions[0].bIsSolid );
    EXPECT_EQ( M.VertexCount(), 16 );
    EXPECT_EQ( M.TriangleCount(), 24 );
    EXPECT_EQ( CountBoundaryEdges( M ), 0 );
}

TEST( InsetMeshRegion, CubeFaceInsetShrinksByTheDistance )
{
    DynamicMesh3     M        = MakeCube();
    const int        TopGroup = M.GetTriangleGroup( 2 );
    InsetMeshRegion  Op( &M );
    Op.m_Triangles     = TopFace();
    Op.m_InsetDistance = 10.0;
    ASSERT_TRUE( Op.Apply() ) << Op.m_FailureReason;
    EXPECT_EQ( M.VertexCount(), 12 );
    EXPECT_EQ( M.TriangleCount(), 20 );
    EXPECT_EQ( CountGroups( M ), 10 ); // the face keeps its group, the ring gets four
    EXPECT_EQ( CountBoundaryEdges( M ), 0 );
    for ( int32_t const t : Op.m_InsetRegions[0].InitialTriangles )
    {
        EXPECT_EQ( M.GetTriangleGroup( t ), TopGroup );
        Index3i Tri = M.GetTriangle( t );
        for ( int j = 0; j < 3; ++j )
        {
            const glm::dvec3 P = M.GetVertex( Tri[j] );
            EXPECT_DOUBLE_EQ( P.z, 100.0 );
            EXPECT_NEAR( std::min( P.x, 100 - P.x ), 10.0, 1e-9 );
            EXPECT_NEAR( std::min( P.y, 100 - P.y ), 10.0, 1e-9 );
        }
    }
    for ( int32_t const t : Op.m_InsetRegions[0].StitchTriangles[0] )
        EXPECT_NEAR( M.GetTriNormal( t ).z, 1.0, 1e-9 ) << "ring triangle " << t << " is not in the face's plane";
}

TEST( InsetMeshRegion, RegionWithInteriorVertexIsRefused )
{
    DynamicMesh3     M      = MakeCube();
    const int        Before = M.TriangleCount();
    InsetMeshRegion  Op( &M );
    // The +Z face plus the four side faces around it: vertices 4..7 are interior.
    for ( int t = 2; t < 12; ++t )
        Op.m_Triangles.push_back( t );
    EXPECT_FALSE( Op.Apply() );
    EXPECT_NE( Op.m_FailureReason.find( "interior" ), std::string::npos ) << Op.m_FailureReason;
    EXPECT_EQ( M.TriangleCount(), Before );
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
