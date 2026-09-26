// FMeshBevel (ported from UE MeshBevel.cpp) on a welded cube with one polygroup per face: the topology build must
// classify each corner the way UE does (one edge -> two terminators whose end cap joins the perpendicular face,
// B:1061; all twelve edges -> eight junctions of three wedges), and the unlink phase must open every beveled edge
// into two boundary edges without moving anything. Displacement then insets every unlinked vertex InsetDistance
// into its own face. The subdivided cube (each face 2x2 quads) gives spans of two mesh edges, whose middle vertex
// is split by the span interior unlink (ReconcileTriangleSets) before the terminator and junction unlinks.
#include "Engine/Geometry/DynamicMeshRenderConversion.hpp"
#include "Engine/Geometry/UECore/DynamicMesh/DynamicMeshAttributeSet.hpp"
#include "Engine/Geometry/UECore/DynamicMesh/Operations/MeshBevel.hpp"

#include <gtest/gtest.h>

#include <glm/geometric.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <numbers>
#include <set>
#include <string>
#include <utility>
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

    // region_operation_test.cpp's TangentCube: faces +X, -X, +Y, -Y, +Z, -Z, face f is polygroup f + 1; each face
    // is an n x n grid of quads.
    FDynamicMesh3 TangentCube( int n = 1 )
    {
        const float     half = 50.0f;
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
            const glm::vec3 c = face.N * half;
            for ( int i = 0; i < n; ++i )
            {
                for ( int j = 0; j < n; ++j )
                {
                    const auto  base = static_cast<uint32_t>( vertices.size() );
                    const float u0   = -half + 2.0f * half * static_cast<float>( i ) / static_cast<float>( n );
                    const float u1   = -half + 2.0f * half * static_cast<float>( i + 1 ) / static_cast<float>( n );
                    const float v0   = -half + 2.0f * half * static_cast<float>( j ) / static_cast<float>( n );
                    const float v1   = -half + 2.0f * half * static_cast<float>( j + 1 ) / static_cast<float>( n );
                    vertices.push_back( MakeVertex( c + face.U * u0 + face.V * v0, face.N, face.U, { 0, 0 } ) );
                    vertices.push_back( MakeVertex( c + face.U * u1 + face.V * v0, face.N, face.U, { 1, 0 } ) );
                    vertices.push_back( MakeVertex( c + face.U * u1 + face.V * v1, face.N, face.U, { 1, 1 } ) );
                    vertices.push_back( MakeVertex( c + face.U * u0 + face.V * v1, face.N, face.U, { 0, 1 } ) );
                    indices.push_back( { base, base + 1, base + 2 } );
                    indices.push_back( { base, base + 2, base + 3 } );
                }
            }
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
            mesh.SetTriangleGroup( t, 1 + t / ( 2 * n * n ) );
        return mesh;
    }

    // The same cube with the triangles re-appended even cells first, then odd ones. An edge's first triangle (the
    // side the one-ring split calls Set0) then alternates between the two faces along some spans, which the welded
    // import (face by face) never produces.
    FDynamicMesh3 InterleavedTangentCube( int n )
    {
        const FDynamicMesh3 source = TangentCube( n );
        FDynamicMesh3       mesh;
        mesh.EnableTriangleGroups();
        for ( const int v : source.VertexIndicesItr() )
            EXPECT_EQ( mesh.AppendVertex( source.GetVertex( v ) ), v );
        for ( const int parity : { 0, 1 } )
        {
            for ( const int t : source.TriangleIndicesItr() )
            {
                if ( ( t / 2 ) % 2 == parity )
                    EXPECT_GE( mesh.AppendTriangle( source.GetTriangle( t ), source.GetTriangleGroup( t ) ), 0 );
            }
        }
        return mesh;
    }

    std::set<int> Ring( const FDynamicMesh3& mesh, int v )
    {
        std::set<int> ring;
        for ( const int t : mesh.VtxTrianglesItr( v ) )
            ring.insert( t );
        return ring;
    }

    // The protected phases and their state are the port's contract with the later phases; the probe reads them.
    class FMeshBevelProbe : public FMeshBevel
    {
    public:
        using FMeshBevel::DisplaceVertices;
        using FMeshBevel::Edges;
        using FMeshBevel::FixUpUnlinkedBevelEdges;
        using FMeshBevel::Loops;
        using FMeshBevel::MeshEdgePairs;
        using FMeshBevel::UnlinkEdges;
        using FMeshBevel::UnlinkLoops;
        using FMeshBevel::UnlinkVertices;
        using FMeshBevel::Vertices;

        // The unlink phases in UE Apply's order (B:576).
        void Unlink( FDynamicMesh3& mesh )
        {
            UnlinkEdges( mesh );
            UnlinkLoops( mesh );
            UnlinkVertices( mesh );
            FixUpUnlinkedBevelEdges( mesh );
        }
    };

    int CountBoundaryEdges( const FDynamicMesh3& mesh )
    {
        int boundary = 0;
        for ( const int e : mesh.BoundaryEdgeIndicesItr() )
        {
            (void)e;
            ++boundary;
        }
        return boundary;
    }

    // Face f of TangentCube: normal axis f / 2, sign + for even f.
    int FaceAxis( int group )
    {
        return ( group - 1 ) / 2;
    }
    double FaceSign( int group )
    {
        return ( ( group - 1 ) % 2 == 0 ) ? 1.0 : -1.0;
    }

    // With every edge beveled, each vertex belongs to one face island; it keeps its coordinate on the face axis
    // and is pulled InsetDistance in from every cube edge it sat on: an in-face coordinate of +-50 becomes +-(50 -
    // W).
    void ExpectEveryVertexInsetIntoItsFace( const FDynamicMesh3& mesh, const std::map<int, glm::dvec3>& before,
                                            double w )
    {
        for ( const int v : mesh.VertexIndicesItr() )
        {
            std::set<int> groups;
            for ( const int t : mesh.VtxTrianglesItr( v ) )
                groups.insert( mesh.GetTriangleGroup( t ) );
            ASSERT_EQ( groups.size(), 1u ) << "vertex " << v << " is not on a single face island";
            const int       group = *groups.begin();
            const glm::dvec3 orig  = before.at( v );
            const glm::dvec3 p     = mesh.GetVertex( v );
            for ( int a = 0; a < 3; ++a )
            {
                double expected = orig[a];
                if ( a == FaceAxis( group ) )
                    expected = 50.0 * FaceSign( group );
                else if ( std::abs( orig[a] ) == 50.0 )
                    expected = std::copysign( 50.0 - w, orig[a] );
                EXPECT_NEAR( p[a], expected, 1e-9 ) << "vertex " << v << " group " << group << " axis " << a;
            }
        }
    }

    std::map<int, glm::dvec3> Positions( const FDynamicMesh3& mesh )
    {
        std::map<int, glm::dvec3> positions;
        for ( const int v : mesh.VertexIndicesItr() )
            positions[v] = mesh.GetVertex( v );
        return positions;
    }

    // Unlink only re-indexes: every vertex still sits on one of the cube's corners.
    void ExpectPositionsAreCubeCorners( const FDynamicMesh3& mesh )
    {
        for ( const int v : mesh.VertexIndicesItr() )
        {
            const glm::dvec3 p = mesh.GetVertex( v );
            for ( int a = 0; a < 3; ++a )
                EXPECT_DOUBLE_EQ( std::abs( p[a] ), 50.0 ) << "vertex " << v << " axis " << a;
        }
    }

    // After unlink, both sides of every span edge are boundary edges paired with each other, at the same place.
    void ExpectEdgesUnlinkedIntoPairs( const FDynamicMesh3& mesh, const FMeshBevelProbe& bevel )
    {
        for ( const FMeshBevel::FBevelEdge& e : bevel.Edges )
        {
            ASSERT_EQ( e.NewMeshEdges.Num(), e.MeshEdges.Num() );
            ASSERT_EQ( e.NewMeshVertices.Num(), e.MeshVertices.Num() );
            for ( int32_t i = 0; i < e.MeshEdges.Num(); ++i )
            {
                const int e0 = e.MeshEdges[i];
                const int e1 = e.NewMeshEdges[i];
                EXPECT_NE( e0, e1 ) << "bevel edge " << e.EdgeIndex;
                EXPECT_TRUE( mesh.IsBoundaryEdge( e0 ) ) << e0;
                EXPECT_TRUE( mesh.IsBoundaryEdge( e1 ) ) << e1;
                const int32_t* partner = bevel.MeshEdgePairs.Find( e0 );
                ASSERT_NE( partner, nullptr ) << e0;
                EXPECT_EQ( *partner, e1 );
            }
            for ( int32_t i = 0; i < e.MeshVertices.Num(); ++i )
            {
                EXPECT_NE( e.MeshVertices[i], e.NewMeshVertices[i] ) << "bevel edge " << e.EdgeIndex;
                EXPECT_EQ( mesh.GetVertex( e.MeshVertices[i] ), e.InitialPositions[i] );
                EXPECT_EQ( mesh.GetVertex( e.NewMeshVertices[i] ), e.InitialPositions[i] );
            }
        }
    }

    void ExpectPairsSymmetric( const FMeshBevelProbe& bevel )
    {
        for ( const auto& pair : bevel.MeshEdgePairs )
        {
            const int32_t* back = bevel.MeshEdgePairs.Find( pair.second );
            ASSERT_NE( back, nullptr ) << pair.first << " -> " << pair.second;
            EXPECT_EQ( *back, pair.first );
        }
    }

    int GroupEdgeBetween( const FGroupTopology& topology, int groupA, int groupB )
    {
        for ( int32_t i = 0; i < topology.Edges.Num(); ++i )
        {
            const FIndex2i g = topology.Edges[i].Groups;
            if ( ( g.A == groupA && g.B == groupB ) || ( g.A == groupB && g.B == groupA ) )
                return i;
        }
        return -1;
    }

    // The wedges of a bevel vertex are disjoint and together are its whole triangle fan.
    void ExpectWedgesPartitionRing( const FDynamicMesh3& mesh, const FMeshBevel::FBevelVertex& v )
    {
        std::set<int> all;
        for ( const FMeshBevel::FOneRingWedge& w : v.Wedges )
        {
            EXPECT_GT( w.Triangles.Num(), 0 );
            for ( const int32_t t : w.Triangles )
                EXPECT_TRUE( all.insert( t ).second ) << "triangle " << t << " is in two wedges of " << v.VertexID;
        }
        EXPECT_EQ( all, Ring( mesh, v.VertexID ) );
    }
} // namespace

// +X (group 1) / +Y (group 3) edge runs along Z: its top end caps into +Z (5), its bottom end into -Z (6).
TEST( MeshBevel, OneGroupEdgeEndsInTwoTerminatorsCappedByThePerpendicularFace )
{
    const FDynamicMesh3  mesh = TangentCube();
    const FGroupTopology topology( &mesh, true );
    const int            edge = GroupEdgeBetween( topology, 1, 3 );
    ASSERT_GE( edge, 0 );

    FMeshBevelProbe bevel;
    ASSERT_TRUE( bevel.InitializeFromGroupTopologyEdges( mesh, topology, { edge } ) ) << bevel.FailureReason;
    ASSERT_EQ( bevel.Edges.Num(), 1 );
    ASSERT_EQ( bevel.Vertices.Num(), 2 );
    for ( const FMeshBevel::FBevelVertex& v : bevel.Vertices )
    {
        ASSERT_EQ( v.VertexType, FMeshBevel::EBevelVertexType::TerminatorVertex ) << v.VertexID;
        ASSERT_EQ( v.Wedges.Num(), 2 );
        ExpectWedgesPartitionRing( mesh, v );
        const int capGroup = mesh.GetVertex( v.VertexID ).z > 0 ? 5 : 6;
        EXPECT_EQ( v.NewGroupID, capGroup ) << "the end cap must join the existing perpendicular face (B:1061)";
        // the ring is split inside that face, so both triangles of the split edge belong to it
        const FIndex2i splitT = mesh.GetEdgeT( v.TerminatorInfo.A );
        EXPECT_EQ( mesh.GetTriangleGroup( splitT.A ), capGroup );
        EXPECT_EQ( mesh.GetTriangleGroup( splitT.B ), capGroup );
        EXPECT_EQ( mesh.GetEdgeV( v.TerminatorInfo.A ).OtherElement( v.VertexID ), v.TerminatorInfo.B );
    }
}

TEST( MeshBevel, AllTwelveEdgesMakeEightJunctionsOfThreeSingleFaceWedges )
{
    const FDynamicMesh3  mesh = TangentCube();
    const FGroupTopology topology( &mesh, true );

    FMeshBevelProbe bevel;
    ASSERT_TRUE( bevel.InitializeFromGroupTopology( mesh, topology ) ) << bevel.FailureReason;
    ASSERT_EQ( bevel.Edges.Num(), 12 );
    ASSERT_EQ( bevel.Vertices.Num(), 8 );
    for ( const FMeshBevel::FBevelVertex& v : bevel.Vertices )
    {
        ASSERT_EQ( v.VertexType, FMeshBevel::EBevelVertexType::JunctionVertex ) << v.VertexID;
        ASSERT_EQ( v.Wedges.Num(), 3 );
        ExpectWedgesPartitionRing( mesh, v );
        std::set<int> wedgeGroups;
        for ( const FMeshBevel::FOneRingWedge& w : v.Wedges )
        {
            const int g = mesh.GetTriangleGroup( w.Triangles[0] );
            for ( const int32_t t : w.Triangles )
                EXPECT_EQ( mesh.GetTriangleGroup( t ), g ) << "a wedge between bevel edges is one face";
            wedgeGroups.insert( g );
            // each wedge is bounded by two of the vertex's incoming bevel edges
            EXPECT_TRUE( v.IncomingBevelMeshEdges.Contains( w.BorderEdges.A ) );
            EXPECT_TRUE( v.IncomingBevelMeshEdges.Contains( w.BorderEdges.B ) );
        }
        EXPECT_EQ( wedgeGroups.size(), 3u );
    }
}

TEST( MeshBevel, UnknownGroupEdgeIsRefusedByName )
{
    const FDynamicMesh3  mesh = TangentCube();
    const FGroupTopology topology( &mesh, true );
    FMeshBevelProbe      bevel;
    EXPECT_FALSE( bevel.InitializeFromGroupTopologyEdges( mesh, topology, { 99 } ) );
    EXPECT_NE( bevel.FailureReason.find( "group edge 99" ), std::string::npos ) << bevel.FailureReason;
}

TEST( MeshBevel, UnlinkAllTwelveEdgesGivesEachFaceItsOwnCorners )
{
    FDynamicMesh3        mesh = TangentCube();
    const FGroupTopology topology( &mesh, true );

    FMeshBevelProbe bevel;
    ASSERT_TRUE( bevel.InitializeFromGroupTopology( mesh, topology ) ) << bevel.FailureReason;
    bevel.Unlink( mesh );
    ASSERT_TRUE( bevel.FailureReason.empty() ) << bevel.FailureReason;

    // each corner splits into its three wedges: 8 + 16, and every face is an island of 4 boundary edges
    EXPECT_EQ( mesh.VertexCount(), 24 );
    int boundary = 0;
    for ( const int e : mesh.BoundaryEdgeIndicesItr() )
    {
        (void)e;
        ++boundary;
    }
    EXPECT_EQ( boundary, 24 );
    EXPECT_EQ( bevel.MeshEdgePairs.Num(), 24 );
    ExpectEdgesUnlinkedIntoPairs( mesh, bevel );
    ExpectPairsSymmetric( bevel );
    ExpectPositionsAreCubeCorners( mesh );
    EXPECT_TRUE( mesh.CheckValidity( FDynamicMesh3::FValidityOptions(), EValidityCheckFailMode::ReturnOnly ) );
}

TEST( MeshBevel, UnlinkOneEdgeOpensOneSixEdgeHole )
{
    FDynamicMesh3        mesh = TangentCube();
    const FGroupTopology topology( &mesh, true );
    const int            edge = GroupEdgeBetween( topology, 1, 3 );
    ASSERT_GE( edge, 0 );

    FMeshBevelProbe bevel;
    ASSERT_TRUE( bevel.InitializeFromGroupTopologyEdges( mesh, topology, { edge } ) ) << bevel.FailureReason;
    bevel.Unlink( mesh );
    ASSERT_TRUE( bevel.FailureReason.empty() ) << bevel.FailureReason;

    // each terminator splits once; the bevel edge and both cap-face split edges open into pairs
    EXPECT_EQ( mesh.VertexCount(), 10 );
    std::map<int, std::vector<int>> adjacency;
    int                             boundary = 0;
    for ( const int e : mesh.BoundaryEdgeIndicesItr() )
    {
        const FIndex2i ev = mesh.GetEdgeV( e );
        adjacency[ev.A].push_back( ev.B );
        adjacency[ev.B].push_back( ev.A );
        ++boundary;
    }
    EXPECT_EQ( boundary, 6 );
    // one simple loop: six vertices of boundary degree two, all reachable from one
    ASSERT_EQ( adjacency.size(), 6u );
    for ( const auto& [v, next] : adjacency )
        EXPECT_EQ( next.size(), 2u ) << "vertex " << v;
    std::set<int>    seen{ adjacency.begin()->first };
    std::vector<int> stack{ adjacency.begin()->first };
    while ( !stack.empty() )
    {
        const int v = stack.back();
        stack.pop_back();
        for ( const int n : adjacency[v] )
        {
            if ( seen.insert( n ).second )
                stack.push_back( n );
        }
    }
    EXPECT_EQ( seen.size(), 6u );
    ExpectEdgesUnlinkedIntoPairs( mesh, bevel );
    ExpectPairsSymmetric( bevel );
    ExpectPositionsAreCubeCorners( mesh );
    EXPECT_TRUE( mesh.CheckValidity( FDynamicMesh3::FValidityOptions(), EValidityCheckFailMode::ReturnOnly ) );
}

TEST( MeshBevel, DisplaceAllTwelveEdgesInsetsEveryCornerIntoItsFace )
{
    FDynamicMesh3        mesh = TangentCube();
    const FGroupTopology topology( &mesh, true );

    FMeshBevelProbe bevel;
    bevel.InsetDistance = 5.0;
    ASSERT_TRUE( bevel.InitializeFromGroupTopology( mesh, topology ) ) << bevel.FailureReason;
    bevel.Unlink( mesh );
    const std::map<int, glm::dvec3> before = Positions( mesh );
    bevel.DisplaceVertices( mesh );
    ASSERT_TRUE( bevel.FailureReason.empty() ) << bevel.FailureReason;

    ExpectEveryVertexInsetIntoItsFace( mesh, before, 5.0 );
    for ( const FMeshBevel::FBevelVertex& vertex : bevel.Vertices )
    {
        for ( const FMeshBevel::FOneRingWedge& wedge : vertex.Wedges )
            EXPECT_TRUE( wedge.bHaveNewPosition ) << "wedge vertex " << wedge.WedgeVertex;
    }
}

// n = 3 gives two interior vertices per span; interleaved, the one-ring split picks opposite faces as Set0 at
// consecutive span vertices, and ReconcileTriangleSets must turn them back to one side.
TEST( MeshBevel, SubdividedCubeSplitsEachSpanInteriorVertexAndInsetsIt )
{
    for ( const auto& [n, bInterleaved] : { std::pair{ 2, false }, std::pair{ 3, false }, std::pair{ 3, true } } )
    {
        SCOPED_TRACE( "faces of " + std::to_string( n ) + "x" + std::to_string( n ) + " quads" +
                      ( bInterleaved ? ", interleaved" : "" ) );
        FDynamicMesh3 mesh = bInterleaved ? InterleavedTangentCube( n ) : TangentCube( n );
        ASSERT_EQ( mesh.VertexCount(), 6 * n * n + 2 );
        const FGroupTopology topology( &mesh, true );

        FMeshBevelProbe bevel;
        bevel.InsetDistance = 7.0;
        ASSERT_TRUE( bevel.InitializeFromGroupTopology( mesh, topology ) ) << bevel.FailureReason;
        ASSERT_EQ( bevel.Edges.Num(), 12 );
        int flippedSpans = 0;
        for ( const FMeshBevel::FBevelEdge& e : bevel.Edges )
        {
            ASSERT_EQ( e.MeshEdges.Num(), n ) << "bevel edge " << e.EdgeIndex;
            std::set<int> firstSides;
            for ( const int32_t me : e.MeshEdges )
                firstSides.insert( mesh.GetTriangleGroup( mesh.GetEdgeT( me ).A ) );
            flippedSpans += firstSides.size() > 1 ? 1 : 0;
        }
        if ( bInterleaved )
            EXPECT_GT( flippedSpans, 0 ) << "the fixture no longer exercises ReconcileTriangleSets";
        bevel.Unlink( mesh );
        ASSERT_TRUE( bevel.FailureReason.empty() ) << bevel.FailureReason;

        // six islands of (n+1)^2 vertices, each bounded by 4n edges: corners split in three, span interiors in two
        EXPECT_EQ( mesh.VertexCount(), 6 * ( n + 1 ) * ( n + 1 ) );
        EXPECT_EQ( CountBoundaryEdges( mesh ), 24 * n );
        EXPECT_EQ( bevel.MeshEdgePairs.Num(), 24 * n );
        ExpectEdgesUnlinkedIntoPairs( mesh, bevel );
        ExpectPairsSymmetric( bevel );
        EXPECT_TRUE( mesh.CheckValidity( FDynamicMesh3::FValidityOptions(), EValidityCheckFailMode::ReturnOnly ) );

        const std::map<int, glm::dvec3> before = Positions( mesh );
        bevel.DisplaceVertices( mesh );
        ASSERT_TRUE( bevel.FailureReason.empty() ) << bevel.FailureReason;
        ExpectEveryVertexInsetIntoItsFace( mesh, before, 7.0 );
    }
}

TEST( MeshBevel, SubdividedCubeOneEdgeTerminatorsSlideOntoTheirOwnFace )
{
    FDynamicMesh3        mesh = TangentCube( 2 );
    const FGroupTopology topology( &mesh, true );
    const int            edge = GroupEdgeBetween( topology, 1, 3 );
    ASSERT_GE( edge, 0 );

    FMeshBevelProbe bevel;
    bevel.InsetDistance = 5.0;
    ASSERT_TRUE( bevel.InitializeFromGroupTopologyEdges( mesh, topology, { edge } ) ) << bevel.FailureReason;
    bevel.Unlink( mesh );
    ASSERT_TRUE( bevel.FailureReason.empty() ) << bevel.FailureReason;
    // the span middle splits once, each terminator once; the hole is the two span pairs plus both cap-face pairs
    EXPECT_EQ( mesh.VertexCount(), 29 );
    EXPECT_EQ( CountBoundaryEdges( mesh ), 8 );

    const std::map<int, glm::dvec3> before = Positions( mesh );
    bevel.DisplaceVertices( mesh );
    ASSERT_TRUE( bevel.FailureReason.empty() ) << bevel.FailureReason;

    // The beveled edge is x = y = 50 (faces +X, group 1, and +Y, group 3). Every vertex on it moves 5 into the one
    // of those faces it still touches; everything else stays.
    std::map<double, int> movedIntoX;
    std::map<double, int> movedIntoY;
    for ( const int v : mesh.VertexIndicesItr() )
    {
        const glm::dvec3 orig = before.at( v );
        const glm::dvec3 p    = mesh.GetVertex( v );
        if ( orig.x != 50.0 || orig.y != 50.0 )
        {
            for ( int a = 0; a < 3; ++a )
                EXPECT_DOUBLE_EQ( p[a], orig[a] )
                     << "vertex " << v << " is off the beveled edge and must not move";
            continue;
        }
        bool touchesX = false;
        bool touchesY = false;
        for ( const int t : mesh.VtxTrianglesItr( v ) )
        {
            touchesX = touchesX || mesh.GetTriangleGroup( t ) == 1;
            touchesY = touchesY || mesh.GetTriangleGroup( t ) == 3;
        }
        ASSERT_NE( touchesX, touchesY ) << "vertex " << v << " must touch exactly one of the two beveled faces";
        EXPECT_DOUBLE_EQ( p.z, orig.z ) << "vertex " << v;
        if ( touchesX )
        {
            EXPECT_DOUBLE_EQ( p.x, 50.0 ) << "vertex " << v;
            EXPECT_NEAR( p.y, 45.0, 1e-9 ) << "vertex " << v;
            ++movedIntoX[orig.z];
        }
        else
        {
            EXPECT_NEAR( p.x, 45.0, 1e-9 ) << "vertex " << v;
            EXPECT_DOUBLE_EQ( p.y, 50.0 ) << "vertex " << v;
            ++movedIntoY[orig.z];
        }
    }
    const std::map<double, int> onePerHeight{ { -50.0, 1 }, { 0.0, 1 }, { 50.0, 1 } };
    EXPECT_EQ( movedIntoX, onePerHeight );
    EXPECT_EQ( movedIntoY, onePerHeight );
}

namespace
{
    // UE winding: GetTriNormal is (C - A) x (B - A), so an outward-facing closed mesh sums NEGATIVE under the
    // right-hand rule; negate to read the enclosed volume.
    double SignedVolume( const FDynamicMesh3& mesh )
    {
        double volume = 0.0;
        for ( const int t : mesh.TriangleIndicesItr() )
        {
            const FIndex3i  tri = mesh.GetTriangle( t );
            const glm::dvec3 a   = mesh.GetVertex( tri.A );
            const glm::dvec3 b   = mesh.GetVertex( tri.B );
            const glm::dvec3 c   = mesh.GetVertex( tri.C );
            volume -= glm::dot( a, glm::cross( b, c ) ) / 6.0;
        }
        return volume;
    }

    std::set<int> Groups( const FDynamicMesh3& mesh )
    {
        std::set<int> groups;
        for ( const int t : mesh.TriangleIndicesItr() )
            groups.insert( mesh.GetTriangleGroup( t ) );
        return groups;
    }

    // Closed, valid, consistently outward: a chamfer only ever removes material from the cube.
    void ExpectClosedSolid( const FDynamicMesh3& mesh, double expectedVolume )
    {
        EXPECT_EQ( CountBoundaryEdges( mesh ), 0 );
        EXPECT_TRUE( mesh.CheckValidity( FDynamicMesh3::FValidityOptions(), EValidityCheckFailMode::ReturnOnly ) );
        EXPECT_NEAR( SignedVolume( mesh ), expectedVolume, 1e-6 * expectedVolume );
    }

    // ComputeUVs: every new triangle has primary UVs that are finite and not degenerate; the other UV layers stay
    // unset, as UE. That the old triangles keep theirs is checked by ApplyKeepingOldUVs.
    void ExpectNewTriangleUVs( const FDynamicMesh3& mesh, const FMeshBevel& bevel )
    {
        if ( mesh.Attributes()->NumUVLayers() == 0 )
            return;
        const FDynamicMeshUVOverlay& uvs = *mesh.Attributes()->PrimaryUV();
        for ( const int t : bevel.NewTriangles )
        {
            if ( !uvs.IsSetTriangle( t ) )
            {
                ADD_FAILURE() << "new triangle " << t << " has no primary UVs";
                continue;
            }
            glm::vec2 a{};
            glm::vec2 b{};
            glm::vec2 c{};
            uvs.GetTriElements( t, a, b, c );
            for ( const glm::vec2& uv : { a, b, c } )
                EXPECT_TRUE( std::isfinite( uv.x ) && std::isfinite( uv.y ) ) << "new triangle " << t;
            const double area = 0.5 * std::abs( static_cast<double>( b.x - a.x ) * ( c.y - a.y ) -
                                                static_cast<double>( b.y - a.y ) * ( c.x - a.x ) );
            EXPECT_GT( area, 1e-6 ) << "new triangle " << t;
            for ( int layer = 1; layer < mesh.Attributes()->NumUVLayers(); ++layer )
                EXPECT_FALSE( mesh.Attributes()->GetUVLayer( layer )->IsSetTriangle( t ) ) << "new triangle " << t;
        }
    }

    // Every new triangle of a flat region carries its own geometric normal at each corner; a loop strip wrapping
    // a corner is ONE region with per-vertex normals, so there the normal only has to be unit and outward. No
    // normal element is shared by two polygroups.
    void ExpectNewTriangleNormals( const FDynamicMesh3& mesh, const FMeshBevel& bevel, bool bFlatRegions = true )
    {
        if ( !mesh.HasAttributes() )
            return;
        const FDynamicMeshNormalOverlay& normals = *mesh.Attributes()->PrimaryNormals();
        for ( const int t : bevel.NewTriangles )
        {
            ASSERT_TRUE( normals.IsSetTriangle( t ) ) << "new triangle " << t;
            const FIndex3i  elements = normals.GetTriangle( t );
            const glm::dvec3 faceN    = mesh.GetTriNormal( t );
            for ( int j = 0; j < 3; ++j )
            {
                const glm::vec3 n      = normals.GetElement( elements[j] );
                const double    cosine = faceN.x * n.x + faceN.y * n.y + faceN.z * n.z;
                EXPECT_NEAR( n.x * n.x + n.y * n.y + n.z * n.z, 1.0, 1e-5 ) << "new triangle " << t;
                if ( bFlatRegions )
                    EXPECT_NEAR( cosine, 1.0, 1e-5 ) << "new triangle " << t;
                else
                    EXPECT_GT( cosine, 0.8 ) << "new triangle " << t;
            }
        }
        ExpectNewTriangleUVs( mesh, bevel );
        std::map<int, int> elementGroup;
        for ( const int t : mesh.TriangleIndicesItr() )
        {
            if ( !normals.IsSetTriangle( t ) )
                continue;
            const FIndex3i elements = normals.GetTriangle( t );
            for ( int j = 0; j < 3; ++j )
            {
                const auto [it, inserted] = elementGroup.emplace( elements[j], mesh.GetTriangleGroup( t ) );
                EXPECT_EQ( it->second, mesh.GetTriangleGroup( t ) ) << "normal element " << elements[j];
            }
        }
    }

    // Apply, and every triangle that had primary UVs before still exists with the same UV values in every corner:
    // ComputeUVs writes the new triangles only, and the unlink may split an old element but never moves it.
    bool ApplyKeepingOldUVs( FMeshBevel& bevel, FDynamicMesh3& mesh )
    {
        std::map<int, std::array<glm::vec2, 3>> before;
        const FDynamicMeshUVOverlay*            uvs = mesh.HasAttributes() && mesh.Attributes()->NumUVLayers() > 0
                                                           ? mesh.Attributes()->PrimaryUV()
                                                           : nullptr;
        if ( uvs != nullptr )
            for ( const int t : mesh.TriangleIndicesItr() )
                if ( uvs->IsSetTriangle( t ) )
                    uvs->GetTriElements( t, before[t][0], before[t][1], before[t][2] );
        if ( !bevel.Apply( mesh ) )
            return false;
        EXPECT_EQ( uvs == nullptr, before.empty() );
        for ( const auto& [t, corners] : before )
        {
            if ( !mesh.IsTriangle( t ) || !uvs->IsSetTriangle( t ) )
            {
                ADD_FAILURE() << "old triangle " << t << " lost its primary UVs";
                continue;
            }
            std::array<glm::vec2, 3> after;
            uvs->GetTriElements( t, after[0], after[1], after[2] );
            for ( int j = 0; j < 3; ++j )
            {
                EXPECT_EQ( after[j].x, corners[j].x ) << "old triangle " << t << " corner " << j;
                EXPECT_EQ( after[j].y, corners[j].y ) << "old triangle " << t << " corner " << j;
            }
        }
        return true;
    }

    // The end cap of the one beveled edge 1|3 at the top of the cube lies in face 5, at the bottom in face 6.
    int CapMaterial( const glm::dvec3& p )
    {
        return p.z > 0.0 ? 50 : 60;
    }

    // Face f (polygroup f + 1) gets material 10 * (f + 1), so every face differs.
    void SetFaceMaterials( FDynamicMesh3& mesh )
    {
        FDynamicMeshMaterialAttribute* materials = mesh.Attributes()->GetMaterialID();
        for ( const int t : mesh.TriangleIndicesItr() )
            materials->SetValue( t, 10 * mesh.GetTriangleGroup( t ) );
    }

    int Material( const FDynamicMesh3& mesh, int t )
    {
        return mesh.Attributes()->GetMaterialID()->GetValue( t );
    }
} // namespace

// One edge of the n x n cube: the strip is the edge's new group, each terminator cap joins the face it closes
// (B:1061), so 6 + 1 groups; the removed prism has legs 5 x 5 over the full 100 length.
TEST( MeshBevel, ChamferOneEdgeClosesTheCubeWithOneStripGroup )
{
    for ( const int n : { 1, 2 } )
    {
        SCOPED_TRACE( "faces of " + std::to_string( n ) + "x" + std::to_string( n ) + " quads" );
        FDynamicMesh3        mesh = TangentCube( n );
        const FGroupTopology topology( &mesh, true );
        const int            edge = GroupEdgeBetween( topology, 1, 3 );
        ASSERT_GE( edge, 0 );
        ExpectClosedSolid( mesh, 1.0e6 );

        FMeshBevelProbe bevel;
        bevel.InsetDistance = 5.0;
        ASSERT_TRUE( bevel.InitializeFromGroupTopologyEdges( mesh, topology, { edge } ) ) << bevel.FailureReason;
        ASSERT_TRUE( ApplyKeepingOldUVs( bevel, mesh ) ) << bevel.FailureReason;
        ExpectNewTriangleNormals( mesh, bevel );

        EXPECT_EQ( Groups( mesh ).size(), 7u );
        ExpectClosedSolid( mesh, 1.0e6 - 100.0 * 12.5 );
        ASSERT_EQ( bevel.Edges.Num(), 1 );
        const FMeshBevel::FBevelEdge& strip = bevel.Edges[0];
        EXPECT_EQ( strip.StripQuads.Num(), n );
        for ( const int t : mesh.TriangleIndicesItr() )
        {
            if ( mesh.GetTriangleGroup( t ) != strip.NewGroupID )
                continue;
            const FIndex3i tri = mesh.GetTriangle( t );
            for ( int j = 0; j < 3; ++j )
            {
                // the original edge is the line x = y = 50
                const glm::dvec3 p = mesh.GetVertex( tri[j] );
                EXPECT_NEAR( std::hypot( p.x - 50.0, p.y - 50.0 ), 5.0, 1e-9 ) << "strip vertex " << tri[j];
            }
        }
        for ( const FMeshBevel::FBevelVertex& v : bevel.Vertices )
        {
            ASSERT_EQ( v.VertexType, FMeshBevel::EBevelVertexType::TerminatorVertex );
            EXPECT_EQ( v.NewTriangles.Num(), 1 ) << "terminator " << v.VertexID;
            ASSERT_EQ( v.NewTriangles.Num(), 1 );
            // the top cap closes +Z (group 5), the bottom one -Z (group 6)
            const int group = mesh.GetTriangleGroup( v.NewTriangles[0] );
            EXPECT_EQ( group, mesh.GetVertex( v.VertexID ).z > 0.0 ? 5 : 6 ) << "terminator " << v.VertexID;
        }
    }
}

// All twelve edges: 6 faces + 12 strips + 8 corner triangles = 26 groups. Octant by octant the solid is
// {u, v, w in [0, 50], u + v >= 5 (each pair), u + v + w >= 10} with u = 50 - x: it loses
// 625 + 2 * 562.5 + 125 / 3 cm^3 per octant.
TEST( MeshBevel, ChamferAllTwelveEdgesGivesTwentySixGroups )
{
    for ( const auto& [n, bInterleaved] :
          { std::pair{ 1, false }, std::pair{ 2, false }, std::pair{ 3, false }, std::pair{ 3, true } } )
    {
        SCOPED_TRACE( "faces of " + std::to_string( n ) + "x" + std::to_string( n ) + " quads" +
                      ( bInterleaved ? ", interleaved" : "" ) );
        FDynamicMesh3        mesh = bInterleaved ? InterleavedTangentCube( n ) : TangentCube( n );
        const FGroupTopology topology( &mesh, true );

        FMeshBevelProbe bevel;
        bevel.InsetDistance = 5.0;
        ASSERT_TRUE( bevel.InitializeFromGroupTopology( mesh, topology ) ) << bevel.FailureReason;
        ASSERT_TRUE( ApplyKeepingOldUVs( bevel, mesh ) ) << bevel.FailureReason;
        ExpectNewTriangleNormals( mesh, bevel );

        EXPECT_EQ( Groups( mesh ).size(), 26u );
        EXPECT_EQ( mesh.VertexCount(), 6 * ( n + 1 ) * ( n + 1 ) );
        EXPECT_EQ( mesh.TriangleCount(), 12 * n * n + 12 * 2 * n + 8 );
        ExpectClosedSolid( mesh, 1.0e6 - 8.0 * ( 625.0 + 1125.0 + 125.0 / 3.0 ) );
        for ( const FMeshBevel::FBevelVertex& v : bevel.Vertices )
        {
            EXPECT_EQ( v.NewTriangles.Num(), 1 ) << "junction " << v.VertexID;
            ASSERT_EQ( v.NewTriangles.Num(), 1 );
            // the corner triangle faces away from the cube centre along (+-1, +-1, +-1)
            const glm::dvec3 normal = mesh.GetTriNormal( v.NewTriangles[0] );
            const glm::dvec3 corner = mesh.GetTriCentroid( v.NewTriangles[0] );
            EXPECT_NEAR( glm::dot( normal, Normalized( corner ) ), 1.0, 1e-3 ) << "junction " << v.VertexID;
        }
    }
}

// Material IDs of the one-edge chamfer (faces 10..60): the strip lies between +X (10) and +Y (30), so it is
// ambiguous; each terminator cap joins the face it closes and takes its material, the most frequent around it.
TEST( MeshBevel, ApplyOneEdgeAssignsMaterialsPerMode )
{
    using EMode = FMeshBevel::EMaterialIDMode;
    for ( const EMode mode :
          { EMode::ConstantMaterialID, EMode::InferMaterialID, EMode::InferMaterialID_ConstantIfAmbiguous } )
    {
        SCOPED_TRACE( "mode " + std::to_string( static_cast<int>( mode ) ) );
        FDynamicMesh3 mesh = TangentCube();
        ASSERT_TRUE( mesh.HasAttributes() && mesh.Attributes()->HasMaterialID() );
        SetFaceMaterials( mesh );
        const FGroupTopology topology( &mesh, true );
        FMeshBevelProbe      bevel;
        bevel.MaterialIDMode        = mode;
        bevel.SetConstantMaterialID = 7;
        ASSERT_TRUE(
             bevel.InitializeFromGroupTopologyEdges( mesh, topology, { GroupEdgeBetween( topology, 1, 3 ) } ) )
             << bevel.FailureReason;
        ASSERT_TRUE( ApplyKeepingOldUVs( bevel, mesh ) ) << bevel.FailureReason;
        EXPECT_EQ( bevel.NewTriangles.Num(), 4 );
        ExpectNewTriangleNormals( mesh, bevel );

        const int stripMaterial = mode == EMode::InferMaterialID ? 10 : 7;
        for ( const FIndex2i& quad : bevel.Edges[0].StripQuads )
        {
            EXPECT_EQ( Material( mesh, quad.A ), stripMaterial );
            EXPECT_EQ( Material( mesh, quad.B ), stripMaterial );
        }
        for ( const FMeshBevel::FBevelVertex& v : bevel.Vertices )
        {
            const int capMaterial =
                 mode == EMode::ConstantMaterialID ? 7 : CapMaterial( mesh.GetVertex( v.VertexID ) );
            EXPECT_EQ( Material( mesh, v.NewTriangles[0] ), capMaterial ) << "terminator " << v.VertexID;
        }
    }
}

// All twelve edges, InferMaterialID: a strip takes the lower of its two faces, so around the corner of octant
// (gx, gy, gz) the strips read gx, gx, gy and the corner triangle takes the most frequent, 10 * gx.
TEST( MeshBevel, ApplyAllEdgesCornerTakesTheMostFrequentStripMaterial )
{
    FDynamicMesh3 mesh = TangentCube();
    SetFaceMaterials( mesh );
    const FGroupTopology topology( &mesh, true );
    FMeshBevelProbe      bevel;
    bevel.MaterialIDMode = FMeshBevel::EMaterialIDMode::InferMaterialID;
    ASSERT_TRUE( bevel.InitializeFromGroupTopology( mesh, topology ) ) << bevel.FailureReason;
    ASSERT_TRUE( ApplyKeepingOldUVs( bevel, mesh ) ) << bevel.FailureReason;
    EXPECT_EQ( bevel.NewTriangles.Num(), 8 + 12 * 2 );
    for ( const FMeshBevel::FBevelVertex& v : bevel.Vertices )
    {
        const int gx = mesh.GetVertex( v.VertexID ).x > 0.0 ? 1 : 2;
        EXPECT_EQ( Material( mesh, v.NewTriangles[0] ), 10 * gx ) << "junction " << v.VertexID;
    }
}

// A refusal at initialization makes Apply refuse with the same reason and leave the mesh alone.
TEST( MeshBevel, ApplyAfterRefusedInitializationReturnsFalse )
{
    FDynamicMesh3        mesh = TangentCube();
    const FGroupTopology topology( &mesh, true );
    FMeshBevel           bevel;
    EXPECT_FALSE( bevel.InitializeFromGroupTopologyEdges( mesh, topology, { 999 } ) );
    const std::string reason = bevel.FailureReason;
    ASSERT_FALSE( reason.empty() );
    EXPECT_FALSE( bevel.Apply( mesh ) );
    EXPECT_EQ( bevel.FailureReason, reason );
    EXPECT_EQ( mesh.VertexCount(), 8 );
    EXPECT_EQ( mesh.TriangleCount(), 12 );
    EXPECT_EQ( bevel.NewTriangles.Num(), 0 );
}

// Top (group 1), sides (group 2), bottom (group 3): the two group edges are closed loops without a corner, so they
// go through UnlinkBevelLoop / AppendLoopQuads. The top square insets to half-width 45 while the sides drop to
// z = 45: per loop the removed volume is the integral over z in [45, 50] of 100^2 - (2 (95 - z))^2 = 5000 - 500/3.
TEST( MeshBevel, ApplyClosedLoopsWithoutCornersGiveTwoLoopStrips )
{
    for ( const int n : { 1, 2 } )
    {
        SCOPED_TRACE( "faces of " + std::to_string( n ) + "x" + std::to_string( n ) + " quads" );
        FDynamicMesh3 mesh = TangentCube( n );
        for ( const int t : mesh.TriangleIndicesItr() )
        {
            const int face  = t / ( 2 * n * n );
            int       group = 2;
            if ( face == 4 )
                group = 1;
            else if ( face == 5 )
                group = 3;
            mesh.SetTriangleGroup( t, group );
        }
        SetFaceMaterials( mesh );
        const FGroupTopology topology( &mesh, true );
        FMeshBevelProbe      bevel;
        bevel.MaterialIDMode = FMeshBevel::EMaterialIDMode::InferMaterialID;
        ASSERT_TRUE( bevel.InitializeFromGroupTopology( mesh, topology ) ) << bevel.FailureReason;
        ASSERT_EQ( bevel.Loops.Num(), 2 );
        EXPECT_EQ( bevel.Edges.Num(), 0 );
        EXPECT_EQ( bevel.Vertices.Num(), 0 );
        ASSERT_TRUE( ApplyKeepingOldUVs( bevel, mesh ) ) << bevel.FailureReason;

        EXPECT_EQ( Groups( mesh ).size(), 5u );
        ExpectClosedSolid( mesh, 1.0e6 - 2.0 * ( 5000.0 - 500.0 / 3.0 ) );
        ExpectNewTriangleNormals( mesh, bevel, false );
        for ( const FMeshBevel::FBevelLoop& loop : bevel.Loops )
        {
            EXPECT_EQ( loop.StripQuads.Num(), 4 * n );
            // top loop: sides 20 against top 10; bottom loop: sides 20 against bottom 30
            const int material = mesh.GetVertex( loop.MeshVertices[0] ).z > 0.0 ? 10 : 20;
            for ( const FIndex2i& quad : loop.StripQuads )
            {
                EXPECT_EQ( Material( mesh, quad.A ), material );
                EXPECT_EQ( Material( mesh, quad.B ), material );
            }
        }
    }
}

// The vertical edges at (50, 50) and (-50, -50): both ends of each split along the cap face's diagonal, which
// joins the two terminators, so each cap is one quad in the cap face's group (AppendTerminatorVertexPairQuad).
TEST( MeshBevel, ApplyTwoTerminatorsOnOneDiagonalShareAQuad )
{
    FDynamicMesh3        mesh = TangentCube();
    const FGroupTopology topology( &mesh, true );
    FMeshBevelProbe      bevel;
    ASSERT_TRUE( bevel.InitializeFromGroupTopologyEdges(
         mesh, topology, { GroupEdgeBetween( topology, 1, 3 ), GroupEdgeBetween( topology, 2, 4 ) } ) )
         << bevel.FailureReason;
    ASSERT_EQ( bevel.Vertices.Num(), 4 );
    for ( const FMeshBevel::FBevelVertex& v : bevel.Vertices )
    {
        ASSERT_EQ( v.VertexType, FMeshBevel::EBevelVertexType::TerminatorVertex );
        EXPECT_GE( v.ConnectedBevelVertex, 0 ) << "terminator " << v.VertexID;
    }
    ASSERT_TRUE( ApplyKeepingOldUVs( bevel, mesh ) ) << bevel.FailureReason;

    EXPECT_EQ( Groups( mesh ).size(), 8u );
    EXPECT_EQ( bevel.NewTriangles.Num(), 2 * 2 + 2 * 2 );
    ExpectClosedSolid( mesh, 1.0e6 - 2.0 * 100.0 * 12.5 );
    ExpectNewTriangleNormals( mesh, bevel );
    for ( const FMeshBevel::FBevelVertex& v : bevel.Vertices )
    {
        ASSERT_EQ( v.NewTriangles.Num(), 1 );
        EXPECT_EQ( mesh.GetTriangleGroup( v.NewTriangles[0] ), mesh.GetVertex( v.VertexID ).z > 0.0 ? 5 : 6 );
    }
}

// The top face tilted to z = 50 + x / 2: the edge x = y = 50 now meets its top cap at a non-right angle. The
// terminator still slides onto both inset lines, so every strip vertex stays 5 from the edge line and the removed
// prism is the 5 x 5 triangle times the height over it, 100 + x / 2 at its centroid x = 50 - 5/3.
TEST( MeshBevel, ApplyTerminatorOnATiltedCapStaysOnTheInsetLines )
{
    FDynamicMesh3 mesh = TangentCube();
    for ( const int v : mesh.VertexIndicesItr() )
    {
        const glm::dvec3 p = mesh.GetVertex( v );
        if ( p.z > 0.0 )
            mesh.SetVertex( v, glm::dvec3( p.x, p.y, 50.0 + 0.5 * p.x ) );
    }
    ExpectClosedSolid( mesh, 1.0e6 );
    const FGroupTopology topology( &mesh, true );
    FMeshBevelProbe      bevel;
    ASSERT_TRUE( bevel.InitializeFromGroupTopologyEdges( mesh, topology, { GroupEdgeBetween( topology, 1, 3 ) } ) )
         << bevel.FailureReason;
    ASSERT_TRUE( ApplyKeepingOldUVs( bevel, mesh ) ) << bevel.FailureReason;

    EXPECT_EQ( Groups( mesh ).size(), 7u );
    ExpectClosedSolid( mesh, 1.0e6 - 12.5 * ( 100.0 + 0.5 * ( 50.0 - 5.0 / 3.0 ) ) );
    ExpectNewTriangleNormals( mesh, bevel );
    for ( const FIndex2i& quad : bevel.Edges[0].StripQuads )
    {
        for ( const int t : { quad.A, quad.B } )
        {
            const FIndex3i tri = mesh.GetTriangle( t );
            for ( int j = 0; j < 3; ++j )
            {
                // 5 from the edge line and, at the top end, on the tilted cap plane
                const glm::dvec3 p = mesh.GetVertex( tri[j] );
                EXPECT_NEAR( std::hypot( p.x - 50.0, p.y - 50.0 ), 5.0, 1e-9 ) << "strip vertex " << tri[j];
                if ( p.z > 0.0 )
                    EXPECT_NEAR( p.z, 50.0 + 0.5 * p.x, 1e-9 ) << "strip vertex " << tri[j];
            }
        }
    }
}

// Multi-segment flat bevel (UE CreateBevelMeshing_Multi with RoundWeight 0): every strip gains NumSubdivisions
// evenly spaced rows in the chamfer plane and each corner a tessellation matching them, so the solid is the
// chamfered one (same volume) with more, still outward and closed, triangles.
namespace
{
    // Every strip column is a straight, evenly spaced run from one side of the bevel to the other.
    void ExpectEvenStripColumns( const FDynamicMesh3& mesh, const FQuadGridPatch& patch, int subdivisions )
    {
        ASSERT_EQ( patch.NumVertexRows(), subdivisions + 2 );
        for ( int c = 0; c < patch.NumVertexCols(); ++c )
        {
            TArray<int32_t> column;
            ASSERT_TRUE( patch.GetVertexColumn( c, column ) );
            const glm::dvec3 a = mesh.GetVertex( column[0] );
            const glm::dvec3 b = mesh.GetVertex( column.Last() );
            for ( int j = 0; j < column.Num(); ++j )
            {
                const glm::dvec3 expected = Lerp( a, b, static_cast<double>( j ) / ( subdivisions + 1 ) );
                EXPECT_NEAR( Distance( mesh.GetVertex( column[j] ), expected ), 0.0, 1e-9 ) << "column " << c;
            }
        }
    }

    double ChamferVolume( FDynamicMesh3 mesh, const TArray<int32_t>& groupEdges )
    {
        const FGroupTopology topology( &mesh, true );
        FMeshBevel           bevel;
        if ( !bevel.InitializeFromGroupTopologyEdges( mesh, topology, groupEdges ) || !bevel.Apply( mesh ) )
        {
            ADD_FAILURE() << bevel.FailureReason;
            return 0.0;
        }
        return SignedVolume( mesh );
    }
} // namespace

TEST( MeshBevel, MultiSegmentAllTwelveEdgesKeepsTheChamferVolume )
{
    for ( const int n : { 1, 2 } )
    {
        for ( const int N : { 2, 3 } )
        {
            SCOPED_TRACE( "faces of " + std::to_string( n ) + "x" + std::to_string( n ) + ", " +
                          std::to_string( N ) + " subdivisions" );
            FDynamicMesh3        mesh = TangentCube( n );
            const FGroupTopology topology( &mesh, true );
            FMeshBevelProbe      bevel;
            bevel.InsetDistance   = 5.0;
            bevel.NumSubdivisions = N;
            ASSERT_TRUE( bevel.InitializeFromGroupTopology( mesh, topology ) ) << bevel.FailureReason;
            ASSERT_TRUE( ApplyKeepingOldUVs( bevel, mesh ) ) << bevel.FailureReason;
            ExpectNewTriangleNormals( mesh, bevel );

            EXPECT_EQ( Groups( mesh ).size(), 26u );
            // faces 12 n^2, strips 12 edges x n x (N + 1) quads, corners (N + 1)^2 each
            EXPECT_EQ( mesh.TriangleCount(), 12 * n * n + 24 * n * ( N + 1 ) + 8 * ( N + 1 ) * ( N + 1 ) );
            EXPECT_EQ( mesh.VertexCount(),
                       6 * ( n + 1 ) * ( n + 1 ) + 12 * ( n + 1 ) * N + 8 * N * ( N - 1 ) / 2 );
            ExpectClosedSolid( mesh, 1.0e6 - 8.0 * ( 625.0 + 1125.0 + 125.0 / 3.0 ) );
            for ( const FMeshBevel::FBevelEdge& edge : bevel.Edges )
                ExpectEvenStripColumns( mesh, edge.StripQuadPatch, N );
            for ( const FMeshBevel::FBevelVertex& v : bevel.Vertices )
            {
                ASSERT_EQ( v.NewTriangles.Num(), ( N + 1 ) * ( N + 1 ) ) << "junction " << v.VertexID;
                for ( const int t : v.NewTriangles )
                {
                    const glm::dvec3 corner = mesh.GetTriCentroid( t );
                    EXPECT_NEAR( glm::dot( mesh.GetTriNormal( t ), Normalized( corner ) ), 1.0, 1e-3 )
                         << "junction " << v.VertexID;
                }
            }
        }
    }
}

// Terminators (one edge: a fan each; two edges whose terminators pair up: a quad column per pair): the same solid
// as the one-segment chamfer, closed and outward.
TEST( MeshBevel, MultiSegmentTerminatorsMatchTheChamferSolid )
{
    for ( const int n : { 1, 2 } )
    {
        const FDynamicMesh3  base = TangentCube( n );
        const FGroupTopology baseTopology( &base, true );
        struct FCase
        {
            std::string     Name;
            TArray<int32_t> GroupEdges;
            int             TerminatorTrianglesPerSegment;
        };
        const std::vector<FCase> cases = {
             { "one edge", { GroupEdgeBetween( baseTopology, 1, 3 ) }, 2 },
             { "diagonal pair",
               { GroupEdgeBetween( baseTopology, 1, 3 ), GroupEdgeBetween( baseTopology, 2, 4 ) },
               4 },
        };
        for ( const FCase& c : cases )
        {
            for ( const int N : { 2, 3 } )
            {
                SCOPED_TRACE( c.Name + ", n " + std::to_string( n ) + ", " + std::to_string( N ) +
                              " subdivisions" );
                const double         expected = ChamferVolume( base, c.GroupEdges );
                FDynamicMesh3        mesh     = base;
                const FGroupTopology topology( &mesh, true );
                FMeshBevelProbe      bevel;
                bevel.NumSubdivisions = N;
                ASSERT_TRUE( bevel.InitializeFromGroupTopologyEdges( mesh, topology, c.GroupEdges ) )
                     << bevel.FailureReason;
                ASSERT_TRUE( ApplyKeepingOldUVs( bevel, mesh ) ) << bevel.FailureReason;
                ExpectClosedSolid( mesh, expected );
                ExpectNewTriangleNormals( mesh, bevel );
                for ( const FMeshBevel::FBevelEdge& edge : bevel.Edges )
                    ExpectEvenStripColumns( mesh, edge.StripQuadPatch, N );
                int terminatorTriangles = 0;
                for ( const FMeshBevel::FBevelVertex& v : bevel.Vertices )
                    terminatorTriangles += v.NewTriangles.Num();
                EXPECT_EQ( terminatorTriangles, c.TerminatorTrianglesPerSegment * ( N + 1 ) );
            }
        }
    }
}

namespace
{
    // Convex solid around the origin: every triangle faces away from it.
    void ExpectAllTrianglesOutward( const FDynamicMesh3& mesh )
    {
        for ( const int t : mesh.TriangleIndicesItr() )
            EXPECT_GT( glm::dot( mesh.GetTriNormal( t ), Normalized( mesh.GetTriCentroid( t ) ) ), 0.0 )
                 << "triangle " << t;
    }

    // Top face chamfered on its four edges by d: the cube cut by x+z, -x+z, y+z, -y+z <= 100 - d. Four prisms of
    // d^2/2 x 100, whose corner overlaps (integral of (d - w)^2 over w in [0, d]) are counted twice.
    double TopFaceChamferVolume( double d )
    {
        return 1.0e6 - 4.0 * ( 50.0 * d * d ) + 4.0 * ( d * d * d / 3.0 );
    }
} // namespace

// The top face's four edges with the sides in one group: one closed loop, no corners (AppendLoopQuads_Multi).
TEST( MeshBevel, MultiSegmentTopLoopKeepsTheChamferVolume )
{
    for ( const int n : { 1, 2 } )
    {
        for ( const int N : { 2, 3 } )
        {
            SCOPED_TRACE( "faces of " + std::to_string( n ) + "x" + std::to_string( n ) + ", " +
                          std::to_string( N ) + " subdivisions" );
            FDynamicMesh3 mesh = TangentCube( n );
            for ( const int t : mesh.TriangleIndicesItr() )
                mesh.SetTriangleGroup( t, t / ( 2 * n * n ) == 4 ? 1 : 2 );
            const FGroupTopology topology( &mesh, true );
            FMeshBevelProbe      bevel;
            bevel.InsetDistance   = 5.0;
            bevel.NumSubdivisions = N;
            ASSERT_TRUE( bevel.InitializeFromGroupTopology( mesh, topology ) ) << bevel.FailureReason;
            ASSERT_EQ( bevel.Loops.Num(), 1 );
            ASSERT_TRUE( ApplyKeepingOldUVs( bevel, mesh ) ) << bevel.FailureReason;
            ExpectClosedSolid( mesh, TopFaceChamferVolume( 5.0 ) );
            ExpectNewTriangleNormals( mesh, bevel, false );
            ExpectAllTrianglesOutward( mesh );
            EXPECT_EQ( Groups( mesh ).size(), 3u );
            ExpectEvenStripColumns( mesh, bevel.Loops[0].StripQuadPatch, N );
        }
    }
}

// The top face's four edges with every side its own group: four edges whose corners each join two bevelled edges
// and one untouched vertical edge (a two-wedge junction).
TEST( MeshBevel, MultiSegmentTopFourEdgesKeepTheChamferVolume )
{
    for ( const int n : { 1, 2 } )
    {
        const FDynamicMesh3  base = TangentCube( n );
        const FGroupTopology baseTopology( &base, true );
        TArray<int32_t>      groupEdges;
        for ( const int side : { 1, 2, 3, 4 } )
            groupEdges.Add( GroupEdgeBetween( baseTopology, 5, side ) );
        const double chamfer = ChamferVolume( base, groupEdges );
        EXPECT_NEAR( chamfer, TopFaceChamferVolume( 5.0 ), 1e-6 * chamfer );
        for ( const int N : { 2, 3 } )
        {
            SCOPED_TRACE( "faces of " + std::to_string( n ) + "x" + std::to_string( n ) + ", " +
                          std::to_string( N ) + " subdivisions" );
            FDynamicMesh3        mesh = base;
            const FGroupTopology topology( &mesh, true );
            FMeshBevelProbe      bevel;
            bevel.InsetDistance   = 5.0;
            bevel.NumSubdivisions = N;
            ASSERT_TRUE( bevel.InitializeFromGroupTopologyEdges( mesh, topology, groupEdges ) )
                 << bevel.FailureReason;
            ASSERT_EQ( bevel.Edges.Num(), 4 );
            ASSERT_EQ( bevel.Vertices.Num(), 4 );
            for ( const FMeshBevel::FBevelVertex& v : bevel.Vertices )
            {
                EXPECT_EQ( v.VertexType, FMeshBevel::EBevelVertexType::JunctionVertex ) << "vertex " << v.VertexID;
                EXPECT_EQ( v.Wedges.Num(), 2 ) << "vertex " << v.VertexID;
            }
            ASSERT_TRUE( ApplyKeepingOldUVs( bevel, mesh ) ) << bevel.FailureReason;
            ExpectClosedSolid( mesh, TopFaceChamferVolume( 5.0 ) );
            ExpectNewTriangleNormals( mesh, bevel );
            ExpectAllTrianglesOutward( mesh );
            for ( const FMeshBevel::FBevelEdge& edge : bevel.Edges )
                ExpectEvenStripColumns( mesh, edge.StripQuadPatch, N );
        }
    }
}

// Four quadrant groups on a flat top face: the centre joins four bevelled edges (a valence-4 grid junction), the
// outer ends are terminators. Coplanar edges: the bevel re-meshes the face without changing the solid.
TEST( MeshBevel, MultiSegmentFlatFourEdgeJunctionKeepsTheCube )
{
    for ( const int N : { 1, 2, 3 } )
    {
        SCOPED_TRACE( std::to_string( N ) + " subdivisions" );
        FDynamicMesh3 mesh = TangentCube( 2 );
        for ( const int t : mesh.TriangleIndicesItr() )
        {
            const glm::dvec3 c = mesh.GetTriCentroid( t );
            if ( c.z > 49.0 )
                mesh.SetTriangleGroup( t, 7 + ( c.x > 0.0 ? 1 : 0 ) + ( c.y > 0.0 ? 2 : 0 ) );
        }
        const FGroupTopology  topology( &mesh, true );
        const TArray<int32_t> groupEdges = {
             GroupEdgeBetween( topology, 7, 8 ), GroupEdgeBetween( topology, 8, 10 ),
             GroupEdgeBetween( topology, 10, 9 ), GroupEdgeBetween( topology, 9, 7 ) };
        FMeshBevelProbe bevel;
        bevel.InsetDistance   = 5.0;
        bevel.NumSubdivisions = N;
        ASSERT_TRUE( bevel.InitializeFromGroupTopologyEdges( mesh, topology, groupEdges ) ) << bevel.FailureReason;
        int junctions = 0;
        for ( const FMeshBevel::FBevelVertex& v : bevel.Vertices )
        {
            if ( v.VertexType != FMeshBevel::EBevelVertexType::JunctionVertex )
                continue;
            ++junctions;
            EXPECT_EQ( v.Wedges.Num(), 4 ) << "vertex " << v.VertexID;
            EXPECT_NEAR( Distance( mesh.GetVertex( v.VertexID ), glm::dvec3( 0, 0, 50 ) ), 0.0, 1e-9 );
        }
        EXPECT_EQ( junctions, 1 );
        ASSERT_TRUE( ApplyKeepingOldUVs( bevel, mesh ) ) << bevel.FailureReason;
        ExpectClosedSolid( mesh, 1.0e6 );
        // the centre polygon: an (N + 1) x (N + 1) quad grid
        for ( const FMeshBevel::FBevelVertex& v : bevel.Vertices )
            if ( v.VertexType == FMeshBevel::EBevelVertexType::JunctionVertex )
                EXPECT_EQ( v.NewTriangles.Num(), 2 * ( N + 1 ) * ( N + 1 ) );
        ExpectNewTriangleNormals( mesh, bevel );
        ExpectAllTrianglesOutward( mesh );
    }
}

namespace
{
    // UE's arc Hermite (tangents sqrt(2) x the right-angle square's sides) stays inside the true arc: radius r at
    // the ends, r (sqrt(2)/2 + 1/4) = 0.957 r at the middle.
    constexpr double kArcInnerRatio = 0.70710678118654752 + 0.25;
    constexpr double kPi            = std::numbers::pi;

    struct FRoundRun
    {
        FDynamicMesh3   Mesh;
        FMeshBevelProbe Bevel{};
        bool            bApplied = false;
    };

    void RunRound( FRoundRun& run, const TArray<int32_t>& groupEdges, int subdivisions, double roundWeight )
    {
        const FGroupTopology topology( &run.Mesh, true );
        run.Bevel.InsetDistance   = 5.0;
        run.Bevel.NumSubdivisions = subdivisions;
        run.Bevel.RoundWeight     = roundWeight;
        ASSERT_TRUE( groupEdges.IsEmpty()
                          ? run.Bevel.InitializeFromGroupTopology( run.Mesh, topology )
                          : run.Bevel.InitializeFromGroupTopologyEdges( run.Mesh, topology, groupEdges ) )
             << run.Bevel.FailureReason;
        run.bApplied = run.Bevel.Apply( run.Mesh );
    }

    // Distance of p to the line through a along the unit direction dir.
    double DistanceToLine( const glm::dvec3& p, const glm::dvec3& a, const glm::dvec3& dir )
    {
        const glm::dvec3 ap = p - a;
        return Distance( ap, glm::dot( ap, dir ) * dir );
    }

    void ExpectSameVertices( const FDynamicMesh3& a, const FDynamicMesh3& b )
    {
        ASSERT_EQ( a.MaxVertexID(), b.MaxVertexID() );
        for ( const int v : a.VertexIndicesItr() )
        {
            ASSERT_TRUE( b.IsVertex( v ) );
            const glm::dvec3 pa = a.GetVertex( v );
            const glm::dvec3 pb = b.GetVertex( v );
            EXPECT_EQ( pa.x, pb.x ) << "vertex " << v;
            EXPECT_EQ( pa.y, pb.y ) << "vertex " << v;
            EXPECT_EQ( pa.z, pb.z ) << "vertex " << v;
        }
    }
} // namespace

// One 90-degree cube edge, round: every strip column bends onto the arc of radius d tangent to both faces (centre
// = A + B - the original edge point), within the Hermite's 4.3 % inward bound; the solid lies strictly between the
// chamfer and the cube, and grows with the segment count.
TEST( MeshBevel, RoundEdgeColumnsLieOnTheArcAndVolumeGrowsWithSegments )
{
    const FDynamicMesh3   base = TangentCube( 1 );
    const FGroupTopology  baseTopology( &base, true );
    const TArray<int32_t> groupEdges = { GroupEdgeBetween( baseTopology, 1, 3 ) };
    const double          chamfer    = ChamferVolume( base, groupEdges );
    double                previous   = chamfer;
    for ( const int N : { 1, 2, 3, 5, 8 } )
    {
        SCOPED_TRACE( std::to_string( N ) + " subdivisions" );
        FRoundRun run{ base };
        RunRound( run, groupEdges, N, 1.0 );
        ASSERT_TRUE( run.bApplied ) << run.Bevel.FailureReason;
        EXPECT_EQ( CountBoundaryEdges( run.Mesh ), 0 );
        EXPECT_TRUE( run.Mesh.CheckValidity() );
        const FMeshBevel::FBevelEdge& edge  = run.Bevel.Edges[0];
        const glm::dvec3              e0    = edge.InitialPositions[0];
        const glm::dvec3              dir   = Normalized( edge.InitialPositions.Last() - e0 );
        const FQuadGridPatch&         patch = edge.StripQuadPatch;
        ASSERT_EQ( patch.NumVertexRows(), N + 2 );
        for ( int c = 0; c < patch.NumVertexCols(); ++c )
        {
            TArray<int32_t> column;
            ASSERT_TRUE( patch.GetVertexColumn( c, column ) );
            const glm::dvec3 a      = run.Mesh.GetVertex( column[0] );
            const glm::dvec3 b      = run.Mesh.GetVertex( column.Last() );
            const glm::dvec3 p0     = e0 + glm::dot( ( a - e0 ), dir ) * dir;
            const glm::dvec3 centre = a + b - p0;
            EXPECT_NEAR( Distance( a, centre ), 5.0, 1e-9 );
            EXPECT_NEAR( Distance( b, centre ), 5.0, 1e-9 );
            for ( int k = 1; k < column.Num() - 1; ++k )
            {
                const double r = Distance( run.Mesh.GetVertex( column[k] ), centre );
                EXPECT_LE( r, 5.0 + 1e-9 ) << "column " << c << " row " << k;
                EXPECT_GE( r, 5.0 * kArcInnerRatio - 1e-9 ) << "column " << c << " row " << k;
            }
        }
        const double volume = SignedVolume( run.Mesh );
        EXPECT_GT( volume, chamfer );
        EXPECT_LT( volume, 1.0e6 );
        EXPECT_GT( volume, previous );
        previous = volume;
        ExpectAllTrianglesOutward( run.Mesh );
    }
}

// The top loop, round: closed and outward, strictly between the chamfer and the cube, growing with the segments.
TEST( MeshBevel, RoundTopLoopLiesBetweenChamferAndCube )
{
    double previous = TopFaceChamferVolume( 5.0 );
    for ( const int N : { 1, 2, 3, 5 } )
    {
        SCOPED_TRACE( std::to_string( N ) + " subdivisions" );
        FRoundRun run{ TangentCube( 1 ) };
        for ( const int t : run.Mesh.TriangleIndicesItr() )
            run.Mesh.SetTriangleGroup( t, t / 2 == 4 ? 1 : 2 );
        RunRound( run, {}, N, 1.0 );
        ASSERT_EQ( run.Bevel.Loops.Num(), 1 );
        ASSERT_TRUE( run.bApplied ) << run.Bevel.FailureReason;
        EXPECT_EQ( CountBoundaryEdges( run.Mesh ), 0 );
        EXPECT_TRUE( run.Mesh.CheckValidity() );
        const double volume = SignedVolume( run.Mesh );
        EXPECT_GT( volume, previous );
        EXPECT_LT( volume, 1.0e6 );
        previous = volume;
        ExpectAllTrianglesOutward( run.Mesh );
    }
}

// A 4-edge junction on the top-front cube edge: the top and the +Y side each split at x = 0. Two strips are
// quarter-cylinders around the same axis, two are flat; the 4-sided patch between them must stay on that cylinder.
TEST( MeshBevel, RoundFourEdgeJunctionPatchStaysOnTheCylinder )
{
    for ( const int N : { 1, 2, 3 } )
    {
        SCOPED_TRACE( std::to_string( N ) + " subdivisions" );
        FRoundRun run{ TangentCube( 2 ) };
        for ( const int t : run.Mesh.TriangleIndicesItr() )
        {
            const glm::dvec3 c = run.Mesh.GetTriCentroid( t );
            if ( c.z > 49.0 )
                run.Mesh.SetTriangleGroup( t, c.x > 0.0 ? 8 : 7 );
            else if ( c.y > 49.0 )
                run.Mesh.SetTriangleGroup( t, c.x > 0.0 ? 10 : 9 );
        }
        const FGroupTopology  topology( &run.Mesh, true );
        const TArray<int32_t> groupEdges = {
             GroupEdgeBetween( topology, 7, 8 ), GroupEdgeBetween( topology, 7, 9 ),
             GroupEdgeBetween( topology, 8, 10 ), GroupEdgeBetween( topology, 9, 10 ) };
        const double chamfer = ChamferVolume( run.Mesh, groupEdges );
        RunRound( run, groupEdges, N, 1.0 );
        ASSERT_TRUE( run.bApplied ) << run.Bevel.FailureReason;
        EXPECT_EQ( CountBoundaryEdges( run.Mesh ), 0 );
        EXPECT_TRUE( run.Mesh.CheckValidity() );
        int patches = 0;
        for ( const FMeshBevel::FBevelVertex& v : run.Bevel.Vertices )
        {
            if ( v.VertexType != FMeshBevel::EBevelVertexType::JunctionVertex )
                continue;
            ++patches;
            ASSERT_EQ( v.Wedges.Num(), 4 );
            ASSERT_EQ( v.InteriorVertices.Num(), N * N );
            // the X curves run along the cylinder axis (the flat strips): a patch vertex stays strictly between
            // the corners' x, or an end tangent of the blended X curve points the wrong way
            double cornerMinX = std::numeric_limits<double>::max();
            double cornerMaxX = -cornerMinX;
            for ( const int corner : v.InteriorBorderLoop )
            {
                cornerMinX = std::min( cornerMinX, run.Mesh.GetVertex( corner ).x );
                cornerMaxX = std::max( cornerMaxX, run.Mesh.GetVertex( corner ).x );
            }
            ASSERT_LT( cornerMinX, cornerMaxX );
            for ( const FMeshBevel::FBevelVertex_InteriorVertex& iv : v.InteriorVertices )
            {
                const glm::dvec3 p = run.Mesh.GetVertex( iv.VertexID );
                const double     r = DistanceToLine( p, glm::dvec3( 0, 45, 45 ), glm::dvec3( 1, 0, 0 ) );
                EXPECT_LE( r, 5.0 + 1e-9 ) << "vertex " << iv.VertexID;
                EXPECT_GE( r, 5.0 * kArcInnerRatio - 1e-9 ) << "vertex " << iv.VertexID;
                EXPECT_GT( p.x, cornerMinX ) << "vertex " << iv.VertexID;
                EXPECT_LT( p.x, cornerMaxX ) << "vertex " << iv.VertexID;
            }
        }
        EXPECT_EQ( patches, 1 );
        const double volume = SignedVolume( run.Mesh );
        EXPECT_GT( volume, chamfer );
        EXPECT_LT( volume, 1.0e6 );
        ExpectAllTrianglesOutward( run.Mesh );
    }
}

// All 12 cube edges, round: each corner is a valence-3 junction filled with UE's PN triangle. Its interior
// vertices stay in the corner's octant around the inset corner, at a distance from it between the PN centre's
// (UE's b111 is "too flat": 0.849 r, the patch's closest point, pinned exactly where the centre is a lattice
// point) and r; the solid is closed, lies strictly between the chamfer and the cube, and grows with N.
TEST( MeshBevel, RoundCubeCornerPatchesLieNearTheSphere )
{
    const FDynamicMesh3  base = TangentCube( 1 );
    const FGroupTopology baseTopology( &base, true );
    TArray<int32_t>      allEdges;
    for ( int e = 0; e < baseTopology.Edges.Num(); ++e )
        allEdges.Add( e );
    ASSERT_EQ( allEdges.Num(), 12 );
    const double chamfer = ChamferVolume( base, allEdges );
    // PN centre (w = u = v = 1/3) for three unit corners and sqrt(2)-scaled right-angle border tangents:
    // b300/27 + 3 (sum of edge points)/27 + 6 b111/27 per axis, times sqrt(3)
    const double edgeSum     = 2.0 + 2.0 * std::numbers::sqrt2 / 3.0;
    const double b111        = 1.0 / 3.0 + 1.5 * std::numbers::sqrt2 / 9.0;
    const double centreRatio = std::numbers::sqrt3 * ( 1.0 / 27.0 + 3.0 * edgeSum / 27.0 + 6.0 * b111 / 27.0 );
    double       previous    = chamfer;
    for ( const int N : { 1, 2, 3, 5, 8 } )
    {
        SCOPED_TRACE( std::to_string( N ) + " subdivisions" );
        FRoundRun run{ base };
        RunRound( run, allEdges, N, 1.0 );
        ASSERT_TRUE( run.bApplied ) << run.Bevel.FailureReason;
        EXPECT_EQ( CountBoundaryEdges( run.Mesh ), 0 );
        EXPECT_TRUE( run.Mesh.CheckValidity() );
        int patches = 0;
        for ( const FMeshBevel::FBevelVertex& v : run.Bevel.Vertices )
        {
            if ( v.VertexType != FMeshBevel::EBevelVertexType::JunctionVertex )
                continue;
            ++patches;
            ASSERT_EQ( v.Wedges.Num(), 3 );
            ASSERT_EQ( v.InteriorBorderLoop.Num(), 3 );
            ASSERT_EQ( v.InteriorVertices.Num(), N * ( N - 1 ) / 2 );
            const glm::dvec3 corner = base.GetVertex( v.VertexID );
            const glm::dvec3 sign( corner.x > 0 ? 1.0 : -1.0, corner.y > 0 ? 1.0 : -1.0,
                                   corner.z > 0 ? 1.0 : -1.0 );
            const glm::dvec3 inset = corner - 5.0 * sign;
            for ( const int c : v.InteriorBorderLoop )
                EXPECT_NEAR( Distance( run.Mesh.GetVertex( c ), inset ), 5.0, 1e-9 );
            double closest = std::numeric_limits<double>::max();
            for ( const FMeshBevel::FBevelVertex_InteriorVertex& iv : v.InteriorVertices )
            {
                const glm::dvec3 q = run.Mesh.GetVertex( iv.VertexID ) - inset;
                EXPECT_GT( q.x * sign.x, 0.0 ) << "vertex " << iv.VertexID;
                EXPECT_GT( q.y * sign.y, 0.0 ) << "vertex " << iv.VertexID;
                EXPECT_GT( q.z * sign.z, 0.0 ) << "vertex " << iv.VertexID;
                const double r = Distance( run.Mesh.GetVertex( iv.VertexID ), inset );
                EXPECT_LE( r, 5.0 + 1e-9 ) << "vertex " << iv.VertexID;
                EXPECT_GE( r, 5.0 * centreRatio - 1e-9 ) << "vertex " << iv.VertexID;
                closest = std::min( closest, r );
            }
            if ( N % 3 == 2 )
                EXPECT_NEAR( closest, 5.0 * centreRatio, 1e-9 );
        }
        EXPECT_EQ( patches, 8 );
        const double volume = SignedVolume( run.Mesh );
        EXPECT_GT( volume, chamfer );
        EXPECT_LT( volume, 1.0e6 );
        EXPECT_GT( volume, previous );
        previous = volume;
        ExpectAllTrianglesOutward( run.Mesh );
    }
}

namespace
{
    // Regular n-gonal pyramid: base radius 50 at z = -25, apex at z = 50 (the origin is inside); lateral face i is
    // polygroup 1 + i, the base (a fan around its centre) n + 1.
    FDynamicMesh3 Pyramid( int n )
    {
        const glm::vec3        apex( 0.0f, 0.0f, 50.0f );
        std::vector<glm::vec3> ring;
        for ( int i = 0; i < n; ++i )
        {
            const double angle = 2.0 * kPi * i / n;
            ring.emplace_back( 50.0f * static_cast<float>( std::cos( angle ) ),
                               50.0f * static_cast<float>( std::sin( angle ) ), -25.0f );
        }
        std::vector<Vertex> vertices;
        std::vector<Index>  indices;
        auto                triangle = [&]( const glm::vec3& a, const glm::vec3& b, const glm::vec3& c )
        {
            const glm::vec3 normal = glm::normalize( glm::cross( b - a, c - a ) );
            const glm::vec3 t      = glm::normalize( b - a );
            const auto      first  = static_cast<uint32_t>( vertices.size() );
            vertices.push_back( MakeVertex( a, normal, t, { 0, 0 } ) );
            vertices.push_back( MakeVertex( b, normal, t, { 1, 0 } ) );
            vertices.push_back( MakeVertex( c, normal, t, { 0, 1 } ) );
            indices.push_back( { first, first + 1, first + 2 } );
        };
        for ( int i = 0; i < n; ++i )
            triangle( ring[i], ring[( i + 1 ) % n], apex );
        const glm::vec3 centre( 0.0f, 0.0f, -25.0f );
        for ( int i = 0; i < n; ++i )
            triangle( centre, ring[( i + 1 ) % n], ring[i] );
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
            mesh.SetTriangleGroup( t, t < n ? 1 + t : n + 1 );
        return mesh;
    }
} // namespace

// The apex of a regular 5- and 6-sided pyramid, all lateral edges round: a 5+-sided junction, UE's mean-value
// patch. Every lateral dihedral is theta, so the arc tangent to two neighbouring faces at their inset lines
// (d = 5 from the edge) is centred rho = d tan(theta / 2) from both, and the point C on the axis at rho from every
// face has each wedge vertex as its foot: the patch border runs on curves around C (corners exactly at rho). The
// patch is closed, n-fold symmetric (its centre on the axis), every interior vertex moves away from C relative to
// the flat patch, and stays within the border's radial range around C. The solid lies between the chamfer and the
// pyramid and grows with N; RoundWeight under the tolerance is the flat profile bit for bit.
TEST( MeshBevel, RoundFiveAndSixEdgeApexPatchesBulgeAroundTheInsetApex )
{
    for ( const int n : { 5, 6 } )
    {
        SCOPED_TRACE( std::to_string( n ) + "-sided pyramid" );
        const FDynamicMesh3  base = Pyramid( n );
        const FGroupTopology topology( &base, true );
        TArray<int32_t>      lateral;
        for ( int i = 0; i < n; ++i )
            lateral.Add( GroupEdgeBetween( topology, 1 + i, 1 + ( i + 1 ) % n ) );
        const double original = SignedVolume( base );
        const double chamfer  = ChamferVolume( base, lateral );
        // outward normals of two neighbouring lateral faces (edge midpoints at angles pi/n and 3 pi/n)
        const double H          = 75.0;
        const double a          = 50.0 * std::cos( kPi / n );
        auto         faceNormal = [&]( double angle )
        { return Normalized( glm::dvec3( H * std::cos( angle ), H * std::sin( angle ), a ) ); };
        const double     theta = kPi - std::acos( glm::dot( faceNormal( kPi / n ), faceNormal( 3.0 * kPi / n ) ) );
        const double    rho   = 5.0 * std::tan( theta / 2.0 );
        const glm::dvec3 centre( 0.0, 0.0, 50.0 - rho * std::sqrt( H * H + a * a ) / a );
        double          previous = chamfer;
        for ( const int N : { 1, 2, 3, 5 } )
        {
            SCOPED_TRACE( std::to_string( N ) + " subdivisions" );
            FRoundRun run{ base };
            FRoundRun flat{ base };
            FRoundRun tiny{ base };
            RunRound( run, lateral, N, 1.0 );
            RunRound( flat, lateral, N, 0.0 );
            RunRound( tiny, lateral, N, 1e-9 );
            ASSERT_TRUE( run.bApplied ) << run.Bevel.FailureReason;
            ASSERT_TRUE( flat.bApplied && tiny.bApplied ) << flat.Bevel.FailureReason;
            ExpectSameVertices( flat.Mesh, tiny.Mesh );
            EXPECT_EQ( CountBoundaryEdges( run.Mesh ), 0 );
            EXPECT_TRUE( run.Mesh.CheckValidity() );
            const FMeshBevel::FBevelVertex* apex = nullptr;
            for ( const FMeshBevel::FBevelVertex& v : run.Bevel.Vertices )
            {
                if ( v.VertexType == FMeshBevel::EBevelVertexType::JunctionVertex )
                    apex = &v;
            }
            ASSERT_NE( apex, nullptr );
            ASSERT_EQ( apex->Wedges.Num(), n );
            ASSERT_EQ( apex->InteriorBorderLoop.Num(), n * ( N + 1 ) );
            ASSERT_FALSE( apex->InteriorVertices.IsEmpty() );
            for ( const FMeshBevel::FOneRingWedge& w : apex->Wedges )
                EXPECT_NEAR( Distance( run.Mesh.GetVertex( w.WedgeVertex ), centre ), rho, 1e-6 );
            double borderLow  = std::numeric_limits<double>::max();
            double borderHigh = 0.0;
            for ( const int b : apex->InteriorBorderLoop )
            {
                const double r = Distance( run.Mesh.GetVertex( b ), centre );
                borderLow      = std::min( borderLow, r );
                borderHigh     = std::max( borderHigh, r );
            }
            double onAxis = std::numeric_limits<double>::max();
            double low    = std::numeric_limits<double>::max();
            double high   = 0.0;
            for ( const FMeshBevel::FBevelVertex_InteriorVertex& iv : apex->InteriorVertices )
            {
                const glm::dvec3 p = run.Mesh.GetVertex( iv.VertexID );
                const double    r = Distance( p, centre );
                EXPECT_GT( r, Distance( flat.Mesh.GetVertex( iv.VertexID ), centre ) + 1e-3 )
                     << "vertex " << iv.VertexID;
                EXPECT_GT( p.z, centre.z ) << "vertex " << iv.VertexID;
                onAxis = std::min( onAxis, std::hypot( p.x, p.y ) );
                low    = std::min( low, r );
                high   = std::max( high, r );
            }
            std::cout << std::setprecision( 9 ) << "n " << n << " N " << N << " rho " << rho << " border ["
                      << borderLow / rho << ", " << borderHigh / rho << "] patch [" << low / rho << ", "
                      << high / rho << "] axis " << onAxis << "\n";
            // float UVs from an iterative spectral solve: symmetric to ~1e-2 cm
            EXPECT_LT( onAxis, 2e-2 );
            // The patch radii are PINNED, not bounded: no closed form exists (the blend runs on spectral-conformal
            // UVs), and an envelope hides regressions: mean-value weights replaced by uniform ones overshoot only
            // 2-6 % past a 1.25 rho cap. UE's MVC blend itself overshoots its own border by ~20 % (its comment:
            // far border vertices exert too much influence); these are those values, measured (P13g3b) as
            // {nearest, farthest interior vertex} / rho per subdivision count. The tolerance covers the float UV
            // solve (the axis offset above is ~1e-2 cm, i.e. ~1e-3 rho sideways, second order in a radius).
            const std::map<std::pair<int, int>, std::pair<double, double>> pinned{
                 { { 5, 1 }, { 1.13618957, 1.19888555 } }, { { 5, 2 }, { 1.08152474, 1.19008789 } },
                 { { 5, 3 }, { 1.05407579, 1.18632232 } }, { { 5, 5 }, { 1.02923656, 1.18312904 } },
                 { { 6, 1 }, { 1.15199036, 1.21848439 } }, { { 6, 2 }, { 1.09608874, 1.21366289 } },
                 { { 6, 3 }, { 1.06595106, 1.21155562 } }, { { 6, 5 }, { 1.03718914, 1.20972736 } } };
            const std::pair<double, double> expected = pinned.at( { n, N } );
            EXPECT_GE( low, borderLow );
            EXPECT_NEAR( low / rho, expected.first, 5e-4 );
            EXPECT_NEAR( high / rho, expected.second, 5e-4 );
            const double volume = SignedVolume( run.Mesh );
            EXPECT_GT( volume, chamfer );
            EXPECT_LT( volume, original );
            EXPECT_GT( volume, previous );
            previous = volume;
            ExpectAllTrianglesOutward( run.Mesh );
        }
    }
}

// Five sectors meeting on a flat face: the round 5-sided patch across a 180-degree "dihedral" stays in the face.
// The volume is not the cube's: the sector edge running to the cube corner (50, 50, 50) ends in a terminator,
// which removes the corner vertex and fans the hole to its far neighbour (50, 50, 0) on the vertical crease (UE
// UnlinkTerminatorVertex + AppendTerminatorVertexTriangle), cutting off the pyramid over the corner polygon
// {corner, A, end column, B}. Its end column is rounded IN the top face: the corner and its inset positions A, B
// span the top plane, so the section plane is the top face, and UE's whole-mesh vertex normals at A and B (they
// lean into the side faces) project to +X and +Y. Every vertex stays on the cube; only the cut pyramid's base
// changes.
TEST( MeshBevel, RoundValenceFiveJunctionOnAFlatFaceStaysFlat )
{
    FDynamicMesh3 fan = TangentCube( 2 );
    for ( const int t : fan.TriangleIndicesItr() )
    {
        const glm::dvec3 c = fan.GetTriCentroid( t );
        if ( c.z > 49.0 )
        {
            const double angle = std::atan2( c.y, c.x ) + kPi;
            fan.SetTriangleGroup( t, 7 + std::min( 4, static_cast<int>( angle / ( 2.0 * kPi / 5.0 ) ) ) );
        }
    }
    const FGroupTopology topology( &fan, true );
    TArray<int32_t>      groupEdges;
    for ( int s = 0; s < 5; ++s )
        groupEdges.Add( GroupEdgeBetween( topology, 7 + s, 7 + ( s + 1 ) % 5 ) );
    FRoundRun run{ fan };
    RunRound( run, groupEdges, 2, 1.0 );
    ASSERT_TRUE( run.bApplied ) << run.Bevel.FailureReason;
    int patches = 0;
    for ( const FMeshBevel::FBevelVertex& v : run.Bevel.Vertices )
    {
        if ( v.VertexType != FMeshBevel::EBevelVertexType::JunctionVertex || v.Wedges.Num() != 5 )
            continue;
        ++patches;
        ASSERT_FALSE( v.InteriorVertices.IsEmpty() );
        for ( const FMeshBevel::FBevelVertex_InteriorVertex& iv : v.InteriorVertices )
            EXPECT_NEAR( run.Mesh.GetVertex( iv.VertexID ).z, 50.0, 1e-6 ) << "vertex " << iv.VertexID;
    }
    EXPECT_EQ( patches, 1 );
    EXPECT_EQ( CountBoundaryEdges( run.Mesh ), 0 );
    EXPECT_TRUE( run.Mesh.CheckValidity() );

    // The cut pyramid: apex (50, 50, 0), base in z = 50 bounded by the corner, A = (50, 50 - 5 sqrt 2), the end
    // column's Hermite points and B = (50 - 5 sqrt 2, 50). Tangents per MakeArcSplineCurve: A->B with its X part
    // removed, B->A with its Y part removed, both scaled by RoundWeight sqrt 2 (T1 negated). The flat run keeps
    // the triangle {corner, A, B} (area 25) for every N.
    const double s = 5.0 * std::numbers::sqrt2;
    for ( const int N : { 1, 2, 3 } )
    {
        for ( const double w : { 0.0, 1.0 } )
        {
            SCOPED_TRACE( "N " + std::to_string( N ) + " RoundWeight " + std::to_string( w ) );
            FRoundRun cut{ fan };
            RunRound( cut, groupEdges, N, w );
            ASSERT_TRUE( cut.bApplied ) << cut.Bevel.FailureReason;
            for ( const int vid : cut.Mesh.VertexIndicesItr() )
            {
                const glm::dvec3 p = cut.Mesh.GetVertex( vid );
                EXPECT_NEAR( std::max( { std::abs( p.x ), std::abs( p.y ), std::abs( p.z ) } ), 50.0, 1e-9 )
                     << "vertex " << vid << " left the cube";
            }
            const glm::dvec3        A( 50.0, 50.0 - s, 50.0 );
            const glm::dvec3        B( 50.0 - s, 50.0, 50.0 );
            const glm::dvec3        T0 = w * std::numbers::sqrt2 * glm::dvec3( 0.0, s, 0.0 );
            const glm::dvec3        T1 = -w * std::numbers::sqrt2 * glm::dvec3( s, 0.0, 0.0 );
            std::vector<glm::dvec3> base{ glm::dvec3( 50.0, 50.0, 50.0 ) };
            for ( int k = 0; k <= N + 1; ++k )
            {
                const double t = static_cast<double>( k ) / static_cast<double>( N + 1 );
                base.push_back( ( 2 * t * t * t - 3 * t * t + 1 ) * A + ( t * t * t - 2 * t * t + t ) * T0 +
                                ( -2 * t * t * t + 3 * t * t ) * B + ( t * t * t - t * t ) * T1 );
            }
            double area2 = 0.0;
            for ( size_t i = 0; i < base.size(); ++i )
            {
                const glm::dvec3& p = base[i];
                const glm::dvec3& q = base[( i + 1 ) % base.size()];
                area2 += p.x * q.y - q.x * p.y;
            }
            const double area = std::abs( area2 ) / 2.0;
            if ( w == 0.0 )
                EXPECT_NEAR( area, 25.0, 1e-9 );
            else
                EXPECT_LT( area, 25.0 );
            EXPECT_NEAR( SignedVolume( cut.Mesh ), 1.0e6 - 50.0 * area / 3.0, 1e-6 );
        }
    }
}

// RoundWeight 0 (the default) is the flat profile bit for bit: an explicit 0 and a value under the tolerance give
// the default's vertices exactly, and with no subdivisions RoundWeight is ignored.
TEST( MeshBevel, RoundWeightZeroIsTheFlatProfileBitExact )
{
    for ( const int N : { 0, 2, 3 } )
    {
        SCOPED_TRACE( std::to_string( N ) + " subdivisions" );
        FRoundRun reference{ TangentCube( 2 ) };
        {
            const FGroupTopology topology( &reference.Mesh, true );
            reference.Bevel.InsetDistance   = 5.0;
            reference.Bevel.NumSubdivisions = N;
            ASSERT_TRUE( reference.Bevel.InitializeFromGroupTopology( reference.Mesh, topology ) );
            ASSERT_TRUE( reference.Bevel.Apply( reference.Mesh ) ) << reference.Bevel.FailureReason;
        }
        for ( const double w : { 0.0, 1e-9 } )
        {
            FRoundRun run{ TangentCube( 2 ) };
            RunRound( run, {}, N, w );
            ASSERT_TRUE( run.bApplied ) << run.Bevel.FailureReason;
            ExpectSameVertices( reference.Mesh, run.Mesh );
        }
        if ( N == 0 )
        {
            FRoundRun run{ TangentCube( 2 ) };
            RunRound( run, {}, 0, 1.0 );
            ASSERT_TRUE( run.bApplied ) << run.Bevel.FailureReason;
            ExpectSameVertices( reference.Mesh, run.Mesh );
        }
    }
}
