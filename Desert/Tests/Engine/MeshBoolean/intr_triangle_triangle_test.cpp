// IntrTriangle3Triangle3 (P17, first brick of the FMeshBoolean port): the crossing segment of two triangles is
// exactly the chord of each triangle's plane through the other, clipped to both triangles.

#include "Engine/Geometry/MeshCore/Intersection/IntrTriangle3Triangle3.hpp"

#include <gtest/gtest.h>

namespace
{
    using namespace Desert::Geometry;

    // A horizontal triangle in z = 0 and a vertical one in x = 0.25 crossing it.
    const Triangle3d kFloor( glm::dvec3( 0, 0, 0 ), glm::dvec3( 2, 0, 0 ), glm::dvec3( 0, 2, 0 ) );
    const Triangle3d kWall( glm::dvec3( 0.25, -1, -1 ), glm::dvec3( 0.25, 3, -1 ), glm::dvec3( 0.25, 1, 3 ) );

    TEST( IntrTriangle3Triangle3, CrossingTrianglesMeetAlongTheClippedChord )
    {
        IntrTriangle3Triangle3 intr( kFloor, kWall );
        ASSERT_TRUE( intr.Find() );
        EXPECT_EQ( intr.Result, TriTriResult::Intersects );
        ASSERT_EQ( intr.Type, TriTriType::Segment );
        ASSERT_EQ( intr.Quantity, 2 );
        // The wall's chord in z = 0 runs y in [-0.5, 2.5]; the floor clips it at x = 0.25 to y in [0, 1.75].
        const double y0 = std::min( intr.Points[0].y, intr.Points[1].y );
        const double y1 = std::max( intr.Points[0].y, intr.Points[1].y );
        EXPECT_NEAR( y0, 0.0, 1e-12 );
        EXPECT_NEAR( y1, 1.75, 1e-12 );
        for ( const glm::dvec3& p : intr.Points )
        {
            EXPECT_NEAR( p.x, 0.25, 1e-12 );
            EXPECT_NEAR( p.z, 0.0, 1e-12 );
        }
    }

    TEST( IntrTriangle3Triangle3, OrderOfTheTwoTrianglesDoesNotChangeTheSegment )
    {
        IntrTriangle3Triangle3 ab( kFloor, kWall );
        IntrTriangle3Triangle3 ba( kWall, kFloor );
        ASSERT_TRUE( ab.Find() );
        ASSERT_TRUE( ba.Find() );
        const double lab = glm::length( ab.Points[1] - ab.Points[0] );
        const double lba = glm::length( ba.Points[1] - ba.Points[0] );
        EXPECT_NEAR( lab, 1.75, 1e-12 );
        EXPECT_NEAR( lba, 1.75, 1e-12 );
    }

    TEST( IntrTriangle3Triangle3, SeparatedAndCoplanarTrianglesReportNothing )
    {
        const Triangle3d       above( glm::dvec3( 0, 0, 1 ), glm::dvec3( 1, 0, 1 ), glm::dvec3( 0, 1, 2 ) );
        IntrTriangle3Triangle3 apart( kFloor, above );
        EXPECT_FALSE( apart.Find() );
        EXPECT_EQ( apart.Result, TriTriResult::NoIntersection );

        // Coplanar overlap: UE's Boolean runs with the coplanar report off, so this is "no intersection".
        const Triangle3d       flat( glm::dvec3( 0.5, 0.5, 0 ), glm::dvec3( 3, 0.5, 0 ), glm::dvec3( 0.5, 3, 0 ) );
        IntrTriangle3Triangle3 coplanar( kFloor, flat );
        EXPECT_FALSE( coplanar.Find() );
    }

    TEST( IntrTriangle3Triangle3, AWallMissingTheFloorSideways )
    {
        // Same wall moved to x = 2.5: its chord in z = 0 lies outside the floor triangle.
        const Triangle3d wall( glm::dvec3( 2.5, -1, -1 ), glm::dvec3( 2.5, 3, -1 ), glm::dvec3( 2.5, 1, 3 ) );
        IntrTriangle3Triangle3 intr( kFloor, wall );
        EXPECT_FALSE( intr.Find() );
    }

    TEST( IntrTriangle3Triangle3, SetTriangleResetsAReusedQuery )
    {
        IntrTriangle3Triangle3 intr( kFloor, kWall );
        ASSERT_TRUE( intr.Find() );
        const Triangle3d wall( glm::dvec3( 2.5, -1, -1 ), glm::dvec3( 2.5, 3, -1 ), glm::dvec3( 2.5, 1, 3 ) );
        intr.SetTriangle1( wall );
        EXPECT_FALSE( intr.Find() );
    }
} // namespace

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
