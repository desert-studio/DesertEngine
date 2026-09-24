// RunRegionOperation (MeshRegionOperation.hpp) on the input the editor gives it: a mesh imported from render data
// WITH a tangent space (as Create Shape's meshes are), one polygroup per face, selected in PolyGroup mode. The
// result must convert back to render data (ToRenderMesh refuses a triangle unset in the tangent overlay).
#include "Engine/Geometry/DynamicMeshRenderConversion.hpp"
#include "Engine/Geometry/MeshRegionOperation.hpp"
#include "Engine/Geometry/UECore/DynamicMesh/DynamicMeshAttributeSet.hpp"
#include "Engine/Geometry/UECore/DynamicMesh/GroupTopology.hpp"

#include <gtest/gtest.h>

#include <glm/geometric.hpp>

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
