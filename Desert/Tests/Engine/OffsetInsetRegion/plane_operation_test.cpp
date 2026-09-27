// Plane Cut and Mirror on the DynamicMesh3 (MeshPlaneOperation.hpp; the ports of FMeshPlaneCut, FMeshMirror and
// FPlanarHoleFiller): the halves of a closed cube are closed and their volumes sum to the cube's, the cap is one
// new polygroup and the result renders (every overlay set); an open mesh is cut with its open spans reported, not
// refused; a section with a hole is refused as the v1 limitation (P14b); Cut and Mirror is symmetric and closed;
// and MeshBoundaryLoops' ported EdgeFilterFunc walks only the edges it passes.

#include "Engine/Geometry/DynamicMeshRenderConversion.hpp"
#include "Engine/Geometry/MeshCore/MeshBoundaryLoops.hpp"
#include "Engine/Geometry/MeshPlaneOperation.hpp"

#include <gtest/gtest.h>

#include <glm/geometric.hpp>

#include <string>
#include <vector>

using namespace Desert;
using namespace Desert::Geometry;

namespace
{
    Vertex MakeVertex( const glm::vec3& p, const glm::vec3& n, const glm::vec3& t, const glm::vec2& uv )
    {
        Vertex v{};
        v.Position  = p;
        v.Normal    = n;
        v.Tangent   = t;
        v.Bitangent = glm::cross( n, t );
        v.TexCoord  = uv;
        return v;
    }

    // region_operation_test.cpp's HardCube, centred on @p centre; faces +X, -X, +Y, -Y, +Z, -Z (triangles 2f,
    // 2f+1).
    DynamicMesh3 Cube( float half, const glm::vec3& centre = glm::vec3( 0.0f ) )
    {
        const glm::vec3 X( 1, 0, 0 );
        const glm::vec3 Y( 0, 1, 0 );
        const glm::vec3 Z( 0, 0, 1 );
        struct Face
        {
            glm::vec3 N{}, U{}, V{};
        };
        const Face faces[6] = { { X, Y, Z }, { -X, Z, Y }, { Y, Z, X }, { -Y, X, Z }, { Z, X, Y }, { -Z, Y, X } };
        std::vector<Vertex> vertices;
        std::vector<Index>  indices;
        for ( const Face& face : faces )
        {
            const glm::vec3 c    = centre + face.N * half;
            const auto      base = static_cast<uint32_t>( vertices.size() );
            vertices.push_back( MakeVertex( c - face.U * half - face.V * half, face.N, face.U, { 0, 0 } ) );
            vertices.push_back( MakeVertex( c + face.U * half - face.V * half, face.N, face.U, { 1, 0 } ) );
            vertices.push_back( MakeVertex( c + face.U * half + face.V * half, face.N, face.U, { 1, 1 } ) );
            vertices.push_back( MakeVertex( c - face.U * half + face.V * half, face.N, face.U, { 0, 1 } ) );
            indices.push_back( { base, base + 1, base + 2 } );
            indices.push_back( { base, base + 2, base + 3 } );
        }
        RenderMeshData render;
        Submesh        s{};
        s.Name          = "MaterialID 0";
        s.VertexCount   = static_cast<uint32_t>( vertices.size() );
        s.IndexCount    = static_cast<uint32_t>( indices.size() * 3 );
        s.Transform     = glm::mat4( 1.0f );
        render.Vertices = vertices;
        render.Indices  = indices;
        render.Submeshes.push_back( s );
        render.SubmeshMaterialIds.push_back( 0 );
        auto imported = DynamicMeshFromRenderMesh( render );
        EXPECT_TRUE( imported.IsSuccess() ) << ( imported.IsSuccess() ? "" : imported.GetError() );
        DynamicMesh3 mesh = std::move( imported.ExtractValue().Mesh );
        mesh.EnableTriangleGroups();
        for ( int const t : mesh.TriangleIndicesItr() )
            mesh.SetTriangleGroup( t, 1 + t / 2 );
        return mesh;
    }

    // The cube with its +Z face removed: an open box.
    DynamicMesh3 OpenBox( float half )
    {
        DynamicMesh3 mesh = Cube( half );
        EXPECT_EQ( mesh.RemoveTriangle( 8 ), MeshResult::Ok );
        EXPECT_EQ( mesh.RemoveTriangle( 9 ), MeshResult::Ok );
        return mesh;
    }

    int BoundaryEdges( const DynamicMesh3& mesh )
    {
        int n = 0;
        for ( int const e : mesh.EdgeIndicesItr() )
            n += mesh.IsBoundaryEdge( e ) ? 1 : 0;
        return n;
    }

    // Signed: the test cube is wound so that it comes out NEGATIVE (-1e6 for Cube(50)); a half or a reflection
    // wound against its source would flip the sign, so every expectation is a multiple of the source's own volume.
    double Volume( const DynamicMesh3& mesh )
    {
        double v = 0.0;
        for ( int const t : mesh.TriangleIndicesItr() )
        {
            glm::dvec3 a;
            glm::dvec3 b;
            glm::dvec3 c;
            mesh.GetTriVertices( t, a, b, c );
            v += glm::dot( a, glm::cross( b, c ) ) / 6.0;
        }
        return v;
    }

    void ExpectRenders( const DynamicMesh3& mesh )
    {
        auto render = ToRenderMesh( mesh );
        EXPECT_TRUE( render.IsSuccess() ) << ( render.IsSuccess() ? "" : render.GetError() );
    }

    template <typename T>
    void ExpectRefused( const Common::ResultStr<T>& result, const std::string& words )
    {
        ASSERT_FALSE( result.IsSuccess() );
        EXPECT_NE( result.GetError().find( words ), std::string::npos ) << result.GetError();
    }
} // namespace

TEST( PlaneCutMesh, CubeInHalvesIsTwoClosedCappedMeshesThatSumToIt )
{
    const DynamicMesh3 cube = Cube( 50.0f );
    auto cut = PlaneCutMesh( cube, MeshPlane{ { 10, 0, 0 }, { 1, 0, 0 } }, PlaneCutMode::KeepBothHalves, true );
    ASSERT_TRUE( cut.IsSuccess() ) << cut.GetError();
    const PlaneCutOutcome& out = cut.GetValue();
    ASSERT_TRUE( out.OtherHalf );
    EXPECT_EQ( BoundaryEdges( *out.Kept.Mesh ), 0 );
    EXPECT_EQ( BoundaryEdges( *out.OtherHalf ), 0 );
    EXPECT_NEAR( Volume( *out.Kept.Mesh ), 0.4 * Volume( cube ), 1e-3 );
    EXPECT_NEAR( Volume( *out.OtherHalf ), 0.6 * Volume( cube ), 1e-3 );
    EXPECT_EQ( out.Kept.Selection.Mode(), ElementMode::PolyGroup );
    EXPECT_EQ( out.Kept.Selection.Size(), 1 );
    EXPECT_TRUE( out.Report.empty() ) << out.Report;
    ExpectRenders( *out.Kept.Mesh );
    ExpectRenders( *out.OtherHalf );
}

TEST( PlaneCutMesh, AnObliquePlaneKeepsTheVolume )
{
    const DynamicMesh3 cube = Cube( 50.0f );
    auto cut = PlaneCutMesh( cube, MeshPlane{ { 5, -3, 7 }, { 1, 2, 3 } }, PlaneCutMode::KeepBothHalves, true );
    ASSERT_TRUE( cut.IsSuccess() ) << cut.GetError();
    EXPECT_EQ( BoundaryEdges( *cut.GetValue().Kept.Mesh ), 0 );
    EXPECT_EQ( BoundaryEdges( *cut.GetValue().OtherHalf ), 0 );
    EXPECT_NEAR( Volume( *cut.GetValue().Kept.Mesh ) + Volume( *cut.GetValue().OtherHalf ), Volume( cube ), 1e-2 );
}

TEST( PlaneCutMesh, WithoutFillTheCutStaysOpenAndDiscardKeepsThePositiveSide )
{
    auto cut = PlaneCutMesh( Cube( 50.0f ), MeshPlane{ { 0, 0, 0 }, { 0, 0, -1 } },
                             PlaneCutMode::DiscardNegativeSide, false );
    ASSERT_TRUE( cut.IsSuccess() ) << cut.GetError();
    const DynamicMesh3& kept = *cut.GetValue().Kept.Mesh;
    EXPECT_FALSE( cut.GetValue().OtherHalf );
    EXPECT_EQ( BoundaryEdges( kept ), 8 ); // the square outline, each side split once by the face diagonals
    for ( int const v : kept.VertexIndicesItr() )
        EXPECT_LE( kept.GetVertex( v ).z, 1e-6 );
}

TEST( PlaneCutMesh, AnOpenMeshIsCutAndItsOpenSpansAreReportedNotRefused )
{
    // The plane x = 0 crosses the open box's own border (its missing top): the outline is an open span on each
    // side.
    auto cut = PlaneCutMesh( OpenBox( 50.0f ), MeshPlane{ { 0, 0, 0 }, { 1, 0, 0 } }, PlaneCutMode::KeepBothHalves,
                             true );
    ASSERT_TRUE( cut.IsSuccess() ) << cut.GetError();
    const PlaneCutOutcome& out = cut.GetValue();
    EXPECT_EQ( out.UnfilledSpans, 1 );
    EXPECT_EQ( out.OtherHalfUnfilledSpans, 1 );
    EXPECT_NE( out.Report.find( "1 loop(s) not filled on the kept half, 1 on the other" ), std::string::npos )
         << out.Report;
    EXPECT_EQ( out.Kept.Selection.Size(), 0 ); // no closed loop, no cap
    ExpectRenders( *out.Kept.Mesh );

    // Across the box below its open top the section is a closed loop: capped, nothing to report.
    auto low = PlaneCutMesh( OpenBox( 50.0f ), MeshPlane{ { 0, 0, 0 }, { 0, 0, 1 } },
                             PlaneCutMode::DiscardNegativeSide, true );
    ASSERT_TRUE( low.IsSuccess() ) << low.GetError();
    EXPECT_TRUE( low.GetValue().Report.empty() ) << low.GetValue().Report;
    EXPECT_EQ( low.GetValue().Kept.Selection.Size(), 1 );
}

TEST( PlaneCutMesh, ASectionWithAHoleIsTheV1Limitation )
{
    // Two nested cubes: the section is a square loop inside a square loop.
    DynamicMesh3       mesh  = Cube( 50.0f );
    const DynamicMesh3 inner = Cube( 20.0f );
    std::vector<int>   map( static_cast<size_t>( inner.MaxVertexID() ), -1 );
    for ( int const v : inner.VertexIndicesItr() )
        map[v] = mesh.AppendVertex( inner.GetVertex( v ) );
    for ( int const t : inner.TriangleIndicesItr() )
    {
        const Index3i tri = inner.GetTriangle( t );
        ASSERT_GE( mesh.AppendTriangle( map[tri.A], map[tri.B], map[tri.C], 100 ), 0 );
    }
    ExpectRefused(
         PlaneCutMesh( mesh, MeshPlane{ { 0, 0, 0 }, { 0, 0, 1 } }, PlaneCutMode::DiscardNegativeSide, true ),
         "limitation of this port (v1)" );
    ExpectRefused(
         PlaneCutMesh( mesh, MeshPlane{ { 0, 0, 0 }, { 0, 0, 1 } }, PlaneCutMode::DiscardNegativeSide, true ),
         "P14b" );
}

TEST( PlaneCutMesh, Refusals )
{
    const DynamicMesh3 cube = Cube( 50.0f );
    ExpectRefused( PlaneCutMesh( cube, MeshPlane{ {}, { 0, 0, 0 } }, PlaneCutMode::DiscardNegativeSide, true ),
                   "normal is zero" );
    ExpectRefused(
         PlaneCutMesh( cube, MeshPlane{ { 200, 0, 0 }, { 1, 0, 0 } }, PlaneCutMode::DiscardNegativeSide, true ),
         "does not cross the mesh" );
}

TEST( MirrorMesh, CutAndMirrorIsSymmetricAndClosed )
{
    auto mirrored = MirrorMesh( Cube( 50.0f ), MeshPlane{ { 10, 0, 0 }, { 1, 0, 0 } }, MirrorMode::CutAndMirror,
                                0.01f, ElementMode::Triangle );
    ASSERT_TRUE( mirrored.IsSuccess() ) << mirrored.GetError();
    const DynamicMesh3& mesh = *mirrored.GetValue().Mesh;
    EXPECT_EQ( BoundaryEdges( mesh ), 0 );
    EXPECT_NEAR( Volume( mesh ), 0.8 * Volume( Cube( 50.0f ) ), 1e-3 );
    const AxisAlignedBox3d bounds = mesh.GetBounds();
    EXPECT_NEAR( bounds.Min.x, -30.0, 1e-6 );
    EXPECT_NEAR( bounds.Max.x, 50.0, 1e-6 );
    EXPECT_EQ( mirrored.GetValue().Selection.Size(), 0 );
    ExpectRenders( mesh );
}

TEST( MirrorMesh, TheReflectionOfAnOpenHalfWeldsItClosed )
{
    auto half = PlaneCutMesh( Cube( 50.0f ), MeshPlane{ { 0, 0, 0 }, { 1, 0, 0 } },
                              PlaneCutMode::DiscardNegativeSide, false );
    ASSERT_TRUE( half.IsSuccess() ) << half.GetError();
    auto whole = MirrorMesh( *half.GetValue().Kept.Mesh, MeshPlane{ { 0, 0, 0 }, { 1, 0, 0 } },
                             MirrorMode::AddMirroredCopy, 0.01f, ElementMode::Vertex );
    ASSERT_TRUE( whole.IsSuccess() ) << whole.GetError();
    EXPECT_EQ( BoundaryEdges( *whole.GetValue().Mesh ), 0 );
    EXPECT_NEAR( Volume( *whole.GetValue().Mesh ), Volume( Cube( 50.0f ) ), 1e-3 );
    ExpectRenders( *whole.GetValue().Mesh );
}

TEST( MirrorMesh, Refusals )
{
    const DynamicMesh3 cube = Cube( 50.0f );
    ExpectRefused(
         MirrorMesh( cube, MeshPlane{ {}, { 0, 0, 0 } }, MirrorMode::AddMirroredCopy, 0.01f, ElementMode::Vertex ),
         "normal is zero" );
    ExpectRefused(
         MirrorMesh( cube, MeshPlane{ {}, { 1, 0, 0 } }, MirrorMode::AddMirroredCopy, -1.0f, ElementMode::Vertex ),
         "must be >= 0" );
    ExpectRefused( MirrorMesh( cube, MeshPlane{ { 200, 0, 0 }, { 1, 0, 0 } }, MirrorMode::CutAndMirror, 0.01f,
                               ElementMode::Vertex ),
                   "nothing lies on the kept side" );
}

TEST( MeshBoundaryLoops, EdgeFilterFuncWalksOnlyTheEdgesItPasses )
{
    const DynamicMesh3 box = OpenBox( 50.0f );
    MeshBoundaryLoops  all( &box, true );
    EXPECT_EQ( all.GetLoopCount(), 1 );

    MeshBoundaryLoops none( &box, false );
    none.EdgeFilterFunc = []( int ) { return false; };
    ASSERT_TRUE( none.Compute() );
    EXPECT_EQ( none.GetLoopCount(), 0 );
    EXPECT_TRUE( none.m_Spans.empty() );

    // Two of the rim's four edges: one open span, no loop.
    std::vector<int> rim;
    for ( int const e : box.EdgeIndicesItr() )
        if ( box.IsBoundaryEdge( e ) )
            rim.push_back( e );
    ASSERT_EQ( rim.size(), 4u );
    const Index2i first = box.GetEdgeV( rim[0] );
    int           next  = -1;
    for ( int const e : rim )
        if ( e != rim[0] )
        {
            const Index2i ev = box.GetEdgeV( e );
            if ( ev.A == first.A || ev.A == first.B || ev.B == first.A || ev.B == first.B )
                next = e;
        }
    ASSERT_GE( next, 0 );
    MeshBoundaryLoops two( &box, false );
    two.EdgeFilterFunc = [&]( int e ) { return e == rim[0] || e == next; };
    ASSERT_TRUE( two.Compute() );
    EXPECT_EQ( two.GetLoopCount(), 0 );
    ASSERT_EQ( two.m_Spans.size(), 1u );
    EXPECT_EQ( two.m_Spans[0].Vertices.size(), 3u );
}
