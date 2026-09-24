// Operations on a mesh element selection that still run on the EditMesh (EditMeshOperations.hpp): Delete and
// Offset on a cube. Extrude / Push-Pull / Inset / Outset are ported (OffsetInsetRegion suite). Topology is checked
// by formula, closedness by the edge table, orientation by signed volume and by the normal layer agreeing with the
// winding, and the refusals by name.

#include <gtest/gtest.h>

#include "OperationsTestSupport.hpp"

using namespace OperationsTest;

// ── Delete ──────────────────────────────────────────────────────────────────────────────────────────────

TEST( Delete, OpenEdgesAreExactlyTheRegionBoundary )
{
    for ( const bool cylinder : { false, true } )
    {
        const EditMesh mesh   = cylinder ? MakeCylinder12() : MakeCube();
        const auto     sel    = Select( mesh, ElementMode::PolyGroup, { GroupFacing( mesh, { 0, 1, 0 } ) } );
        const auto     region = TrianglesOf( mesh, sel );
        auto           result = DeleteSelection( mesh, sel );
        ASSERT_TRUE( result.IsSuccess() ) << result.GetError();
        const EditMesh& out = result.GetValue().Mesh;
        EXPECT_TRUE( Valid( out ) );
        EXPECT_EQ( out.TriangleCount(), mesh.TriangleCount() - static_cast<int>( region.size() ) );
        EXPECT_EQ( OpenEdges( out ), RegionBoundary( mesh, region ) ) << ( cylinder ? "cylinder" : "cube" );
        EXPECT_TRUE( result.GetValue().Selection.Empty() );
        // Vertices only the region used are gone (the cylinder cap's centre); the rim stays.
        for ( const int v : out.VertexIds() )
            EXPECT_FALSE( out.GetVertexTriangles( v ).empty() ) << "isolated vertex " << v;
    }
}

TEST( Delete, VertexSelectionDeletesTheTrianglesItCovers )
{
    const EditMesh   cube = MakeCube();
    ElementSelection vertices( ElementMode::Vertex );
    for ( const int v : cube.VertexIds() )
        if ( cube.GetPosition( v ).y > 199.0f )
            ASSERT_TRUE( vertices.Add( cube, v ).IsSuccess() );
    ASSERT_EQ( vertices.Size(), 4 );
    auto result = DeleteSelection( cube, vertices );
    ASSERT_TRUE( result.IsSuccess() ) << result.GetError();
    EXPECT_EQ( result.GetValue().Mesh.TriangleCount(), 10 );
    EXPECT_EQ( OpenEdges( result.GetValue().Mesh ).size(), 4u );
}

// ── Offset ──────────────────────────────────────────────────────────────────────────────────────────────

TEST( Offset, MovesTheRegionWithoutTouchingTopology )
{
    const EditMesh cube   = MakeCube();
    const auto     sel    = Select( cube, ElementMode::PolyGroup, { GroupFacing( cube, { 0, 1, 0 } ) } );
    auto           result = OffsetSelection( cube, sel, 20.0f );
    ASSERT_TRUE( result.IsSuccess() ) << result.GetError();
    const EditMesh& out = result.GetValue().Mesh;
    ASSERT_EQ( out.VertexCount(), cube.VertexCount() );
    ASSERT_EQ( out.TriangleCount(), cube.TriangleCount() );
    ASSERT_EQ( out.EdgeCount(), cube.EdgeCount() );
    for ( const int t : cube.TriangleIds() )
        EXPECT_EQ( out.GetTriangle( t ), cube.GetTriangle( t ) );
    EXPECT_TRUE( ClosedAndConsistent( out ) );
    EXPECT_NEAR( SignedVolume( out ), 8.0e6 + 200.0 * 200.0 * 20.0, 10.0 );
    EXPECT_FLOAT_EQ( Top( out, result.GetValue().Selection ), 220.0f );
}

// ── refusals ────────────────────────────────────────────────────────────────────────────────────────────

TEST( Refusal, EmptyZeroAndUncovered )
{
    const EditMesh cube = MakeCube();
    const int      top  = GroupFacing( cube, { 0, 1, 0 } );
    const auto     sel  = Select( cube, ElementMode::PolyGroup, { top } );

    auto expectRefused = [&]( const Common::ResultStr<MeshEditOutcome>& r, const char* needle )
    {
        ASSERT_FALSE( r.IsSuccess() ) << needle;
        EXPECT_NE( r.GetError().find( needle ), std::string::npos ) << r.GetError();
    };
    expectRefused( DeleteSelection( cube, ElementSelection( ElementMode::Triangle ) ), "empty" );
    expectRefused( OffsetSelection( cube, sel, 0.0f ), "distance 0" );

    // An open mesh: delete the top, then offset a side, which reaches the hole.
    auto opened = DeleteSelection( cube, sel );
    ASSERT_TRUE( opened.IsSuccess() );
    const EditMesh& open = opened.GetValue().Mesh;
    const auto      side = Select( open, ElementMode::PolyGroup, { GroupFacing( open, { 1, 0, 0 } ) } );
    // Offset needs no boundary to stitch: it works on the open mesh.
    EXPECT_TRUE( OffsetSelection( open, side, 10.0f ).IsSuccess() );

    // Two vertices cover no triangle.
    const auto verts = Select( cube, ElementMode::Vertex, { 0, 1 } );
    expectRefused( DeleteSelection( cube, verts ), "cover no whole triangle" );
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
