// P10: the element selection on DynamicMesh3 + GroupTopology: pruning, and the ported picks (UE's corner and
// group-edge rules).

#include <gtest/gtest.h>

#include <Engine/Geometry/DynamicMeshSelection.hpp>
#include <Engine/Geometry/MeshCore/DynamicMesh/DynamicMesh3.hpp>
#include <Engine/Geometry/MeshCore/DynamicMesh/GroupTopology.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <set>
#include <utility>
#include <vector>

namespace
{
    using namespace Desert::Geometry;

    struct Source
    {
        std::vector<glm::vec3>          Positions;
        std::vector<std::array<int, 3>> Triangles;
        std::vector<int>                Groups;
    };

    Source Cube()
    {
        Source s;
        for ( int i = 0; i < 8; ++i )
            s.Positions.push_back(
                 { ( i & 1 ) ? 50.0f : -50.0f, ( i & 2 ) ? 50.0f : -50.0f, ( i & 4 ) ? 50.0f : -50.0f } );
        const int quads[6][4] = { { 0, 2, 3, 1 }, { 4, 5, 7, 6 }, { 0, 1, 5, 4 },
                                  { 2, 6, 7, 3 }, { 0, 4, 6, 2 }, { 1, 3, 7, 5 } };
        for ( int f = 0; f < 6; ++f )
        {
            s.Triangles.push_back( { quads[f][0], quads[f][1], quads[f][2] } );
            s.Triangles.push_back( { quads[f][0], quads[f][2], quads[f][3] } );
            s.Groups.push_back( f + 1 );
            s.Groups.push_back( f + 1 );
        }
        return s;
    }

    // Eight sides (one group), a fan cap top and bottom (a group each).
    Source Cylinder()
    {
        constexpr int kSides = 8;
        Source        s;
        for ( int ring = 0; ring < 2; ++ring )
            for ( int i = 0; i < kSides; ++i )
            {
                const float a = 6.2831853f * static_cast<float>( i ) / kSides;
                s.Positions.push_back( { 40.0f * std::cos( a ), ring ? 100.0f : 0.0f, 40.0f * std::sin( a ) } );
            }
        const int bottom = static_cast<int>( s.Positions.size() );
        s.Positions.push_back( { 0.0f, 0.0f, 0.0f } );
        const int top = static_cast<int>( s.Positions.size() );
        s.Positions.push_back( { 0.0f, 100.0f, 0.0f } );
        for ( int i = 0; i < kSides; ++i )
        {
            const int j = ( i + 1 ) % kSides;
            s.Triangles.push_back( { i, j, kSides + j } );
            s.Triangles.push_back( { i, kSides + j, kSides + i } );
            s.Groups.push_back( 1 );
            s.Groups.push_back( 1 );
            s.Triangles.push_back( { bottom, j, i } );
            s.Groups.push_back( 2 );
            s.Triangles.push_back( { top, kSides + i, kSides + j } );
            s.Groups.push_back( 3 );
        }
        return s;
    }

    struct Pair
    {
        DynamicMesh3  New;
        GroupTopology Topology;
    };

    std::unique_ptr<Pair> Build( const Source& s )
    {
        auto p = std::make_unique<Pair>();
        p->New.EnableTriangleGroups();
        for ( const glm::vec3& v : s.Positions )
        {
            (void)p->New.AppendVertex( glm::dvec3( v.x, v.y, v.z ) );
        }
        for ( size_t t = 0; t < s.Triangles.size(); ++t )
        {
            const auto& tri = s.Triangles[t];
            EXPECT_GE( p->New.AppendTriangle( Index3i( tri[0], tri[1], tri[2] ), s.Groups[t] ), 0 );
        }
        p->Topology = GroupTopology( &p->New, true );
        return p;
    }

    // A selection as comparable values: IDs, except edges, which are their sorted vertex pairs.
} // namespace

TEST( DynamicMeshSelection, PruneDropsAGroupNoTriangleCarries )
{
    auto             p = Build( Cube() );
    ElementSelection sel( ElementMode::PolyGroup );
    ASSERT_TRUE( sel.Add( p->New, 3 ).IsSuccess() );
    EXPECT_FALSE( sel.Add( p->New, 42 ).IsSuccess() );
    (void)p->New.RemoveTriangle( 4 );
    (void)p->New.RemoveTriangle( 5 );
    const PruneReport report = sel.Prune( p->New );
    EXPECT_EQ( report.Missing, 1 );
    EXPECT_TRUE( sel.Empty() );
}

// The ported corner pick (UE DoCornerBasedSelection / FindNearestPointToRay) takes, of the vertices within
// tolerance, the one nearest ALONG THE RAY, not the one nearest the cursor in pixels.
TEST( DynamicMeshSelection, CornerPickPrefersNearerAlongTheRay )
{
    auto p = Build( Cube() );
    ASSERT_GT( p->New.VertexCount(), 0 );
    // Looking down a cube's corner diagonal from far away, several corners fall within a wide tolerance.
    const glm::vec3 eye( 400.0f, 400.0f, 400.0f );
    PickView        view;
    view.ViewProj = glm::perspective( glm::radians( 60.0f ), 1.0f, 1.0f, 10000.0f ) *
                    glm::lookAt( eye, glm::vec3( 0.0f ), glm::vec3( 0.0f, 1.0f, 0.0f ) );
    view.ViewportSize    = { 512.0f, 512.0f };
    view.Cursor          = { 256.0f, 256.0f };
    view.RayOrigin       = eye;
    view.RayDirection    = glm::normalize( -eye );
    view.TolerancePixels = 400.0f;
    const ElementHit n   = PickElement( p->New, p->Topology, ElementMode::Vertex, view, TopologyLevel::Triangle );
    ASSERT_TRUE( n.IsHit() );
    float nearest = std::numeric_limits<float>::infinity();
    for ( const int v : p->New.VertexIndicesItr() )
    {
        const glm::dvec3 q = p->New.GetVertex( v );
        nearest            = std::min( nearest, glm::dot( glm::vec3( q.x, q.y, q.z ) - eye, view.RayDirection ) );
    }
    EXPECT_FLOAT_EQ( n.RayT, nearest );
}

namespace
{
    // A 100 cm cube, each face one group split 2x2 into quads (two triangles each): group edges have two
    // segments, every face has a centre vertex and quad diagonals that lie INSIDE its group.
    Source SplitCube()
    {
        Source s;
        auto   vertex = [&s]( const glm::vec3& v )
        {
            for ( size_t i = 0; i < s.Positions.size(); ++i )
                if ( glm::length( s.Positions[i] - v ) < 1e-3f )
                    return static_cast<int>( i );
            s.Positions.push_back( v );
            return static_cast<int>( s.Positions.size() ) - 1;
        };
        for ( int axis = 0; axis < 3; ++axis )
            for ( int side = 0; side < 2; ++side )
            {
                const int u = ( axis + 1 ) % 3;
                const int w = ( axis + 2 ) % 3;
                for ( int i = 0; i < 2; ++i )
                    for ( int j = 0; j < 2; ++j )
                    {
                        int q[4];
                        for ( int k = 0; k < 4; ++k )
                        {
                            glm::vec3 p( 0.0f );
                            p[axis] = side ? 50.0f : -50.0f;
                            p[u]    = -50.0f + 50.0f * static_cast<float>( i + ( k == 1 || k == 2 ) );
                            p[w]    = -50.0f + 50.0f * static_cast<float>( j + ( k >= 2 ) );
                            q[k]    = vertex( p );
                        }
                        const bool flip = side == 1;
                        s.Triangles.push_back( { q[0], flip ? q[2] : q[1], flip ? q[1] : q[2] } );
                        s.Triangles.push_back( { q[0], flip ? q[3] : q[2], flip ? q[2] : q[3] } );
                        s.Groups.push_back( 1 + axis * 2 + side );
                        s.Groups.push_back( 1 + axis * 2 + side );
                    }
            }
        return s;
    }

    // A camera at `eye` whose ray goes through `target`.
    PickView ViewThrough( const glm::vec3& eye, const glm::vec3& target )
    {
        PickView view;
        view.ViewProj = glm::perspective( glm::radians( 60.0f ), 1.0f, 1.0f, 10000.0f ) *
                        glm::lookAt( eye, glm::vec3( 0.0f ), glm::vec3( 0.0f, 1.0f, 0.0f ) );
        view.ViewportSize = { 512.0f, 512.0f };
        EXPECT_TRUE(
             ProjectToViewport( target, view.ViewProj, view.ViewportPos, view.ViewportSize, view.Cursor ) );
        view.RayOrigin    = eye;
        view.RayDirection = glm::normalize( target - eye );
        return view;
    }

    glm::vec3 At( const DynamicMesh3& mesh, int v )
    {
        const glm::dvec3 p = mesh.GetVertex( v );
        return { static_cast<float>( p.x ), static_cast<float>( p.y ), static_cast<float>( p.z ) };
    }
} // namespace

// UE PolyEdit (FMeshTopologySelector over GroupTopology): an edge pick lands only on GROUP edges. Aimed at the
// middle of a diagonal inside the +Z face's group, the group level misses while the triangle level hits it.
TEST( DynamicMeshSelection, GroupEdgePickSkipsTheDiagonalInsideAGroup )
{
    auto p = Build( SplitCube() );
    ASSERT_EQ( static_cast<int32_t>( p->Topology.m_Groups.size() ), 6 );
    int diagonal = -1;
    for ( const int e : p->New.EdgeIndicesItr() )
    {
        const Index2i   ev = p->New.GetEdgeV( e );
        const glm::vec3 a  = At( p->New, ev.A );
        const glm::vec3 b  = At( p->New, ev.B );
        if ( a.z == 50.0f && b.z == 50.0f && a.x != b.x && a.y != b.y )
        {
            diagonal = e;
            ASSERT_LT( p->Topology.FindGroupEdgeID( e ), 0 ) << "a diagonal belongs to no group edge";
            break;
        }
    }
    ASSERT_GE( diagonal, 0 );
    const Index2i    ev   = p->New.GetEdgeV( diagonal );
    const glm::vec3  mid  = 0.5f * ( At( p->New, ev.A ) + At( p->New, ev.B ) );
    const PickView   view = ViewThrough( { 0.0f, 0.0f, 400.0f }, mid );
    const ElementHit tri  = PickElement( p->New, p->Topology, ElementMode::Edge, view, TopologyLevel::Triangle );
    ASSERT_TRUE( tri.IsHit() );
    EXPECT_EQ( tri.Id, diagonal );
    const ElementHit group = PickElement( p->New, p->Topology, ElementMode::Edge, view, TopologyLevel::Group );
    EXPECT_FALSE( group.IsHit() ) << "picked mesh edge " << group.Id;
}

// Aimed at a cube edge, the group level selects the whole GROUP edge: every mesh edge along the cube edge
// (found here by position, not through the topology) and nothing else.
TEST( DynamicMeshSelection, GroupEdgePickSelectsEveryMeshEdgeOfTheGroupEdge )
{
    auto          p = Build( SplitCube() );
    std::set<int> alongCubeEdge;
    for ( const int e : p->New.EdgeIndicesItr() )
    {
        const Index2i ev = p->New.GetEdgeV( e );
        if ( At( p->New, ev.A ).y == 50.0f && At( p->New, ev.A ).z == 50.0f && At( p->New, ev.B ).y == 50.0f &&
             At( p->New, ev.B ).z == 50.0f )
            alongCubeEdge.insert( e );
    }
    ASSERT_EQ( alongCubeEdge.size(), 2u );
    const PickView   view = ViewThrough( { 0.0f, 300.0f, 400.0f }, { -20.0f, 50.0f, 50.0f } );
    const ElementHit hit  = PickElement( p->New, p->Topology, ElementMode::Edge, view, TopologyLevel::Group );
    ASSERT_TRUE( hit.IsHit() );
    const std::vector<int> picked = HitElements( p->Topology, ElementMode::Edge, TopologyLevel::Group, hit );
    EXPECT_EQ( std::set<int>( picked.begin(), picked.end() ), alongCubeEdge );
    EXPECT_EQ( HitElements( p->Topology, ElementMode::Edge, TopologyLevel::Triangle, hit ).size(), 1u );
}

// A vertex pick at the group level lands only on CORNERS: the centre vertex of a face (inside its group) is
// not pickable there, while the triangle level picks it; a cube corner is picked as a group corner.
TEST( DynamicMeshSelection, GroupVertexPickIsAGroupCorner )
{
    auto p = Build( SplitCube() );
    ASSERT_EQ( static_cast<int32_t>( p->Topology.m_Corners.size() ), 8 );
    const glm::vec3  centre( 0.0f, 0.0f, 50.0f );
    const PickView   front = ViewThrough( { 0.0f, 0.0f, 400.0f }, centre );
    const ElementHit tri = PickElement( p->New, p->Topology, ElementMode::Vertex, front, TopologyLevel::Triangle );
    ASSERT_TRUE( tri.IsHit() );
    EXPECT_EQ( At( p->New, tri.Id ), centre );
    EXPECT_FALSE( PickElement( p->New, p->Topology, ElementMode::Vertex, front, TopologyLevel::Group ).IsHit() );

    const glm::vec3  corner( 50.0f, 50.0f, 50.0f );
    const PickView   view = ViewThrough( { 200.0f, 250.0f, 400.0f }, corner );
    const ElementHit hit  = PickElement( p->New, p->Topology, ElementMode::Vertex, view, TopologyLevel::Group );
    ASSERT_TRUE( hit.IsHit() );
    EXPECT_EQ( At( p->New, hit.Id ), corner );
    EXPECT_GE( p->Topology.GetCornerIDFromVertexID( hit.Id ), 0 );
}
