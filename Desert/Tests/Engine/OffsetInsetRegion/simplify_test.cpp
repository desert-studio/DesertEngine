// Simplify (MeshRegionOperation.hpp SimplifyMesh over the QEM port in MeshSimplification.hpp) on a closed
// cube-sphere with one polygroup per cube face: the triangle target is met, the mesh stays valid, and with
// Preserve PolyGroups every edge between two groups survives exactly (same end positions) - without it they do
// not.
#include "Engine/Geometry/MeshCore/DynamicMesh/DynamicMeshAttributeSet.hpp"
#include "Engine/Geometry/MeshCore/DynamicMesh/MeshNormals.hpp"
#include "Engine/Geometry/MeshRegionOperation.hpp"

#include <gtest/gtest.h>

#include <glm/geometric.hpp>

#include <array>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

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
        const glm::dvec3                 X( 1, 0, 0 );
        const glm::dvec3                 Y( 0, 1, 0 );
        const glm::dvec3                 Z( 0, 0, 1 );
        const std::array<glm::dvec3, 18> faces = { X, Y, Z, -X, Z, Y, Y, Z, X, -Y, X, Z, Z, X, Y, -Z, Y, X };
        for ( int f = 0; f < 6; ++f )
        {
            const glm::dvec3 nrm = faces[static_cast<size_t>( f ) * 3];
            const glm::dvec3 u   = faces[static_cast<size_t>( f ) * 3 + 1];
            const glm::dvec3 v   = faces[static_cast<size_t>( f ) * 3 + 2];
            const auto       at  = [&]( int i, int j ) {
                return vertex( nrm * kHalf + u * ( -kHalf + 2 * kHalf * i / n ) +
                                      v * ( -kHalf + 2 * kHalf * j / n ) );
            };
            for ( int i = 0; i < n; ++i )
                for ( int j = 0; j < n; ++j )
                {
                    const int a = at( i, j );
                    const int b = at( i + 1, j );
                    const int c = at( i + 1, j + 1 );
                    const int d = at( i, j + 1 );
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
            const Point   a = Key( mesh.GetVertex( v.A ) );
            const Point   b = Key( mesh.GetVertex( v.B ) );
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
        return mesh.CheckValidity( DynamicMesh3::ValidityOptions{}, ValidityCheckFailMode::ReturnOnly );
    }
} // namespace

TEST( Simplify, HalfOfACubeSphereKeepsEveryPolygroupBorder )
{
    const DynamicMesh3 before = CubeSphere( 8, true );
    ASSERT_EQ( before.TriangleCount(), 768 );
    const auto borders = GroupBorders( before );
    ASSERT_EQ( borders.size(), 12u * 8u );
    const SimplifySettings settings; // 50 %, Preserve PolyGroups
    const DynamicMesh3     after = Simplified( before, settings );
    // Each interior collapse removes two triangles: the simplifier stops AT the target, not below it.
    EXPECT_LE( after.TriangleCount(), 384 );
    EXPECT_GE( after.TriangleCount(), 383 );
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
    EXPECT_EQ( after.VertexCount(), 250 ); // one vertex per collapse
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

namespace
{
    // The flat cube with every face-interior vertex moved in its face's plane by up to @p jitter of a cell
    // (a fixed LCG, so the mesh is the same on every run). A flat face has zero quadrics, so the collapse
    // position is the fallback, and a jittered one-ring is not convex: without the flip check
    // (CreatesFlipOrInvalid) collapses here turn triangles inside out and leave zero-area ones.
    DynamicMesh3 JitteredCube( int n, double jitter )
    {
        DynamicMesh3     mesh = CubeSphere( n, false );
        std::vector<int> ids;
        for ( const int v : mesh.VertexIndicesItr() )
            ids.push_back( v );
        unsigned   seed = 12345u;
        const auto rnd  = [&]
        {
            seed = seed * 1664525u + 1013904223u;
            return static_cast<double>( seed >> 8 ) / static_cast<double>( 1u << 24 ) - 0.5;
        };
        const double cell        = 2.0 * kHalf / n;
        const auto   onFacePlane = [&]( double c ) { return std::abs( std::abs( c ) - kHalf ) < 1e-6; };
        for ( const int v : ids )
        {
            glm::dvec3 p = mesh.GetVertex( v );
            if ( static_cast<int>( onFacePlane( p.x ) ) + static_cast<int>( onFacePlane( p.y ) ) +
                      static_cast<int>( onFacePlane( p.z ) ) !=
                 1 )
                continue; // on a cube edge: moving it would bend the face
            for ( int k = 0; k < 3; ++k )
                if ( !onFacePlane( p[k] ) )
                    p[k] += rnd() * 2.0 * jitter * cell;
            mesh.SetVertex( v, p );
        }
        return mesh;
    }

    // Triangles facing the centre (the cube is convex, so none may) and triangles of zero area.
    std::pair<int, int> InwardAndDegenerate( const DynamicMesh3& mesh )
    {
        int inward     = 0;
        int degenerate = 0;
        for ( const int t : mesh.TriangleIndicesItr() )
        {
            const Index3i    tri    = mesh.GetTriangle( t );
            const glm::dvec3 a      = mesh.GetVertex( tri.A );
            const glm::dvec3 b      = mesh.GetVertex( tri.B );
            const glm::dvec3 c      = mesh.GetVertex( tri.C );
            const glm::dvec3 normal = glm::cross( b - a, c - a );
            if ( glm::length( normal ) < 1e-6 )
                ++degenerate;
            else if ( glm::dot( normal, a + b + c ) < 0.0 )
                ++inward;
        }
        return { inward, degenerate };
    }
} // namespace

TEST( Simplify, FlatJitteredFacesNeverTurnATriangleInsideOut )
{
    const DynamicMesh3 before = JitteredCube( 16, 0.3 );
    ASSERT_EQ( InwardAndDegenerate( before ), std::make_pair( 0, 0 ) );
    for ( const bool preserve : { false, true } )
    {
        SimplifySettings settings;
        settings.Percentage              = 30.0f;
        settings.PreserveGroupBoundaries = preserve;
        const DynamicMesh3 after         = Simplified( before, settings );
        EXPECT_EQ( after.TriangleCount(), 920 ) << "preserve " << preserve;
        EXPECT_TRUE( Valid( after ) );
        // Measured without the flip check: 128 / 30 (free borders) and 108 / 60 (kept borders).
        EXPECT_EQ( InwardAndDegenerate( after ), std::make_pair( 0, 0 ) ) << "preserve " << preserve;
    }
}

namespace
{
    // The cube-sphere with a normal layer split along every polygroup border (the editor's Box, sphere-projected,
    // with hard edges between its faces) or at every triangle (Per Face normals), and no UV layer.
    DynamicMesh3 SeamedCubeSphere( int n, bool perTriangle )
    {
        DynamicMesh3 mesh = CubeSphere( n, true );
        mesh.EnableAttributes();
        mesh.Attributes()->SetNumUVLayers( 0 );
        DynamicMeshNormalOverlay* normals = mesh.Attributes()->PrimaryNormals();
        if ( perTriangle )
            MeshNormals::InitializeOverlayToPerTriangleNormals( normals );
        else
        {
            MeshNormals::InitializeOverlayTopologyFromFaceGroups( &mesh, normals );
            MeshNormals::QuickRecomputeOverlayNormals( mesh );
        }
        return mesh;
    }

    int SeamEdges( const DynamicMesh3& mesh )
    {
        int seams = 0;
        for ( const int e : mesh.EdgeIndicesItr() )
            seams += mesh.Attributes()->IsSeamEdge( e ) ? 1 : 0;
        return seams;
    }
} // namespace

// M18c: a seam used to be NoCollapse + a vertex that cannot move, so a second Simplify of a hard-edged mesh found
// nothing to collapse ("1984 of 2304 edges constrained"). UE's default lets a seam collapse along its own line.
TEST( Simplify, ASecondSimplifyOfAHardEdgedSphereCollapsesAlongItsSeams )
{
    for ( const bool perTriangle : { false, true } )
    {
        const DynamicMesh3 before = SeamedCubeSphere( 8, perTriangle );
        SimplifySettings   settings;
        settings.Percentage       = 50.0f;
        const DynamicMesh3 half   = Simplified( before, settings );
        const DynamicMesh3 fourth = Simplified( half, settings );
        std::printf( "perTriangle %d: %d -> %d -> %d triangles, seams %d -> %d -> %d\n",
                     static_cast<int>( perTriangle ), before.TriangleCount(), half.TriangleCount(),
                     fourth.TriangleCount(), SeamEdges( before ), SeamEdges( half ), SeamEdges( fourth ) );
        EXPECT_EQ( half.TriangleCount(), before.TriangleCount() / 2 ) << perTriangle;
        EXPECT_EQ( fourth.TriangleCount(), before.TriangleCount() / 4 ) << perTriangle;
        EXPECT_TRUE( Valid( half ) && Valid( fourth ) ) << perTriangle;
        // M18d: without the fin rule 10 / 12 triangles of the quarter faced inward - ears cut off a seam line,
        // three corners on one great circle; none is degenerate (bPreventTinyTriangles).
        EXPECT_EQ( InwardAndDegenerate( half ), std::make_pair( 0, 0 ) ) << perTriangle;
        EXPECT_EQ( InwardAndDegenerate( fourth ), std::make_pair( 0, 0 ) ) << perTriangle;
        if ( perTriangle )
        {
            // Every triangle keeps its own normal elements: every edge stays a seam.
            EXPECT_EQ( SeamEdges( fourth ), fourth.EdgeCount() );
            continue;
        }
        // The seams are the polygroup borders, before and after: a collapse moved them along their line but
        // never opened, closed or crossed one.
        for ( const DynamicMesh3* mesh : { &half, &fourth } )
            for ( const int e : mesh->EdgeIndicesItr() )
                EXPECT_EQ( mesh->Attributes()->IsSeamEdge( e ), mesh->IsGroupBoundaryEdge( e ) ) << "edge " << e;
    }
}

// M18c: with the polygroup borders kept, a collapse onto a border could leave a triangle whose three corners lie
// on one border arc - a sliver standing across the surface (71 of 372 facing inward on this sphere, 51 with the
// UE checks ported). M18d: the fin rule (QemSimplification::CreatesFin) refuses it - 0 of 384; every face keeps
// two more triangles than the pure triangulation of its 64-vertex border (62).
TEST( Simplify, KeptBordersLeaveNoSliverStandingAcrossTheSurface )
{
    const DynamicMesh3 before = CubeSphere( 16, true );
    SimplifySettings   settings;
    settings.Percentage              = 10.0f;
    settings.PreserveGroupBoundaries = true;
    const DynamicMesh3 after         = Simplified( before, settings );
    std::printf( "kept borders 10%%: %d triangles, inward/degenerate %d/%d\n", after.TriangleCount(),
                 InwardAndDegenerate( after ).first, InwardAndDegenerate( after ).second );
    EXPECT_EQ( after.TriangleCount(), 384 );
    EXPECT_TRUE( Valid( after ) );
    EXPECT_EQ( GroupBorders( after ), GroupBorders( before ) );
    EXPECT_EQ( InwardAndDegenerate( after ), std::make_pair( 0, 0 ) );
}
