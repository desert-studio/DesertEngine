#include <gtest/gtest.h>

#include "EditMeshTestSupport.hpp"

#include <Engine/Geometry/EditMeshConversion.hpp>
#include <Engine/Geometry/EditMeshNormals.hpp>
#include <Engine/Geometry/EditMeshPolyGroups.hpp>
#include <Engine/Geometry/ShapeGenerators.hpp>

#include <glm/geometric.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <set>
#include <vector>

using namespace Desert::Geometry;
using EditMeshTest::MakeGrid;
using EditMeshTest::Tri;
using EditMeshTest::Valid;

// Attribute layers of EditMesh: overlays with seams, polygroups, material IDs, the render conversion
// and Generate PolyGroups. The per-operation fuzz with layers lives in editmesh_test.cpp next to the M2 fuzz
// it extends; this file pins each rule on a case small enough to reason about by hand.

namespace
{
    // An axis-aligned cube of 8 shared corners and 12 triangles, every face wound outward, no layers.
    EditMesh MakeBareCube( float size = 100.0f )
    {
        EditMesh mesh;
        for ( int i = 0; i < 8; ++i )
            mesh.AppendVertex( { ( i & 1 ) ? size : 0.0f, ( i & 2 ) ? size : 0.0f, ( i & 4 ) ? size : 0.0f } );
        // Quads (a, b, c, d) counter-clockwise seen from outside.
        const int quads[6][4] = { { 0, 2, 3, 1 }, { 4, 5, 7, 6 }, { 0, 1, 5, 4 },
                                  { 2, 6, 7, 3 }, { 0, 4, 6, 2 }, { 1, 3, 7, 5 } };
        for ( const auto& q : quads )
        {
            Tri( mesh, q[0], q[1], q[2] );
            Tri( mesh, q[0], q[2], q[3] );
        }
        return mesh;
    }

    RenderMeshData FromShape( const ShapeMesh& shape )
    {
        RenderMeshData render;
        render.Vertices = shape.Vertices;
        render.Indices  = shape.Indices;
        return render;
    }

    ImportedEditMesh Import( const RenderMeshData& render )
    {
        auto result = FromRenderMesh( render );
        EXPECT_TRUE( result.IsSuccess() ) << ( result.IsSuccess() ? "" : result.GetError() );
        return result.IsSuccess() ? result.ExtractValue() : ImportedEditMesh{};
    }

    RenderMeshData Render( const EditMesh& mesh, int uvLayer = 0 )
    {
        auto result = ToRenderMesh( mesh, uvLayer );
        EXPECT_TRUE( result.IsSuccess() ) << ( result.IsSuccess() ? "" : result.GetError() );
        return result.IsSuccess() ? result.ExtractValue() : RenderMeshData{};
    }

    // A render mesh as the multiset of its triangles, each the exact bytes of its three vertices (rotated to
    // a canonical start, winding kept) plus its material: equal sets = the same surface, attributes and all,
    // whatever order the vertices and triangles were emitted in.
    using TriangleKey = std::array<uint8_t, 3 * sizeof( Desert::Vertex ) + sizeof( int )>;
    std::multiset<TriangleKey> Triangles( const RenderMeshData& render )
    {
        std::multiset<TriangleKey> out;
        // No submeshes = one over everything (a ShapeMesh), the same rule FromRenderMesh applies.
        std::vector<Desert::Submesh> submeshes = render.Submeshes;
        if ( submeshes.empty() )
        {
            submeshes.emplace_back();
            submeshes[0].IndexCount = static_cast<uint32_t>( render.Indices.size() * 3 );
        }
        for ( size_t s = 0; s < submeshes.size(); ++s )
        {
            const auto& sub = submeshes[s];
            for ( uint32_t k = 0; k < sub.IndexCount / 3; ++k )
            {
                const auto& ix = render.Indices[sub.IndexOffset / 3 + k];
                std::array<std::array<uint8_t, sizeof( Desert::Vertex )>, 3> v{};
                const uint32_t                                               local[3] = { ix.V1, ix.V2, ix.V3 };
                for ( int j = 0; j < 3; ++j )
                    std::memcpy( v[j].data(), &render.Vertices[sub.VertexOffset + local[j]],
                                 sizeof( Desert::Vertex ) );
                const int   start = static_cast<int>( std::min_element( v.begin(), v.end() ) - v.begin() );
                TriangleKey key{};
                for ( int j = 0; j < 3; ++j )
                    std::memcpy( key.data() + j * sizeof( Desert::Vertex ), v[( start + j ) % 3].data(),
                                 sizeof( Desert::Vertex ) );
                const int material =
                     render.SubmeshMaterialIds.empty() ? static_cast<int>( s ) : render.SubmeshMaterialIds[s];
                std::memcpy( key.data() + 3 * sizeof( Desert::Vertex ), &material, sizeof( int ) );
                out.insert( key );
            }
        }
        return out;
    }

    // A one-quad mesh (0,1,2) + (0,2,3) whose shared edge is the diagonal 0-2, with a UV layer.
    struct Quad
    {
        EditMesh Mesh;
        int      T0 = InvalidId, T1 = InvalidId, Diagonal = InvalidId;
    };
    Quad MakeQuad()
    {
        Quad q;
        for ( const glm::vec3 p :
              { glm::vec3( 0, 0, 0 ), glm::vec3( 100, 0, 0 ), glm::vec3( 100, 100, 0 ), glm::vec3( 0, 100, 0 ) } )
            q.Mesh.AppendVertex( p );
        q.T0       = Tri( q.Mesh, 0, 1, 2 );
        q.T1       = Tri( q.Mesh, 0, 2, 3 );
        q.Diagonal = q.Mesh.FindEdge( 0, 2 );
        EXPECT_TRUE( q.Mesh.Attributes().SetUVLayerCount( 1 ) );
        return q;
    }

    // UVs = position / 100 on each corner, shared across the diagonal (seamed = false), or the second
    // triangle offset by +5 in u so the diagonal is a seam at both ends.
    void SetQuadUVs( Quad& q, bool seamed )
    {
        UVOverlay&         uv = *q.Mesh.Attributes().UV( 0 );
        std::array<int, 4> shared{};
        // Vertex 3 is only on T1; appending an element for it that T1 then does not use would leave an orphan.
        for ( int v = 0; v < ( seamed ? 3 : 4 ); ++v )
            shared[v] = uv.AppendElement( glm::vec2( q.Mesh.GetPosition( v ) ) / 100.0f );
        ASSERT_EQ( uv.SetTriangle( q.Mesh, q.T0, { shared[0], shared[1], shared[2] } ), EditResult::Ok );
        if ( !seamed )
        {
            ASSERT_EQ( uv.SetTriangle( q.Mesh, q.T1, { shared[0], shared[2], shared[3] } ), EditResult::Ok );
            return;
        }
        std::array<int, 3> own{};
        const int          corners[3] = { 0, 2, 3 };
        for ( int j = 0; j < 3; ++j )
            own[j] =
                 uv.AppendElement( glm::vec2( q.Mesh.GetPosition( corners[j] ) ) / 100.0f + glm::vec2( 5, 0 ) );
        ASSERT_EQ( uv.SetTriangle( q.Mesh, q.T1, own ), EditResult::Ok );
    }

    // 2x2 grid (vertex id = y * 3 + x) with the lower-right cell cut off in UV: its corners 1 and 4 get
    // their own elements, 2 and 5 do not.
    EditMesh MakeLowerRightCut()
    {
        EditMesh   mesh = MakeGrid( 2 );
        const bool ok   = mesh.Attributes().SetUVLayerCount( 1 );
        EXPECT_TRUE( ok );
        UVOverlay& uv = *mesh.Attributes().UV( 0 );
        // Right side of the cut: triangles whose centroid is right of x = 100 AND below y = 100.
        std::vector<std::array<int, 2>> element( static_cast<size_t>( mesh.MaxVertexId() ),
                                                 { InvalidId, InvalidId } );
        for ( const int t : mesh.TriangleIds() )
        {
            const auto& c = mesh.GetTriangle( t );
            glm::vec3   centroid( 0.0f );
            for ( const int v : c )
                centroid += mesh.GetPosition( v ) / 3.0f;
            const bool         right = centroid.x > 100.0f && centroid.y < 100.0f;
            std::array<int, 3> corners{};
            for ( int j = 0; j < 3; ++j )
            {
                // Only the cut's own vertices (1 and 4) have a separate right-side element.
                const int side = right && ( c[j] == 1 || c[j] == 4 ) ? 1 : 0;
                int&      e    = element[c[j]][side];
                if ( e == InvalidId )
                    e = uv.AppendElement( glm::vec2( mesh.GetPosition( c[j] ) ) / 100.0f +
                                          glm::vec2( side == 1 ? 5.0f : 0.0f, 0.0f ) );
                corners[j] = e;
            }
            EXPECT_EQ( uv.SetTriangle( mesh, t, corners ), EditResult::Ok );
        }
        return mesh;
    }

} // namespace

// ── the brief's acceptance case ──────────────────────────────────────────────────────────────────────

TEST( EditMeshAttributes, CubeHasSixPolyGroupsAndTwentyFourRenderVertices )
{
    EditMesh cube = MakeBareCube();
    ASSERT_EQ( cube.VertexCount(), 8 );
    EXPECT_EQ( GeneratePolyGroupsByAngle( cube, 30.0f ), 6 );
    ComputeNormalsByPolyGroup( cube );
    ASSERT_TRUE( Valid( cube ) );
    EXPECT_EQ( cube.Attributes().Normals()->ElementCount(), 24 ); // 8 corners x 3 faces

    const RenderMeshData render = Render( cube );
    EXPECT_EQ( render.Vertices.size(), 24u );
    EXPECT_EQ( render.Indices.size(), 12u );
    ASSERT_EQ( render.Submeshes.size(), 1u );
    for ( const auto& v : render.Vertices )
    {
        // Hard edges: every render normal is exactly one of the six axes.
        const float largest =
             std::max( { std::abs( v.Normal.x ), std::abs( v.Normal.y ), std::abs( v.Normal.z ) } );
        EXPECT_NEAR( largest, 1.0f, 1e-6f );
        EXPECT_NEAR( glm::length( v.Normal ), 1.0f, 1e-6f );
    }

    // Negative control: one group means one smooth element per corner, and the render side has no seam
    // to cut - 8 vertices, not 24. The count comes from the seams, not from the triangle count.
    EXPECT_EQ( GeneratePolyGroupsByAngle( cube, 100.0f ), 1 );
    ComputeNormalsByPolyGroup( cube );
    ASSERT_TRUE( Valid( cube ) );
    EXPECT_EQ( Render( cube ).Vertices.size(), 8u );
}

TEST( EditMeshAttributes, EngineBoxImportsAsEightCornersWithSeamedLayers )
{
    const ShapeMesh        box    = MakeBox( { 200.0f, 100.0f, 50.0f } );
    const RenderMeshData   source = FromShape( box );
    const ImportedEditMesh import = Import( source );
    const EditMesh&        mesh   = import.Mesh;
    ASSERT_TRUE( Valid( mesh ) );
    EXPECT_EQ( mesh.VertexCount(), 8 );
    EXPECT_EQ( mesh.TriangleCount(), 12 );
    EXPECT_EQ( mesh.EdgeCount(), 18 );
    EXPECT_EQ( import.DroppedDegenerate + import.DroppedDuplicate + import.DetachedTriangles, 0 );
    EXPECT_EQ( mesh.Attributes().Normals()->ElementCount(), 24 );

    EditMesh grouped = mesh;
    EXPECT_EQ( GeneratePolyGroupsByAngle( grouped, 30.0f ), 6 );
    EXPECT_EQ( GeneratePolyGroupsCoplanar( grouped, 1.0f, 0.01f ), 6 );
    const auto islands = GeneratePolyGroupsByUVIslands( grouped, 0 );
    ASSERT_TRUE( islands.IsSuccess() );
    EXPECT_EQ( islands.GetValue(), 6 );

    const RenderMeshData back = Render( mesh );
    EXPECT_EQ( back.Vertices.size(), 24u );
    EXPECT_TRUE( Triangles( back ) == Triangles( source ) ) << "render -> EditMesh -> render changed the surface";
}

// ── round trips ──────────────────────────────────────────────────────────────────────────────────────

TEST( EditMeshAttributes, EditMeshRenderEditMeshKeepsTopologyAndSeams )
{
    // A sphere: smooth normals (shared across every edge), one UV seam meridian whose column of render
    // vertices is duplicated, and two poles whose ring of render vertices (half of them at x = -0.0) must
    // weld onto ONE corner - so the weld and the seams are both exercised.
    const RenderMeshData   source = FromShape( MakeSphere( 100.0f, 12, 8 ) );
    const ImportedEditMesh first  = Import( source );
    ASSERT_TRUE( Valid( first.Mesh ) );
    EXPECT_EQ( first.DroppedDegenerate + first.DroppedDuplicate + first.DetachedTriangles, 0 );
    EXPECT_EQ( first.Mesh.VertexCount(), 2 + 7 * 12 ) << "two poles and seven rings of twelve";
    EXPECT_EQ( first.Mesh.VertexCount() - first.Mesh.EdgeCount() + first.Mesh.TriangleCount(), 2 )
         << "a welded sphere is closed";

    const RenderMeshData   rendered = Render( first.Mesh );
    const ImportedEditMesh second   = Import( rendered );
    ASSERT_TRUE( Valid( second.Mesh ) );
    EXPECT_EQ( second.DroppedDegenerate + second.DroppedDuplicate + second.DetachedTriangles, 0 );
    EXPECT_EQ( second.Mesh.VertexCount(), first.Mesh.VertexCount() );
    EXPECT_EQ( second.Mesh.TriangleCount(), first.Mesh.TriangleCount() );
    EXPECT_EQ( second.Mesh.EdgeCount(), first.Mesh.EdgeCount() );
    const auto& a = first.Mesh.Attributes();
    const auto& b = second.Mesh.Attributes();
    EXPECT_EQ( b.Normals()->ElementCount(), a.Normals()->ElementCount() );
    EXPECT_EQ( b.Tangents()->ElementCount(), a.Tangents()->ElementCount() );
    EXPECT_EQ( b.UV( 0 )->ElementCount(), a.UV( 0 )->ElementCount() );
    EXPECT_GT( a.UV( 0 )->ElementCount(), first.Mesh.VertexCount() ) << "the UV meridian must be a seam";
    EXPECT_TRUE( Triangles( Render( second.Mesh ) ) == Triangles( rendered ) );
}

TEST( EditMeshAttributes, MaterialIdsBecomeSubmeshesAndComeBack )
{
    ImportedEditMesh import = Import( FromShape( MakeBox( glm::vec3( 100.0f ) ) ) );
    EditMesh&        mesh   = import.Mesh;
    ASSERT_EQ( GeneratePolyGroupsByAngle( mesh, 30.0f ), 6 );
    for ( const int t : mesh.TriangleIds() )
        mesh.Attributes().SetMaterialId( t, mesh.Attributes().GetPolyGroup( t ) % 2 == 0 ? 7 : 3 );

    const RenderMeshData render = Render( mesh );
    ASSERT_EQ( render.Submeshes.size(), 2u );
    EXPECT_EQ( render.SubmeshMaterialIds, ( std::vector<int>{ 3, 7 } ) ); // ascending MaterialID
    uint32_t vertexEnd = 0, indexEnd = 0;
    for ( const auto& sub : render.Submeshes )
    {
        EXPECT_EQ( sub.VertexOffset, vertexEnd );
        EXPECT_EQ( sub.IndexOffset, indexEnd );
        EXPECT_EQ( sub.IndexCount, 18u );  // three faces each
        EXPECT_EQ( sub.VertexCount, 12u ); // four corners per hard face
        for ( uint32_t k = 0; k < sub.IndexCount / 3; ++k )
        {
            const auto& ix = render.Indices[sub.IndexOffset / 3 + k];
            EXPECT_LT( std::max( { ix.V1, ix.V2, ix.V3 } ), sub.VertexCount ) << "indices are submesh-local";
        }
        vertexEnd += sub.VertexCount;
        indexEnd += sub.IndexCount;
    }
    ASSERT_EQ( render.SourceTriangles.size(), render.Indices.size() );
    for ( size_t k = 0; k < render.Indices.size(); ++k )
        EXPECT_EQ( mesh.Attributes().GetMaterialId( render.SourceTriangles[k] ), k < 6 ? 3 : 7 );

    // Back again: every triangle, found by its corner positions, carries the material it left with.
    const auto positions = []( const EditMesh& m, int t )
    {
        std::array<std::array<float, 3>, 3> key{};
        for ( int j = 0; j < 3; ++j )
        {
            const glm::vec3 p = m.GetPosition( m.GetTriangle( t )[j] );
            key[j]            = { p.x, p.y, p.z };
        }
        std::sort( key.begin(), key.end() );
        return key;
    };
    const ImportedEditMesh back = Import( render );
    ASSERT_EQ( back.Mesh.TriangleCount(), mesh.TriangleCount() );
    for ( const int t : back.Mesh.TriangleIds() )
    {
        int original = InvalidId;
        for ( const int u : mesh.TriangleIds() )
            if ( positions( mesh, u ) == positions( back.Mesh, t ) )
                original = mesh.Attributes().GetMaterialId( u );
        EXPECT_EQ( back.Mesh.Attributes().GetMaterialId( t ), original ) << "triangle " << t;
    }
    EXPECT_TRUE( Triangles( Render( back.Mesh ) ) == Triangles( render ) );
}

TEST( EditMeshAttributes, ConversionRefusalsNameTheCause )
{
    EditMesh cube      = MakeBareCube();
    auto     noNormals = ToRenderMesh( cube );
    ASSERT_FALSE( noNormals.IsSuccess() );
    EXPECT_NE( noNormals.GetError().find( "normal layer is disabled" ), std::string::npos );

    ComputeNormalsByPolyGroup( cube );
    ASSERT_TRUE( cube.Attributes().SetUVLayerCount( 1 ) );
    auto unsetUV = ToRenderMesh( cube );
    ASSERT_FALSE( unsetUV.IsSuccess() ) << "a UV layer with unset triangles must not silently emit TexCoord 0";
    EXPECT_NE( unsetUV.GetError().find( "unset in the UV layer" ), std::string::npos ) << unsetUV.GetError();
    auto missingLayer = ToRenderMesh( cube, 3 );
    ASSERT_FALSE( missingLayer.IsSuccess() );

    RenderMeshData bad = FromShape( MakeBox( glm::vec3( 100.0f ) ) );
    bad.Indices[4].V2  = 999;
    auto badIndex      = FromRenderMesh( bad );
    ASSERT_FALSE( badIndex.IsSuccess() );
    EXPECT_NE( badIndex.GetError().find( "999" ), std::string::npos ) << badIndex.GetError();
}

TEST( EditMeshAttributes, ImportDetachesAThirdTriangleOnAnEdgeAndSaysSo )
{
    // Three triangles on the edge (0, 1): EditMesh refuses the third, the render mesh had it.
    RenderMeshData render;
    for ( const glm::vec3 p : { glm::vec3( 0, 0, 0 ), glm::vec3( 100, 0, 0 ), glm::vec3( 50, 100, 0 ),
                                glm::vec3( 50, -100, 0 ), glm::vec3( 50, 0, 100 ) } )
    {
        Desert::Vertex v{};
        v.Position = p;
        v.Normal   = { 0, 0, 1 };
        render.Vertices.push_back( v );
    }
    render.Indices                = { { 0, 1, 2 }, { 1, 0, 3 }, { 1, 0, 4 } };
    const ImportedEditMesh import = Import( render );
    ASSERT_TRUE( Valid( import.Mesh ) );
    EXPECT_EQ( import.Mesh.TriangleCount(), 3 );
    EXPECT_EQ( import.DetachedTriangles, 1 );
    // 5 welded + copies of the two corners already in use (0 and 1); vertex 4 was new and is kept, so
    // nothing is left isolated.
    EXPECT_EQ( import.Mesh.VertexCount(), 7 );
    for ( const int v : import.Mesh.VertexIds() )
        EXPECT_FALSE( import.Mesh.GetVertexEdges( v ).empty() ) << "vertex " << v << " is isolated";
}

// ── the edits keep the layers ────────────────────────────────────────────────────────────────────────

TEST( EditMeshAttributes, SplitSharesTheNewElementUnlessTheEdgeIsASeam )
{
    for ( const bool seamed : { false, true } )
    {
        Quad q = MakeQuad();
        SetQuadUVs( q, seamed );
        ASSERT_TRUE( Valid( q.Mesh ) );
        UVOverlay& uv     = *q.Mesh.Attributes().UV( 0 );
        const int  before = uv.ElementCount();

        SplitEdgeInfo info;
        ASSERT_EQ( q.Mesh.SplitEdge( q.Diagonal, 0.25f, info ), EditResult::Ok );
        ASSERT_TRUE( Valid( q.Mesh ) );
        EXPECT_EQ( uv.ElementCount(), before + ( seamed ? 2 : 1 ) ) << "seamed " << seamed;

        // Every element at the new vertex is the lerp of its own side's ends at the split parameter.
        const glm::vec3 p = q.Mesh.GetPosition( info.NewVertex );
        EXPECT_NEAR( p.x, 25.0f, 1e-4f );
        std::set<int> atNew;
        for ( const int t : q.Mesh.GetVertexTriangles( info.NewVertex ) )
        {
            const int       e        = uv.GetElementAtVertex( q.Mesh, t, info.NewVertex );
            const bool      offset   = uv.GetElement( e ).x > 2.5f;
            const glm::vec2 expected = glm::vec2( p ) / 100.0f + ( offset ? glm::vec2( 5, 0 ) : glm::vec2( 0 ) );
            EXPECT_NEAR( uv.GetElement( e ).x, expected.x, 1e-5f );
            EXPECT_NEAR( uv.GetElement( e ).y, expected.y, 1e-5f );
            EXPECT_EQ( uv.GetParentVertex( e ), info.NewVertex );
            atNew.insert( e );
        }
        EXPECT_EQ( atNew.size(), seamed ? 2u : 1u );
        EXPECT_EQ( uv.IsSeamEdge( q.Mesh, q.Diagonal ), seamed );
    }
}

TEST( EditMeshAttributes, FlipIsRefusedAcrossAnyAttributeBoundary )
{
    const auto refused = []( Quad& q, const char* why )
    {
        const auto   before = q.Mesh.GetTriangle( q.T0 );
        FlipEdgeInfo info;
        EXPECT_EQ( q.Mesh.FlipEdge( q.Diagonal, info ), EditResult::AttributeSeam ) << why;
        EXPECT_EQ( q.Mesh.GetTriangle( q.T0 ), before ) << why;
        EXPECT_TRUE( Valid( q.Mesh ) ) << why;
    };

    Quad uvSeam = MakeQuad();
    SetQuadUVs( uvSeam, true );
    refused( uvSeam, "UV seam" );

    Quad halfSet = MakeQuad();
    SetQuadUVs( halfSet, false );
    halfSet.Mesh.Attributes().UV( 0 )->UnsetTriangle( halfSet.T1 );
    refused( halfSet, "one side set, one unset" );

    Quad groups = MakeQuad();
    groups.Mesh.Attributes().SetPolyGroup( groups.T1, 4 );
    refused( groups, "polygroup boundary" );

    Quad materials = MakeQuad();
    materials.Mesh.Attributes().SetMaterialId( materials.T1, 2 );
    refused( materials, "material boundary" );

    // Positive control: shared UVs flip, and the layer follows the new corners.
    Quad smooth = MakeQuad();
    SetQuadUVs( smooth, false );
    FlipEdgeInfo info;
    ASSERT_EQ( smooth.Mesh.FlipEdge( smooth.Diagonal, info ), EditResult::Ok );
    ASSERT_TRUE( Valid( smooth.Mesh ) );
    const UVOverlay& uv = *smooth.Mesh.Attributes().UV( 0 );
    for ( const int t : { smooth.T0, smooth.T1 } )
        for ( int j = 0; j < 3; ++j )
        {
            const glm::vec2 expected =
                 glm::vec2( smooth.Mesh.GetPosition( smooth.Mesh.GetTriangle( t )[j] ) ) / 100.0f;
            EXPECT_EQ( uv.GetElement( uv.GetTriangle( t )[j] ), expected );
        }
}

TEST( EditMeshAttributes, SeamEndCollapseIsRefusedAndASeamAlongTheEdgeCollapses )
{
    // 2x2 grid; vertex id = y * 3 + x. The lower-right cell is cut off in UV: its corners 1 and 4 get their
    // own elements, 2 and 5 do not. So the seam runs 1 -> 4 (split at both ends), turns to 4 -> 5 and ENDS
    // at 5, where both sides share one element: 4-5 is a seam end.
    EditMesh mesh = MakeLowerRightCut();
    ASSERT_TRUE( Valid( mesh ) );
    const int along = mesh.FindEdge( 1, 4 );
    const int above = mesh.FindEdge( 4, 7 );
    ASSERT_TRUE( mesh.Attributes().UV( 0 )->IsSeamEdge( mesh, along ) );
    ASSERT_TRUE( mesh.Attributes().UV( 0 )->IsSeamEdge( mesh, mesh.FindEdge( 4, 5 ) ) );
    ASSERT_FALSE( mesh.Attributes().UV( 0 )->IsSeamEdge( mesh, above ) );

    // Edge 4-5 is split at 4 only. Refused both ways, mesh untouched.
    {
        CollapseEdgeInfo info;
        const EditMesh   copy = mesh;
        EXPECT_EQ( mesh.CollapseEdge( 5, 4, 0.5f, info ), EditResult::AttributeSeam );
        EXPECT_EQ( mesh.CollapseEdge( 4, 5, 0.5f, info ), EditResult::AttributeSeam );
        EXPECT_EQ( mesh.VertexCount(), copy.VertexCount() );
        EXPECT_EQ( mesh.Attributes().UV( 0 )->ElementCount(), copy.Attributes().UV( 0 )->ElementCount() );
        ASSERT_TRUE( Valid( mesh ) );
    }

    // Edge 1-4 lies on the cut, split at both ends: collapses, and each side keeps its own offset.
    CollapseEdgeInfo info;
    ASSERT_EQ( mesh.CollapseEdge( 4, 1, 0.5f, info ), EditResult::Ok );
    ASSERT_TRUE( Valid( mesh ) );
    const UVOverlay& uv = *mesh.Attributes().UV( 0 );
    std::set<int>    atKept;
    for ( const int t : mesh.GetVertexTriangles( 4 ) )
        atKept.insert( uv.GetElementAtVertex( mesh, t, 4 ) );
    ASSERT_EQ( atKept.size(), 2u ) << "the two sides of the cut must stay two elements";
    std::vector<float> us;
    for ( const int e : atKept )
    {
        EXPECT_NEAR( uv.GetElement( e ).y, 0.5f, 1e-5f ); // halfway between v = 0 and v = 1
        us.push_back( uv.GetElement( e ).x );
    }
    std::sort( us.begin(), us.end() );
    EXPECT_NEAR( us[0], 1.0f, 1e-5f );
    EXPECT_NEAR( us[1], 6.0f, 1e-5f );
}

TEST( EditMeshAttributes, CollapseMovesAnElementOffTheEdgeOntoTheKeptVertex )
{
    // Grid 2x2 with a full cut along x = 1 (vertices 1, 4, 7). Collapsing the non-seam edge 3-4 into 3
    // (3 is off the cut, 4 on it) leaves 4's right-side element on no collapsing pair: it must move to 3.
    EditMesh mesh = MakeGrid( 2 );
    ASSERT_TRUE( mesh.Attributes().SetUVLayerCount( 1 ) );
    UVOverlay&                      uv = *mesh.Attributes().UV( 0 );
    std::vector<std::array<int, 2>> element( static_cast<size_t>( mesh.MaxVertexId() ), { InvalidId, InvalidId } );
    for ( const int t : mesh.TriangleIds() )
    {
        const auto& c = mesh.GetTriangle( t );
        const float cx =
             ( mesh.GetPosition( c[0] ).x + mesh.GetPosition( c[1] ).x + mesh.GetPosition( c[2] ).x ) / 3;
        const int          side = cx > 100.0f ? 1 : 0;
        std::array<int, 3> corners{};
        for ( int j = 0; j < 3; ++j )
        {
            int& e = element[c[j]][side];
            if ( e == InvalidId )
                e = uv.AppendElement( glm::vec2( mesh.GetPosition( c[j] ) ) / 100.0f +
                                      glm::vec2( 5.0f * side, 0 ) );
            corners[j] = e;
        }
        ASSERT_EQ( uv.SetTriangle( mesh, t, corners ), EditResult::Ok );
    }
    ASSERT_TRUE( Valid( mesh ) );
    const int rightOf4 = element[4][1];

    CollapseEdgeInfo info;
    ASSERT_EQ( mesh.CollapseEdge( 3, 4, 1.0f, info ), EditResult::Ok );
    ASSERT_TRUE( Valid( mesh ) ); // CheckValidity verifies every element's parent against its corners
    ASSERT_TRUE( uv.IsElement( rightOf4 ) );
    EXPECT_EQ( uv.GetParentVertex( rightOf4 ), 3 );
}

TEST( EditMeshAttributes, RemoveAndCompactCarryTheLayers )
{
    ImportedEditMesh import = Import( FromShape( MakeBox( glm::vec3( 100.0f ) ) ) );
    EditMesh&        mesh   = import.Mesh;
    ASSERT_EQ( GeneratePolyGroupsByAngle( mesh, 30.0f ), 6 );
    const NormalOverlay& normals = *mesh.Attributes().Normals();

    // Removing one face (two triangles) frees exactly the four normal elements only that face used.
    std::vector<int> face;
    for ( const int t : mesh.TriangleIds() )
        if ( mesh.Attributes().GetPolyGroup( t ) == 2 )
            face.push_back( t );
    ASSERT_EQ( face.size(), 2u );
    for ( const int t : face )
        ASSERT_EQ( mesh.RemoveTriangle( t, false ), EditResult::Ok );
    ASSERT_TRUE( Valid( mesh ) );
    EXPECT_EQ( normals.ElementCount(), 20 );
    EXPECT_LT( normals.ElementCount(), normals.MaxElementId() );

    const RenderMeshData before = Render( mesh );
    const CompactMaps    maps   = mesh.Compact();
    ASSERT_TRUE( Valid( mesh ) );
    EXPECT_EQ( normals.MaxElementId(), 20 );
    EXPECT_EQ( maps.NormalElements.size(), 24u );
    EXPECT_EQ( std::count( maps.NormalElements.begin(), maps.NormalElements.end(), InvalidId ), 4 );
    ASSERT_EQ( maps.UVElements.size(), 1u );
    EXPECT_TRUE( Triangles( Render( mesh ) ) == Triangles( before ) )
         << "Compact renumbered and changed the surface";
}

TEST( EditMeshAttributes, OverlayRefusalsAndOrphans )
{
    Quad       q  = MakeQuad();
    UVOverlay& uv = *q.Mesh.Attributes().UV( 0 );
    const int  a  = uv.AppendElement( { 0, 0 } );
    const int  b  = uv.AppendElement( { 1, 0 } );
    const int  c  = uv.AppendElement( { 1, 1 } );
    ASSERT_EQ( uv.SetTriangle( q.Mesh, q.T0, { a, b, c } ), EditResult::Ok );
    // a is bound to vertex 0; T1's corner 1 is vertex 2.
    const int d = uv.AppendElement( { 0, 1 } );
    EXPECT_EQ( uv.SetTriangle( q.Mesh, q.T1, { d, a, c } ), EditResult::ElementOnOtherVertex );
    EXPECT_EQ( uv.SetTriangle( q.Mesh, q.T1, { a, c, 99 } ), EditResult::InvalidElement );
    EXPECT_EQ( uv.SetTriangle( q.Mesh, 57, { a, c, d } ), EditResult::InvalidTriangle );
    EXPECT_FALSE( uv.IsSetTriangle( q.Mesh.FindTriangle( 0, 2, 3 ) ) );

    // d was appended and never used: a caller defect, and CheckValidity says which element.
    const auto orphan = q.Mesh.CheckValidity();
    ASSERT_FALSE( orphan.IsSuccess() );
    EXPECT_NE( orphan.GetError().find( "element 3 is used by no triangle" ), std::string::npos )
         << orphan.GetError();
    ASSERT_EQ( uv.SetTriangle( q.Mesh, q.T1, { a, c, d } ), EditResult::Ok );
    EXPECT_TRUE( Valid( q.Mesh ) );

    EXPECT_FALSE( q.Mesh.Attributes().SetUVLayerCount( EditMeshAttributes::MaxUVLayers + 1 ) );
    EXPECT_FALSE( q.Mesh.Attributes().SetUVLayerCount( -1 ) );
    EXPECT_EQ( q.Mesh.Attributes().UVLayerCount(), 1 );
}

// ── Generate PolyGroups ─────────────────────────────────────────────────────────────────────────────

TEST( EditMeshAttributes, AngleFollowsNeighboursCoplanarFollowsTheSeed )
{
    // A smooth sphere: neighbouring facets differ by ~30 degrees at 12 slices, so a 40-degree crease
    // threshold keeps the whole ball one group - but no two facets of a ball are coplanar with a seed a
    // quarter-turn away, so the planar mode must split it into many.
    ImportedEditMesh import = Import( FromShape( MakeSphere( 200.0f, 12, 8 ) ) );
    EditMesh&        mesh   = import.Mesh;
    EXPECT_EQ( GeneratePolyGroupsByAngle( mesh, 40.0f ), 1 );
    const int planar = GeneratePolyGroupsCoplanar( mesh, 40.0f, 1.0f );
    EXPECT_GT( planar, 12 ) << "coplanar must measure against the seed, not drift neighbour to neighbour";

    // Deterministic numbering: 0..N-1 in the order of the lowest triangle ID of each group.
    int              expectNext = 0;
    std::vector<int> firstSeen;
    (void)GeneratePolyGroupsCoplanar( mesh, 40.0f, 1.0f );
    for ( const int t : mesh.TriangleIds() )
    {
        const int g = mesh.Attributes().GetPolyGroup( t );
        if ( std::find( firstSeen.begin(), firstSeen.end(), g ) == firstSeen.end() )
        {
            EXPECT_EQ( g, expectNext );
            firstSeen.push_back( g );
            ++expectNext;
        }
    }
}

TEST( EditMeshAttributes, UVIslandsFollowTheSeamsOfTheChosenLayer )
{
    Quad joined = MakeQuad();
    SetQuadUVs( joined, false );
    auto one = GeneratePolyGroupsByUVIslands( joined.Mesh, 0 );
    ASSERT_TRUE( one.IsSuccess() );
    EXPECT_EQ( one.GetValue(), 1 );

    Quad cut = MakeQuad();
    SetQuadUVs( cut, true );
    auto two = GeneratePolyGroupsByUVIslands( cut.Mesh, 0 );
    ASSERT_TRUE( two.IsSuccess() );
    EXPECT_EQ( two.GetValue(), 2 );
    EXPECT_NE( cut.Mesh.Attributes().GetPolyGroup( cut.T0 ), cut.Mesh.Attributes().GetPolyGroup( cut.T1 ) );

    auto missing = GeneratePolyGroupsByUVIslands( cut.Mesh, 1 );
    ASSERT_FALSE( missing.IsSuccess() );
    EXPECT_NE( missing.GetError().find( "UV layer 1" ), std::string::npos );
}
