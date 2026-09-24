// Operations on a mesh element selection (EditMeshOperations.hpp): Delete, Extrude, Push/Pull, Offset,
// Inset/Outset on a cube and a cylinder. Topology is checked by formula, closedness by the edge table,
// orientation by signed volume and by the normal layer agreeing with the winding, and the refusals by name.

#include <gtest/gtest.h>

#include <EditMeshTestSupport.hpp>

#include <Engine/Geometry/EditMeshConversion.hpp>
#include <Engine/Geometry/EditMeshNormals.hpp>
#include <Engine/Geometry/EditMeshOperations.hpp>
#include <Engine/Geometry/EditMeshPolyGroups.hpp>
#include <Engine/Geometry/ShapeGenerators.hpp>

#include <algorithm>
#include <cmath>
#include <set>
#include <utility>

namespace
{
    using namespace Desert::Geometry;
    using EditMeshTest::Valid;

    // turnInsideOut reverses every triangle and its normal: MakeCylinder builds an inside-out solid (winding
    // and normals both face the axis), and an extrude test needs the outward one.
    EditMesh Import( ShapeMesh shape, bool turnInsideOut )
    {
        if ( turnInsideOut )
        {
            for ( auto& index : shape.Indices )
                std::swap( index.V2, index.V3 );
            for ( auto& vertex : shape.Vertices )
                vertex.Normal = -vertex.Normal;
        }
        RenderMeshData render;
        render.Vertices = shape.Vertices;
        render.Indices  = shape.Indices;
        auto imported   = FromRenderMesh( render );
        EXPECT_TRUE( imported.IsSuccess() ) << imported.GetError();
        EditMesh mesh = std::move( imported.GetValue().Mesh );
        return mesh;
    }

    double SignedVolume( const EditMesh& mesh )
    {
        double volume = 0.0;
        for ( const int t : mesh.TriangleIds() )
        {
            const auto& c = mesh.GetTriangle( t );
            volume += glm::dot( mesh.GetPosition( c[0] ),
                                glm::cross( mesh.GetPosition( c[1] ), mesh.GetPosition( c[2] ) ) ) /
                      6.0;
        }
        return volume;
    }

    double Area( const EditMesh& mesh, const std::vector<int>& triangles )
    {
        double area = 0.0;
        for ( const int t : triangles )
        {
            const auto& c = mesh.GetTriangle( t );
            area += 0.5 * glm::length( glm::cross( mesh.GetPosition( c[1] ) - mesh.GetPosition( c[0] ),
                                                   mesh.GetPosition( c[2] ) - mesh.GetPosition( c[0] ) ) );
        }
        return area;
    }

    // 200 cm box, base at y = 0: x, z in [-100, 100], y in [0, 200]; six polygroups, wound outwards.
    EditMesh MakeCube()
    {
        EditMesh mesh = Import( MakeBox( glm::vec3( 200.0f ) ), false );
        EXPECT_EQ( GeneratePolyGroupsByAngle( mesh, 30.0f ), 6 );
        EXPECT_NEAR( SignedVolume( mesh ), 8.0e6, 1.0 );
        return mesh;
    }

    // 12 slices, diameter 200, height 200: wall + two caps = three polygroups, turned outwards (the signed
    // volume is the 12-gon prism's, 30000 cm^2 x 200 cm).
    EditMesh MakeCylinder12()
    {
        EditMesh mesh = Import( MakeCylinder( 200.0f, 200.0f, 12 ), true );
        EXPECT_EQ( GeneratePolyGroupsByAngle( mesh, 45.0f ), 3 );
        EXPECT_NEAR( SignedVolume( mesh ), 6.0e6, 1.0 );
        return mesh;
    }

    // The polygroup whose triangles face `direction`.
    int GroupFacing( const EditMesh& mesh, const glm::vec3& direction )
    {
        for ( const int t : mesh.TriangleIds() )
            if ( glm::dot( TriangleNormal( mesh, t ), direction ) > 0.99f )
                return mesh.Attributes().GetPolyGroup( t );
        ADD_FAILURE() << "no triangle faces the direction";
        return InvalidId;
    }

    ElementSelection Select( const EditMesh& mesh, ElementMode mode, std::initializer_list<int> ids )
    {
        ElementSelection selection( mode );
        for ( const int id : ids )
            EXPECT_TRUE( selection.Add( mesh, id ).IsSuccess() );
        return selection;
    }

    std::vector<int> TrianglesOf( const EditMesh& mesh, const ElementSelection& selection )
    {
        const ElementSelection triangles = ConvertSelection( mesh, selection, ElementMode::Triangle );
        return { triangles.Ids().begin(), triangles.Ids().end() };
    }

    std::set<std::pair<int, int>> OpenEdges( const EditMesh& mesh )
    {
        std::set<std::pair<int, int>> open;
        for ( const int e : mesh.EdgeIds() )
            if ( mesh.IsBoundaryEdge( e ) )
                open.insert( { mesh.GetEdgeVertices( e )[0], mesh.GetEdgeVertices( e )[1] } );
        return open;
    }

    // The region's boundary edges (vertex pairs) on the mesh it was selected on.
    std::set<std::pair<int, int>> RegionBoundary( const EditMesh& mesh, const std::vector<int>& triangles )
    {
        const std::set<int>           in( triangles.begin(), triangles.end() );
        std::set<std::pair<int, int>> boundary;
        for ( const int t : triangles )
            for ( const int e : mesh.GetTriangleEdges( t ) )
            {
                const auto& et    = mesh.GetEdgeTriangles( e );
                const int   other = et[0] == t ? et[1] : et[0];
                if ( other == InvalidId || !in.count( other ) )
                    boundary.insert( { mesh.GetEdgeVertices( e )[0], mesh.GetEdgeVertices( e )[1] } );
            }
        return boundary;
    }

    int GroupCount( const EditMesh& mesh )
    {
        std::set<int> groups;
        for ( const int t : mesh.TriangleIds() )
            groups.insert( mesh.Attributes().GetPolyGroup( t ) );
        return static_cast<int>( groups.size() );
    }

    // Closed, manifold, valid, and every corner's normal on the side the winding faces: with a positive
    // volume that is "outwards".
    ::testing::AssertionResult ClosedAndConsistent( const EditMesh& mesh )
    {
        if ( auto valid = Valid( mesh ); !valid )
            return valid;
        if ( !mesh.IsManifold() )
            return ::testing::AssertionFailure() << "a bowtie vertex";
        if ( const auto open = OpenEdges( mesh ); !open.empty() )
            return ::testing::AssertionFailure() << open.size() << " open edges, first (" << open.begin()->first
                                                 << ", " << open.begin()->second << ")";
        const NormalOverlay* normals = mesh.Attributes().Normals();
        for ( const int t : mesh.TriangleIds() )
        {
            if ( !normals->IsSetTriangle( t ) )
                return ::testing::AssertionFailure() << "triangle " << t << " has no normals";
            for ( const int e : normals->GetTriangle( t ) )
                if ( glm::dot( normals->GetElement( e ), TriangleNormal( mesh, t ) ) <= 0.0f )
                    return ::testing::AssertionFailure()
                           << "triangle " << t << " has a normal against its winding";
        }
        if ( auto render = ToRenderMesh( mesh ); !render.IsSuccess() )
            return ::testing::AssertionFailure() << "not renderable: " << render.GetError();
        return ::testing::AssertionSuccess();
    }

    float Top( const EditMesh& mesh, const ElementSelection& selection )
    {
        float y = -1e9f;
        for ( const int t : TrianglesOf( mesh, selection ) )
            for ( const int v : mesh.GetTriangle( t ) )
                y = std::max( y, mesh.GetPosition( v ).y );
        return y;
    }
} // namespace

// ── Extrude ─────────────────────────────────────────────────────────────────────────────────────────────

TEST( Extrude, CubeFaceByFormulaClosedAndOutwards )
{
    const EditMesh cube = MakeCube();
    const int      top  = GroupFacing( cube, { 0, 1, 0 } );
    const auto     sel  = Select( cube, ElementMode::PolyGroup, { top } );

    auto result = ExtrudeSelection( cube, sel, 50.0f, ExtrudeDirection::VertexNormals );
    ASSERT_TRUE( result.IsSuccess() ) << result.GetError();
    const MeshEditOutcome& out = result.GetValue();
    // Boundary of one face: Vb = Eb = 4. V + Vb, T + 2 Eb.
    EXPECT_EQ( out.Mesh.VertexCount(), 8 + 4 );
    EXPECT_EQ( out.Mesh.TriangleCount(), 12 + 2 * 4 );
    EXPECT_TRUE( ClosedAndConsistent( out.Mesh ) );
    EXPECT_NEAR( SignedVolume( out.Mesh ), 8.0e6 + 200.0 * 200.0 * 50.0, 10.0 );
    // Four side walls, each against a different outside group, each its own new group.
    EXPECT_EQ( GroupCount( out.Mesh ), 6 + 4 );
    // The selection follows the face: same group, now at 250 cm.
    EXPECT_EQ( out.Selection.Mode(), ElementMode::PolyGroup );
    EXPECT_TRUE( out.Selection.Contains( top ) );
    EXPECT_FLOAT_EQ( Top( out.Mesh, out.Selection ), 250.0f );
}

TEST( Extrude, CylinderCapGetsOneSmoothRing )
{
    const EditMesh cylinder = MakeCylinder12();
    const int      cap      = GroupFacing( cylinder, { 0, 1, 0 } );
    const auto     sel      = Select( cylinder, ElementMode::PolyGroup, { cap } );
    const auto     capTris  = TrianglesOf( cylinder, sel );
    const auto     rim      = RegionBoundary( cylinder, capTris );
    ASSERT_EQ( rim.size(), 12u );

    auto result = ExtrudeSelection( cylinder, sel, 40.0f, ExtrudeDirection::VertexNormals );
    ASSERT_TRUE( result.IsSuccess() ) << result.GetError();
    const MeshEditOutcome& out = result.GetValue();
    EXPECT_EQ( out.Mesh.VertexCount(), cylinder.VertexCount() + 12 );
    EXPECT_EQ( out.Mesh.TriangleCount(), cylinder.TriangleCount() + 2 * 12 );
    EXPECT_TRUE( ClosedAndConsistent( out.Mesh ) );
    EXPECT_NEAR( SignedVolume( out.Mesh ), SignedVolume( cylinder ) + Area( cylinder, capTris ) * 40.0, 50.0 );
    // The whole rim faces ONE outside group (the wall), so the new wall ring is one group.
    EXPECT_EQ( GroupCount( out.Mesh ), 3 + 1 );
}

TEST( PushPull, NegativeCutsAPocketThatStaysClosed )
{
    const EditMesh cube = MakeCube();
    const auto     sel  = Select( cube, ElementMode::PolyGroup, { GroupFacing( cube, { 0, 1, 0 } ) } );

    auto result = PushPullSelection( cube, sel, -80.0f );
    ASSERT_TRUE( result.IsSuccess() ) << result.GetError();
    const MeshEditOutcome& out = result.GetValue();
    EXPECT_EQ( out.Mesh.TriangleCount(), 20 );
    EXPECT_TRUE( ClosedAndConsistent( out.Mesh ) );
    EXPECT_NEAR( SignedVolume( out.Mesh ), 8.0e6 - 200.0 * 200.0 * 80.0, 10.0 );
    EXPECT_FLOAT_EQ( Top( out.Mesh, out.Selection ), 120.0f );
}

TEST( PushPull, PastTheFarSideIsStillOneDirection )
{
    // Region-normal push moves every vertex the same way: the walls stay parallel however deep it goes.
    const EditMesh cube   = MakeCube();
    const auto     sel    = Select( cube, ElementMode::PolyGroup, { GroupFacing( cube, { 1, 0, 0 } ) } );
    auto           result = PushPullSelection( cube, sel, 30.0f );
    ASSERT_TRUE( result.IsSuccess() ) << result.GetError();
    for ( const int t : TrianglesOf( result.GetValue().Mesh, result.GetValue().Selection ) )
        for ( const int v : result.GetValue().Mesh.GetTriangle( t ) )
            EXPECT_FLOAT_EQ( result.GetValue().Mesh.GetPosition( v ).x, 130.0f );
}

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

// ── Inset / Outset ──────────────────────────────────────────────────────────────────────────────────────

TEST( Inset, KeepsTheFaceAreaAndItsTexture )
{
    const EditMesh cube   = MakeCube();
    const int      top    = GroupFacing( cube, { 0, 1, 0 } );
    const auto     sel    = Select( cube, ElementMode::PolyGroup, { top } );
    const auto     before = TrianglesOf( cube, sel );

    auto result = InsetSelection( cube, sel, 30.0f );
    ASSERT_TRUE( result.IsSuccess() ) << result.GetError();
    const MeshEditOutcome& out = result.GetValue();
    EXPECT_EQ( out.Mesh.VertexCount(), 8 + 4 );
    EXPECT_EQ( out.Mesh.TriangleCount(), 12 + 2 * 4 );
    EXPECT_TRUE( ClosedAndConsistent( out.Mesh ) );
    EXPECT_NEAR( SignedVolume( out.Mesh ), 8.0e6, 10.0 );

    // Region + ring (everything facing up) covers the old face exactly; the region itself is 140 x 140.
    std::vector<int> up;
    for ( const int t : out.Mesh.TriangleIds() )
        if ( TriangleNormal( out.Mesh, t ).y > 0.99f )
            up.push_back( t );
    EXPECT_NEAR( Area( out.Mesh, up ), Area( cube, before ), 1e-2 );
    const auto region = TrianglesOf( out.Mesh, out.Selection );
    EXPECT_NEAR( Area( out.Mesh, region ), 140.0 * 140.0, 1e-2 );

    // The texture does not move: UV per unit length is the same in the region before and after.
    const UVOverlay* uvBefore = cube.Attributes().UV( 0 );
    const UVOverlay* uvAfter  = out.Mesh.Attributes().UV( 0 );
    ASSERT_NE( uvBefore, nullptr );
    auto uvArea = []( const UVOverlay* uv, const std::vector<int>& tris )
    {
        double area = 0.0;
        for ( const int t : tris )
        {
            const auto&     e  = uv->GetTriangle( t );
            const glm::vec2 d1 = uv->GetElement( e[1] ) - uv->GetElement( e[0] );
            const glm::vec2 d2 = uv->GetElement( e[2] ) - uv->GetElement( e[0] );
            area += 0.5 * std::abs( d1.x * d2.y - d2.x * d1.y );
        }
        return area;
    };
    EXPECT_NEAR( uvArea( uvAfter, region ) / Area( out.Mesh, region ),
                 uvArea( uvBefore, before ) / Area( cube, before ), 1e-9 );
    EXPECT_NEAR( uvArea( uvAfter, up ), uvArea( uvBefore, before ), 1e-6 );
}

TEST( Inset, WiderThanTheFaceIsRefused )
{
    const EditMesh cube   = MakeCube();
    const auto     sel    = Select( cube, ElementMode::PolyGroup, { GroupFacing( cube, { 0, 1, 0 } ) } );
    auto           result = InsetSelection( cube, sel, 150.0f );
    ASSERT_FALSE( result.IsSuccess() );
    EXPECT_NE( result.GetError().find( "Inset" ), std::string::npos ) << result.GetError();
}

TEST( Outset, GrowsTheRegion )
{
    const EditMesh cube   = MakeCube();
    const auto     sel    = Select( cube, ElementMode::PolyGroup, { GroupFacing( cube, { 0, 1, 0 } ) } );
    auto           result = OutsetSelection( cube, sel, 20.0f );
    ASSERT_TRUE( result.IsSuccess() ) << result.GetError();
    const MeshEditOutcome& out = result.GetValue();
    EXPECT_TRUE( Valid( out.Mesh ) );
    EXPECT_EQ( out.Mesh.TriangleCount(), 20 );
    EXPECT_NEAR( Area( out.Mesh, TrianglesOf( out.Mesh, out.Selection ) ), 240.0 * 240.0, 1e-2 );
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

TEST( Refusal, EmptyZeroOpenClosedAndUncovered )
{
    const EditMesh cube = MakeCube();
    const int      top  = GroupFacing( cube, { 0, 1, 0 } );
    const auto     sel  = Select( cube, ElementMode::PolyGroup, { top } );

    auto expectRefused = [&]( const Common::ResultStr<MeshEditOutcome>& r, const char* needle )
    {
        ASSERT_FALSE( r.IsSuccess() ) << needle;
        EXPECT_NE( r.GetError().find( needle ), std::string::npos ) << r.GetError();
    };
    expectRefused( ExtrudeSelection( cube, ElementSelection( ElementMode::Triangle ), 10.0f,
                                     ExtrudeDirection::VertexNormals ),
                   "empty" );
    expectRefused( ExtrudeSelection( cube, sel, 0.0f, ExtrudeDirection::VertexNormals ), "distance 0" );
    expectRefused( PushPullSelection( cube, sel, 0.0f ), "distance 0" );
    expectRefused( OffsetSelection( cube, sel, 0.0f ), "distance 0" );
    expectRefused( InsetSelection( cube, sel, 0.0f ), "distance 0" );
    expectRefused( ExtrudeSelection( cube, sel, -5.0f, ExtrudeDirection::VertexNormals ), "Push/Pull" );

    // The whole cube: no boundary to wall.
    ElementSelection all( ElementMode::Triangle );
    for ( const int t : cube.TriangleIds() )
        ASSERT_TRUE( all.Add( cube, t ).IsSuccess() );
    expectRefused( ExtrudeSelection( cube, all, 10.0f, ExtrudeDirection::VertexNormals ), "closed piece" );
    expectRefused( InsetSelection( cube, all, 10.0f ), "closed piece" );

    // An open mesh: delete the top, then extrude a side, which reaches the hole.
    auto opened = DeleteSelection( cube, sel );
    ASSERT_TRUE( opened.IsSuccess() );
    const EditMesh& open = opened.GetValue().Mesh;
    const auto      side = Select( open, ElementMode::PolyGroup, { GroupFacing( open, { 1, 0, 0 } ) } );
    expectRefused( ExtrudeSelection( open, side, 10.0f, ExtrudeDirection::VertexNormals ), "open border" );
    expectRefused( InsetSelection( open, side, 10.0f ), "open border" );
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
