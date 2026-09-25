// FMeshBevel (ported from UE MeshBevel.cpp) on a welded cube with one polygroup per face: the topology build must
// classify each corner the way UE does (one edge -> two terminators whose end cap joins the perpendicular face,
// B:1061; all twelve edges -> eight junctions of three wedges), and the unlink phase must open every beveled edge
// into two boundary edges without moving anything. Displacement then insets every unlinked vertex InsetDistance
// into its own face. The subdivided cube (each face 2x2 quads) gives spans of two mesh edges, whose middle vertex
// is split by the span interior unlink (ReconcileTriangleSets) before the terminator and junction unlinks.
#include "Engine/Geometry/DynamicMeshRenderConversion.hpp"
#include "Engine/Geometry/UECore/DynamicMesh/Operations/MeshBevel.hpp"

#include <gtest/gtest.h>

#include <glm/geometric.hpp>

#include <cmath>
#include <map>
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
            glm::vec3 N, U, V;
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
    void ExpectEveryVertexInsetIntoItsFace( const FDynamicMesh3& mesh, const std::map<int, FVector3d>& before,
                                            double w )
    {
        for ( const int v : mesh.VertexIndicesItr() )
        {
            std::set<int> groups;
            for ( const int t : mesh.VtxTrianglesItr( v ) )
                groups.insert( mesh.GetTriangleGroup( t ) );
            ASSERT_EQ( groups.size(), 1u ) << "vertex " << v << " is not on a single face island";
            const int       group = *groups.begin();
            const FVector3d orig  = before.at( v );
            const FVector3d p     = mesh.GetVertex( v );
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

    std::map<int, FVector3d> Positions( const FDynamicMesh3& mesh )
    {
        std::map<int, FVector3d> positions;
        for ( const int v : mesh.VertexIndicesItr() )
            positions[v] = mesh.GetVertex( v );
        return positions;
    }

    // Unlink only re-indexes: every vertex still sits on one of the cube's corners.
    void ExpectPositionsAreCubeCorners( const FDynamicMesh3& mesh )
    {
        for ( const int v : mesh.VertexIndicesItr() )
        {
            const FVector3d p = mesh.GetVertex( v );
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
            for ( int32 i = 0; i < e.MeshEdges.Num(); ++i )
            {
                const int e0 = e.MeshEdges[i];
                const int e1 = e.NewMeshEdges[i];
                EXPECT_NE( e0, e1 ) << "bevel edge " << e.EdgeIndex;
                EXPECT_TRUE( mesh.IsBoundaryEdge( e0 ) ) << e0;
                EXPECT_TRUE( mesh.IsBoundaryEdge( e1 ) ) << e1;
                const int32* partner = bevel.MeshEdgePairs.Find( e0 );
                ASSERT_NE( partner, nullptr ) << e0;
                EXPECT_EQ( *partner, e1 );
            }
            for ( int32 i = 0; i < e.MeshVertices.Num(); ++i )
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
            const int32* back = bevel.MeshEdgePairs.Find( pair.second );
            ASSERT_NE( back, nullptr ) << pair.first << " -> " << pair.second;
            EXPECT_EQ( *back, pair.first );
        }
    }

    int GroupEdgeBetween( const FGroupTopology& topology, int groupA, int groupB )
    {
        for ( int32 i = 0; i < topology.Edges.Num(); ++i )
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
            for ( const int32 t : w.Triangles )
                EXPECT_TRUE( all.insert( t ).second ) << "triangle " << t << " is in two wedges of " << v.VertexID;
        }
        EXPECT_EQ( all, Ring( mesh, v.VertexID ) );
    }
} // namespace

// +X (group 1) / +Y (group 3) edge runs along Z: its top end caps into +Z (5), its bottom end into -Z (6).
TEST( MeshBevel, OneGroupEdgeEndsInTwoTerminatorsCappedByThePerpendicularFace )
{
    const FDynamicMesh3 mesh = TangentCube();
    FGroupTopology      topology( &mesh, true );
    const int           edge = GroupEdgeBetween( topology, 1, 3 );
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
        const int capGroup = mesh.GetVertex( v.VertexID ).Z > 0 ? 5 : 6;
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
    const FDynamicMesh3 mesh = TangentCube();
    FGroupTopology      topology( &mesh, true );

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
            for ( const int32 t : w.Triangles )
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
    const FDynamicMesh3 mesh = TangentCube();
    FGroupTopology      topology( &mesh, true );
    FMeshBevelProbe     bevel;
    EXPECT_FALSE( bevel.InitializeFromGroupTopologyEdges( mesh, topology, { 99 } ) );
    EXPECT_NE( bevel.FailureReason.find( "group edge 99" ), std::string::npos ) << bevel.FailureReason;
}

TEST( MeshBevel, UnlinkAllTwelveEdgesGivesEachFaceItsOwnCorners )
{
    FDynamicMesh3  mesh = TangentCube();
    FGroupTopology topology( &mesh, true );

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
    FDynamicMesh3  mesh = TangentCube();
    FGroupTopology topology( &mesh, true );
    const int      edge = GroupEdgeBetween( topology, 1, 3 );
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
    FDynamicMesh3  mesh = TangentCube();
    FGroupTopology topology( &mesh, true );

    FMeshBevelProbe bevel;
    bevel.InsetDistance = 5.0;
    ASSERT_TRUE( bevel.InitializeFromGroupTopology( mesh, topology ) ) << bevel.FailureReason;
    bevel.Unlink( mesh );
    const std::map<int, FVector3d> before = Positions( mesh );
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
        FGroupTopology topology( &mesh, true );

        FMeshBevelProbe bevel;
        bevel.InsetDistance = 7.0;
        ASSERT_TRUE( bevel.InitializeFromGroupTopology( mesh, topology ) ) << bevel.FailureReason;
        ASSERT_EQ( bevel.Edges.Num(), 12 );
        int flippedSpans = 0;
        for ( const FMeshBevel::FBevelEdge& e : bevel.Edges )
        {
            ASSERT_EQ( e.MeshEdges.Num(), n ) << "bevel edge " << e.EdgeIndex;
            std::set<int> firstSides;
            for ( const int32 me : e.MeshEdges )
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

        const std::map<int, FVector3d> before = Positions( mesh );
        bevel.DisplaceVertices( mesh );
        ASSERT_TRUE( bevel.FailureReason.empty() ) << bevel.FailureReason;
        ExpectEveryVertexInsetIntoItsFace( mesh, before, 7.0 );
    }
}

TEST( MeshBevel, SubdividedCubeOneEdgeTerminatorsSlideOntoTheirOwnFace )
{
    FDynamicMesh3  mesh = TangentCube( 2 );
    FGroupTopology topology( &mesh, true );
    const int      edge = GroupEdgeBetween( topology, 1, 3 );
    ASSERT_GE( edge, 0 );

    FMeshBevelProbe bevel;
    bevel.InsetDistance = 5.0;
    ASSERT_TRUE( bevel.InitializeFromGroupTopologyEdges( mesh, topology, { edge } ) ) << bevel.FailureReason;
    bevel.Unlink( mesh );
    ASSERT_TRUE( bevel.FailureReason.empty() ) << bevel.FailureReason;
    // the span middle splits once, each terminator once; the hole is the two span pairs plus both cap-face pairs
    EXPECT_EQ( mesh.VertexCount(), 29 );
    EXPECT_EQ( CountBoundaryEdges( mesh ), 8 );

    const std::map<int, FVector3d> before = Positions( mesh );
    bevel.DisplaceVertices( mesh );
    ASSERT_TRUE( bevel.FailureReason.empty() ) << bevel.FailureReason;

    // The beveled edge is x = y = 50 (faces +X, group 1, and +Y, group 3). Every vertex on it moves 5 into the one
    // of those faces it still touches; everything else stays.
    std::map<double, int> movedIntoX;
    std::map<double, int> movedIntoY;
    for ( const int v : mesh.VertexIndicesItr() )
    {
        const FVector3d orig = before.at( v );
        const FVector3d p    = mesh.GetVertex( v );
        if ( !( orig.X == 50.0 && orig.Y == 50.0 ) )
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
        EXPECT_DOUBLE_EQ( p.Z, orig.Z ) << "vertex " << v;
        if ( touchesX )
        {
            EXPECT_DOUBLE_EQ( p.X, 50.0 ) << "vertex " << v;
            EXPECT_NEAR( p.Y, 45.0, 1e-9 ) << "vertex " << v;
            ++movedIntoX[orig.Z];
        }
        else
        {
            EXPECT_NEAR( p.X, 45.0, 1e-9 ) << "vertex " << v;
            EXPECT_DOUBLE_EQ( p.Y, 50.0 ) << "vertex " << v;
            ++movedIntoY[orig.Z];
        }
    }
    const std::map<double, int> onePerHeight{ { -50.0, 1 }, { 0.0, 1 }, { 50.0, 1 } };
    EXPECT_EQ( movedIntoX, onePerHeight );
    EXPECT_EQ( movedIntoY, onePerHeight );
}
