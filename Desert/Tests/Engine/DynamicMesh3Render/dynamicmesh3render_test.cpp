// MeshNormals (ported from UE) and DynamicMesh3 <-> render-mesh conversion.
//
// The render side is the one EditMesh already converts to, so the load-bearing assertion is the RELATION between
// the two cores: for the same render input, ToRenderMesh(DynamicMesh3) is byte-identical to
// ToRenderMesh(EditMesh). Round trips are asserted on the canonical form (the output of one conversion), because
// the weld is allowed to move a seam vertex onto the position of the first vertex it welds to.
#include <gtest/gtest.h>

#include <Engine/Geometry/DynamicMeshRenderConversion.hpp>
#include <Engine/Geometry/EditMeshConversion.hpp>
#include <Engine/Geometry/UECore/DynamicMesh/DynamicMeshAttributeSet.hpp>
#include <Engine/Geometry/UECore/DynamicMesh/MeshNormals.hpp>

#include <glm/geometric.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace Desert;
using namespace Desert::Geometry;

namespace
{
    constexpr float kPi = 3.14159265358979f;

    bool Valid( const DynamicMesh3& mesh )
    {
        return mesh.CheckValidity( DynamicMesh3::ValidityOptions(), ValidityCheckFailMode::ReturnOnly );
    }

    Vertex MakeVertex( const glm::vec3& p, const glm::vec3& n, const glm::vec3& t, const glm::vec2& uv )
    {
        Vertex v{};
        v.Position  = p;
        v.Normal    = n;
        v.Tangent   = t;
        v.Bitangent = glm::cross( n, t ); // exactly what the EditMesh layer rebuilds from handedness +1
        v.TexCoord  = uv;
        return v;
    }

    // Appends one submesh built from `vertices` / `indices` (submesh-local), in first-use order.
    void AppendSubmesh( RenderMeshData& render, const std::vector<Vertex>& vertices,
                        const std::vector<Index>& indices, int material )
    {
        Submesh s{};
        s.Name         = "MaterialID " + std::to_string( material );
        s.VertexOffset = static_cast<uint32_t>( render.Vertices.size() );
        s.VertexCount  = static_cast<uint32_t>( vertices.size() );
        s.IndexOffset  = static_cast<uint32_t>( render.Indices.size() * 3 );
        s.IndexCount   = static_cast<uint32_t>( indices.size() * 3 );
        s.Transform    = glm::mat4( 1.0f );
        render.Vertices.insert( render.Vertices.end(), vertices.begin(), vertices.end() );
        render.Indices.insert( render.Indices.end(), indices.begin(), indices.end() );
        render.Submeshes.push_back( s );
        render.SubmeshMaterialIds.push_back( material );
    }

    // A cube with one quad per face (24 render vertices), faces 0-2 material 0, faces 3-5 material 1.
    RenderMeshData HardCube( float half )
    {
        const glm::vec3 X( 1, 0, 0 ), Y( 0, 1, 0 ), Z( 0, 0, 1 );
        struct Face
        {
            glm::vec3 N{}, U{}, V{};
        };
        const Face faces[6] = { { X, Y, Z }, { -X, Z, Y }, { Y, Z, X }, { -Y, X, Z }, { Z, X, Y }, { -Z, Y, X } };
        RenderMeshData render;
        for ( int group = 0; group < 2; ++group )
        {
            std::vector<Vertex> vertices;
            std::vector<Index>  indices;
            for ( int f = group * 3; f < group * 3 + 3; ++f )
            {
                const Face&     face = faces[f];
                const glm::vec3 c    = face.N * half;
                const uint32_t  base = static_cast<uint32_t>( vertices.size() );
                vertices.push_back( MakeVertex( c - face.U * half - face.V * half, face.N, face.U, { 0, 0 } ) );
                vertices.push_back( MakeVertex( c + face.U * half - face.V * half, face.N, face.U, { 1, 0 } ) );
                vertices.push_back( MakeVertex( c + face.U * half + face.V * half, face.N, face.U, { 1, 1 } ) );
                vertices.push_back( MakeVertex( c - face.U * half + face.V * half, face.N, face.U, { 0, 1 } ) );
                indices.push_back( { base, base + 1, base + 2 } );
                indices.push_back( { base, base + 2, base + 3 } );
            }
            AppendSubmesh( render, vertices, indices, group );
        }
        return render;
    }

    // A UV sphere with a duplicated seam column and one pole corner per pole triangle, outward winding.
    RenderMeshData UVSphere( float radius, int rings, int segments )
    {
        std::vector<Vertex> vertices;
        const auto          at = [&]( int i, int j ) { return static_cast<uint32_t>( i * ( segments + 1 ) + j ); };
        for ( int i = 0; i <= rings; ++i )
            for ( int j = 0; j <= segments; ++j )
            {
                const float     theta = kPi * static_cast<float>( i ) / static_cast<float>( rings );
                const float     phi   = 2.0f * kPi * static_cast<float>( j ) / static_cast<float>( segments );
                const glm::vec3 n( std::sin( theta ) * std::cos( phi ), std::cos( theta ),
                                   std::sin( theta ) * std::sin( phi ) );
                const glm::vec3 t( -std::sin( phi ), 0.0f, std::cos( phi ) );
                vertices.push_back( MakeVertex( n * radius, n, t,
                                                { static_cast<float>( j ) / static_cast<float>( segments ),
                                                  static_cast<float>( i ) / static_cast<float>( rings ) } ) );
            }
        std::vector<Index> indices;
        const auto         push = [&]( uint32_t a, uint32_t b, uint32_t c )
        {
            const glm::vec3& pa = vertices[a].Position;
            const glm::vec3& pb = vertices[b].Position;
            const glm::vec3& pc = vertices[c].Position;
            if ( glm::dot( glm::cross( pb - pa, pc - pa ), pa + pb + pc ) < 0.0f )
                std::swap( b, c );
            indices.push_back( { a, b, c } );
        };
        for ( int i = 0; i < rings; ++i )
            for ( int j = 0; j < segments; ++j )
            {
                if ( i != 0 )
                    push( at( i, j ), at( i + 1, j ), at( i, j + 1 ) );
                if ( i != rings - 1 )
                    push( at( i, j + 1 ), at( i + 1, j ), at( i + 1, j + 1 ) );
            }
        RenderMeshData render;
        AppendSubmesh( render, vertices, indices, 0 );
        return render;
    }

    // A flat grid in XZ, left half material 0, right half material 2 (a gap in the IDs on purpose).
    RenderMeshData Grid( int cells )
    {
        RenderMeshData render;
        for ( int half = 0; half < 2; ++half )
        {
            std::vector<Vertex> vertices;
            std::vector<Index>  indices;
            const int           x0 = half * cells / 2, x1 = ( half + 1 ) * cells / 2;
            const int           w = x1 - x0 + 1;
            for ( int z = 0; z <= cells; ++z )
                for ( int x = x0; x <= x1; ++x )
                    vertices.push_back(
                         MakeVertex( { 10.0f * x, 0.0f, 10.0f * z }, { 0, 1, 0 }, { 1, 0, 0 },
                                     { static_cast<float>( x ) / cells, static_cast<float>( z ) / cells } ) );
            for ( int z = 0; z < cells; ++z )
                for ( int x = 0; x < w - 1; ++x )
                {
                    const uint32_t a = z * w + x, b = a + 1, c = a + w, d = c + 1;
                    indices.push_back( { a, c, b } );
                    indices.push_back( { b, c, d } );
                }
            AppendSubmesh( render, vertices, indices, half * 2 );
        }
        return render;
    }

    // Byte equality of everything the renderer reads, plus the source maps.
    void ExpectSameRender( const RenderMeshData& a, const RenderMeshData& b )
    {
        ASSERT_EQ( a.Vertices.size(), b.Vertices.size() );
        ASSERT_EQ( a.Indices.size(), b.Indices.size() );
        ASSERT_EQ( a.Submeshes.size(), b.Submeshes.size() );
        EXPECT_EQ( 0, std::memcmp( a.Vertices.data(), b.Vertices.data(), a.Vertices.size() * sizeof( Vertex ) ) );
        EXPECT_EQ( 0, std::memcmp( a.Indices.data(), b.Indices.data(), a.Indices.size() * sizeof( Index ) ) );
        for ( size_t i = 0; i < a.Submeshes.size(); ++i )
        {
            EXPECT_EQ( a.Submeshes[i].Name, b.Submeshes[i].Name );
            EXPECT_EQ( a.Submeshes[i].VertexOffset, b.Submeshes[i].VertexOffset );
            EXPECT_EQ( a.Submeshes[i].VertexCount, b.Submeshes[i].VertexCount );
            EXPECT_EQ( a.Submeshes[i].IndexOffset, b.Submeshes[i].IndexOffset );
            EXPECT_EQ( a.Submeshes[i].IndexCount, b.Submeshes[i].IndexCount );
            EXPECT_EQ( a.Submeshes[i].BoundingBox.Min, b.Submeshes[i].BoundingBox.Min );
            EXPECT_EQ( a.Submeshes[i].BoundingBox.Max, b.Submeshes[i].BoundingBox.Max );
        }
        EXPECT_EQ( a.SubmeshMaterialIds, b.SubmeshMaterialIds );
        EXPECT_EQ( a.SourceVertices, b.SourceVertices );
        EXPECT_EQ( a.SourceTriangles, b.SourceTriangles );
    }

    DynamicMesh3 Import( const RenderMeshData& render )
    {
        auto imported = DynamicMeshFromRenderMesh( render );
        EXPECT_TRUE( imported.IsSuccess() );
        return std::move( imported.GetValue().Mesh );
    }

    RenderMeshData Export( const DynamicMesh3& mesh )
    {
        auto render = ToRenderMesh( mesh );
        EXPECT_TRUE( render.IsSuccess() ) << ( render.IsSuccess() ? "" : render.GetError() );
        return render.IsSuccess() ? render.GetValue() : RenderMeshData{};
    }

    // Reaches the protected element-triangle table of an overlay, so a test can corrupt it the way a missed
    // attribute update in an edit operator would.
    struct OverlayAccess : DynamicMeshNormalOverlay
    {
        static DynamicVector<int>& Triangles( DynamicMeshNormalOverlay& overlay )
        {
            return overlay.*( &OverlayAccess::m_ElementTriangles );
        }
    };
} // namespace

TEST( DynamicMesh3Render, HardCubeFromFaceGroupsIs24VerticesWithFaceNormals )
{
    DynamicMesh3 mesh = Import( HardCube( 50.0f ) );
    ASSERT_EQ( mesh.VertexCount(), 8 );
    ASSERT_EQ( mesh.TriangleCount(), 12 );
    DynamicMeshAttributeSet& attributes = *mesh.Attributes();
    attributes.SetNumUVLayers( 0 );
    attributes.DisableTangents();
    DynamicMeshNormalOverlay* normals = attributes.PrimaryNormals();

    // Per-vertex first: one element per vertex, so the only split left is the material one (every corner is
    // used by both submeshes, 2 x 8).
    MeshNormals::InitializeOverlayToPerVertexNormals( normals, false );
    ASSERT_TRUE( Valid( mesh ) );
    EXPECT_EQ( Export( mesh ).Vertices.size(), 16u );

    // Then the hard edges from the polygroups, the UE way.
    mesh.EnableTriangleGroups();
    for ( const int t : mesh.TriangleIndicesItr() )
        mesh.SetTriangleGroup( t, t / 2 + 1 );
    MeshNormals::InitializeOverlayTopologyFromFaceGroups( &mesh, normals );
    ASSERT_TRUE( MeshNormals::QuickRecomputeOverlayNormals( mesh ) );
    ASSERT_TRUE( Valid( mesh ) );

    const RenderMeshData render = Export( mesh );
    ASSERT_EQ( render.Vertices.size(), 24u );
    for ( size_t k = 0; k < render.Indices.size(); ++k )
    {
        const glm::dvec3 face   = mesh.GetTriNormal( render.SourceTriangles[k] );
        const Index&    index  = render.Indices[k];
        const uint32_t  offset = render.Submeshes[k < 6 ? 0 : 1].VertexOffset;
        for ( const uint32_t local : { index.V1, index.V2, index.V3 } )
        {
            const glm::vec3& n = render.Vertices[offset + local].Normal;
            EXPECT_NEAR( n.x, face.x, 1e-6 );
            EXPECT_NEAR( n.y, face.y, 1e-6 );
            EXPECT_NEAR( n.z, face.z, 1e-6 );
        }
    }
}

TEST( DynamicMesh3Render, SphereSmoothNormalsFollowTheRadius )
{
    DynamicMesh3 mesh = Import( UVSphere( 100.0f, 12, 24 ) );
    ASSERT_TRUE( Valid( mesh ) );
    DynamicMeshNormalOverlay* normals = mesh.Attributes()->PrimaryNormals();
    // Wipe what the import carried, so the values come from MeshNormals and not from the render input.
    MeshNormals::InitializeOverlayToPerVertexNormals( normals, false );
    ASSERT_TRUE( Valid( mesh ) );
    EXPECT_EQ( normals->ElementCount(), mesh.VertexCount() );

    const RenderMeshData render = Export( mesh );
    for ( const Vertex& v : render.Vertices )
    {
        EXPECT_NEAR( glm::length( v.Normal ), 1.0f, 1e-5f );
        EXPECT_GT( glm::dot( v.Normal, glm::normalize( v.Position ) ), 0.995f );
    }

    MeshNormals vertexNormals( &mesh );
    vertexNormals.ComputeVertexNormals();
    for ( const int vid : mesh.VertexIndicesItr() )
        EXPECT_GT( glm::dot( vertexNormals[vid], Normalized( mesh.GetVertex( vid ) ) ), 0.995 );

    MeshNormals triangleNormals( &mesh );
    triangleNormals.ComputeTriangleNormals();
    for ( const int tid : mesh.TriangleIndicesItr() )
        EXPECT_GT( glm::dot( triangleNormals[tid], Normalized( mesh.GetTriCentroid( tid ) ) ), 0.95 );
}

TEST( DynamicMesh3Render, AngleWeightingIsSymmetricAtCubeCornersAreaWeightingIsNot )
{
    // Each cube corner sees 90 degrees of every face whatever the triangulation, so angle weighting gives the
    // diagonal; the triangulation gives one face two triangles there and area weighting leans towards it.
    const DynamicMesh3  mesh      = Import( HardCube( 50.0f ) );
    bool                areaLeans = false;
    for ( const int vid : mesh.VertexIndicesItr() )
    {
        const glm::dvec3 angle = MeshNormals::ComputeVertexNormal( mesh, vid, false, true );
        const glm::dvec3 area  = MeshNormals::ComputeVertexNormal( mesh, vid, true, false );
        EXPECT_NEAR( std::abs( angle.x ), 1.0 / std::sqrt( 3.0 ), 1e-9 );
        EXPECT_NEAR( std::abs( angle.y ), 1.0 / std::sqrt( 3.0 ), 1e-9 );
        EXPECT_NEAR( std::abs( angle.z ), 1.0 / std::sqrt( 3.0 ), 1e-9 );
        areaLeans |= std::abs( std::abs( area.x ) - 1.0 / std::sqrt( 3.0 ) ) > 1e-3;
    }
    EXPECT_TRUE( areaLeans );
}

TEST( DynamicMesh3Render, RoundTripIsByteStableForThreeShapes )
{
    for ( const RenderMeshData& input : { HardCube( 50.0f ), UVSphere( 100.0f, 10, 20 ), Grid( 6 ) } )
    {
        const RenderMeshData first = Export( Import( input ) );
        const DynamicMesh3   again = Import( first );
        ASSERT_TRUE( Valid( again ) );
        ExpectSameRender( first, Export( again ) );
        EXPECT_EQ( first.Indices.size(), input.Indices.size() );
    }
    // The cube is built in first-use order with no seam that welds onto another position, so it round trips byte
    // for byte from the input itself (the grid's rows are not in first-use order, the sphere seam welds).
    for ( const RenderMeshData& input : { HardCube( 50.0f ) } )
    {
        const RenderMeshData out = Export( Import( input ) );
        ASSERT_EQ( out.Vertices.size(), input.Vertices.size() );
        EXPECT_EQ( 0, std::memcmp( out.Vertices.data(), input.Vertices.data(),
                                   out.Vertices.size() * sizeof( Vertex ) ) );
        EXPECT_EQ( 0,
                   std::memcmp( out.Indices.data(), input.Indices.data(), out.Indices.size() * sizeof( Index ) ) );
        EXPECT_EQ( out.SubmeshMaterialIds, input.SubmeshMaterialIds );
    }
}

TEST( DynamicMesh3Render, WeldSharesCornersAcrossSubmeshesAndTheSphereSeam )
{
    EXPECT_EQ( Import( HardCube( 50.0f ) ).VertexCount(), 8 );
    // 10 rings of 20: two poles plus 9 inner rings of 20 unique positions once the seam column welds.
    EXPECT_EQ( Import( UVSphere( 100.0f, 10, 20 ) ).VertexCount(), 2 + 9 * 20 );
    EXPECT_EQ( Import( Grid( 6 ) ).VertexCount(), 7 * 7 );
}

TEST( DynamicMesh3Render, NewCoreMatchesEditMeshToRenderMesh )
{
    for ( const RenderMeshData& input : { HardCube( 50.0f ), UVSphere( 100.0f, 10, 20 ), Grid( 6 ) } )
    {
        auto oldMesh = FromRenderMesh( input );
        ASSERT_TRUE( oldMesh.IsSuccess() );
        auto oldRender = ToRenderMesh( oldMesh.GetValue().Mesh );
        ASSERT_TRUE( oldRender.IsSuccess() );
        RenderMeshData expected = oldRender.GetValue();
        RenderMeshData actual   = Export( Import( input ) );
        ASSERT_EQ( expected.Vertices.size(), actual.Vertices.size() );
        // The one field the cores store differently: EditMesh keeps a handedness sign and rebuilds the bitangent
        // as cross(N, T) from the welded normal and tangent elements, DynamicMesh3 keeps the bitangent overlay
        // itself. Where the first-seen normal and tangent at a welded vertex came from different render vertices
        // the two differ by float rounding; everything else must match bit for bit.
        int bitangentDiffers = 0;
        for ( size_t i = 0; i < expected.Vertices.size(); ++i )
        {
            const glm::vec3 a = expected.Vertices[i].Bitangent, b = actual.Vertices[i].Bitangent;
            bitangentDiffers += std::memcmp( &a, &b, sizeof( a ) ) != 0;
            EXPECT_LT( glm::length( a - b ), 1e-6f );
            expected.Vertices[i].Bitangent = actual.Vertices[i].Bitangent = glm::vec3( 0.0f );
        }
        std::printf( "bitangents differing by rounding: %d of %zu\n", bitangentDiffers, expected.Vertices.size() );
        ExpectSameRender( expected, actual );
    }
}

TEST( DynamicMesh3Render, CorruptedOverlayIsCaughtByCheckValidity )
{
    // Negative control for the P4 validity checks the round trips lean on: point triangle 0's first corner at an
    // element whose parent is another vertex.
    DynamicMesh3 mesh = Import( HardCube( 50.0f ) );
    ASSERT_TRUE( Valid( mesh ) );
    DynamicMeshNormalOverlay&  normals = *mesh.Attributes()->PrimaryNormals();
    const Index3i              tri     = normals.GetTriangle( 0 );
    ASSERT_NE( normals.GetParentVertex( tri.A ), normals.GetParentVertex( tri.B ) );
    OverlayAccess::Triangles( normals )[0] = tri.B;
    EXPECT_FALSE( normals.CheckValidity( true, ValidityCheckFailMode::ReturnOnly ) );
    EXPECT_FALSE( Valid( mesh ) );
}

TEST( DynamicMesh3Render, UnsetOverlayTriangleIsRefusedByName )
{
    DynamicMesh3 mesh = Import( Grid( 2 ) );
    mesh.Attributes()->PrimaryNormals()->UnsetTriangle( 3 );
    const auto render = ToRenderMesh( mesh );
    ASSERT_FALSE( render.IsSuccess() );
    EXPECT_NE( render.GetError().find( "triangle 3" ), std::string::npos );
}
int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
