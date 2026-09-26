// DynamicMesh3 topology edit operators (ported from UE GeometryCore): split/flip/collapse/merge/poke/split-vertex
// applied to closed (cube, torus) and open (plane) meshes. After every accepted edit the mesh must pass
// CheckValidity, the element counts must move by exactly the amounts the operator's topology dictates (so the
// Euler characteristic is kept, or changes by the amount a merge of components implies), and the returned Info
// must name the elements UE documents: the new ids exist and sit where the header comment says. Forbidden edits
// must be refused with UE's result codes and leave the mesh byte-for-byte unchanged in counts.
#include <gtest/gtest.h>

#include <Engine/Geometry/UECore/DynamicMesh/DynamicMesh3.hpp>

#include <cmath>
#include <set>
#include <vector>

using namespace Desert::Geometry;

namespace
{
    constexpr double Side = 100.0; // 1 m cube in centimetres

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
            Mesh.AppendTriangle( Q[0], Q[1], Q[2] );
            Mesh.AppendTriangle( Q[0], Q[2], Q[3] );
        }
        return Mesh;
    }

    // N x N quads in the XY plane, 10 cm cells.
    DynamicMesh3 MakePlane( int N )
    {
        DynamicMesh3 Mesh;
        for ( int y = 0; y <= N; ++y )
            for ( int x = 0; x <= N; ++x )
                Mesh.AppendVertex( glm::dvec3( x * 10.0, y * 10.0, 0.0 ) );
        auto V = [N]( int x, int y ) { return y * ( N + 1 ) + x; };
        for ( int y = 0; y < N; ++y )
            for ( int x = 0; x < N; ++x )
            {
                Mesh.AppendTriangle( V( x, y ), V( x + 1, y ), V( x + 1, y + 1 ) );
                Mesh.AppendTriangle( V( x, y ), V( x + 1, y + 1 ), V( x, y + 1 ) );
            }
        return Mesh;
    }

    int TorusV( int NU, int NV, int u, int v )
    {
        return ( u % NU ) * NV + ( v % NV );
    }

    DynamicMesh3 MakeTorus( int NU, int NV )
    {
        DynamicMesh3 Mesh;
        for ( int u = 0; u < NU; ++u )
            for ( int v = 0; v < NV; ++v )
            {
                const double a = glm::two_pi<double>() * u / NU;
                const double b = glm::two_pi<double>() * v / NV;
                Mesh.AppendVertex( glm::dvec3( ( 50.0 + 20.0 * std::cos( b ) ) * std::cos( a ),
                                               ( 50.0 + 20.0 * std::cos( b ) ) * std::sin( a ),
                                               20.0 * std::sin( b ) ) );
            }
        for ( int u = 0; u < NU; ++u )
            for ( int v = 0; v < NV; ++v )
            {
                Mesh.AppendTriangle( TorusV( NU, NV, u, v ), TorusV( NU, NV, u + 1, v ),
                                     TorusV( NU, NV, u + 1, v + 1 ) );
                Mesh.AppendTriangle( TorusV( NU, NV, u, v ), TorusV( NU, NV, u + 1, v + 1 ),
                                     TorusV( NU, NV, u, v + 1 ) );
            }
        return Mesh;
    }

    DynamicMesh3 MakeTetrahedron()
    {
        DynamicMesh3 Mesh;
        Mesh.AppendVertex( glm::dvec3( 0, 0, 0 ) );
        Mesh.AppendVertex( glm::dvec3( 10, 0, 0 ) );
        Mesh.AppendVertex( glm::dvec3( 0, 10, 0 ) );
        Mesh.AppendVertex( glm::dvec3( 0, 0, 10 ) );
        Mesh.AppendTriangle( 0, 2, 1 );
        Mesh.AppendTriangle( 0, 1, 3 );
        Mesh.AppendTriangle( 0, 3, 2 );
        Mesh.AppendTriangle( 1, 2, 3 );
        return Mesh;
    }

    // Two triangles that touch along a doubled boundary edge: v1/v3 and v2/v4 are coincident pairs.
    // bOpposite winds the second triangle so the doubled edges run in opposite directions (weldable).
    DynamicMesh3 MakeSeam( bool bOpposite )
    {
        DynamicMesh3 Mesh;
        Mesh.AppendVertex( glm::dvec3( 0, 0, 0 ) );
        Mesh.AppendVertex( glm::dvec3( 10, 0, 0 ) );
        Mesh.AppendVertex( glm::dvec3( 0, 10, 0 ) );
        Mesh.AppendVertex( glm::dvec3( 10, 0, 0 ) );
        Mesh.AppendVertex( glm::dvec3( 0, 10, 0 ) );
        Mesh.AppendVertex( glm::dvec3( 10, 10, 0 ) );
        Mesh.AppendTriangle( 0, 1, 2 );
        if ( bOpposite )
            Mesh.AppendTriangle( 3, 5, 4 );
        else
            Mesh.AppendTriangle( 3, 4, 5 );
        return Mesh;
    }

    bool Valid( const DynamicMesh3& Mesh, bool bPermissive = false )
    {
        return Mesh.CheckValidity( bPermissive ? DynamicMesh3::ValidityOptions::Permissive()
                                               : DynamicMesh3::ValidityOptions(),
                                   ValidityCheckFailMode::ReturnOnly );
    }

    struct FCounts
    {
        int V, E, T;
        [[nodiscard]] int Euler() const
        {
            return V - E + T;
        }
    };

    FCounts Counts( const DynamicMesh3& Mesh )
    {
        return { Mesh.VertexCount(), Mesh.EdgeCount(), Mesh.TriangleCount() };
    }

    // Triangle equality up to rotation (orientation must match).
    bool SameTri( const Index3i& T, int a, int b, int c )
    {
        for ( int r = 0; r < 3; ++r )
            if ( T[r] == a && T[( r + 1 ) % 3] == b && T[( r + 2 ) % 3] == c )
                return true;
        return false;
    }

    int FirstInteriorEdge( const DynamicMesh3& Mesh )
    {
        for ( int const EID : Mesh.EdgeIndicesItr() )
            if ( !Mesh.IsBoundaryEdge( EID ) && !Mesh.IsBoundaryVertex( Mesh.GetEdgeV( EID ).A ) &&
                 !Mesh.IsBoundaryVertex( Mesh.GetEdgeV( EID ).B ) )
                return EID;
        return DynamicMesh3::InvalidID;
    }
} // namespace

// ---------------------------------------------------------------- SplitEdge

TEST( DynamicMesh3Edits, SplitInteriorEdgeKeepsEulerAndNamesTheNewElements )
{
    for ( DynamicMesh3 Mesh : { MakeCube(), MakeTorus( 6, 4 ) } )
    {
        const FCounts  Before  = Counts( Mesh );
        const int      OldMaxV = Mesh.MaxVertexID();
        const int      OldMaxT = Mesh.MaxTriangleID();
        const int      OldMaxE = Mesh.MaxEdgeID();
        const int      EID = 0;
        const Index2i  AB  = Mesh.GetEdgeV( EID );
        const Index2i  OV  = Mesh.GetEdgeOpposingV( EID );

        DynamicMesh3::EdgeSplitInfo Info;
        ASSERT_EQ( Mesh.SplitEdge( EID, Info, 0.25 ), MeshResult::Ok );
        ASSERT_TRUE( Valid( Mesh ) );

        const FCounts After = Counts( Mesh );
        EXPECT_EQ( After.V, Before.V + 1 );
        EXPECT_EQ( After.E, Before.E + 3 );
        EXPECT_EQ( After.T, Before.T + 2 );
        EXPECT_EQ( After.Euler(), Before.Euler() );

        EXPECT_FALSE( Info.bIsBoundary );
        EXPECT_EQ( Info.OriginalEdge, EID );
        EXPECT_EQ( Info.OriginalVertices, AB );
        EXPECT_EQ( Info.OtherVertices, OV );
        EXPECT_EQ( Info.NewVertex, OldMaxV );
        EXPECT_GE( Info.NewTriangles.A, OldMaxT );
        EXPECT_GE( Info.NewTriangles.B, OldMaxT );
        const int f = Info.NewVertex;
        const int a = AB.A;
        const int b = AB.B;
        const int c = OV.A;
        const int d = OV.B;
        // Header contract: t2=[f,b,c], t3=[f,d,b]; new edges [f,b],[f,c],[f,d]; the original edge becomes [a,f].
        EXPECT_TRUE( SameTri( Mesh.GetTriangle( Info.NewTriangles.A ), f, b, c ) );
        EXPECT_TRUE( SameTri( Mesh.GetTriangle( Info.NewTriangles.B ), f, d, b ) );
        EXPECT_EQ( Info.NewEdges.A, Mesh.FindEdge( f, b ) );
        EXPECT_EQ( Info.NewEdges.B, Mesh.FindEdge( f, c ) );
        EXPECT_EQ( Info.NewEdges.C, Mesh.FindEdge( f, d ) );
        for ( int k = 0; k < 3; ++k )
            EXPECT_GE( Info.NewEdges[k], OldMaxE );
        EXPECT_EQ( Mesh.FindEdge( a, f ), EID );
        EXPECT_EQ( Mesh.FindEdge( a, b ), DynamicMesh3::InvalidID );
        const glm::dvec3 Expected = Mesh.GetVertex( a ) * 0.75 + Mesh.GetVertex( b ) * 0.25;
        EXPECT_NEAR( Distance( Mesh.GetVertex( f ), Expected ), 0.0, 1e-9 );
    }
}

TEST( DynamicMesh3Edits, SplitBoundaryEdgeAddsOneTriangle )
{
    DynamicMesh3 Mesh = MakePlane( 3 );
    int          EID  = DynamicMesh3::InvalidID;
    for ( int const E : Mesh.EdgeIndicesItr() )
        if ( Mesh.IsBoundaryEdge( E ) )
        {
            EID = E;
            break;
        }
    ASSERT_NE( EID, DynamicMesh3::InvalidID );
    const FCounts Before = Counts( Mesh );

    DynamicMesh3::EdgeSplitInfo Info;
    ASSERT_EQ( Mesh.SplitEdge( EID, Info ), MeshResult::Ok );
    ASSERT_TRUE( Valid( Mesh ) );
    const FCounts After = Counts( Mesh );
    EXPECT_EQ( After.V, Before.V + 1 );
    EXPECT_EQ( After.E, Before.E + 2 );
    EXPECT_EQ( After.T, Before.T + 1 );
    EXPECT_EQ( After.Euler(), Before.Euler() );
    EXPECT_TRUE( Info.bIsBoundary );
    EXPECT_EQ( Info.OtherVertices.B, DynamicMesh3::InvalidID );
    EXPECT_EQ( Info.NewTriangles.B, DynamicMesh3::InvalidID );
    EXPECT_EQ( Info.NewEdges.C, DynamicMesh3::InvalidID );
    EXPECT_TRUE( Mesh.IsBoundaryVertex( Info.NewVertex ) );

    DynamicMesh3::EdgeSplitInfo Dead;
    EXPECT_EQ( Mesh.SplitEdge( Mesh.MaxEdgeID() + 5, Dead ), MeshResult::Failed_NotAnEdge );
}

// ---------------------------------------------------------------- FlipEdge

TEST( DynamicMesh3Edits, FlipRotatesTheEdgeAndKeepsEveryCount )
{
    for ( DynamicMesh3 Mesh : { MakeCube(), MakeTorus( 6, 4 ), MakePlane( 3 ) } )
    {
        const int EID = FirstInteriorEdge( Mesh ) != DynamicMesh3::InvalidID ? FirstInteriorEdge( Mesh ) : 0;
        ASSERT_FALSE( Mesh.IsBoundaryEdge( EID ) );
        const FCounts  Before = Counts( Mesh );
        const Index2i  AB     = Mesh.GetEdgeV( EID );
        const Index2i  CD     = Mesh.GetEdgeOpposingV( EID );

        DynamicMesh3::EdgeFlipInfo Info;
        ASSERT_EQ( Mesh.FlipEdge( EID, Info ), MeshResult::Ok );
        ASSERT_TRUE( Valid( Mesh ) );
        const FCounts After = Counts( Mesh );
        EXPECT_EQ( After.V, Before.V );
        EXPECT_EQ( After.E, Before.E );
        EXPECT_EQ( After.T, Before.T );

        EXPECT_EQ( Info.EdgeID, EID );
        // Info reports [a,b] re-oriented to the first triangle, so compare the pairs as sets.
        EXPECT_EQ( std::set<int>( { Info.OriginalVerts.A, Info.OriginalVerts.B } ),
                   std::set<int>( { AB.A, AB.B } ) );
        EXPECT_EQ( std::set<int>( { Info.OpposingVerts.A, Info.OpposingVerts.B } ),
                   std::set<int>( { CD.A, CD.B } ) );
        EXPECT_EQ( Mesh.FindEdge( CD.A, CD.B ), EID );
        EXPECT_EQ( Mesh.FindEdge( AB.A, AB.B ), DynamicMesh3::InvalidID );
        // InfoTypes contract: shared edge at index 0 of both new triangles, which are [c,d,b] and [d,c,a].
        const Index2i O = Info.OriginalVerts;
        const Index2i P = Info.OpposingVerts;
        EXPECT_EQ( Mesh.GetTriEdge( Info.Triangles.A, 0 ), EID );
        EXPECT_EQ( Mesh.GetTriEdge( Info.Triangles.B, 0 ), EID );
        EXPECT_TRUE( SameTri( Mesh.GetTriangle( Info.Triangles.A ), P.A, P.B, O.B ) );
        EXPECT_TRUE( SameTri( Mesh.GetTriangle( Info.Triangles.B ), P.B, P.A, O.A ) );
    }
}

TEST( DynamicMesh3Edits, FlipRefusesBoundaryAndExistingEdges )
{
    DynamicMesh3  Plane  = MakePlane( 2 );
    const FCounts Before = Counts( Plane );
    for ( int const EID : Plane.EdgeIndicesItr() )
    {
        DynamicMesh3::EdgeFlipInfo Info;
        if ( Plane.IsBoundaryEdge( EID ) )
            EXPECT_EQ( Plane.FlipEdge( EID, Info ), MeshResult::Failed_IsBoundaryEdge );
    }
    EXPECT_EQ( Counts( Plane ).E, Before.E );
    EXPECT_TRUE( Valid( Plane ) );

    // In a tetrahedron the opposing vertices of every edge are already joined.
    DynamicMesh3               Tet = MakeTetrahedron();
    DynamicMesh3::EdgeFlipInfo Info;
    EXPECT_EQ( Tet.FlipEdge( 0, Info ), MeshResult::Failed_FlippedEdgeExists );
    EXPECT_TRUE( Valid( Tet ) );
}

// ---------------------------------------------------------------- CollapseEdge

TEST( DynamicMesh3Edits, CollapseInteriorEdgeRemovesOneVertexThreeEdgesTwoTriangles )
{
    DynamicMesh3 Mesh = MakeTorus( 8, 6 );
    ASSERT_TRUE( Valid( Mesh ) );
    const FCounts   Before = Counts( Mesh );
    const int        Keep    = TorusV( 8, 6, 2, 2 );
    const int        Remove  = TorusV( 8, 6, 3, 2 );
    const int       EID   = Mesh.FindEdge( Keep, Remove );
    const glm::dvec3 KeepP   = Mesh.GetVertex( Keep );
    const glm::dvec3 RemoveP = Mesh.GetVertex( Remove );
    ASSERT_NE( EID, DynamicMesh3::InvalidID );
    EXPECT_EQ( Mesh.CanCollapseEdge( Keep, Remove ), MeshResult::Ok );

    DynamicMesh3::EdgeCollapseInfo Info;
    ASSERT_EQ( Mesh.CollapseEdge( Keep, Remove, 0.5, Info ), MeshResult::Ok );
    ASSERT_TRUE( Valid( Mesh ) );
    const FCounts After = Counts( Mesh );
    EXPECT_EQ( After.V, Before.V - 1 );
    EXPECT_EQ( After.E, Before.E - 3 );
    EXPECT_EQ( After.T, Before.T - 2 );
    EXPECT_EQ( After.Euler(), Before.Euler() ); // genus unchanged: torus stays at 0
    EXPECT_EQ( After.Euler(), 0 );

    EXPECT_EQ( Info.KeptVertex, Keep );
    EXPECT_EQ( Info.RemovedVertex, Remove );
    EXPECT_EQ( Info.CollapsedEdge, EID );
    EXPECT_FALSE( Info.bIsBoundary );
    EXPECT_FALSE( Mesh.IsVertex( Remove ) );
    EXPECT_FALSE( Mesh.IsEdge( EID ) );
    EXPECT_FALSE( Mesh.IsTriangle( Info.RemovedTris.A ) );
    EXPECT_FALSE( Mesh.IsTriangle( Info.RemovedTris.B ) );
    EXPECT_FALSE( Mesh.IsEdge( Info.RemovedEdges.A ) );
    EXPECT_FALSE( Mesh.IsEdge( Info.RemovedEdges.B ) );
    EXPECT_TRUE( Mesh.IsEdge( Info.KeptEdges.A ) );
    EXPECT_TRUE( Mesh.IsEdge( Info.KeptEdges.B ) );
    EXPECT_EQ( Mesh.FindEdge( Keep, Info.OpposingVerts.A ), Info.KeptEdges.A );
    EXPECT_EQ( Mesh.FindEdge( Keep, Info.OpposingVerts.B ), Info.KeptEdges.B );
    EXPECT_NEAR( Distance( Mesh.GetVertex( Keep ), ( KeepP + RemoveP ) * 0.5 ), 0.0, 1e-9 );
}

TEST( DynamicMesh3Edits, CollapseRefusesEditsThatBreakManifoldness )
{
    // Minor ring of three: the two ends of a ring edge share a third neighbour outside their two triangles,
    // so collapsing would fold the ring into a non-manifold edge.
    DynamicMesh3 Thin = MakeTorus( 6, 3 );
    ASSERT_TRUE( Valid( Thin ) );
    const FCounts                    Before = Counts( Thin );
    DynamicMesh3::EdgeCollapseInfo   Info;
    EXPECT_EQ( Thin.CollapseEdge( TorusV( 6, 3, 1, 0 ), TorusV( 6, 3, 1, 1 ), Info ),
               MeshResult::Failed_InvalidNeighbourhood );
    EXPECT_EQ( Counts( Thin ).V, Before.V );
    EXPECT_EQ( Counts( Thin ).T, Before.T );
    EXPECT_TRUE( Valid( Thin ) );

    DynamicMesh3 Tet = MakeTetrahedron();
    EXPECT_EQ( Tet.CollapseEdge( 0, 1, Info ), MeshResult::Failed_CollapseTetrahedron );
    EXPECT_EQ( Tet.TriangleCount(), 4 );

    // Interior diagonal of a single quad: both ends are boundary vertices, the collapse would make a bowtie.
    DynamicMesh3 Quad = MakePlane( 1 );
    EXPECT_EQ( Quad.CollapseEdge( 0, 3, Info ), MeshResult::Failed_InvalidNeighbourhood );

    DynamicMesh3 Tri;
    Tri.AppendVertex( glm::dvec3( 0, 0, 0 ) );
    Tri.AppendVertex( glm::dvec3( 10, 0, 0 ) );
    Tri.AppendVertex( glm::dvec3( 0, 10, 0 ) );
    Tri.AppendTriangle( 0, 1, 2 );
    EXPECT_EQ( Tri.CollapseEdge( 0, 1, Info ), MeshResult::Failed_CollapseTriangle );
    EXPECT_EQ( Tri.TriangleCount(), 1 );

    EXPECT_EQ( Tri.CollapseEdge( 1, 1, Info ), MeshResult::Failed_NotAnEdge );
}

TEST( DynamicMesh3Edits, CollapseBoundaryEdgeOnPlane )
{
    DynamicMesh3  Mesh   = MakePlane( 3 );
    const FCounts Before = Counts( Mesh );
    // (1,0)-(2,0) on the bottom row: a boundary edge whose ends are not corners.
    DynamicMesh3::EdgeCollapseInfo Info;
    ASSERT_EQ( Mesh.CollapseEdge( 1, 2, Info ), MeshResult::Ok );
    ASSERT_TRUE( Valid( Mesh ) );
    const FCounts After = Counts( Mesh );
    EXPECT_TRUE( Info.bIsBoundary );
    EXPECT_EQ( Info.RemovedTris.B, DynamicMesh3::InvalidID );
    EXPECT_EQ( After.V, Before.V - 1 );
    EXPECT_EQ( After.E, Before.E - 2 );
    EXPECT_EQ( After.T, Before.T - 1 );
    EXPECT_EQ( After.Euler(), 1 );
}

// ---------------------------------------------------------------- MergeEdges / MergeVertices

TEST( DynamicMesh3Edits, MergeEdgesWeldsTwoComponentsIntoOne )
{
    DynamicMesh3 Mesh = MakeSeam( true );
    ASSERT_TRUE( Valid( Mesh ) );
    EXPECT_EQ( Counts( Mesh ).Euler(), 2 ); // two discs
    const int Keep    = Mesh.FindEdge( 1, 2 );
    const int Discard = Mesh.FindEdge( 3, 4 );

    DynamicMesh3::MergeEdgesInfo Info;
    ASSERT_EQ( Mesh.MergeEdges( Keep, Discard, Info ), MeshResult::Ok );
    ASSERT_TRUE( Valid( Mesh ) );
    const FCounts After = Counts( Mesh );
    EXPECT_EQ( After.V, 4 );
    EXPECT_EQ( After.E, 5 );
    EXPECT_EQ( After.T, 2 );
    EXPECT_EQ( After.Euler(), 1 ); // one disc
    EXPECT_EQ( Info.KeptEdge, Keep );
    EXPECT_EQ( Info.RemovedEdge, Discard );
    EXPECT_FALSE( Mesh.IsEdge( Discard ) );
    EXPECT_FALSE( Mesh.IsBoundaryEdge( Keep ) );
    EXPECT_FALSE( Mesh.IsVertex( 3 ) );
    EXPECT_FALSE( Mesh.IsVertex( 4 ) );
    EXPECT_EQ( std::set<int>( { Info.RemovedVerts.A, Info.RemovedVerts.B } ), std::set<int>( { 3, 4 } ) );
    EXPECT_TRUE( Info.BowtiesRemovedEdges.empty() );
}

TEST( DynamicMesh3Edits, MergeEdgesRefusesInteriorAndSameOrientation )
{
    DynamicMesh3                 Same = MakeSeam( false );
    DynamicMesh3::MergeEdgesInfo Info;
    EXPECT_EQ( Same.MergeEdges( Same.FindEdge( 1, 2 ), Same.FindEdge( 3, 4 ), Info ),
               MeshResult::Failed_SameOrientation );
    EXPECT_EQ( Same.VertexCount(), 6 );

    DynamicMesh3 Plane = MakePlane( 2 );
    int          Inner = DynamicMesh3::InvalidID;
    int          Outer = DynamicMesh3::InvalidID;
    for ( int const E : Plane.EdgeIndicesItr() )
        ( Plane.IsBoundaryEdge( E ) ? Outer : Inner ) = E;
    ASSERT_NE( Inner, DynamicMesh3::InvalidID );
    EXPECT_EQ( Plane.MergeEdges( Outer, Inner, Info ), MeshResult::Failed_NotABoundaryEdge );
    EXPECT_TRUE( Valid( Plane ) );
}

TEST( DynamicMesh3Edits, MergeVerticesResolvesToTheRightOperator )
{
    // Connected by an edge: resolves as a collapse.
    DynamicMesh3                    Torus = MakeTorus( 8, 6 );
    DynamicMesh3::MergeVerticesInfo Info;
    ASSERT_EQ( Torus.MergeVertices( TorusV( 8, 6, 2, 2 ), TorusV( 8, 6, 3, 2 ), Info ), MeshResult::Ok );
    EXPECT_TRUE( Info.EdgeCollapseInfo.has_value() );
    EXPECT_FALSE( Info.MergeEdgesInfo.has_value() );
    EXPECT_TRUE( Valid( Torus ) );

    // Unconnected boundary vertices with no shared neighbour: a boundary bowtie, which only the permissive
    // validity options accept.
    DynamicMesh3                    Seam = MakeSeam( true );
    DynamicMesh3::MergeVerticesInfo Bowtie;
    ASSERT_EQ( Seam.MergeVertices( 1, 3, Bowtie ), MeshResult::Ok );
    EXPECT_FALSE( Bowtie.EdgeCollapseInfo.has_value() );
    EXPECT_FALSE( Bowtie.MergeEdgesInfo.has_value() );
    EXPECT_FALSE( Seam.IsVertex( 3 ) );
    EXPECT_EQ( Seam.VertexCount(), 5 );
    EXPECT_TRUE( Valid( Seam, true ) );
    EXPECT_FALSE( Valid( Seam ) );

    EXPECT_EQ( Seam.MergeVertices( 1, 1, Bowtie ), MeshResult::Failed_VertexAlreadyExists );
}

// ---------------------------------------------------------------- PokeTriangle / SplitVertex

TEST( DynamicMesh3Edits, PokeSplitsOneTriangleIntoThree )
{
    for ( DynamicMesh3 Mesh : { MakeCube(), MakePlane( 2 ), MakeTorus( 6, 4 ) } )
    {
        const FCounts   Before  = Counts( Mesh );
        const int        OldMaxV = Mesh.MaxVertexID();
        const int        OldMaxT = Mesh.MaxTriangleID();
        const int       TID = 1;
        const Index3i    Tri = Mesh.GetTriangle( TID );
        const glm::dvec3 Centroid =
             ( Mesh.GetVertex( Tri.A ) + Mesh.GetVertex( Tri.B ) + Mesh.GetVertex( Tri.C ) ) / 3.0;

        DynamicMesh3::PokeTriangleInfo Info;
        ASSERT_EQ( Mesh.PokeTriangle( TID, Info ), MeshResult::Ok );
        ASSERT_TRUE( Valid( Mesh ) );
        const FCounts After = Counts( Mesh );
        EXPECT_EQ( After.V, Before.V + 1 );
        EXPECT_EQ( After.E, Before.E + 3 );
        EXPECT_EQ( After.T, Before.T + 2 );
        EXPECT_EQ( After.Euler(), Before.Euler() );

        EXPECT_EQ( Info.OriginalTriangle, TID );
        EXPECT_EQ( Info.TriVertices, Tri );
        EXPECT_EQ( Info.NewVertex, OldMaxV );
        EXPECT_GE( Info.NewTriangles.A, OldMaxT );
        EXPECT_GE( Info.NewTriangles.B, OldMaxT );
        EXPECT_TRUE( Mesh.IsTriangle( TID ) ); // reused
        for ( int k = 0; k < 3; ++k )
            EXPECT_EQ( Info.NewEdges[k], Mesh.FindEdge( Info.NewVertex, Tri[k] ) );
        EXPECT_NEAR( Distance( Mesh.GetVertex( Info.NewVertex ), Centroid ), 0.0, 1e-9 );
    }
    DynamicMesh3                   Mesh = MakeCube();
    DynamicMesh3::PokeTriangleInfo Info;
    EXPECT_EQ( Mesh.PokeTriangle( 99, Info ), MeshResult::Failed_NotATriangle );
}

TEST( DynamicMesh3Edits, SplitVertexDetachesTheGivenFan )
{
    // Vertex 4 is the centre of a 2x2 plane; detaching the two triangles of the lower-left quad makes a
    // corner cut: the new vertex carries them, the original keeps the rest, both become boundary vertices.
    DynamicMesh3     Mesh = MakePlane( 2 );
    std::vector<int> Fan;
    Mesh.GetVtxTriangles( 4, Fan );
    ASSERT_EQ( static_cast<int32_t>( Fan.size() ), 6 );
    EXPECT_TRUE( Mesh.SplitVertexWouldLeaveIsolated( 4, Fan ) );

    const std::vector<int> Quad( { 0, 1 } ); // triangles of quad (0,0)
    EXPECT_FALSE( Mesh.SplitVertexWouldLeaveIsolated( 4, Quad ) );
    const FCounts                   Before = Counts( Mesh );
    DynamicMesh3::VertexSplitInfo   Info;
    ASSERT_EQ( Mesh.SplitVertex( 4, Quad, Info ), MeshResult::Ok );
    EXPECT_TRUE( Valid( Mesh, true ) );
    EXPECT_EQ( Info.OriginalVertex, 4 );
    EXPECT_EQ( Info.NewVertex, Before.V );
    EXPECT_EQ( Counts( Mesh ).V, Before.V + 1 );
    EXPECT_EQ( Counts( Mesh ).T, Before.T );
    EXPECT_TRUE( Mesh.GetTriangle( 0 ).Contains( Info.NewVertex ) );
    EXPECT_TRUE( Mesh.GetTriangle( 1 ).Contains( Info.NewVertex ) );
    EXPECT_TRUE( Mesh.IsBoundaryVertex( 4 ) );
    EXPECT_TRUE( Mesh.IsBoundaryVertex( Info.NewVertex ) );

    EXPECT_EQ( Mesh.SplitVertex( 999, Quad, Info ), MeshResult::Failed_NotAVertex );
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
