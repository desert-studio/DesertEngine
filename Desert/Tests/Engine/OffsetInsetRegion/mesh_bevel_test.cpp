// FMeshBevel (ported from UE MeshBevel.cpp) on a welded cube with one polygroup per face: the topology build must
// classify each corner the way UE does (one edge -> two terminators whose end cap joins the perpendicular face,
// B:1061; all twelve edges -> eight junctions of three wedges), and the unlink phase must open every beveled edge
// into two boundary edges without moving anything.
#include "Engine/Geometry/DynamicMeshRenderConversion.hpp"
#include "Engine/Geometry/UECore/DynamicMesh/Operations/MeshBevel.hpp"

#include <gtest/gtest.h>

#include <glm/geometric.hpp>

#include <cmath>
#include <map>
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

    // The protected phases and their state are the port's contract with the later phases; the probe reads them.
    class FMeshBevelProbe : public FMeshBevel
    {
    public:
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
