// P10: the element selection on the ported core (FDynamicMesh3 + FGroupTopology) selects EXACTLY what the
// EditMesh path selects on the same mesh. Both meshes are built from the same vertex/triangle/group lists, so
// vertex, triangle and group IDs must agree as numbers; edge IDs are each core's own and are compared as the
// vertex pairs they name. Every seed element of every mode goes through Convert (to all four modes), Grow,
// Shrink and SelectConnected, and a fan of rays through PickElement in all four modes.

#include <gtest/gtest.h>

#include <Engine/Geometry/DynamicMeshSelection.hpp>
#include <Engine/Geometry/EditMesh.hpp>
#include <Engine/Geometry/UECore/DynamicMesh/DynamicMesh3.hpp>
#include <Engine/Geometry/UECore/DynamicMesh/GroupTopology.hpp>

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
        EditMesh       Old;
        FDynamicMesh3  New;
        FGroupTopology Topology;
        // Vertex / Edge picks against the EditMesh path's pixel-nearest rule (reported, not required equal).
        int PickSame          = 0;
        int PickOther         = 0;
        int PickMissAgreement = 0;
    };

    std::unique_ptr<Pair> Build( const Source& s )
    {
        auto p = std::make_unique<Pair>();
        p->New.EnableTriangleGroups();
        for ( const glm::vec3& v : s.Positions )
        {
            EXPECT_EQ( p->Old.AppendVertex( v ), p->New.AppendVertex( FVector3d( v.x, v.y, v.z ) ) );
        }
        for ( size_t t = 0; t < s.Triangles.size(); ++t )
        {
            const auto& tri  = s.Triangles[t];
            int         oldT = -1;
            (void)p->Old.AppendTriangle( tri[0], tri[1], tri[2], oldT );
            const int newT = p->New.AppendTriangle( FIndex3i( tri[0], tri[1], tri[2] ), s.Groups[t] );
            EXPECT_EQ( oldT, newT );
            p->Old.Attributes().SetPolyGroup( oldT, s.Groups[t] );
        }
        p->Topology = FGroupTopology( &p->New, true );
        return p;
    }

    // A selection as comparable values: IDs, except edges, which are their sorted vertex pairs.
    std::set<std::pair<int, int>> OldValues( const EditMesh& m, const ElementSelection& s )
    {
        std::set<std::pair<int, int>> out;
        for ( const int id : s.Ids() )
        {
            if ( s.Mode() != ElementMode::Edge )
            {
                out.insert( { id, -1 } );
                continue;
            }
            const auto& ev = m.GetEdgeVertices( id );
            out.insert( { std::min( ev[0], ev[1] ), std::max( ev[0], ev[1] ) } );
        }
        return out;
    }

    std::set<std::pair<int, int>> NewValues( const FDynamicMesh3& m, const ElementSelection& s )
    {
        std::set<std::pair<int, int>> out;
        for ( const int id : s.Ids() )
        {
            if ( s.Mode() != ElementMode::Edge )
            {
                out.insert( { id, -1 } );
                continue;
            }
            const FIndex2i ev = m.GetEdgeV( id );
            out.insert( { std::min( ev.A, ev.B ), std::max( ev.A, ev.B ) } );
        }
        return out;
    }

    constexpr ElementMode kModes[] = { ElementMode::Vertex, ElementMode::Edge, ElementMode::Triangle,
                                       ElementMode::PolyGroup };

    // Every single-element seed of every mode, both cores, through every operation. Returns the comparisons made.
    int CompareOperations( Pair& p )
    {
        int compared = 0;
        for ( const ElementMode mode : kModes )
        {
            std::vector<std::pair<int, int>> seeds; // (old id, new id)
            if ( mode == ElementMode::Edge )
                for ( const int e : p.Old.EdgeIds() )
                {
                    const auto& ev = p.Old.GetEdgeVertices( e );
                    seeds.push_back( { e, p.New.FindEdge( ev[0], ev[1] ) } );
                }
            else if ( mode == ElementMode::Vertex )
                for ( const int v : p.Old.VertexIds() )
                    seeds.push_back( { v, v } );
            else if ( mode == ElementMode::Triangle )
                for ( const int t : p.Old.TriangleIds() )
                    seeds.push_back( { t, t } );
            else
                for ( const auto& group : p.Topology.Groups )
                    seeds.push_back( { group.GroupID, group.GroupID } );
            for ( const auto& [oldId, newId] : seeds )
            {
                ElementSelection oldSel( mode );
                ElementSelection newSel( mode );
                EXPECT_TRUE( oldSel.Add( p.Old, oldId ).IsSuccess() );
                EXPECT_TRUE( newSel.Add( p.New, newId ).IsSuccess() );
                const auto check = [&]( const ElementSelection& o, const ElementSelection& n )
                {
                    EXPECT_EQ( o.Mode(), n.Mode() );
                    EXPECT_EQ( OldValues( p.Old, o ), NewValues( p.New, n ) )
                         << ToString( mode ) << " seed " << oldId << " -> " << ToString( o.Mode() );
                    ++compared;
                };
                for ( const ElementMode target : kModes )
                    check( ConvertSelection( p.Old, oldSel, target ),
                           ConvertSelection( p.New, p.Topology, newSel, target ) );
                check( GrowSelection( p.Old, oldSel ), GrowSelection( p.New, p.Topology, newSel ) );
                check( ShrinkSelection( p.Old, GrowSelection( p.Old, oldSel ) ),
                       ShrinkSelection( p.New, p.Topology, GrowSelection( p.New, p.Topology, newSel ) ) );
                check( SelectConnected( p.Old, oldSel ), SelectConnected( p.New, p.Topology, newSel ) );
            }
        }
        return compared;
    }

    // A fan of rays from outside the mesh at its centre region, in all four modes.
    int ComparePicks( Pair& p, const glm::vec3& eye, const glm::vec3& centre )
    {
        const glm::mat4 viewProj = glm::perspective( glm::radians( 60.0f ), 1.0f, 1.0f, 10000.0f ) *
                                   glm::lookAt( eye, centre, glm::vec3( 0.0f, 1.0f, 0.0f ) );
        const glm::mat4 inverse = glm::inverse( viewProj );
        int             hits    = 0;
        for ( int y = 1; y < 16; ++y )
            for ( int x = 1; x < 16; ++x )
            {
                PickView view;
                view.ViewProj     = viewProj;
                view.ViewportSize = { 512.0f, 512.0f };
                view.Cursor       = { 32.0f * x, 32.0f * y };
                const glm::vec2 ndc( view.Cursor.x / 256.0f - 1.0f, 1.0f - view.Cursor.y / 256.0f );
                glm::vec4       far4 = inverse * glm::vec4( ndc, 1.0f, 1.0f );
                view.RayOrigin       = eye;
                view.RayDirection    = glm::normalize( glm::vec3( far4 ) / far4.w - eye );
                for ( const ElementMode mode : kModes )
                {
                    const ElementHit o = PickElement( p.Old, mode, view );
                    const ElementHit n = PickElement( p.New, p.Topology, mode, view );
                    if ( mode == ElementMode::Vertex || mode == ElementMode::Edge )
                    {
                        // The ported pick (UE FindSelectedElement) chooses among the elements within tolerance
                        // by ray parameter (vertices) or ray-area metric (edges), not by pixel distance, and
                        // misses when that choice is occluded; what both rules share is the tolerance.
                        if ( n.IsHit() )
                        {
                            ++hits;
                            EXPECT_LE( n.PixelDistance, view.TolerancePixels ) << ToString( mode );
                        }
                        if ( n.IsHit() != o.IsHit() )
                            ++p.PickMissAgreement;
                        else if ( n.IsHit() )
                        {
                            ElementSelection os( mode );
                            ElementSelection ns( mode );
                            (void)os.Add( p.Old, o.Id );
                            (void)ns.Add( p.New, n.Id );
                            ( OldValues( p.Old, os ) == NewValues( p.New, ns ) ? p.PickSame : p.PickOther )++;
                        }
                        continue;
                    }
                    EXPECT_EQ( o.IsHit(), n.IsHit() ) << ToString( mode ) << " at " << x << "," << y;
                    if ( !o.IsHit() || !n.IsHit() )
                        continue;
                    ++hits;
                    ElementSelection os( mode );
                    ElementSelection ns( mode );
                    (void)os.Add( p.Old, o.Id );
                    (void)ns.Add( p.New, n.Id );
                    EXPECT_EQ( OldValues( p.Old, os ), NewValues( p.New, ns ) ) << ToString( mode );
                    EXPECT_FLOAT_EQ( o.RayT, n.RayT );
                }
            }
        return hits;
    }
} // namespace

TEST( DynamicMeshSelection, CubeSelectsWhatTheEditMeshPathSelects )
{
    auto p = Build( Cube() );
    ASSERT_EQ( p->Topology.Groups.Num(), 6 );
    EXPECT_EQ( CompareOperations( *p ), ( 8 + 18 + 12 + 6 ) * 7 );
    EXPECT_GT( ComparePicks( *p, { 180.0f, 140.0f, 220.0f }, { 0.0f, 0.0f, 0.0f } ), 50 );
    std::cout << "[ pick ] vertex/edge: same " << p->PickSame << ", other " << p->PickOther << ", hit/miss differ "
              << p->PickMissAgreement << "\n";
    EXPECT_GT( p->PickSame, p->PickOther );
}

TEST( DynamicMeshSelection, CylinderSelectsWhatTheEditMeshPathSelects )
{
    auto p = Build( Cylinder() );
    ASSERT_EQ( p->Topology.Groups.Num(), 3 );
    EXPECT_EQ( CompareOperations( *p ), ( 18 + 48 + 32 + 3 ) * 7 );
    EXPECT_GT( ComparePicks( *p, { 150.0f, 220.0f, 180.0f }, { 0.0f, 50.0f, 0.0f } ), 50 );
    std::cout << "[ pick ] vertex/edge: same " << p->PickSame << ", other " << p->PickOther << ", hit/miss differ "
              << p->PickMissAgreement << "\n";
    EXPECT_GT( p->PickSame, p->PickOther );
}

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
// tolerance, the one nearest ALONG THE RAY, where the EditMesh path took the one nearest the cursor in pixels.
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
    const ElementHit n   = PickElement( p->New, p->Topology, ElementMode::Vertex, view );
    ASSERT_TRUE( n.IsHit() );
    float nearest = std::numeric_limits<float>::infinity();
    for ( const int v : p->New.VertexIndicesItr() )
    {
        const FVector3d q = p->New.GetVertex( v );
        nearest           = std::min( nearest, glm::dot( glm::vec3( q.X, q.Y, q.Z ) - eye, view.RayDirection ) );
    }
    EXPECT_FLOAT_EQ( n.RayT, nearest );
}
