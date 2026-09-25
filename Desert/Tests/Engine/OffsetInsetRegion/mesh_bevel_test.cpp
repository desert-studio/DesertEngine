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

#include <array>
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
    const FGroupTopology topology( &mesh, true );
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
    const FGroupTopology topology( &mesh, true );
    FMeshBevelProbe     bevel;
    EXPECT_FALSE( bevel.InitializeFromGroupTopologyEdges( mesh, topology, { 99 } ) );
    EXPECT_NE( bevel.FailureReason.find( "group edge 99" ), std::string::npos ) << bevel.FailureReason;
}

TEST( MeshBevel, UnlinkAllTwelveEdgesGivesEachFaceItsOwnCorners )
{
    FDynamicMesh3  mesh = TangentCube();
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
    FDynamicMesh3  mesh = TangentCube();
    const FGroupTopology topology( &mesh, true );
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
    const FGroupTopology topology( &mesh, true );

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
    const FGroupTopology topology( &mesh, true );
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
        if ( orig.X != 50.0 || orig.Y != 50.0 )
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
            const FVector3d a   = mesh.GetVertex( tri.A );
            const FVector3d b   = mesh.GetVertex( tri.B );
            const FVector3d c   = mesh.GetVertex( tri.C );
            volume -= a.Dot( b.Cross( c ) ) / 6.0;
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
            FVector2f a;
            FVector2f b;
            FVector2f c;
            uvs.GetTriElements( t, a, b, c );
            for ( const FVector2f& uv : { a, b, c } )
                EXPECT_TRUE( std::isfinite( uv.X ) && std::isfinite( uv.Y ) ) << "new triangle " << t;
            const double area = 0.5 * std::abs( static_cast<double>( b.X - a.X ) * ( c.Y - a.Y ) -
                                                static_cast<double>( b.Y - a.Y ) * ( c.X - a.X ) );
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
            const FVector3d faceN    = mesh.GetTriNormal( t );
            for ( int j = 0; j < 3; ++j )
            {
                const FVector3f n      = normals.GetElement( elements[j] );
                const double    cosine = faceN.X * n.X + faceN.Y * n.Y + faceN.Z * n.Z;
                EXPECT_NEAR( n.X * n.X + n.Y * n.Y + n.Z * n.Z, 1.0, 1e-5 ) << "new triangle " << t;
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
        std::map<int, std::array<FVector2f, 3>> before;
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
            std::array<FVector2f, 3> after;
            uvs->GetTriElements( t, after[0], after[1], after[2] );
            for ( int j = 0; j < 3; ++j )
            {
                EXPECT_EQ( after[j].X, corners[j].X ) << "old triangle " << t << " corner " << j;
                EXPECT_EQ( after[j].Y, corners[j].Y ) << "old triangle " << t << " corner " << j;
            }
        }
        return true;
    }

    // The end cap of the one beveled edge 1|3 at the top of the cube lies in face 5, at the bottom in face 6.
    int CapMaterial( const FVector3d& p )
    {
        return p.Z > 0.0 ? 50 : 60;
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
        FDynamicMesh3  mesh = TangentCube( n );
        const FGroupTopology topology( &mesh, true );
        const int      edge = GroupEdgeBetween( topology, 1, 3 );
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
                const FVector3d p = mesh.GetVertex( tri[j] );
                EXPECT_NEAR( std::hypot( p.X - 50.0, p.Y - 50.0 ), 5.0, 1e-9 ) << "strip vertex " << tri[j];
            }
        }
        for ( const FMeshBevel::FBevelVertex& v : bevel.Vertices )
        {
            ASSERT_EQ( v.VertexType, FMeshBevel::EBevelVertexType::TerminatorVertex );
            EXPECT_EQ( v.NewTriangles.Num(), 1 ) << "terminator " << v.VertexID;
            ASSERT_EQ( v.NewTriangles.Num(), 1 );
            // the top cap closes +Z (group 5), the bottom one -Z (group 6)
            const int group = mesh.GetTriangleGroup( v.NewTriangles[0] );
            EXPECT_EQ( group, mesh.GetVertex( v.VertexID ).Z > 0.0 ? 5 : 6 ) << "terminator " << v.VertexID;
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
        FDynamicMesh3  mesh = bInterleaved ? InterleavedTangentCube( n ) : TangentCube( n );
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
            const FVector3d normal = mesh.GetTriNormal( v.NewTriangles[0] );
            const FVector3d corner = mesh.GetTriCentroid( v.NewTriangles[0] );
            EXPECT_NEAR( normal.Dot( Normalized( corner ) ), 1.0, 1e-3 ) << "junction " << v.VertexID;
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
        FMeshBevelProbe bevel;
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
    FMeshBevelProbe bevel;
    bevel.MaterialIDMode = FMeshBevel::EMaterialIDMode::InferMaterialID;
    ASSERT_TRUE( bevel.InitializeFromGroupTopology( mesh, topology ) ) << bevel.FailureReason;
    ASSERT_TRUE( ApplyKeepingOldUVs( bevel, mesh ) ) << bevel.FailureReason;
    EXPECT_EQ( bevel.NewTriangles.Num(), 8 + 12 * 2 );
    for ( const FMeshBevel::FBevelVertex& v : bevel.Vertices )
    {
        const int gx = mesh.GetVertex( v.VertexID ).X > 0.0 ? 1 : 2;
        EXPECT_EQ( Material( mesh, v.NewTriangles[0] ), 10 * gx ) << "junction " << v.VertexID;
    }
}

// A refusal at initialization makes Apply refuse with the same reason and leave the mesh alone.
TEST( MeshBevel, ApplyAfterRefusedInitializationReturnsFalse )
{
    FDynamicMesh3  mesh = TangentCube();
    FGroupTopology topology( &mesh, true );
    FMeshBevel     bevel;
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
            const int face = t / ( 2 * n * n );
            mesh.SetTriangleGroup( t, face == 4 ? 1 : ( face == 5 ? 3 : 2 ) );
        }
        SetFaceMaterials( mesh );
        FGroupTopology  topology( &mesh, true );
        FMeshBevelProbe bevel;
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
            const int material = mesh.GetVertex( loop.MeshVertices[0] ).Z > 0.0 ? 10 : 20;
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
    FDynamicMesh3   mesh = TangentCube();
    FGroupTopology  topology( &mesh, true );
    FMeshBevelProbe bevel;
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
        EXPECT_EQ( mesh.GetTriangleGroup( v.NewTriangles[0] ), mesh.GetVertex( v.VertexID ).Z > 0.0 ? 5 : 6 );
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
        const FVector3d p = mesh.GetVertex( v );
        if ( p.Z > 0.0 )
            mesh.SetVertex( v, FVector3d( p.X, p.Y, 50.0 + 0.5 * p.X ) );
    }
    ExpectClosedSolid( mesh, 1.0e6 );
    FGroupTopology  topology( &mesh, true );
    FMeshBevelProbe bevel;
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
                const FVector3d p = mesh.GetVertex( tri[j] );
                EXPECT_NEAR( std::hypot( p.X - 50.0, p.Y - 50.0 ), 5.0, 1e-9 ) << "strip vertex " << tri[j];
                if ( p.Z > 0.0 )
                    EXPECT_NEAR( p.Z, 50.0 + 0.5 * p.X, 1e-9 ) << "strip vertex " << tri[j];
            }
        }
    }
}
