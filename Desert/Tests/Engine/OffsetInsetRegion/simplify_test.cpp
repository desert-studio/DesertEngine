// Simplify (MeshRegionOperation.hpp SimplifyMesh over the QEM port in MeshSimplification.hpp) on a closed
// cube-sphere with one polygroup per cube face: the triangle target is met, the mesh stays valid, and with
// Preserve PolyGroups every edge between two groups survives exactly (same end positions) - without it they do
// not.
#include "Engine/Geometry/MeshRegionOperation.hpp"

#include <gtest/gtest.h>

#include <glm/geometric.hpp>

#include <array>
#include <map>
#include <set>
#include <string>
#include <tuple>

using namespace Desert;
using namespace Desert::Geometry;

namespace
{
    constexpr double kHalf = 50.0;

    // A closed cube of N x N quads per face, face f in polygroup f + 1; @p sphere pushes it onto the sphere of
    // radius kHalf (every face curved, so the quadrics are not all zero).
    DynamicMesh3 CubeSphere( int n, bool sphere )
    {
        DynamicMesh3 mesh;
        mesh.EnableTriangleGroups();
        std::map<std::tuple<long, long, long>, int> welded;
        const auto                                  vertex = [&]( glm::dvec3 p )
        {
            const auto key = std::make_tuple( std::lround( p.x * 1000 ), std::lround( p.y * 1000 ),
                                              std::lround( p.z * 1000 ) );
            if ( const auto found = welded.find( key ); found != welded.end() )
                return found->second;
            if ( sphere )
                p = glm::normalize( p ) * kHalf;
            const int id = mesh.AppendVertex( p );
            welded.emplace( key, id );
            return id;
        };
        const glm::dvec3                 X( 1, 0, 0 ), Y( 0, 1, 0 ), Z( 0, 0, 1 );
        const std::array<glm::dvec3, 18> faces = { X, Y, Z, -X, Z, Y, Y, Z, X, -Y, X, Z, Z, X, Y, -Z, Y, X };
        for ( int f = 0; f < 6; ++f )
        {
            const glm::dvec3 nrm = faces[f * 3], u = faces[f * 3 + 1], v = faces[f * 3 + 2];
            const auto       at = [&]( int i, int j ) {
                return vertex( nrm * kHalf + u * ( -kHalf + 2 * kHalf * i / n ) +
                                     v * ( -kHalf + 2 * kHalf * j / n ) );
            };
            for ( int i = 0; i < n; ++i )
                for ( int j = 0; j < n; ++j )
                {
                    const int a = at( i, j ), b = at( i + 1, j ), c = at( i + 1, j + 1 ), d = at( i, j + 1 );
                    mesh.AppendTriangle( Index3i( a, b, c ), f + 1 );
                    mesh.AppendTriangle( Index3i( a, c, d ), f + 1 );
                }
        }
        return mesh;
    }

    using Point = std::tuple<long, long, long>;
    Point Key( const glm::dvec3& p )
    {
        return { std::lround( p.x * 1000 ), std::lround( p.y * 1000 ), std::lround( p.z * 1000 ) };
    }

    // Every edge between two polygroups, by its end positions.
    std::set<std::pair<Point, Point>> GroupBorders( const DynamicMesh3& mesh )
    {
        std::set<std::pair<Point, Point>> borders;
        for ( const int e : mesh.EdgeIndicesItr() )
        {
            if ( !mesh.IsGroupBoundaryEdge( e ) )
                continue;
            const Index2i v = mesh.GetEdgeV( e );
            const Point   a = Key( mesh.GetVertex( v.A ) ), b = Key( mesh.GetVertex( v.B ) );
            borders.emplace( std::min( a, b ), std::max( a, b ) );
        }
        return borders;
    }

    DynamicMesh3 Simplified( const DynamicMesh3& before, const SimplifySettings& settings )
    {
        auto result = SimplifyMesh( before, settings, ElementMode::Triangle );
        EXPECT_TRUE( result.IsSuccess() ) << ( result.IsSuccess() ? "" : result.GetError() );
        return result.IsSuccess() ? *result.GetValue().Mesh : DynamicMesh3{};
    }

    bool Valid( const DynamicMesh3& mesh )
    {
        return mesh.CheckValidity( DynamicMesh3::ValidityOptions{},
                                   ValidityCheckFailMode::ReturnOnly );
    }
} // namespace

TEST( Simplify, HalfOfACubeSphereKeepsEveryPolygroupBorder )
{
    const DynamicMesh3 before = CubeSphere( 8, true );
    ASSERT_EQ( before.TriangleCount(), 768 );
    const auto borders = GroupBorders( before );
    ASSERT_EQ( borders.size(), 12u * 8u );
    SimplifySettings   settings; // 50 %, Preserve PolyGroups
    const DynamicMesh3 after = Simplified( before, settings );
    EXPECT_LE( after.TriangleCount(), 384 );
    EXPECT_GT( after.TriangleCount(), 0 );
    EXPECT_TRUE( Valid( after ) );
    EXPECT_EQ( GroupBorders( after ), borders );
}

TEST( Simplify, AFlatCubeKeepsItsBordersAndReachesTheTarget )
{
    const DynamicMesh3 before = CubeSphere( 8, false );
    SimplifySettings   settings;
    settings.Percentage      = 25.0f;
    const DynamicMesh3 after = Simplified( before, settings );
    EXPECT_LE( after.TriangleCount(), 192 );
    EXPECT_TRUE( Valid( after ) );
    EXPECT_EQ( GroupBorders( after ), GroupBorders( before ) );
}

TEST( Simplify, WithoutPreservingTheBordersCollapse )
{
    const DynamicMesh3 before = CubeSphere( 8, true );
    SimplifySettings   settings;
    settings.PreserveGroupBoundaries = false;
    const DynamicMesh3 after         = Simplified( before, settings );
    EXPECT_LE( after.TriangleCount(), 384 );
    EXPECT_TRUE( Valid( after ) );
    EXPECT_LT( GroupBorders( after ).size(), GroupBorders( before ).size() );
}

TEST( Simplify, VertexCountTargetStopsAtTheCount )
{
    const DynamicMesh3 before = CubeSphere( 8, true );
    SimplifySettings   settings;
    settings.Target          = SimplifyTarget::VertexCount;
    settings.VertexCount     = 250;
    const DynamicMesh3 after = Simplified( before, settings );
    EXPECT_LE( after.VertexCount(), 250 );
    EXPECT_TRUE( Valid( after ) );
    EXPECT_EQ( GroupBorders( after ), GroupBorders( before ) );
}

TEST( Simplify, RefusalsNameTheirNumbers )
{
    const DynamicMesh3 before = CubeSphere( 2, true );
    SimplifySettings   settings;
    settings.Percentage = 0.0f;
    auto zero           = SimplifyMesh( before, settings, ElementMode::Triangle );
    ASSERT_FALSE( zero.IsSuccess() );
    EXPECT_NE( zero.GetError().find( "outside (0, 100]" ), std::string::npos ) << zero.GetError();
    settings.Target      = SimplifyTarget::VertexCount;
    settings.VertexCount = 1000;
    auto met             = SimplifyMesh( before, settings, ElementMode::Triangle );
    ASSERT_FALSE( met.IsSuccess() );
    EXPECT_NE( met.GetError().find( "already has" ), std::string::npos ) << met.GetError();
}
