// RunRegionOperation (MeshRegionOperation.hpp) on the input the editor gives it: a mesh imported from render data
// WITH a tangent space (as Create Shape's meshes are), one polygroup per face, selected in PolyGroup mode. The
// result must convert back to render data (ToRenderMesh refuses a triangle unset in the tangent overlay).
#include "Engine/Geometry/DynamicMeshRenderConversion.hpp"
#include "Engine/Geometry/MeshRegionOperation.hpp"
#include "Engine/Geometry/UECore/DynamicMesh/DynamicMeshAttributeSet.hpp"
#include "Engine/Geometry/UECore/DynamicMesh/GroupTopology.hpp"
#include "Engine/Geometry/UECore/DynamicMesh/MeshTangents.hpp"
#include "Engine/Geometry/UECore/DynamicMesh/Operations/MergeCoincidentMeshEdges.hpp"
#include "Engine/Geometry/UECore/DynamicMesh/Operations/SimpleHoleFiller.hpp"
#include "Engine/Geometry/UECore/DynamicMeshEditor.hpp"
#include "Engine/Geometry/UECore/MeshBoundaryLoops.hpp"

#include <gtest/gtest.h>

#include <glm/geometric.hpp>

#include <algorithm>
#include <array>
#include <cmath>
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

    // dynamicmesh3render_test.cpp's HardCube in one submesh: one quad per face with its own normal, tangent and
    // UVs; face f's triangles are 2f and 2f+1. Faces: +X, -X, +Y, -Y, +Z, -Z.
    RenderMeshData HardCube( float half )
    {
        const glm::vec3 X( 1, 0, 0 ), Y( 0, 1, 0 ), Z( 0, 0, 1 );
        struct Face
        {
            glm::vec3 N, U, V;
        };
        const Face faces[6] = { { X, Y, Z }, { -X, Z, Y }, { Y, Z, X }, { -Y, X, Z }, { Z, X, Y }, { -Z, Y, X } };
        std::vector<Vertex> vertices;
        std::vector<Index>  indices;
        for ( const Face& face : faces )
        {
            const glm::vec3 c    = face.N * half;
            const uint32_t  base = static_cast<uint32_t>( vertices.size() );
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
        return render;
    }

    // Face f is polygroup f + 1.
    FDynamicMesh3 TangentCube()
    {
        auto imported = DynamicMeshFromRenderMesh( HardCube( 50.0f ) );
        EXPECT_TRUE( imported.IsSuccess() ) << ( imported.IsSuccess() ? "" : imported.GetError() );
        FDynamicMesh3 mesh = std::move( imported.ExtractValue().Mesh );
        EXPECT_TRUE( mesh.Attributes()->HasTangentSpace() );
        mesh.EnableTriangleGroups();
        for ( int t : mesh.TriangleIndicesItr() )
            mesh.SetTriangleGroup( t, 1 + t / 2 );
        return mesh;
    }

    ElementSelection Groups( const FDynamicMesh3& mesh, std::initializer_list<int> faces )
    {
        ElementSelection selection( ElementMode::PolyGroup );
        for ( int f : faces )
            EXPECT_TRUE( selection.Add( mesh, f + 1 ).IsSuccess() ) << "face " << f;
        return selection;
    }

    constexpr int kPlusX  = 0;
    constexpr int kMinusX = 1;
    constexpr int kPlusY  = 2;
    constexpr int kMinusY = 3;
    constexpr int kPlusZ  = 4;
} // namespace

TEST( RegionOperation, TangentCubeImportsWithTheTopFaceWhereTheTestSaysItIs )
{
    const FDynamicMesh3 mesh = TangentCube();
    EXPECT_NEAR( mesh.GetTriNormal( 2 * kPlusZ ).Z, 1.0, 1e-9 );
    EXPECT_NEAR( mesh.GetTriNormal( 2 * kPlusZ + 1 ).Z, 1.0, 1e-9 );
}

// The editor's defect (P11d): every region operation on a Create-Shape mesh was refused with "ToRenderMesh:
// triangle 12 is unset in the tangent overlay". Each operation must now render back, with a unit tangent frame
// orthogonal to the normal at every render vertex, and return its region in PolyGroup mode.
TEST( RegionOperation, EveryOperationOnATangentCubeRendersBack )
{
    for ( RegionOperation operation : { RegionOperation::Extrude, RegionOperation::PushPull,
                                        RegionOperation::Inset, RegionOperation::Outset } )
    {
        SCOPED_TRACE( ToString( operation ) );
        const FDynamicMesh3 before = TangentCube();
        auto                region = RunRegionOperation( operation, before, Groups( before, { kPlusZ } ), 20.0f );
        ASSERT_TRUE( region.IsSuccess() ) << region.GetError();
        const RegionOutcome done = region.ExtractValue();
        EXPECT_EQ( done.Selection.Mode(), ElementMode::PolyGroup );
        EXPECT_FALSE( done.Selection.Empty() );

        auto render = ToRenderMesh( *done.Mesh );
        ASSERT_TRUE( render.IsSuccess() ) << render.GetError();
        for ( const Vertex& v : render.GetValue().Vertices )
        {
            EXPECT_NEAR( glm::length( v.Tangent ), 1.0f, 1e-5f );
            EXPECT_NEAR( glm::length( v.Bitangent ), 1.0f, 1e-5f );
            EXPECT_NEAR( glm::dot( v.Tangent, v.Normal ), 0.0f, 1e-5f );
            EXPECT_NEAR( glm::dot( v.Bitangent, v.Normal ), 0.0f, 1e-5f );
        }
    }
}

TEST( RegionOperation, ExtrudeLiftsTheTopFaceByTheDistance )
{
    const FDynamicMesh3 before = TangentCube();
    auto region = RunRegionOperation( RegionOperation::Extrude, before, Groups( before, { kPlusZ } ), 20.0f );
    ASSERT_TRUE( region.IsSuccess() ) << region.GetError();
    const RegionOutcome    done = region.ExtractValue();
    const FGroupTopology   topology( done.Mesh.get(), true );
    const ElementSelection triangles =
         ConvertSelection( *done.Mesh, topology, done.Selection, ElementMode::Triangle );
    ASSERT_EQ( triangles.Size(), 2u );
    for ( int t : triangles.Ids() )
    {
        const FIndex3i tri = done.Mesh->GetTriangle( t );
        for ( int j = 0; j < 3; ++j )
            EXPECT_NEAR( done.Mesh->GetVertex( tri[j] ).Z, 70.0, 1e-9 );
    }
}

// Push/Pull moves the whole region along ONE vector, the region's area-weighted normal: for the +X and +Z faces
// (equal areas) that is (1,0,1)/sqrt(2) times the distance.
TEST( RegionOperation, PushPullMovesEveryRegionVertexByTheSameVector )
{
    const FDynamicMesh3 before = TangentCube();
    auto                region =
         RunRegionOperation( RegionOperation::PushPull, before, Groups( before, { kPlusX, kPlusZ } ), 20.0f );
    ASSERT_TRUE( region.IsSuccess() ) << region.GetError();
    const RegionOutcome    done = region.ExtractValue();
    const FGroupTopology   topology( done.Mesh.get(), true );
    const ElementSelection triangles =
         ConvertSelection( *done.Mesh, topology, done.Selection, ElementMode::Triangle );
    ASSERT_EQ( triangles.Size(), 4u );
    const double    step = 20.0 / std::sqrt( 2.0 );
    const FVector3d move( step, 0.0, step );
    for ( int t : triangles.Ids() )
    {
        const FIndex3i tri = done.Mesh->GetTriangle( t );
        for ( int j = 0; j < 3; ++j )
        {
            // Back where it came from: a corner of the +X or +Z face of the 100 cm cube.
            const FVector3d origin = done.Mesh->GetVertex( tri[j] ) - move;
            EXPECT_NEAR( std::abs( origin.X ), 50.0, 1e-6 );
            EXPECT_NEAR( std::abs( origin.Y ), 50.0, 1e-6 );
            EXPECT_NEAR( std::abs( origin.Z ), 50.0, 1e-6 );
            EXPECT_TRUE( std::abs( origin.X - 50.0 ) < 1e-6 || std::abs( origin.Z - 50.0 ) < 1e-6 )
                 << "vertex of triangle " << t << " came from neither face";
        }
    }
}

TEST( RegionOperation, InsetOfFiveFacesIsRefusedForTheInteriorVertices )
{
    const FDynamicMesh3 before = TangentCube();
    auto                region = RunRegionOperation( RegionOperation::Inset, before,
                                                     Groups( before, { kPlusX, kMinusX, kPlusY, kMinusY, kPlusZ } ), 20.0f );
    ASSERT_FALSE( region.IsSuccess() );
    EXPECT_NE( region.GetError().find( "interior" ), std::string::npos ) << region.GetError();
}

TEST( RegionOperation, ZeroAndWronglySignedDistancesAreRefusedByName )
{
    const FDynamicMesh3 before = TangentCube();
    const auto          top    = Groups( before, { kPlusZ } );
    auto                zero   = RunRegionOperation( RegionOperation::Extrude, before, top, 0.0f );
    ASSERT_FALSE( zero.IsSuccess() );
    EXPECT_NE( zero.GetError().find( "Extrude" ), std::string::npos ) << zero.GetError();
    auto negative = RunRegionOperation( RegionOperation::Inset, before, top, -5.0f );
    ASSERT_FALSE( negative.IsSuccess() );
    EXPECT_NE( negative.GetError().find( "use Outset" ), std::string::npos ) << negative.GetError();
    EXPECT_TRUE( RunRegionOperation( RegionOperation::PushPull, before, top, -5.0f ).IsSuccess() );
}

// Weld (P12): the cube imported WITHOUT position welding is twelve loose triangles - every edge, face diagonals
// included, is an open seam with a coincident partner. FMergeCoincidentMeshEdges with the UE Weld Edges tool's
// defaults (ZeroTolerance, split-attribute welding on merged edges, 0.1 degree normal/tangent and 0.01 UV
// thresholds) must close it into one watertight 8-vertex cube that still renders back with its hard normals and
// tangent frame.
TEST( RegionOperation, WeldClosesACubeCutAlongEverySeam )
{
    auto imported = DynamicMeshFromRenderMesh( HardCube( 50.0f ), WeldOptions{ -1.0f, 1e-5f } );
    ASSERT_TRUE( imported.IsSuccess() ) << imported.GetError();
    FDynamicMesh3 mesh = std::move( imported.ExtractValue().Mesh );
    ASSERT_EQ( mesh.VertexCount(), 36 ); // no corner is shared: 12 loose triangles
    int openBefore = 0;
    for ( int e : mesh.BoundaryEdgeIndicesItr() )
        openBefore += e >= 0 ? 1 : 0;
    ASSERT_EQ( openBefore, 36 );

    FMergeCoincidentMeshEdges merger( &mesh );
    merger.MergeVertexTolerance                        = FMathf::ZeroTolerance;
    merger.MergeSearchTolerance                        = 2 * merger.MergeVertexTolerance;
    merger.bWeldAttrsOnMergedEdges                     = true;
    merger.SplitAttributeWelder.UVDistSqrdThreshold    = 0.01f * 0.01f;
    merger.SplitAttributeWelder.NormalVecDotThreshold  = std::abs( 1.f - std::cos( 0.1f * 3.14159265f / 180.f ) );
    merger.SplitAttributeWelder.TangentVecDotThreshold = merger.SplitAttributeWelder.NormalVecDotThreshold;
    ASSERT_TRUE( merger.Apply() );
    EXPECT_EQ( merger.InitialNumBoundaryEdges, 36 );
    EXPECT_EQ( merger.FinalNumBoundaryEdges, 0 );
    EXPECT_EQ( mesh.VertexCount(), 8 );
    EXPECT_EQ( mesh.TriangleCount(), 12 );
    EXPECT_EQ( mesh.EdgeCount(), 18 );

    auto render = ToRenderMesh( mesh );
    ASSERT_TRUE( render.IsSuccess() ) << render.GetError();
    // hard edges survive: three faces meet at 90 degrees at each corner, so no normal element is welded
    EXPECT_EQ( render.GetValue().Vertices.size(), 24u );
    for ( const Vertex& v : render.GetValue().Vertices )
    {
        EXPECT_NEAR( glm::length( v.Tangent ), 1.0f, 1e-5f );
        EXPECT_NEAR( glm::dot( v.Tangent, v.Normal ), 0.0f, 1e-5f );
    }
}

// P12b: a cube with its top face deleted has one four-edge hole; FMeshBoundaryLoops finds it and FSimpleHoleFiller
// closes it with a fan that faces out, and the attributes UE's HoleFillOp sets make it render back.
TEST( RegionOperation, FillHoleClosesACubeWithItsTopFaceDeleted )
{
    FDynamicMesh3 mesh = TangentCube();
    mesh.RemoveTriangle( 2 * kPlusZ );
    mesh.RemoveTriangle( 2 * kPlusZ + 1 );
    ASSERT_FALSE( mesh.IsClosed() );

    FMeshBoundaryLoops loops( &mesh );
    ASSERT_EQ( loops.GetLoopCount(), 1 );
    EXPECT_EQ( loops.Spans.Num(), 0 );
    EXPECT_EQ( loops.Loops[0].GetEdgeCount(), 4 );
    EXPECT_TRUE( loops.Loops[0].IsBoundaryLoop( mesh ) );

    FSimpleHoleFiller filler( &mesh, loops.Loops[0] );
    ASSERT_TRUE( filler.Fill() ) << filler.FailureReason;
    EXPECT_EQ( filler.NewTriangles.Num(), 4 );
    EXPECT_TRUE( mesh.IsClosed() );
    int open = 0;
    for ( int e : mesh.BoundaryEdgeIndicesItr() )
        open += e >= 0 ? 1 : 0;
    EXPECT_EQ( open, 0 );
    for ( int t : filler.NewTriangles )
        EXPECT_NEAR( mesh.GetTriNormal( t ).Z, 1.0, 1e-9 ) << "fan triangle " << t << " faces into the cube";

    FDynamicMeshEditor editor( &mesh );
    editor.SetTriangleNormals( filler.NewTriangles, FVector3f( 0, 0, 1 ) );
    editor.SetTriangleUVsFromProjection( filler.NewTriangles, mesh.GetVertex( filler.NewVertex ),
                                         FVector3d( 0, 0, 1 ), 1.0f );
    FDynamicMeshAttributeSet* attributes = mesh.Attributes();
    FMeshTangentsd            tangents( &mesh );
    tangents.ComputeSeparatePerTriangleTangents( attributes->PrimaryNormals(), attributes->PrimaryUV() );
    ASSERT_TRUE( tangents.CopyToOverlays( mesh ) );

    auto render = ToRenderMesh( mesh );
    ASSERT_TRUE( render.IsSuccess() ) << render.GetError();
    for ( const Vertex& v : render.GetValue().Vertices )
    {
        EXPECT_NEAR( glm::length( v.Tangent ), 1.0f, 1e-5f );
        EXPECT_NEAR( glm::dot( v.Tangent, v.Normal ), 0.0f, 1e-5f );
    }
}

// The editor's Fill Hole: the whole HoleFillOp orchestration, including the plane normal it derives (not given).
TEST( RegionOperation, FillHolesDerivesTheCapNormalFromTheLoop )
{
    FDynamicMesh3 before = TangentCube();
    before.RemoveTriangle( 2 * kPlusZ );
    before.RemoveTriangle( 2 * kPlusZ + 1 );

    auto filled = FillHoles( before, ElementSelection( ElementMode::Triangle ) );
    ASSERT_TRUE( filled.IsSuccess() ) << filled.GetError();
    const FDynamicMesh3& mesh = *filled.GetValue().Mesh;
    EXPECT_TRUE( mesh.IsClosed() );
    EXPECT_EQ( filled.GetValue().Selection.Size(), 4u );
    auto render = ToRenderMesh( mesh );
    ASSERT_TRUE( render.IsSuccess() ) << render.GetError();
    int capVertices = 0;
    for ( const Vertex& v : render.GetValue().Vertices )
    {
        EXPECT_NEAR( glm::dot( v.Tangent, v.Normal ), 0.0f, 1e-5f );
        if ( v.Position.z > 49.0f && v.Normal.z > 0.99f )
            ++capVertices;
    }
    EXPECT_GE( capVertices, 5 ) << "the cap's normals must point out of the cube (+Z)";

    auto again = FillHoles( mesh, ElementSelection( ElementMode::Triangle ) );
    EXPECT_FALSE( again.IsSuccess() ) << "a closed mesh has no hole to fill";
}

TEST( RegionOperation, WeldEdgesClosesAnImportedHardCube )
{
    auto imported = DynamicMeshFromRenderMesh( HardCube( 50.0f ), WeldOptions{ -1.0f, 1e-5f } );
    ASSERT_TRUE( imported.IsSuccess() ) << imported.GetError();
    const FDynamicMesh3 before = std::move( imported.ExtractValue().Mesh );
    ASSERT_FALSE( before.IsClosed() );
    auto welded = WeldEdges( before, ElementMode::Edge );
    ASSERT_TRUE( welded.IsSuccess() ) << welded.GetError();
    EXPECT_TRUE( welded.GetValue().Mesh->IsClosed() );
    EXPECT_TRUE( ToRenderMesh( *welded.GetValue().Mesh ).IsSuccess() );
    EXPECT_FALSE( WeldEdges( *welded.GetValue().Mesh, ElementMode::Edge ).IsSuccess() );
}

// An open box (top face deleted) has four boundary edges and no coincident partner for any of them: the merge
// "succeeds" with nothing merged. A command without an effect must refuse by name rather than leave an empty undo
// step (P12d found the editor reporting +0/+0).
TEST( RegionOperation, WeldEdgesRefusesAnOpenBoxWithNoCoincidentPair )
{
    FDynamicMesh3 before = TangentCube();
    before.RemoveTriangle( 2 * kPlusZ );
    before.RemoveTriangle( 2 * kPlusZ + 1 );
    ASSERT_FALSE( before.IsClosed() );
    auto welded = WeldEdges( before, ElementMode::Edge );
    ASSERT_FALSE( welded.IsSuccess() ) << "nothing was welded, yet the operation succeeded";
    EXPECT_NE( welded.GetError().find( "no coincident edge pair" ), std::string::npos ) << welded.GetError();
    EXPECT_NE( welded.GetError().find( "the 4 boundary edges" ), std::string::npos ) << welded.GetError();
}

namespace
{
    // The Edge selection the editor's group-level pick makes: every mesh edge of the group edge between two faces.
    ElementSelection GroupEdgeBetween( const FDynamicMesh3& mesh, int faceA, int faceB )
    {
        const FGroupTopology topology( &mesh, true );
        ElementSelection     selection( ElementMode::Edge );
        for ( int e = 0; e < topology.Edges.Num(); ++e )
        {
            const FIndex2i g = topology.Edges[e].Groups;
            if ( ( g.A == faceA + 1 && g.B == faceB + 1 ) || ( g.A == faceB + 1 && g.B == faceA + 1 ) )
                for ( const int eid : topology.GetGroupEdgeEdges( e ) )
                    EXPECT_TRUE( selection.Add( mesh, eid ).IsSuccess() ) << "edge " << eid;
        }
        return selection;
    }

    size_t GroupCount( const FDynamicMesh3& mesh )
    {
        std::vector<int> groups;
        for ( const int t : mesh.TriangleIndicesItr() )
            if ( std::find( groups.begin(), groups.end(), mesh.GetTriangleGroup( t ) ) == groups.end() )
                groups.push_back( mesh.GetTriangleGroup( t ) );
        return groups.size();
    }
} // namespace

// The editor's Insert Edge Loop (P12i): the group edge between +X and +Z picked, the loop at 0.5 runs round the
// four faces it crosses, splits each into two new groups (6 -> 10), keeps the cube closed, selects the new loop
// edges and renders back with a tangent frame.
TEST( RegionOperation, InsertEdgeLoopSplitsTheCubeRingIntoTenGroups )
{
    const FDynamicMesh3 before = TangentCube();
    ASSERT_EQ( GroupCount( before ), 6u );
    auto loop = InsertEdgeLoop( before, GroupEdgeBetween( before, kPlusX, kPlusZ ), 0.5f );
    ASSERT_TRUE( loop.IsSuccess() ) << loop.GetError();
    const RegionOutcome done = loop.ExtractValue();
    EXPECT_EQ( GroupCount( *done.Mesh ), 10u );
    EXPECT_TRUE( done.Mesh->IsClosed() );
    EXPECT_EQ( GroupCount( before ), 6u ) << "the input mesh must not be touched";
    EXPECT_EQ( done.Selection.Mode(), ElementMode::Edge );
    EXPECT_GE( done.Selection.Ids().size(), 4u );
    for ( const int eid : done.Selection.Ids() )
    {
        const FIndex2i v = done.Mesh->GetEdgeV( eid );
        EXPECT_NEAR( done.Mesh->GetVertex( v.A ).Y, 0.0, 1e-6 );
        EXPECT_NEAR( done.Mesh->GetVertex( v.B ).Y, 0.0, 1e-6 );
    }
    auto render = ToRenderMesh( *done.Mesh );
    ASSERT_TRUE( render.IsSuccess() ) << render.GetError();
    for ( const Vertex& v : render.GetValue().Vertices )
        EXPECT_NEAR( glm::dot( v.Tangent, v.Normal ), 0.0f, 1e-5f );
}

// The position is measured from the group edge's first corner: 0.25 and 0.75 put the loop on opposite sides.
TEST( RegionOperation, InsertEdgeLoopPositionMovesTheLoopAlongTheEdge )
{
    const FDynamicMesh3        before = TangentCube();
    std::array<double, 2>      y{};
    const std::array<float, 2> at{ 0.25f, 0.75f };
    for ( size_t i = 0; i < at.size(); ++i )
    {
        auto loop = InsertEdgeLoop( before, GroupEdgeBetween( before, kPlusX, kPlusZ ), at[i] );
        ASSERT_TRUE( loop.IsSuccess() ) << loop.GetError();
        const RegionOutcome done = loop.ExtractValue();
        ASSERT_FALSE( done.Selection.Empty() );
        y[i] = done.Mesh->GetVertex( done.Mesh->GetEdgeV( done.Selection.Ids()[0] ).A ).Y;
    }
    EXPECT_NEAR( std::abs( y[0] ), 25.0, 1e-6 );
    EXPECT_NEAR( y[0], -y[1], 1e-6 );
}

TEST( RegionOperation, InsertEdgeLoopRefusalsNameTheCause )
{
    const FDynamicMesh3 cube    = TangentCube();
    auto                refused = [&]( const ElementSelection& selection, float position, const char* text )
    {
        auto r = InsertEdgeLoop( cube, selection, position );
        if ( r.IsSuccess() )
            return ::testing::AssertionFailure() << "succeeded, expected '" << text << "'";
        if ( r.GetError().find( text ) == std::string::npos )
            return ::testing::AssertionFailure() << r.GetError();
        return ::testing::AssertionSuccess();
    };
    const ElementSelection edge = GroupEdgeBetween( cube, kPlusX, kPlusZ );
    EXPECT_TRUE( refused( edge, 0.0f, "outside (0, 1)" ) );
    EXPECT_TRUE( refused( edge, 1.0f, "outside (0, 1)" ) );
    EXPECT_TRUE( refused( Groups( cube, { kPlusZ } ), 0.5f, "Edge mode" ) );
    EXPECT_TRUE( refused( ElementSelection( ElementMode::Edge ), 0.5f, "Edge mode" ) );
}
