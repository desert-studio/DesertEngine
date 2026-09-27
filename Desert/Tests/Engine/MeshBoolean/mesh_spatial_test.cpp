// DynamicMeshAABBTree3 + FastWindingTree (P17, the spatial half of the FMeshBoolean port): every tree answer is
// checked against brute force over all triangles, and the fast winding number against the exact solid-angle sum.

#include "Engine/Geometry/MeshCore/Spatial/FastWinding.hpp"
#include "Engine/Geometry/MeshCore/Spatial/MeshAABBTree3.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <numbers>
#include <random>
#include <tuple>
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

    // A closed cube [-1, 1]^3 with N x N quads per face (faces share no vertices), triangles facing out. With
    // bSphere the vertices are pushed onto the unit sphere, then scaled and moved.
    DynamicMesh3 MakeGridCube( int N, bool bSphere, double Scale, const glm::dvec3& Offset )
    {
        DynamicMesh3 Mesh;
        for ( int Axis = 0; Axis < 3; ++Axis )
        {
            for ( const double Sign : { -1.0, 1.0 } )
            {
                const int u    = ( Axis + 1 ) % 3;
                const int v    = ( Axis + 2 ) % 3;
                const int Base = Mesh.MaxVertexID();
                for ( int j = 0; j <= N; ++j )
                {
                    for ( int i = 0; i <= N; ++i )
                    {
                        glm::dvec3 p( 0 );
                        p[Axis] = Sign;
                        p[u]    = -1.0 + 2.0 * i / N;
                        p[v]    = -1.0 + 2.0 * j / N;
                        if ( bSphere )
                        {
                            p = glm::normalize( p );
                        }
                        Mesh.AppendVertex( p * Scale + Offset );
                    }
                }
                glm::dvec3 Outward( 0 );
                Outward[Axis] = Sign;
                for ( int j = 0; j < N; ++j )
                {
                    for ( int i = 0; i < N; ++i )
                    {
                        const int v00 = Base + j * ( N + 1 ) + i;
                        const int v10 = v00 + 1;
                        const int v01 = v00 + N + 1;
                        const int v11 = v01 + 1;
                        AddOutwardTri( Mesh, v00, v10, v11, Outward );
                        AddOutwardTri( Mesh, v00, v11, v01, Outward );
                    }
                }
            }
        }
        return Mesh;
    }

    double BruteNearestDistSqr( const DynamicMesh3& Mesh, const glm::dvec3& P, bool bOddOnly )
    {
        double Best = std::numeric_limits<double>::max();
        for ( const int tid : Mesh.TriangleIndicesItr() )
        {
            if ( bOddOnly && tid % 2 == 0 )
            {
                continue;
            }
            Triangle3d Tri;
            Mesh.GetTriVertices( tid, Tri.V[0], Tri.V[1], Tri.V[2] );
            Best = std::min( Best, DistPoint3Triangle3<double>( P, Tri ).GetSquared() );
        }
        return Best;
    }

    double ExactWinding( const DynamicMesh3& Mesh, const glm::dvec3& P )
    {
        double Sum = 0;
        for ( const int tid : Mesh.TriangleIndicesItr() )
        {
            glm::dvec3 a;
            glm::dvec3 b;
            glm::dvec3 c;
            Mesh.GetTriVertices( tid, a, b, c );
            Sum += VectorUtil::TriSolidAngle( a, b, c, P );
        }
        return Sum / ( 4.0 * std::numbers::pi );
    }

    TEST( MeshAABBTree3, NearestTriangleMatchesBruteForce )
    {
        const DynamicMesh3   Mesh = MakeGridCube( 5, true, 1.0, glm::dvec3( 0 ) );
        DynamicMeshAABBTree3 Tree( &Mesh );
        ASSERT_TRUE( Tree.IsValid() );

        std::mt19937                             Rng( 17 );
        std::uniform_real_distribution<double>   Coord( -2.0, 2.0 );
        const DynamicMeshAABBTree3::QueryOptions OddOnly( []( int tid ) { return tid % 2 == 1; } );
        for ( int k = 0; k < 300; ++k )
        {
            const glm::dvec3 P( Coord( Rng ), Coord( Rng ), Coord( Rng ) );
            double           DistSqr = -1;
            const int        tid     = Tree.FindNearestTriangle( P, DistSqr );
            ASSERT_TRUE( Mesh.IsTriangle( tid ) );
            EXPECT_DOUBLE_EQ( DistSqr, BruteNearestDistSqr( Mesh, P, false ) ) << "query " << k;

            double    OddDistSqr = -1;
            const int OddTid     = Tree.FindNearestTriangle( P, OddDistSqr, OddOnly );
            EXPECT_EQ( OddTid % 2, 1 );
            EXPECT_DOUBLE_EQ( OddDistSqr, BruteNearestDistSqr( Mesh, P, true ) ) << "query " << k;
        }

        // MaxDistance: nothing within 0.5 of a point 3 away from the unit sphere.
        double    DistSqr = 0;
        const int None =
             Tree.FindNearestTriangle( glm::dvec3( 4, 0, 0 ), DistSqr, DynamicMeshAABBTree3::QueryOptions( 0.5 ) );
        EXPECT_EQ( None, DynamicMesh3::InvalidID );
        EXPECT_DOUBLE_EQ( DistSqr, 0.25 );
    }

    TEST( MeshAABBTree3, AllIntersectionsMatchBruteForce )
    {
        // Two faceted spheres overlapping by half a radius; the second is scaled so no facets align.
        const DynamicMesh3   A = MakeGridCube( 6, true, 1.0, glm::dvec3( 0 ) );
        const DynamicMesh3   B = MakeGridCube( 5, true, 0.83, glm::dvec3( 0.9, 0.31, 0.17 ) );
        DynamicMeshAABBTree3 TreeA( &A );
        DynamicMeshAABBTree3 TreeB( &B );

        using Row = std::tuple<int, int, glm::dvec3, glm::dvec3>;
        std::vector<Row> FromTree;
        for ( const auto& Seg : TreeA.FindAllIntersections( TreeB ).Segments )
        {
            FromTree.emplace_back( Seg.TriangleID[0], Seg.TriangleID[1], Seg.Point[0], Seg.Point[1] );
        }

        std::vector<Row>       Brute;
        IntrTriangle3Triangle3 Intr;
        for ( const int ta : A.TriangleIndicesItr() )
        {
            for ( const int tb : B.TriangleIndicesItr() )
            {
                Triangle3d TA;
                Triangle3d TB;
                A.GetTriVertices( ta, TA.V[0], TA.V[1], TA.V[2] );
                B.GetTriVertices( tb, TB.V[0], TB.V[1], TB.V[2] );
                Intr.SetTriangle0( TB );
                Intr.SetTriangle1( TA );
                if ( Intr.Find() && Intr.Quantity == 2 )
                {
                    Brute.emplace_back( ta, tb, Intr.Points[0], Intr.Points[1] );
                }
            }
        }

        const auto ByIds = []( const Row& L, const Row& R ) {
            return std::tie( std::get<0>( L ), std::get<1>( L ) ) < std::tie( std::get<0>( R ), std::get<1>( R ) );
        };
        std::sort( FromTree.begin(), FromTree.end(), ByIds );
        std::sort( Brute.begin(), Brute.end(), ByIds );
        ASSERT_GT( Brute.size(), 40u ); // the circle of contact crosses many facets
        ASSERT_EQ( FromTree.size(), Brute.size() );
        for ( size_t i = 0; i < Brute.size(); ++i )
        {
            EXPECT_EQ( std::get<0>( FromTree[i] ), std::get<0>( Brute[i] ) );
            EXPECT_EQ( std::get<1>( FromTree[i] ), std::get<1>( Brute[i] ) );
            EXPECT_EQ( std::get<2>( FromTree[i] ), std::get<2>( Brute[i] ) );
            EXPECT_EQ( std::get<3>( FromTree[i] ), std::get<3>( Brute[i] ) );
        }

        // Moved apart, the two trees report nothing.
        const DynamicMesh3   Far = MakeGridCube( 5, true, 0.83, glm::dvec3( 3, 0, 0 ) );
        DynamicMeshAABBTree3 TreeFar( &Far );
        EXPECT_TRUE( TreeA.FindAllIntersections( TreeFar ).Segments.empty() );
    }

    TEST( FastWindingTree, CubeWindingIsOneInsideZeroOutsideAndTracksExact )
    {
        const DynamicMesh3   Cube = MakeGridCube( 6, false, 1.0, glm::dvec3( 0 ) );
        DynamicMeshAABBTree3 Tree( &Cube );
        FastWindingTree      Winding( &Tree );
        ASSERT_TRUE( Winding.IsBuilt() );

        std::mt19937                           Rng( 5 );
        std::uniform_real_distribution<double> Inner( -0.9, 0.9 );
        std::uniform_real_distribution<double> Dir( -1.0, 1.0 );
        std::uniform_real_distribution<double> Radius( 1.9, 6.0 );

        double MaxErrOrder2 = 0;
        double SumErrOrder1 = 0;
        double SumErrOrder2 = 0;
        int    Approximated = 0;
        for ( int k = 0; k < 200; ++k )
        {
            const glm::dvec3 In( Inner( Rng ), Inner( Rng ), Inner( Rng ) );
            const glm::dvec3 Out =
                 glm::normalize( glm::dvec3( Dir( Rng ), Dir( Rng ), Dir( Rng ) ) ) * Radius( Rng );
            for ( const auto& [P, Expected] : { std::pair{ In, 1.0 }, std::pair{ Out, 0.0 } } )
            {
                const double Exact = ExactWinding( Cube, P );
                ASSERT_NEAR( Exact, Expected, 1e-9 ) << "exact winding at query " << k;

                Winding.FWNApproxOrder = 2;
                const double Fast2     = Winding.FastWindingNumber( P );
                Winding.FWNApproxOrder = 1;
                const double Fast1     = Winding.FastWindingNumber( P );
                Winding.FWNApproxOrder = 2;

                EXPECT_EQ( Winding.IsInside( P ), Expected > 0.5 ) << "query " << k;
                MaxErrOrder2 = std::max( MaxErrOrder2, std::abs( Fast2 - Exact ) );
                SumErrOrder1 += std::abs( Fast1 - Exact );
                SumErrOrder2 += std::abs( Fast2 - Exact );
                Approximated += std::abs( Fast2 - Exact ) > 1e-12 ? 1 : 0;
            }
        }
        std::printf( "max err order2 %.3g, mean err order1 %.3g order2 %.3g, approximated %d/400\n", MaxErrOrder2,
                     SumErrOrder1 / 400, SumErrOrder2 / 400, Approximated );
        // The far field must actually be in use, or this test would only be checking the exact leaf sum.
        EXPECT_GT( Approximated, 100 );
        // Measured: max 0.033 (a query 0.1 from a face), mean 0.0038 with order 2 and 0.020 with order 1.
        EXPECT_LT( MaxErrOrder2, 0.05 );
        EXPECT_LT( SumErrOrder2 / 400, 0.01 );
        // The second-order term must improve on the dipole alone.
        EXPECT_LT( SumErrOrder2, 0.5 * SumErrOrder1 );
    }
} // namespace
