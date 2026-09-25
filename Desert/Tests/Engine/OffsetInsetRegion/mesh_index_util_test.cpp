// MeshIndexUtil.hpp (ported from UE for FMeshBevel) on a welded cube with one polygroup per face: the one-ring
// splitters must cut a corner's fan into two disjoint sides that cover it, and the triangle-edge queries must
// return UE's edge order, which BuildTerminatorVertex relies on.
#include "Engine/Geometry/DynamicMeshRenderConversion.hpp"
#include "Engine/Geometry/UECore/DynamicMesh/MeshIndexUtil.hpp"

#include <gtest/gtest.h>

#include <glm/geometric.hpp>

#include <set>
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

    // region_operation_test.cpp's TangentCube: faces +X, -X, +Y, -Y, +Z, -Z, face f is polygroup f + 1.
    FDynamicMesh3 TangentCube()
    {
        const float     half = 50.0f;
        const glm::vec3 X( 1, 0, 0 );
        const glm::vec3 Y( 0, 1, 0 );
        const glm::vec3 Z( 0, 0, 1 );
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
        FDynamicMesh3 mesh = std::move( imported.ExtractValue().Mesh );
        mesh.EnableTriangleGroups();
        for ( const int t : mesh.TriangleIndicesItr() )
            mesh.SetTriangleGroup( t, 1 + t / 2 );
        return mesh;
    }

    std::set<int> Ring( const FDynamicMesh3& mesh, int v )
    {
        std::set<int> ring;
        for ( const int t : mesh.VtxTrianglesItr( v ) )
            ring.insert( t );
        return ring;
    }

    bool IsGroupEdge( const FDynamicMesh3& mesh, int e )
    {
        const FIndex2i t = mesh.GetEdgeT( e );
        return t.B >= 0 && mesh.GetTriangleGroup( t.A ) != mesh.GetTriangleGroup( t.B );
    }

    // Both sets non-empty, disjoint, and together exactly the vertex's triangle fan.
    void ExpectPartition( const FDynamicMesh3& mesh, int v, const TArray<int32>& s0, const TArray<int32>& s1 )
    {
        ASSERT_GT( s0.Num(), 0 );
        ASSERT_GT( s1.Num(), 0 );
        std::set<int> all;
        for ( int32 i = 0; i < s0.Num(); ++i )
            all.insert( s0[i] );
        for ( int32 i = 0; i < s1.Num(); ++i )
            EXPECT_TRUE( all.insert( s1[i] ).second ) << "triangle " << s1[i] << " is on both sides";
        EXPECT_EQ( all, Ring( mesh, v ) );
    }
} // namespace

TEST( MeshIndexUtil, VertexEdgesComeInTriangleOrder )
{
    const FDynamicMesh3 mesh = TangentCube();
    for ( const int t : mesh.TriangleIndicesItr() )
    {
        const FIndex3i tv = mesh.GetTriangle( t );
        for ( int j = 0; j < 3; ++j )
        {
            const FIndex2i e = FindVertexEdgesInTriangle( mesh, t, tv[j] );
            // A = edge (prev, v), B = edge (v, next): BuildTerminatorVertex reads them in this order.
            EXPECT_TRUE( mesh.GetEdgeV( e.A ).Contains( tv[( j + 2 ) % 3] ) );
            EXPECT_TRUE( mesh.GetEdgeV( e.B ).Contains( tv[( j + 1 ) % 3] ) );
            EXPECT_TRUE( mesh.GetEdgeV( e.A ).Contains( tv[j] ) && mesh.GetEdgeV( e.B ).Contains( tv[j] ) );
        }
    }
    const FIndex2i none = FindVertexEdgesInTriangle( mesh, 0, IndexConstants::InvalidID );
    EXPECT_EQ( none.A, IndexConstants::InvalidID );
}

TEST( MeshIndexUtil, SharedEdgeOfAdjacentTrianglesOnly )
{
    const FDynamicMesh3 mesh     = TangentCube();
    int                 adjacent = 0;
    int                 apart    = 0;
    for ( const int a : mesh.TriangleIndicesItr() )
        for ( const int b : mesh.TriangleIndicesItr() )
        {
            if ( a == b )
                continue;
            const int e = FindSharedEdgeInTriangles( mesh, a, b );
            if ( e == IndexConstants::InvalidID )
            {
                ++apart;
                continue;
            }
            ++adjacent;
            const FIndex2i et = mesh.GetEdgeT( e );
            EXPECT_TRUE( et.Contains( a ) && et.Contains( b ) );
        }
    // 12 triangles, 18 edges, each shared by an ordered pair twice.
    EXPECT_EQ( adjacent, 36 );
    EXPECT_EQ( apart, 12 * 11 - 36 );
}

TEST( MeshIndexUtil, InteriorSplitAtCubeCornerPartitionsTheFan )
{
    const FDynamicMesh3 mesh    = TangentCube();
    int                 corners = 0;
    for ( const int v : mesh.VertexIndicesItr() )
    {
        std::vector<int> groupEdges;
        for ( const int e : mesh.VtxEdgesItr( v ) )
            if ( IsGroupEdge( mesh, e ) )
                groupEdges.push_back( e );
        ASSERT_EQ( groupEdges.size(), 3u ) << "cube corner " << v;
        for ( int k = 0; k < 3; ++k )
        {
            TArray<int32> s0;
            TArray<int32> s1;
            ASSERT_TRUE( SplitInteriorVertexTrianglesIntoSubsets( &mesh, v, groupEdges[k],
                                                                  groupEdges[( k + 1 ) % 3], s0, s1 ) );
            ExpectPartition( mesh, v, s0, s1 );
            // Two of a corner's three group edges fence off exactly one face: one side is one polygroup, the other
            // two.
            auto singleGroup = [&]( const TArray<int32>& side )
            {
                for ( int32 i = 1; i < side.Num(); ++i )
                    if ( mesh.GetTriangleGroup( side[i] ) != mesh.GetTriangleGroup( side[0] ) )
                        return false;
                return true;
            };
            EXPECT_TRUE( singleGroup( s0 ) != singleGroup( s1 ) ) << "corner " << v << " split " << k;
        }
        ++corners;
    }
    EXPECT_EQ( corners, 8 );
}

TEST( MeshIndexUtil, BoundarySplitPartitionsTheOpenFan )
{
    FDynamicMesh3 mesh = TangentCube();
    const int     v    = mesh.GetTriangle( 0 ).A;
    ASSERT_EQ( mesh.RemoveTriangle( 0, false, false ), EMeshResult::Ok );
    ASSERT_TRUE( mesh.IsBoundaryVertex( v ) );
    int tested = 0;
    for ( const int e : mesh.VtxEdgesItr( v ) )
    {
        if ( mesh.IsBoundaryEdge( e ) )
            continue;
        TArray<int32> s0;
        TArray<int32> s1;
        ASSERT_TRUE( SplitBoundaryVertexTrianglesIntoSubsets( &mesh, v, e, s0, s1 ) );
        ExpectPartition( mesh, v, s0, s1 );
        ++tested;
    }
    EXPECT_GT( tested, 0 );
    // A boundary edge has one triangle: nothing to split.
    for ( const int e : mesh.VtxEdgesItr( v ) )
        if ( mesh.IsBoundaryEdge( e ) )
        {
            TArray<int32> s0;
            TArray<int32> s1;
            EXPECT_FALSE( SplitBoundaryVertexTrianglesIntoSubsets( &mesh, v, e, s0, s1 ) );
        }
}
