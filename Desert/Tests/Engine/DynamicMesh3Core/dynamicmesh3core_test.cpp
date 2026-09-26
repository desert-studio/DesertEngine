// DynamicMesh3 storage and queries (ported from UE GeometryCore): meshes built by hand, checked against
// their known topology (Euler characteristic, boundaries, valences) and against themselves across edits:
// removing elements leaves id holes, CompactInPlace/CompactCopy close them and the id maps they return must
// carry every surviving triangle to the same geometry.
#include <gtest/gtest.h>

#include <Engine/Geometry/MeshCore/DynamicMesh/DynamicMesh3.hpp>

#include <cmath>
#include <set>
#include <vector>

using namespace Desert::Geometry;

namespace
{
    constexpr double Side = 100.0; // 1 m cube in centimetres

    // Quads wound counter-clockwise seen from outside, split (a,b,c) + (a,c,d).
    DynamicMesh3 MakeCube()
    {
        DynamicMesh3 Mesh;
        for ( int i = 0; i < 8; ++i )
            Mesh.AppendVertex(
                 glm::dvec3( ( i & 1 ) * Side, ( ( i >> 1 ) & 1 ) * Side, ( ( i >> 2 ) & 1 ) * Side ) );
        const int Quads[6][4] = { { 0, 2, 3, 1 }, { 4, 5, 7, 6 }, { 0, 1, 5, 4 },
                                  { 2, 6, 7, 3 }, { 0, 4, 6, 2 }, { 1, 3, 7, 5 } };
        for ( const auto& Q : Quads )
        {
            EXPECT_GE( Mesh.AppendTriangle( Q[0], Q[1], Q[2] ), 0 );
            EXPECT_GE( Mesh.AppendTriangle( Q[0], Q[2], Q[3] ), 0 );
        }
        return Mesh;
    }

    // N x N quads in the XY plane, 10 cm cells, every quad split along the same diagonal.
    DynamicMesh3 MakePlane( int N, bool bGroups = false )
    {
        DynamicMesh3 Mesh;
        if ( bGroups )
            Mesh.EnableTriangleGroups();
        for ( int y = 0; y <= N; ++y )
            for ( int x = 0; x <= N; ++x )
                Mesh.AppendVertex( glm::dvec3( x * 10.0, y * 10.0, 0.0 ) );
        auto V = [N]( int x, int y ) { return y * ( N + 1 ) + x; };
        for ( int y = 0; y < N; ++y )
            for ( int x = 0; x < N; ++x )
            {
                int Group = 0;
                if ( bGroups )
                    Group = x < N / 2 ? 1 : 2;
                Mesh.AppendTriangle( V( x, y ), V( x + 1, y ), V( x + 1, y + 1 ), Group );
                Mesh.AppendTriangle( V( x, y ), V( x + 1, y + 1 ), V( x, y + 1 ), Group );
            }
        return Mesh;
    }

    DynamicMesh3 MakeTorus( int NU, int NV, double R, double r )
    {
        DynamicMesh3 Mesh;
        const auto   TwoPi = glm::two_pi<double>();
        for ( int u = 0; u < NU; ++u )
            for ( int v = 0; v < NV; ++v )
            {
                const double a = TwoPi * u / NU;
                const double b = TwoPi * v / NV;
                Mesh.AppendVertex( glm::dvec3( ( R + r * std::cos( b ) ) * std::cos( a ),
                                               ( R + r * std::cos( b ) ) * std::sin( a ), r * std::sin( b ) ) );
            }
        auto V = [NU, NV]( int u, int v ) { return ( u % NU ) * NV + ( v % NV ); };
        for ( int u = 0; u < NU; ++u )
            for ( int v = 0; v < NV; ++v )
            {
                Mesh.AppendTriangle( V( u, v ), V( u + 1, v ), V( u + 1, v + 1 ) );
                Mesh.AppendTriangle( V( u, v ), V( u + 1, v + 1 ), V( u, v + 1 ) );
            }
        return Mesh;
    }

    bool Valid( const DynamicMesh3& Mesh )
    {
        return Mesh.CheckValidity( DynamicMesh3::ValidityOptions(), ValidityCheckFailMode::ReturnOnly );
    }

    int Euler( const DynamicMesh3& Mesh )
    {
        return Mesh.VertexCount() - Mesh.EdgeCount() + Mesh.TriangleCount();
    }

    int BoundaryEdges( const DynamicMesh3& Mesh )
    {
        int Count = 0;
        for ( int const EID : Mesh.EdgeIndicesItr() )
            Count += Mesh.IsBoundaryEdge( EID ) ? 1 : 0;
        return Count;
    }

    int BoundaryVertices( const DynamicMesh3& Mesh )
    {
        int Count = 0;
        for ( int const VID : Mesh.VertexIndicesItr() )
            Count += Mesh.IsBoundaryVertex( VID ) ? 1 : 0;
        return Count;
    }

    double TotalArea( const DynamicMesh3& Mesh )
    {
        double Area = 0.0;
        for ( int const TID : Mesh.TriangleIndicesItr() )
            Area += Mesh.GetTriArea( TID );
        return Area;
    }

    // Every triangle of Before that survives must land, through the maps, on a triangle of After with the
    // same corner positions in the same order.
    void ExpectMappedGeometry( const DynamicMesh3& Before, const DynamicMesh3& After,
                               const DynamicMeshCompactMaps& Maps )
    {
        int Seen = 0;
        for ( int const TID : Before.TriangleIndicesItr() )
        {
            const int NewTID = Maps.GetTriangleMapping( TID );
            ASSERT_TRUE( After.IsTriangle( NewTID ) ) << "old triangle " << TID;
            const Index3i OldTri = Before.GetTriangle( TID );
            const Index3i NewTri = After.GetTriangle( NewTID );
            for ( int k = 0; k < 3; ++k )
            {
                EXPECT_EQ( Maps.GetVertexMapping( OldTri[k] ), NewTri[k] );
                EXPECT_EQ( Before.GetVertex( OldTri[k] ), After.GetVertex( NewTri[k] ) );
            }
            ++Seen;
        }
        EXPECT_EQ( Seen, After.TriangleCount() );
    }
} // namespace

TEST( DynamicMesh3Core, CubeIsClosedGenusZero )
{
    const DynamicMesh3 Mesh = MakeCube();
    EXPECT_TRUE( Valid( Mesh ) );
    EXPECT_EQ( Mesh.VertexCount(), 8 );
    EXPECT_EQ( Mesh.TriangleCount(), 12 );
    EXPECT_EQ( Mesh.EdgeCount(), 18 );
    EXPECT_EQ( Euler( Mesh ), 2 );
    EXPECT_TRUE( Mesh.IsClosed() );
    EXPECT_EQ( BoundaryEdges( Mesh ), 0 );
    EXPECT_EQ( BoundaryVertices( Mesh ), 0 );
    EXPECT_TRUE( Mesh.IsCompact() );
    EXPECT_NEAR( TotalArea( Mesh ), 6.0 * Side * Side, 1e-6 );

    const AxisAlignedBox3d Bounds = Mesh.GetBounds();
    EXPECT_EQ( Bounds.Min, glm::dvec3( 0 ) );
    EXPECT_EQ( Bounds.Max, glm::dvec3( Side, Side, Side ) );
}

TEST( DynamicMesh3Core, CubeNormalsFollowUEWinding )
{
    // UE's triangle normal is (V2-V0)x(V1-V0): a triangle counter-clockwise from outside faces inward.
    const DynamicMesh3 Mesh = MakeCube();
    const glm::dvec3   Center( Side / 2, Side / 2, Side / 2 );
    for ( int const TID : Mesh.TriangleIndicesItr() )
    {
        const glm::dvec3 N = Mesh.GetTriNormal( TID );
        EXPECT_NEAR( glm::length( N ), 1.0, 1e-12 );
        const glm::dvec3 Out = Normalized( Mesh.GetTriCentroid( TID ) - Center );
        EXPECT_LT( glm::dot( N, Out ), -0.5 ) << "triangle " << TID;
        const AxisAlignedBox3d TB = Mesh.GetTriBounds( TID );
        const glm::dvec3       C  = Mesh.GetTriCentroid( TID );
        EXPECT_TRUE( C.x >= TB.Min.x && C.x <= TB.Max.x && C.y >= TB.Min.y && C.y <= TB.Max.y && C.z >= TB.Min.z &&
                     C.z <= TB.Max.z );
        EXPECT_NEAR( Mesh.GetTriArea( TID ), Side * Side / 2, 1e-9 );
    }
}

TEST( DynamicMesh3Core, CubeAdjacency )
{
    const DynamicMesh3  Mesh       = MakeCube();
    int                 ValenceSum = 0;
    for ( int const VID : Mesh.VertexIndicesItr() )
    {
        std::set<int> FromEdges;
        Mesh.EnumerateVertexEdges( VID, [&]( int32_t EID ) { FromEdges.insert( EID ); } );
        EXPECT_EQ( static_cast<int>( FromEdges.size() ), Mesh.GetVtxEdgeCount( VID ) );
        ValenceSum += Mesh.GetVtxEdgeCount( VID );

        std::set<int> Tris;
        Mesh.EnumerateVertexTriangles( VID, [&]( int32_t TID ) { Tris.insert( TID ); } );
        EXPECT_EQ( static_cast<int>( Tris.size() ), Mesh.GetVtxTriangleCount( VID ) );
        for ( int const TID : Tris )
            EXPECT_GE( IndexUtil::FindTriIndex( VID, Mesh.GetTriangle( TID ) ), 0 );

        // one-ring neighbours are exactly the other ends of the vertex's edges, and FindEdge finds each
        for ( int const Nbr : Mesh.VtxVerticesItr( VID ) )
        {
            const int EID = Mesh.FindEdge( VID, Nbr );
            ASSERT_NE( EID, DynamicMesh3::InvalidID );
            EXPECT_TRUE( FromEdges.count( EID ) == 1 );
            EXPECT_TRUE(
                 IndexUtil::SamePairUnordered( VID, Nbr, Mesh.GetEdgeV( EID ).A, Mesh.GetEdgeV( EID ).B ) );
        }
    }
    EXPECT_EQ( ValenceSum, 2 * Mesh.EdgeCount() );
    EXPECT_EQ( Mesh.FindEdge( 0, 7 ), DynamicMesh3::InvalidID ); // body diagonal is not an edge

    for ( int const EID : Mesh.EdgeIndicesItr() )
    {
        const Index2i T = Mesh.GetEdgeT( EID );
        EXPECT_TRUE( Mesh.IsTriangle( T.A ) && Mesh.IsTriangle( T.B ) );
        EXPECT_EQ( Mesh.FindEdgeFromTriPair( T.A, T.B ), EID );
    }
    for ( int const TID : Mesh.TriangleIndicesItr() )
    {
        const Index3i Nbrs = Mesh.GetTriNeighbourTris( TID );
        for ( int k = 0; k < 3; ++k )
            EXPECT_TRUE( Mesh.IsTriangle( Nbrs[k] ) );
    }
}

TEST( DynamicMesh3Core, PlaneBoundaryAndValence )
{
    constexpr int       N    = 4;
    const DynamicMesh3  Mesh = MakePlane( N );
    EXPECT_TRUE( Valid( Mesh ) );
    EXPECT_EQ( Mesh.VertexCount(), ( N + 1 ) * ( N + 1 ) );
    EXPECT_EQ( Mesh.TriangleCount(), 2 * N * N );
    EXPECT_EQ( Mesh.EdgeCount(), 3 * N * N + 2 * N );
    EXPECT_EQ( Euler( Mesh ), 1 ); // a disc
    EXPECT_FALSE( Mesh.IsClosed() );
    EXPECT_EQ( BoundaryEdges( Mesh ), 4 * N );
    EXPECT_EQ( BoundaryVertices( Mesh ), 4 * N );
    EXPECT_TRUE( Mesh.IsBoundaryVertex( 0 ) );
    const int Center = ( N / 2 ) * ( N + 1 ) + N / 2;
    EXPECT_FALSE( Mesh.IsBoundaryVertex( Center ) );
    EXPECT_EQ( Mesh.GetVtxEdgeCount( Center ), 6 );
    EXPECT_NEAR( TotalArea( Mesh ), N * N * 100.0, 1e-9 );

    int e0 = -1;
    int e1 = -1;
    EXPECT_EQ( Mesh.GetVtxBoundaryEdges( 0, e0, e1 ), 2 );
    EXPECT_TRUE( Mesh.IsBoundaryEdge( e0 ) && Mesh.IsBoundaryEdge( e1 ) );
    int BoundaryTris = 0;
    for ( int const TID : Mesh.TriangleIndicesItr() )
        BoundaryTris += Mesh.IsBoundaryTriangle( TID ) ? 1 : 0;
    EXPECT_EQ( BoundaryTris, 4 * N - 2 ); // two corner triangles each own two boundary edges
}

TEST( DynamicMesh3Core, PlaneGroupsAndGroupBoundaries )
{
    constexpr int       N    = 4;
    const DynamicMesh3  Mesh = MakePlane( N, true );
    ASSERT_TRUE( Mesh.HasTriangleGroups() );
    EXPECT_EQ( Mesh.MaxGroupID(), 3 );
    int GroupBoundary = 0;
    for ( int const EID : Mesh.EdgeIndicesItr() )
        GroupBoundary += Mesh.IsGroupBoundaryEdge( EID ) ? 1 : 0;
    EXPECT_EQ( GroupBoundary, N ); // the vertical line x = N/2
    const int OnSeam = 1 * ( N + 1 ) + N / 2;
    EXPECT_TRUE( Mesh.IsGroupBoundaryVertex( OnSeam ) );
    EXPECT_FALSE( Mesh.IsGroupBoundaryVertex( 1 * ( N + 1 ) + 1 ) );
}

TEST( DynamicMesh3Core, TorusIsClosedGenusOne )
{
    const DynamicMesh3 Mesh = MakeTorus( 8, 6, 100.0, 30.0 );
    EXPECT_TRUE( Valid( Mesh ) );
    EXPECT_EQ( Mesh.VertexCount(), 48 );
    EXPECT_EQ( Mesh.TriangleCount(), 96 );
    EXPECT_EQ( Mesh.EdgeCount(), 144 );
    EXPECT_EQ( Euler( Mesh ), 0 );
    EXPECT_TRUE( Mesh.IsClosed() );
    for ( int const VID : Mesh.VertexIndicesItr() )
        EXPECT_EQ( Mesh.GetVtxEdgeCount( VID ), 6 );
}

TEST( DynamicMesh3Core, RejectsNonManifoldAndDuplicateTriangles )
{
    DynamicMesh3 Mesh = MakePlane( 1 ); // two triangles sharing the diagonal 0-3
    const int    Apex = Mesh.AppendVertex( glm::dvec3( 5.0, 5.0, 10.0 ) );
    EXPECT_EQ( Mesh.AppendTriangle( 0, 3, Apex ), DynamicMesh3::NonManifoldID );
    EXPECT_EQ( Mesh.TriangleCount(), 2 );
    EXPECT_TRUE( Valid( Mesh ) );
    EXPECT_EQ( Mesh.FindTriangle( 0, 1, 3 ), 0 );

    // a duplicate is only detectable while its edges are still boundary edges
    DynamicMesh3 Single;
    for ( int i = 0; i < 3; ++i )
        Single.AppendVertex( glm::dvec3( i * 10.0, static_cast<double>( i == 2 ) * 10.0, 0.0 ) );
    EXPECT_EQ( Single.AppendTriangle( 0, 1, 2 ), 0 );
    EXPECT_EQ( Single.AppendTriangle( 0, 1, 2 ), DynamicMesh3::DuplicateTriangleID );
    EXPECT_EQ( Single.TriangleCount(), 1 );
}

TEST( DynamicMesh3Core, RemoveTriangleOpensAHole )
{
    DynamicMesh3 Mesh = MakeCube();
    EXPECT_EQ( Mesh.RemoveTriangle( 0 ), MeshResult::Ok );
    EXPECT_TRUE( Valid( Mesh ) );
    EXPECT_EQ( Mesh.TriangleCount(), 11 );
    EXPECT_EQ( Mesh.MaxTriangleID(), 12 );
    EXPECT_FALSE( Mesh.IsCompactT() );
    EXPECT_FALSE( Mesh.IsClosed() );
    EXPECT_EQ( BoundaryEdges( Mesh ), 3 );
    EXPECT_EQ( Mesh.EdgeCount(), 18 ); // the removed triangle's edges all had a second triangle
    EXPECT_EQ( Euler( Mesh ), 1 );
    EXPECT_EQ( Mesh.RemoveTriangle( 0 ), MeshResult::Failed_NotATriangle );
}

TEST( DynamicMesh3Core, RemoveVertexThenCompactInPlaceKeepsGeometry )
{
    DynamicMesh3  Mesh     = MakeCube();
    const int     Incident = Mesh.GetVtxTriangleCount( 0 );
    EXPECT_EQ( Mesh.RemoveVertex( 0 ), MeshResult::Ok );
    EXPECT_TRUE( Valid( Mesh ) );
    EXPECT_FALSE( Mesh.IsVertex( 0 ) );
    EXPECT_EQ( Mesh.VertexCount(), 7 );
    EXPECT_EQ( Mesh.TriangleCount(), 12 - Incident );
    EXPECT_FALSE( Mesh.IsCompact() );

    const DynamicMesh3     Before = Mesh;
    DynamicMeshCompactMaps Maps;
    Mesh.CompactInPlace( &Maps );
    EXPECT_TRUE( Mesh.IsCompact() );
    EXPECT_TRUE( Valid( Mesh ) );
    EXPECT_EQ( Mesh.MaxVertexID(), 7 );
    EXPECT_EQ( Mesh.MaxTriangleID(), 12 - Incident );
    EXPECT_EQ( Maps.GetVertexMapping( 0 ), DynamicMeshCompactMaps::InvalidID );
    ExpectMappedGeometry( Before, Mesh, Maps );
    EXPECT_EQ( Euler( Mesh ), Euler( Before ) );
    EXPECT_NEAR( TotalArea( Mesh ), TotalArea( Before ), 1e-9 );
}

TEST( DynamicMesh3Core, CompactCopyEqualsSourceGeometry )
{
    DynamicMesh3 Source = MakePlane( 4, true );
    Source.EnableVertexNormals( glm::vec3( 0, 0, 1 ) );
    Source.SetVertexNormal( 7, glm::vec3( 1.0f, 0.0f, 0.0f ) );
    // Boundary triangles chosen so no vertex becomes a bowtie (7 instead of 6 would pinch vertex 3 and
    // CheckValidity rightly fails); removing all of quad 0 and corner triangle 6 also frees vertices 0 and 4.
    for ( int const TID : { 0, 1, 6, 31 } )
        ASSERT_EQ( Source.RemoveTriangle( TID ), MeshResult::Ok );
    ASSERT_FALSE( Source.IsCompact() );
    ASSERT_TRUE( Valid( Source ) );
    ASSERT_FALSE( Source.IsVertex( 0 ) );
    ASSERT_FALSE( Source.IsVertex( 4 ) );

    DynamicMesh3           Copy;
    DynamicMeshCompactMaps Maps;
    Copy.CompactCopy( Source, true, true, true, true, &Maps );
    EXPECT_TRUE( Copy.IsCompact() );
    EXPECT_TRUE( Valid( Copy ) );
    EXPECT_EQ( Copy.VertexCount(), Source.VertexCount() );
    EXPECT_EQ( Copy.TriangleCount(), Source.TriangleCount() );
    EXPECT_EQ( Copy.EdgeCount(), Source.EdgeCount() );
    EXPECT_EQ( BoundaryEdges( Copy ), BoundaryEdges( Source ) );
    ExpectMappedGeometry( Source, Copy, Maps );
    ASSERT_TRUE( Copy.HasVertexNormals() && Copy.HasTriangleGroups() );
    for ( int const VID : Source.VertexIndicesItr() )
        EXPECT_EQ( Copy.GetVertexNormal( Maps.GetVertexMapping( VID ) ), Source.GetVertexNormal( VID ) );
    for ( int const TID : Source.TriangleIndicesItr() )
        EXPECT_EQ( Copy.GetTriangleGroup( Maps.GetTriangleMapping( TID ) ), Source.GetTriangleGroup( TID ) );
    EXPECT_EQ( Copy.GetBounds().Min, Source.GetBounds().Min );
    EXPECT_EQ( Copy.GetBounds().Max, Source.GetBounds().Max );
}

TEST( DynamicMesh3Core, SetTriangleRewiresAndDropsIsolatedVertex )
{
    DynamicMesh3 Mesh;
    for ( int i = 0; i < 7; ++i )
        Mesh.AppendVertex( glm::dvec3( i * 10.0, ( i % 2 ) * 10.0, 0.0 ) );
    Mesh.AppendTriangle( 0, 1, 2 );
    const int T = Mesh.AppendTriangle( 3, 4, 5 );
    EXPECT_FALSE( Mesh.IsReferencedVertex( 6 ) );
    EXPECT_EQ( Mesh.SetTriangle( T, Index3i( 3, 4, 6 ), true ), MeshResult::Ok );
    EXPECT_TRUE( Valid( Mesh ) );
    EXPECT_EQ( Mesh.GetTriangle( T ), Index3i( 3, 4, 6 ) );
    EXPECT_FALSE( Mesh.IsVertex( 5 ) ); // left isolated, removed
    EXPECT_EQ( Mesh.FindEdge( 4, 6 ) != DynamicMesh3::InvalidID, true );
    EXPECT_EQ( Mesh.EdgeCount(), 6 );
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
