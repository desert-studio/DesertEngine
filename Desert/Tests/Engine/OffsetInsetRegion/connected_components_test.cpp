// FMeshConnectedComponents (seed-list path) and FMeshRegionBoundaryLoops' loop overlay map: the two pieces of
// GeometryCore that UE's FGroupEdgeInserter builds on (CreateNewGroups, DeleteGroupTrianglesAndGetLoop).
#include "Engine/Geometry/UECore/DynamicMesh/DynamicMeshAttributeSet.hpp"
#include "Engine/Geometry/UECore/MeshRegionBoundaryLoops.hpp"
#include "Engine/Geometry/UECore/Selections/MeshConnectedComponents.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

using namespace Desert::Geometry;

namespace
{
    // Four welded unit quads along +X (quad q = triangles 2q, 2q+1; quads 0-1 group 1, quads 2-3 group 2), plus
    // one detached quad (triangles 8, 9; group 3). Vertex (i, j) of the strip is 2i + j.
    FDynamicMesh3 Strip()
    {
        FDynamicMesh3 mesh;
        mesh.EnableTriangleGroups();
        for ( int i = 0; i <= 4; ++i )
        {
            mesh.AppendVertex( FVector3d( i, 0, 0 ) );
            mesh.AppendVertex( FVector3d( i, 1, 0 ) );
        }
        for ( int q = 0; q < 4; ++q )
        {
            const int a = 2 * q, b = 2 * q + 2, c = 2 * q + 3, d = 2 * q + 1;
            const int group = q < 2 ? 1 : 2;
            EXPECT_EQ( mesh.AppendTriangle( a, b, c, group ), 2 * q );
            EXPECT_EQ( mesh.AppendTriangle( a, c, d, group ), 2 * q + 1 );
        }
        const int a = mesh.AppendVertex( FVector3d( 10, 0, 0 ) );
        const int b = mesh.AppendVertex( FVector3d( 11, 0, 0 ) );
        const int c = mesh.AppendVertex( FVector3d( 11, 1, 0 ) );
        const int d = mesh.AppendVertex( FVector3d( 10, 1, 0 ) );
        EXPECT_EQ( mesh.AppendTriangle( a, b, c, 3 ), 8 );
        EXPECT_EQ( mesh.AppendTriangle( a, c, d, 3 ), 9 );
        return mesh;
    }

    std::vector<int> Sorted( const TArray<int>& ids )
    {
        std::vector<int> out( ids.begin(), ids.end() );
        std::sort( out.begin(), out.end() );
        return out;
    }
} // namespace

TEST( MeshConnectedComponents, OneComponentPerSeedNotAlreadyAbsorbed )
{
    const FDynamicMesh3      mesh = Strip();
    FMeshConnectedComponents components( &mesh );
    // Seed 3 lies in seed 0's component; it must not start a second one.
    components.FindTrianglesConnectedToSeeds( { 0, 3, 8 } );
    ASSERT_EQ( components.Num(), 2 );
    EXPECT_EQ( Sorted( components[0].Indices ), ( std::vector<int>{ 0, 1, 2, 3, 4, 5, 6, 7 } ) );
    EXPECT_EQ( Sorted( components[1].Indices ), ( std::vector<int>{ 8, 9 } ) );
}

TEST( MeshConnectedComponents, PredicateStopsGrowthAtAGroupBoundary )
{
    const FDynamicMesh3      mesh = Strip();
    FMeshConnectedComponents components( &mesh );
    components.FindTrianglesConnectedToSeeds(
         { 0, 7 },
         [&]( int32_t t0, int32_t t1 ) { return mesh.GetTriangleGroup( t0 ) == mesh.GetTriangleGroup( t1 ); } );
    ASSERT_EQ( components.Num(), 2 );
    EXPECT_EQ( Sorted( components[0].Indices ), ( std::vector<int>{ 0, 1, 2, 3 } ) );
    EXPECT_EQ( Sorted( components[1].Indices ), ( std::vector<int>{ 4, 5, 6, 7 } ) );
}

// GroupEdgeInserter.cpp:1085-1096: a new group edge splits group 1 in two because the predicate refuses to cross
// the path edges, while the group itself is unchanged.
TEST( MeshConnectedComponents, APathEdgeSplitsAGroupAsInCreateNewGroups )
{
    const FDynamicMesh3 mesh     = Strip();
    const int           pathEdge = mesh.FindEdge( 2, 3 );
    ASSERT_NE( pathEdge, FDynamicMesh3::InvalidID );
    FMeshConnectedComponents components( &mesh );
    components.FindTrianglesConnectedToSeeds( { 1, 2 },
                                              [&]( int32_t t0, int32_t t1 )
                                              {
                                                  return mesh.GetTriangleGroup( t0 ) ==
                                                              mesh.GetTriangleGroup( t1 ) &&
                                                         mesh.FindEdgeFromTriPair( t0, t1 ) != pathEdge;
                                              } );
    ASSERT_EQ( components.Num(), 2 );
    EXPECT_EQ( Sorted( components[0].Indices ), ( std::vector<int>{ 0, 1 } ) );
    EXPECT_EQ( Sorted( components[1].Indices ), ( std::vector<int>{ 2, 3 } ) );
}

TEST( MeshConnectedComponents, ARemovedOrOutOfRangeSeedStartsNothing )
{
    FDynamicMesh3 mesh = Strip();
    ASSERT_EQ( mesh.RemoveTriangle( 9 ), EMeshResult::Ok );
    FMeshConnectedComponents components( &mesh );
    components.FindTrianglesConnectedToSeeds( { 9, -1, 1000, 8 } );
    ASSERT_EQ( components.Num(), 1 );
    EXPECT_EQ( Sorted( components[0].Indices ), ( std::vector<int>{ 8 } ) );
}

namespace
{
    // The strip with one UV layer: UV = position, except that quad 1 (the region) gets its own four elements
    // offset by +10 when bSeamAroundRegion, so every region border vertex sits on a UV seam.
    FDynamicMesh3 StripWithUVs( bool bSeamAroundRegion )
    {
        FDynamicMesh3 mesh = Strip();
        mesh.EnableAttributes();
        FDynamicMeshUVOverlay* uv = mesh.Attributes()->GetUVLayer( 0 );
        std::vector<int>       shared;
        for ( int v = 0; v < 14; ++v )
        {
            const FVector3d p = mesh.GetVertex( v );
            shared.push_back( uv->AppendElement( FVector2f( float( p.X ), float( p.Y ) ) ) );
        }
        std::vector<int> own( 14, -1 );
        for ( int v : { 2, 3, 4, 5 } )
        {
            const FVector3d p = mesh.GetVertex( v );
            own[v]            = uv->AppendElement( FVector2f( float( p.X ) + 10.0f, float( p.Y ) + 10.0f ) );
        }
        for ( int t : mesh.TriangleIndicesItr() )
        {
            const FIndex3i tri      = mesh.GetTriangle( t );
            const bool     inRegion = bSeamAroundRegion && ( t == 2 || t == 3 );
            const FIndex3i elements = inRegion ? FIndex3i( own[tri.A], own[tri.B], own[tri.C] )
                                               : FIndex3i( shared[tri.A], shared[tri.B], shared[tri.C] );
            EXPECT_EQ( uv->SetTriangle( t, elements ), EMeshResult::Ok );
        }
        return mesh;
    }
} // namespace

TEST( MeshRegionBoundaryLoops, LoopOverlayMapTakesTheInsideTrianglesElementAcrossASeam )
{
    FDynamicMesh3                mesh = StripWithUVs( true );
    const FDynamicMeshUVOverlay& uv   = *mesh.Attributes()->GetUVLayer( 0 );
    FMeshRegionBoundaryLoops     loops( &mesh, { 2, 3 } );
    ASSERT_FALSE( loops.bFailed ) << loops.FailureReason;
    ASSERT_EQ( loops.Loops.Num(), 1 );
    ASSERT_EQ( loops.Loops[0].GetVertexCount(), 4 );

    FMeshRegionBoundaryLoops::VidOverlayMap<FVector2f> map;
    ASSERT_TRUE( loops.GetLoopOverlayMap( loops.Loops[0], uv, map ) );
    ASSERT_EQ( map.Num(), 4 );
    for ( int v : { 2, 3, 4, 5 } )
    {
        const auto* entry = map.Find( v );
        ASSERT_NE( entry, nullptr ) << "vertex " << v;
        const FVector3d p = mesh.GetVertex( v );
        EXPECT_EQ( uv.GetParentVertex( entry->Key ), v );
        EXPECT_FLOAT_EQ( entry->Value.X, float( p.X ) + 10.0f ) << "vertex " << v << " took the outside element";
        EXPECT_FLOAT_EQ( entry->Value.Y, float( p.Y ) + 10.0f ) << "vertex " << v;
    }

    // Deleting the region frees its own elements; the map must say so rather than point at a dead element.
    ASSERT_EQ( mesh.RemoveTriangle( 2 ), EMeshResult::Ok );
    ASSERT_EQ( mesh.RemoveTriangle( 3 ), EMeshResult::Ok );
    FMeshRegionBoundaryLoops::UpdateLoopOverlayMapValidity( map, uv );
    for ( int v : { 2, 3, 4, 5 } )
        EXPECT_EQ( map[v].Key, IndexConstants::InvalidID ) << "vertex " << v;
}

TEST( MeshRegionBoundaryLoops, LoopOverlayMapStaysValidWhereNeighboursShareTheElement )
{
    FDynamicMesh3                mesh = StripWithUVs( false );
    const FDynamicMeshUVOverlay& uv   = *mesh.Attributes()->GetUVLayer( 0 );
    FMeshRegionBoundaryLoops     loops( &mesh, { 2, 3 } );
    ASSERT_EQ( loops.Loops.Num(), 1 );
    FMeshRegionBoundaryLoops::VidOverlayMap<FVector2f> map;
    ASSERT_TRUE( loops.GetLoopOverlayMap( loops.Loops[0], uv, map ) );

    ASSERT_EQ( mesh.RemoveTriangle( 2 ), EMeshResult::Ok );
    ASSERT_EQ( mesh.RemoveTriangle( 3 ), EMeshResult::Ok );
    FMeshRegionBoundaryLoops::UpdateLoopOverlayMapValidity( map, uv );
    ASSERT_EQ( map.Num(), 4 );
    for ( int v : { 2, 3, 4, 5 } )
    {
        EXPECT_TRUE( uv.IsElement( map[v].Key ) ) << "vertex " << v;
        EXPECT_FLOAT_EQ( map[v].Value.X, float( mesh.GetVertex( v ).X ) ) << "vertex " << v;
    }
}

TEST( MeshRegionBoundaryLoops, LoopOverlayMapRefusesALoopOfAnotherRegion )
{
    const FDynamicMesh3            mesh = StripWithUVs( false );
    FMeshRegionBoundaryLoops       quad0( &mesh, { 0, 1 } );
    const FMeshRegionBoundaryLoops quad3( &mesh, { 6, 7 } );
    ASSERT_EQ( quad3.Loops.Num(), 1 );
    FMeshRegionBoundaryLoops::VidOverlayMap<FVector2f> map;
    EXPECT_FALSE( quad0.GetLoopOverlayMap( quad3.Loops[0], *mesh.Attributes()->GetUVLayer( 0 ), map ) );
}
