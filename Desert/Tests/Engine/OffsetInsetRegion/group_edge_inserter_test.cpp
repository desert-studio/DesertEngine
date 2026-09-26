// GroupEdgeInserter (GroupEdgeInserter.hpp, PlaneCut mode) on the editor's input: a tangent cube with one
// polygroup per face. An edge loop at the middle of the group edge between +X and +Z must cross the four faces
// around the Y axis (+X, +Z, -X, -Z), split each in two at y = 0, keep the mesh closed and render back.
#include "Engine/Geometry/DynamicMeshRenderConversion.hpp"
#include "Engine/Geometry/UECore/DynamicMesh/DynamicMeshAttributeSet.hpp"
#include "Engine/Geometry/UECore/DynamicMesh/GroupTopology.hpp"
#include "Engine/Geometry/UECore/DynamicMesh/Operations/GroupEdgeInserter.hpp"

#include <gtest/gtest.h>

#include <glm/geometric.hpp>

#include <cmath>
#include <set>

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
    DynamicMesh3 TangentCube()
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
        DynamicMesh3 mesh = std::move( imported.ExtractValue().Mesh );
        mesh.EnableTriangleGroups();
        for ( const int t : mesh.TriangleIndicesItr() )
            mesh.SetTriangleGroup( t, 1 + t / 2 );
        return mesh;
    }

    std::set<int> GroupIDs( const DynamicMesh3& mesh )
    {
        std::set<int> groups;
        for ( const int t : mesh.TriangleIndicesItr() )
            groups.insert( mesh.GetTriangleGroup( t ) );
        return groups;
    }

    constexpr int kPlusXGroup  = 1;
    constexpr int kMinusXGroup = 2;
    constexpr int kPlusZGroup  = 5;

    int GroupEdgeBetween( const GroupTopology& topology, int groupA, int groupB )
    {
        for ( int e = 0; e < static_cast<int32_t>( topology.m_Edges.size() ); ++e )
        {
            const Index2i g = topology.m_Edges[e].Groups;
            if ( ( g.A == groupA && g.B == groupB ) || ( g.A == groupB && g.B == groupA ) )
                return e;
        }
        return -1;
    }
} // namespace

TEST( GroupEdgeInserter, EdgeLoopAcrossTheCubeSplitsTheFourFacesAroundY )
{
    DynamicMesh3 mesh = TangentCube();
    ASSERT_TRUE( mesh.IsClosed() );
    ASSERT_EQ( GroupIDs( mesh ).size(), 6u );

    GroupTopology  topology( &mesh, true );
    int            groupEdge = -1;
    for ( int e = 0; e < static_cast<int32_t>( topology.m_Edges.size() ); ++e )
    {
        const Index2i g = topology.m_Edges[e].Groups;
        if ( ( g.A == kPlusXGroup && g.B == kPlusZGroup ) || ( g.A == kPlusZGroup && g.B == kPlusXGroup ) )
            groupEdge = e;
    }
    ASSERT_GE( groupEdge, 0 );

    const std::vector<double>                    proportions = { 0.5 };
    GroupEdgeInserter::EdgeLoopInsertionParams   params;
    params.Mesh               = &mesh;
    params.Topology           = &topology;
    params.GroupEdgeID        = groupEdge;
    params.SortedInputLengths = &proportions;
    params.StartCornerID      = topology.m_Edges[groupEdge].EndpointCorners.A;

    std::unordered_set<int32_t>               newEids;
    GroupEdgeInserter::OptionalOutputParams   out;
    out.NewEidsOut = &newEids;
    ASSERT_TRUE( GroupEdgeInserter::InsertEdgeLoops( params, out ) );

    EXPECT_EQ( GroupIDs( mesh ).size(), 10u );
    EXPECT_TRUE( mesh.IsClosed() );
    EXPECT_EQ( static_cast<int32_t>( topology.m_Groups.size() ), 10 );

    // The loop runs at y = 0 all the way round: every new edge lies in that plane, and every vertex is either
    // on it or on one of the cube's y = +-50 rings.
    EXPECT_GE( static_cast<int32_t>( newEids.size() ), 4 );
    for ( const int eid : newEids )
    {
        const Index2i v = mesh.GetEdgeV( eid );
        EXPECT_NEAR( mesh.GetVertex( v.A ).y, 0.0, 1e-6 );
        EXPECT_NEAR( mesh.GetVertex( v.B ).y, 0.0, 1e-6 );
    }
    for ( const int vid : mesh.VertexIndicesItr() )
    {
        const double y = mesh.GetVertex( vid ).y;
        EXPECT_TRUE( std::abs( y ) < 1e-6 || std::abs( std::abs( y ) - 50.0 ) < 1e-6 ) << "vertex " << vid;
    }
    // +Y and -Y are not crossed: each still has its two triangles.
    for ( const int group : { 3, 4 } )
    {
        int count = 0;
        for ( const int t : mesh.TriangleIndicesItr() )
            count += mesh.GetTriangleGroup( t ) == group ? 1 : 0;
        EXPECT_EQ( count, 2 ) << "group " << group;
    }

    auto render = ToRenderMesh( mesh );
    ASSERT_TRUE( render.IsSuccess() ) << render.GetError();
    for ( const Vertex& v : render.GetValue().Vertices )
    {
        EXPECT_NEAR( glm::length( v.Tangent ), 1.0f, 1e-5f );
        EXPECT_NEAR( glm::length( v.Bitangent ), 1.0f, 1e-5f );
        EXPECT_NEAR( glm::dot( v.Tangent, v.Normal ), 0.0f, 1e-5f );
    }
}

TEST( GroupEdgeInserter, GroupEdgeAcrossOneFaceSplitsOnlyThatFace )
{
    DynamicMesh3  mesh = TangentCube();
    GroupTopology topology( &mesh, true );
    // +Z face: connect the midpoints of its two group edges running along X (y = -50 and y = +50) by splitting
    // them first, as InsertEdgeLoops does, then cut across.
    const GroupTopology::Group* group = topology.FindGroupByID( kPlusZGroup );
    ASSERT_NE( group, nullptr );
    std::vector<int> alongX;
    for ( const int ge : group->Boundaries[0].GroupEdges )
    {
        const Index2i v = mesh.GetEdgeV( topology.m_Edges[ge].Span.Edges[0] );
        if ( std::abs( mesh.GetVertex( v.A ).y - mesh.GetVertex( v.B ).y ) < 1e-6 )
            alongX.push_back( topology.m_Edges[ge].Span.Edges[0] );
    }
    ASSERT_EQ( alongX.size(), 2u );

    GroupEdgeInserter::GroupEdgeInsertionParams params;
    params.Mesh     = &mesh;
    params.Topology = &topology;
    params.GroupID  = kPlusZGroup;
    for ( int i = 0; i < 2; ++i )
    {
        GroupEdgeInserter::GroupEdgeSplitPoint& p   = i == 0 ? params.StartPoint : params.EndPoint;
        p.ElementID                                 = alongX[i];
        p.bIsVertex                                 = false;
        p.EdgeTValue                                = 0.5;
        const Index2i v                             = mesh.GetEdgeV( alongX[i] );
        p.Tangent                                   = Normalized( mesh.GetVertex( v.B ) - mesh.GetVertex( v.A ) );
    }
    ASSERT_TRUE( GroupEdgeInserter::InsertGroupEdge( params ) );
    EXPECT_EQ( GroupIDs( mesh ).size(), 7u );
    EXPECT_TRUE( mesh.IsClosed() );
    EXPECT_EQ( static_cast<int32_t>( topology.m_Groups.size() ), 7 );
}

// An OPEN strip: the cube without its -X face, so the faces around Y are -Z, +X, +Z with a hole on either end.
// A loop started on the +X/+Z group edge cannot come back round to close itself (as on the whole cube, where
// the forward walk alone reaches every face): it must walk forward into one side AND backward into the other,
// and the three faces of the strip each split in two. Skipping the backward walk leaves one side unsplit.
TEST( GroupEdgeInserter, EdgeLoopOnAnOpenStripWalksBothWays )
{
    DynamicMesh3     mesh = TangentCube();
    std::vector<int> minusX;
    for ( const int t : mesh.TriangleIndicesItr() )
        if ( mesh.GetTriangleGroup( t ) == kMinusXGroup )
            minusX.push_back( t );
    ASSERT_EQ( minusX.size(), 2u );
    for ( const int t : minusX )
        ASSERT_EQ( mesh.RemoveTriangle( t ), MeshResult::Ok );
    ASSERT_FALSE( mesh.IsClosed() );
    ASSERT_EQ( GroupIDs( mesh ).size(), 5u );

    GroupTopology  topology( &mesh, true );
    const int      groupEdge = GroupEdgeBetween( topology, kPlusXGroup, kPlusZGroup );
    ASSERT_GE( groupEdge, 0 );

    const std::vector<double>                    proportions = { 0.5 };
    GroupEdgeInserter::EdgeLoopInsertionParams   params;
    params.Mesh               = &mesh;
    params.Topology           = &topology;
    params.GroupEdgeID        = groupEdge;
    params.SortedInputLengths = &proportions;
    params.StartCornerID      = topology.m_Edges[groupEdge].EndpointCorners.A;
    ASSERT_TRUE( GroupEdgeInserter::InsertEdgeLoops( params ) );

    // -Z, +X and +Z are cut in two; +Y and -Y are not crossed.
    EXPECT_EQ( GroupIDs( mesh ).size(), 8u );
    EXPECT_EQ( static_cast<int32_t>( topology.m_Groups.size() ), 8 );
    for ( const int vid : mesh.VertexIndicesItr() )
    {
        const double y = mesh.GetVertex( vid ).y;
        EXPECT_TRUE( std::abs( y ) < 1e-6 || std::abs( std::abs( y ) - 50.0 ) < 1e-6 ) << "vertex " << vid;
    }
}
