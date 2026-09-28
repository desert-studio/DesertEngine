// MeshMeshCut + MeshBoolean (UE 5.8 Operations/MeshMeshCut.cpp, MeshBoolean.cpp ports) and the
// MeshBooleanOperation wrapper: closed results with the analytic volume, the winding number inside and outside,
// face-touching solids, trimming an open sheet, and byte-identical reruns.
#include "Engine/Geometry/MeshBooleanOperation.hpp"
#include "Engine/Geometry/MeshCore/Operations/MeshBoolean.hpp"
#include "Engine/Geometry/MeshCore/Spatial/FastWinding.hpp"
#include "Engine/Geometry/MeshCore/Spatial/MeshAABBTree3.hpp"
#include "Engine/Geometry/MeshCore/VectorUtil.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstring>
#include <vector>

namespace
{
    using namespace Desert::Geometry;

    // Appends triangle (a, b, c), reversed if needed so that its normal faces along Outward.
    void AddOutwardTri( DynamicMesh3& Mesh, int a, int b, int c, const glm::dvec3& Outward )
    {
        const glm::dvec3 n = VectorUtil::Normal( Mesh.GetVertex( a ), Mesh.GetVertex( b ), Mesh.GetVertex( c ) );
        if ( glm::dot( n, Outward ) < 0 )
        {
            std::swap( b, c );
        }
        ASSERT_GE( Mesh.AppendTriangle( a, b, c ), 0 );
    }

    // A closed box [Min, Min + Size], 8 shared vertices, 12 triangles facing out.
    DynamicMesh3 MakeBox( const glm::dvec3& Min, const glm::dvec3& Size )
    {
        DynamicMesh3 Mesh;
        for ( int i = 0; i < 8; i++ )
        {
            Mesh.AppendVertex( Min + Size * glm::dvec3( i & 1, ( i >> 1 ) & 1, ( i >> 2 ) & 1 ) );
        }
        const glm::dvec3 Center = Min + 0.5 * Size;
        // each face as a quad of corner indices
        const std::array<std::array<int, 4>, 6> Faces{
             { { 0, 2, 6, 4 }, { 1, 3, 7, 5 }, { 0, 1, 5, 4 }, { 2, 3, 7, 6 }, { 0, 1, 3, 2 }, { 4, 5, 7, 6 } } };
        for ( const auto& F : Faces )
        {
            glm::dvec3 FaceCenter( 0 );
            for ( int v : F )
                FaceCenter += 0.25 * Mesh.GetVertex( v );
            const glm::dvec3 Out = FaceCenter - Center;
            AddOutwardTri( Mesh, F[0], F[1], F[2], Out );
            AddOutwardTri( Mesh, F[0], F[2], F[3], Out );
        }
        return Mesh;
    }

    // An open N x N grid sheet at height Z over [Lo, Hi]^2, facing +z.
    DynamicMesh3 MakeSheet( double Lo, double Hi, int N, double Z )
    {
        DynamicMesh3 Mesh;
        for ( int j = 0; j <= N; j++ )
            for ( int i = 0; i <= N; i++ )
                Mesh.AppendVertex( glm::dvec3( Lo + ( Hi - Lo ) * i / N, Lo + ( Hi - Lo ) * j / N, Z ) );
        const auto V = [N]( int i, int j ) { return j * ( N + 1 ) + i; };
        for ( int j = 0; j < N; j++ )
            for ( int i = 0; i < N; i++ )
            {
                AddOutwardTri( Mesh, V( i, j ), V( i + 1, j ), V( i + 1, j + 1 ), glm::dvec3( 0, 0, 1 ) );
                AddOutwardTri( Mesh, V( i, j ), V( i + 1, j + 1 ), V( i, j + 1 ), glm::dvec3( 0, 0, 1 ) );
            }
        return Mesh;
    }

    double RawVolume( const DynamicMesh3& Mesh )
    {
        double Sum = 0;
        for ( int t : Mesh.TriangleIndicesItr() )
        {
            glm::dvec3 a, b, c;
            Mesh.GetTriVertices( t, a, b, c );
            Sum += glm::dot( a, glm::cross( b, c ) ) / 6.0;
        }
        return Sum;
    }

    // Signed volume, positive for a mesh oriented like MakeBox's (the winding convention of the port).
    double Volume( const DynamicMesh3& Mesh )
    {
        const double Sign = RawVolume( MakeBox( glm::dvec3( 0 ), glm::dvec3( 1 ) ) ) > 0 ? 1.0 : -1.0;
        return Sign * RawVolume( Mesh );
    }

    double Area( const DynamicMesh3& Mesh )
    {
        double Sum = 0;
        for ( int t : Mesh.TriangleIndicesItr() )
        {
            glm::dvec3 a, b, c;
            Mesh.GetTriVertices( t, a, b, c );
            Sum += 0.5 * glm::length( glm::cross( b - a, c - a ) );
        }
        return Sum;
    }

    int OpenEdges( const DynamicMesh3& Mesh )
    {
        int Count = 0;
        for ( int e : Mesh.EdgeIndicesItr() )
            Count += Mesh.IsBoundaryEdge( e ) ? 1 : 0;
        return Count;
    }

    bool InsideOf( const DynamicMesh3& Mesh, const glm::dvec3& P )
    {
        DynamicMeshAABBTree3 Tree( &Mesh );
        FastWindingTree      Winding( &Tree );
        return Winding.IsInside( P, 0.5 );
    }

    DynamicMesh3 RunBoolean( const DynamicMesh3& A, const DynamicMesh3& B, MeshBoolean::BooleanOp Op,
                             std::vector<int>* CreatedBoundary = nullptr, bool* bOk = nullptr )
    {
        DynamicMesh3 Out;
        MeshBoolean  Boolean( &A, &B, &Out, Op );
        const bool   bResult = Boolean.Compute();
        if ( bOk )
            *bOk = bResult;
        if ( CreatedBoundary )
            *CreatedBoundary = Boolean.CreatedBoundaryEdges;
        return Out;
    }

    const glm::dvec3 kSize( 100.0 ); // cm

    struct BoolCase
    {
        MeshBoolean::BooleanOp Op;
        double                 VolumeFraction; // of one cube
    };

    // A = [0,100]^3, B = A + (50, 40, 30): overlap 50 x 60 x 70 = 0.21 of a cube, no coincident features.
    TEST( MeshBoolean, OverlappingCubesAreClosedWithAnalyticVolume )
    {
        const DynamicMesh3 A    = MakeBox( glm::dvec3( 0 ), kSize );
        const DynamicMesh3 B    = MakeBox( glm::dvec3( 50, 40, 30 ), kSize );
        const double       Cube = 1e6;
        for ( const BoolCase& C : { BoolCase{ MeshBoolean::BooleanOp::Union, 1.79 },
                                    BoolCase{ MeshBoolean::BooleanOp::Difference, 0.79 },
                                    BoolCase{ MeshBoolean::BooleanOp::Intersect, 0.21 } } )
        {
            SCOPED_TRACE( static_cast<int>( C.Op ) );
            std::vector<int>   Created;
            bool               bOk = false;
            const DynamicMesh3 R   = RunBoolean( A, B, C.Op, &Created, &bOk );
            EXPECT_TRUE( bOk );
            EXPECT_TRUE( Created.empty() ) << Created.size();
            EXPECT_EQ( OpenEdges( R ), 0 );
            EXPECT_NEAR( Volume( R ), C.VolumeFraction * Cube, 1e-6 * Cube );

            const glm::dvec3 OnlyA( 10, 10, 10 ), OnlyB( 140, 130, 120 ), Both( 75, 70, 65 ),
                 Neither( 10, 130, 10 ), Far( -50, -50, -50 );
            const bool bUnion = C.Op == MeshBoolean::BooleanOp::Union;
            const bool bDiff  = C.Op == MeshBoolean::BooleanOp::Difference;
            EXPECT_EQ( InsideOf( R, OnlyA ), bUnion || bDiff );
            EXPECT_EQ( InsideOf( R, OnlyB ), bUnion );
            EXPECT_EQ( InsideOf( R, Both ), !bDiff );
            EXPECT_FALSE( InsideOf( R, Neither ) );
            EXPECT_FALSE( InsideOf( R, Far ) );
        }
    }

    // The brief's case: B = A + (50, 50, 50). Cube diagonals pass through the other cube's edges and corners,
    // so the cut meets vertex-on-edge and vertex-on-vertex snaps.
    TEST( MeshBoolean, HalfOffsetCubesWithCoincidentFeatures )
    {
        const DynamicMesh3 A    = MakeBox( glm::dvec3( 0 ), kSize );
        const DynamicMesh3 B    = MakeBox( glm::dvec3( 50 ), kSize );
        const double       Cube = 1e6;
        for ( const BoolCase& C : { BoolCase{ MeshBoolean::BooleanOp::Union, 1.875 },
                                    BoolCase{ MeshBoolean::BooleanOp::Difference, 0.875 },
                                    BoolCase{ MeshBoolean::BooleanOp::Intersect, 0.125 } } )
        {
            SCOPED_TRACE( static_cast<int>( C.Op ) );
            std::vector<int>   Created;
            const DynamicMesh3 R = RunBoolean( A, B, C.Op, &Created );
            EXPECT_TRUE( Created.empty() ) << Created.size();
            EXPECT_EQ( OpenEdges( R ), 0 );
            EXPECT_NEAR( Volume( R ), C.VolumeFraction * Cube, 1e-6 * Cube );
        }
    }

    // B touches A's +x face over part of it: the shared wall is coplanar with opposite normals and leaves the
    // union; the overlap outline is cut by B's side walls crossing A's face.
    TEST( MeshBoolean, FaceTouchingCubesUnionIsClosed )
    {
        const DynamicMesh3 A = MakeBox( glm::dvec3( 0 ), kSize );
        const DynamicMesh3 B = MakeBox( glm::dvec3( 100, 30, 20 ), kSize );
        std::vector<int>   Created;
        const DynamicMesh3 R = RunBoolean( A, B, MeshBoolean::BooleanOp::Union, &Created );
        EXPECT_TRUE( Created.empty() ) << Created.size();
        EXPECT_EQ( OpenEdges( R ), 0 );
        EXPECT_NEAR( Volume( R ), 2e6, 1e-6 * 2e6 );
        EXPECT_TRUE( InsideOf( R, glm::dvec3( 50, 50, 50 ) ) );
        EXPECT_TRUE( InsideOf( R, glm::dvec3( 150, 80, 70 ) ) );
        EXPECT_FALSE( InsideOf( R, glm::dvec3( 150, 10, 10 ) ) );
    }

    // An open 200 x 200 sheet through the middle of the cube (grid lines avoid the cube's walls).
    TEST( MeshBoolean, TrimOpenSheetByCube )
    {
        const DynamicMesh3 Cube      = MakeBox( glm::dvec3( 0 ), kSize );
        const DynamicMesh3 Sheet     = MakeSheet( -50, 150, 5, 50 );
        const int          SheetOpen = OpenEdges( Sheet );

        std::vector<int>   Created;
        const DynamicMesh3 Outside = RunBoolean( Sheet, Cube, MeshBoolean::BooleanOp::TrimInside, &Created );
        EXPECT_NEAR( Area( Outside ), 200.0 * 200.0 - 100.0 * 100.0, 1e-6 * 4e4 );
        EXPECT_FALSE( Created.empty() );
        EXPECT_EQ( OpenEdges( Outside ), SheetOpen + static_cast<int>( Created.size() ) );
        for ( int t : Outside.TriangleIndicesItr() )
        {
            glm::dvec3 a, b, c;
            Outside.GetTriVertices( t, a, b, c );
            EXPECT_FALSE( InsideOf( Cube, ( a + b + c ) / 3.0 ) ) << t;
        }

        const DynamicMesh3 Inside = RunBoolean( Sheet, Cube, MeshBoolean::BooleanOp::TrimOutside, &Created );
        EXPECT_NEAR( Area( Inside ), 100.0 * 100.0, 1e-6 * 4e4 );
        EXPECT_EQ( OpenEdges( Inside ), static_cast<int>( Created.size() ) );
        for ( int t : Inside.TriangleIndicesItr() )
        {
            glm::dvec3 a, b, c;
            Inside.GetTriVertices( t, a, b, c );
            EXPECT_TRUE( InsideOf( Cube, ( a + b + c ) / 3.0 ) ) << t;
        }
    }

    // NewGroupInside keeps every triangle and puts the part inside the cutter in a new group.
    TEST( MeshBoolean, NewGroupInsideRegroupsWithoutDeleting )
    {
        DynamicMesh3 Sheet = MakeSheet( -50, 150, 5, 50 );
        Sheet.EnableTriangleGroups( 0 );
        const DynamicMesh3 Cube = MakeBox( glm::dvec3( 0 ), kSize );
        std::vector<int>   Created;
        const DynamicMesh3 R = RunBoolean( Sheet, Cube, MeshBoolean::BooleanOp::NewGroupInside, &Created );
        EXPECT_TRUE( Created.empty() );
        EXPECT_NEAR( Area( R ), 200.0 * 200.0, 1e-6 * 4e4 );
        double InGroupArea = 0;
        for ( int t : R.TriangleIndicesItr() )
        {
            glm::dvec3 a, b, c;
            R.GetTriVertices( t, a, b, c );
            const bool bInside = InsideOf( Cube, ( a + b + c ) / 3.0 );
            EXPECT_EQ( R.GetTriangleGroup( t ) != 0, bInside ) << t;
            if ( bInside )
                InGroupArea += 0.5 * glm::length( glm::cross( b - a, c - a ) );
        }
        EXPECT_NEAR( InGroupArea, 100.0 * 100.0, 1e-6 * 4e4 );
    }

    std::vector<unsigned char> Bytes( const DynamicMesh3& Mesh )
    {
        std::vector<unsigned char> Out;
        const auto                 Put = [&Out]( const void* p, size_t n )
        {
            const auto* c = static_cast<const unsigned char*>( p );
            Out.insert( Out.end(), c, c + n );
        };
        for ( int v : Mesh.VertexIndicesItr() )
        {
            const glm::dvec3 P = Mesh.GetVertex( v );
            Put( &v, sizeof v );
            Put( &P, sizeof P );
        }
        for ( int t : Mesh.TriangleIndicesItr() )
        {
            const Index3i T = Mesh.GetTriangle( t );
            Put( &t, sizeof t );
            Put( &T, sizeof T );
        }
        return Out;
    }

    TEST( MeshBoolean, TwoRunsAreByteIdentical )
    {
        const DynamicMesh3 A = MakeBox( glm::dvec3( 0 ), kSize );
        const DynamicMesh3 B = MakeBox( glm::dvec3( 50 ), kSize );
        for ( MeshBoolean::BooleanOp Op : { MeshBoolean::BooleanOp::Union, MeshBoolean::BooleanOp::Difference } )
        {
            const std::vector<unsigned char> First  = Bytes( RunBoolean( A, B, Op ) );
            const std::vector<unsigned char> Second = Bytes( RunBoolean( A, B, Op ) );
            ASSERT_FALSE( First.empty() );
            EXPECT_EQ( First, Second );
        }
    }

    TEST( MeshBooleanOperation, RunsAndRefusesByName )
    {
        const DynamicMesh3 A     = MakeBox( glm::dvec3( 0 ), kSize );
        const DynamicMesh3 B     = MakeBox( glm::dvec3( 50, 40, 30 ), kSize );
        const DynamicMesh3 Far   = MakeBox( glm::dvec3( 500 ), kSize );
        const DynamicMesh3 Sheet = MakeSheet( -50, 150, 5, 50 );

        auto Union = RunMeshBoolean( BooleanOperation::Union, A, B );
        ASSERT_TRUE( Union.IsSuccess() ) << Union.GetError();
        EXPECT_NEAR( Volume( *Union.GetValue().Mesh ), 1.79e6, 1.0 );

        auto Trim = RunMeshBoolean( BooleanOperation::TrimInside, Sheet, A );
        ASSERT_TRUE( Trim.IsSuccess() ) << Trim.GetError();

        auto OpenCutter = RunMeshBoolean( BooleanOperation::TrimInside, A, Sheet );
        ASSERT_FALSE( OpenCutter.IsSuccess() );
        EXPECT_NE( OpenCutter.GetError().find( "open edges" ), std::string::npos ) << OpenCutter.GetError();

        auto OpenTarget = RunMeshBoolean( BooleanOperation::Union, Sheet, A );
        ASSERT_FALSE( OpenTarget.IsSuccess() );
        EXPECT_NE( OpenTarget.GetError().find( "Trim takes an open mesh" ), std::string::npos );

        auto Disjoint = RunMeshBoolean( BooleanOperation::Intersect, A, Far );
        ASSERT_FALSE( Disjoint.IsSuccess() );
        EXPECT_NE( Disjoint.GetError().find( "no triangles left" ), std::string::npos ) << Disjoint.GetError();

        auto Untouched = RunMeshBoolean( BooleanOperation::TrimInside, A, Far );
        ASSERT_FALSE( Untouched.IsSuccess() );
        EXPECT_NE( Untouched.GetError().find( "does not cross" ), std::string::npos ) << Untouched.GetError();

        auto Empty = RunMeshBoolean( BooleanOperation::Union, DynamicMesh3(), A );
        ASSERT_FALSE( Empty.IsSuccess() );
        EXPECT_NE( Empty.GetError().find( "Mesh Union: the mesh has no triangles" ), std::string::npos );
    }

    // A CONCAVE cutter: the L-shaped prism over {[0,100] x [0,50]} u {[0,50] x [50,100]}, z in [0,100], closed
    // and facing out. The Trim it replaced (EditMesh TrimMesh) cut by the intersection of the cutter's face
    // planes and refused a dent; the notch [50,100]^2 is exactly what that would have got wrong.
    DynamicMesh3 MakeLPrism()
    {
        const std::array<glm::dvec2, 7> Outline{ { { 0, 0 },
                                                   { 100, 0 },
                                                   { 100, 50 },
                                                   { 50, 50 },
                                                   { 50, 100 },
                                                   { 0, 100 },
                                                   { 0, 50 } } }; // counter-clockwise
        DynamicMesh3                    Mesh;
        for ( const double Z : { 0.0, 100.0 } )
            for ( const glm::dvec2& P : Outline )
                Mesh.AppendVertex( glm::dvec3( P, Z ) );
        const int Top = static_cast<int>( Outline.size() );
        // the caps: the rectangle [0,100] x [0,50] (0,1,2,3,6) and the square [0,50] x [50,100] (6,3,4,5)
        const std::array<std::array<int, 3>, 5> Cap{
             { { 0, 1, 2 }, { 0, 2, 3 }, { 0, 3, 6 }, { 6, 3, 4 }, { 6, 4, 5 } } };
        for ( const auto& T : Cap )
        {
            AddOutwardTri( Mesh, T[0], T[1], T[2], glm::dvec3( 0, 0, -1 ) );
            AddOutwardTri( Mesh, T[0] + Top, T[1] + Top, T[2] + Top, glm::dvec3( 0, 0, 1 ) );
        }
        for ( int i = 0; i < Top; ++i )
        {
            const int        j = ( i + 1 ) % Top;
            const glm::dvec2 d = Outline[j] - Outline[i];
            const glm::dvec3 Out( d.y, -d.x, 0 ); // right of a counter-clockwise walk
            AddOutwardTri( Mesh, i, j, j + Top, Out );
            AddOutwardTri( Mesh, i, j + Top, i + Top, Out );
        }
        return Mesh;
    }

    TEST( MeshBooleanOperation, ConcaveCutterTrimsItsNotchAndSubtracts )
    {
        const DynamicMesh3 L = MakeLPrism();
        ASSERT_EQ( OpenEdges( L ), 0 );
        ASSERT_NEAR( Volume( L ), 7500.0 * 100.0, 1e-6 );
        const DynamicMesh3 Sheet = MakeSheet( -50, 150, 5, 50 );

        auto Outside = RunMeshBoolean( BooleanOperation::TrimInside, Sheet, L );
        ASSERT_TRUE( Outside.IsSuccess() ) << Outside.GetError();
        // the L's footprint goes, the notch stays: its convex hull would take 100 x 100
        EXPECT_NEAR( Area( *Outside.GetValue().Mesh ), 200.0 * 200.0 - 7500.0, 1e-6 * 4e4 );
        for ( int t : Outside.GetValue().Mesh->TriangleIndicesItr() )
        {
            glm::dvec3 a, b, c;
            Outside.GetValue().Mesh->GetTriVertices( t, a, b, c );
            EXPECT_FALSE( InsideOf( L, ( a + b + c ) / 3.0 ) ) << t;
        }

        auto Inside = RunMeshBoolean( BooleanOperation::TrimOutside, Sheet, L );
        ASSERT_TRUE( Inside.IsSuccess() ) << Inside.GetError();
        EXPECT_NEAR( Area( *Inside.GetValue().Mesh ), 7500.0, 1e-6 * 4e4 );

        // a cube [25,125]^3 minus the L: the overlap is 3125 cm^2 of footprint over 75 cm of height
        const DynamicMesh3 Cube = MakeBox( glm::dvec3( 25 ), kSize );
        auto               Diff = RunMeshBoolean( BooleanOperation::Difference, Cube, L );
        ASSERT_TRUE( Diff.IsSuccess() ) << Diff.GetError();
        EXPECT_EQ( OpenEdges( *Diff.GetValue().Mesh ), 0 );
        EXPECT_NEAR( Volume( *Diff.GetValue().Mesh ), 1e6 - 3125.0 * 75.0, 1.0 );
        EXPECT_FALSE( InsideOf( *Diff.GetValue().Mesh, glm::dvec3( 40, 40, 50 ) ) ); // in the L: removed
        EXPECT_TRUE( InsideOf( *Diff.GetValue().Mesh, glm::dvec3( 75, 75, 50 ) ) );  // in the notch: kept
    }
} // namespace
