// The CubeGrid blockout core (Engine/Geometry/VoxelBlockout): the edits on the voxel volume and the bake, on
// fixed inputs. The bake is checked through properties that do not depend on the hash map's iteration order
// (which differs between standard libraries): quad and triangle counts, closedness (the area vectors of a
// closed surface sum to zero) and the enclosed volume (divergence theorem), which catches a missing, doubled
// or mis-placed face, a wrong corner height and a wrong cell size alike.
#include <Engine/Geometry/VoxelBlockout.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <map>
#include <tuple>
#include <vector>

using namespace Desert::Geometry::VoxelBlockout;

namespace
{
    struct Measure
    {
        glm::dvec3 AreaSum{ 0.0 };
        double     Volume = 0.0;
        double     Area   = 0.0;
    };

    Measure Measured( const Desert::Geometry::RenderMeshData& m )
    {
        Measure r;
        for ( const auto& t : m.Indices )
        {
            const glm::dvec3 a( m.Vertices[t.V1].Position );
            const glm::dvec3 b( m.Vertices[t.V2].Position );
            const glm::dvec3 c( m.Vertices[t.V3].Position );
            const glm::dvec3 n = glm::cross( b - a, c - a ) * 0.5;
            r.AreaSum += n;
            r.Area += glm::length( n );
            r.Volume += glm::dot( a, glm::cross( b, c ) ) / 6.0;
        }
        return r;
    }

    void ExpectClosed( const Measure& m )
    {
        EXPECT_NEAR( m.AreaSum.x, 0.0, 1e-3 );
        EXPECT_NEAR( m.AreaSum.y, 0.0, 1e-3 );
        EXPECT_NEAR( m.AreaSum.z, 0.0, 1e-3 );
    }

    // Directed welded edges that have no opposite partner, or that repeat.
    int NonConformingEdges( const Desert::Geometry::RenderMeshData& m )
    {
        using Key = std::tuple<long, long, long>;
        auto key  = []( const glm::vec3& p )
        { return Key( std::lround( p.x * 100.0f ), std::lround( p.y * 100.0f ), std::lround( p.z * 100.0f ) ); };
        std::map<std::pair<Key, Key>, int> directed;
        // Indices are submesh-local: every submesh's triangles index from its own VertexOffset.
        for ( const auto& sm : m.Submeshes )
            for ( uint32_t i = sm.IndexOffset / 3; i < ( sm.IndexOffset + sm.IndexCount ) / 3; ++i )
            {
                const auto& t    = m.Indices[i];
                const Key   k[3] = { key( m.Vertices[sm.VertexOffset + t.V1].Position ),
                                     key( m.Vertices[sm.VertexOffset + t.V2].Position ),
                                     key( m.Vertices[sm.VertexOffset + t.V3].Position ) };
                for ( int e = 0; e < 3; ++e )
                    ++directed[{ k[e], k[( e + 1 ) % 3] }];
            }
        int bad = 0;
        for ( const auto& [edge, count] : directed )
        {
            const auto back = directed.find( { edge.second, edge.first } );
            if ( count != 1 || back == directed.end() || back->second != 1 )
                ++bad;
        }
        return bad;
    }

    // A ground grid of 100 cm cells at the origin; u is Z, v is X.
    Volume Ground()
    {
        Volume v;
        v.m_Unit = 100.0f;
        return v;
    }
} // namespace

TEST( VoxelBlockout, PackRoundTripsNegativeCellsAndFloorDivRoundsDown )
{
    for ( const glm::ivec3 c :
          { glm::ivec3( 0 ), glm::ivec3( -1, 5, -700000 ), glm::ivec3( 1000000, -1000000, 3 ) } )
        EXPECT_EQ( Unpack( Pack( c ) ), c );
    EXPECT_NE( Pack( { 1, 0, 0 } ), Pack( { 0, 1, 0 } ) );
    EXPECT_EQ( FloorDiv( 7, 2 ), 3 );
    EXPECT_EQ( FloorDiv( -1, 2 ), -1 );
    EXPECT_EQ( FloorDiv( -4, 2 ), -2 );
    EXPECT_EQ( FloorDiv( -5, 2 ), -3 );
}

TEST( VoxelBlockout, OneCellBakesSixClosedQuads )
{
    Volume    v = Ground();
    WorkPlane plane;
    v.PushPull( plane, Rect{}, +1, 1, 0 );
    EXPECT_EQ( plane.Cell, 1 );
    ASSERT_EQ( v.m_Cells.size(), 1u );

    const auto mesh = v.Bake();
    EXPECT_EQ( mesh.Vertices.size(), 24u );
    EXPECT_EQ( mesh.Indices.size(), 12u );
    const Measure m = Measured( mesh );
    ExpectClosed( m );
    EXPECT_NEAR( m.Volume, 1.0e6, 1.0 ); // a 100 cm cube, and positive: the winding faces outward
    EXPECT_NEAR( m.Area, 6.0e4, 1e-2 );
    for ( const auto& vert : mesh.Vertices ) // UVs are world-aligned at one unit per metre
        EXPECT_LE( std::abs( vert.TexCoord.x ) + std::abs( vert.TexCoord.y ), 2.0f + 1e-5f );
}

TEST( VoxelBlockout, FlatWallIsGreedyMergedIntoSixQuads )
{
    Volume    v = Ground();
    WorkPlane plane;
    v.PushPull( plane, Rect{ 0, 3, 0, 1 }, +1, 2, 0 ); // 2 (X) x 2 (Y) x 4 (Z) cells
    EXPECT_EQ( v.m_Cells.size(), 16u );
    const auto mesh = v.Bake();
    EXPECT_EQ( mesh.Indices.size(), 12u );
    const Measure m = Measured( mesh );
    ExpectClosed( m );
    EXPECT_NEAR( m.Volume, 16.0e6, 16.0 );
}

TEST( VoxelBlockout, PullStacksOnTopOfExistingSolidAndPushRemovesUnderThePlane )
{
    Volume    v = Ground();
    WorkPlane plane;
    v.PushPull( plane, Rect{ 0, 1, 0, 0 }, +1, 1, 0 );
    // A second pull from the SAME (stale) plane must skip the occupied cell and stack above it.
    WorkPlane stale;
    v.PushPull( stale, Rect{ 0, 0, 0, 0 }, +1, 1, 0 );
    EXPECT_EQ( v.m_Cells.count( Pack( { 0, 1, 0 } ) ), 1u );
    EXPECT_EQ( v.m_Cells.size(), 3u );

    WorkPlane top{ 1, 1, 2 };
    v.PushPull( top, Rect{ 0, 0, 0, 0 }, -1, 1, 0 );
    EXPECT_EQ( top.Cell, 1 );
    EXPECT_EQ( v.m_Cells.count( Pack( { 0, 1, 0 } ) ), 0u );
    EXPECT_EQ( v.m_Cells.size(), 2u );

    // A side plane (+X facing) extrudes along X: u = Y, v = Z.
    WorkPlane side{ 0, 1, 1 };
    v.PushPull( side, Rect{ 0, 0, 0, 1 }, +1, 2, 0 );
    EXPECT_EQ( side.Cell, 3 );
    EXPECT_EQ( v.m_Cells.count( Pack( { 2, 0, 1 } ) ), 1u );
    EXPECT_EQ( v.m_Cells.size(), 6u );
    ExpectClosed( Measured( v.Bake() ) );
}

TEST( VoxelBlockout, CornerModeBuildsABilinearRampAndReadsItBack )
{
    Volume     v = Ground();
    WorkPlane  plane;
    const Rect sel{ 0, 1, 0, 0 }; // two cells along Z
    v.PushPull( plane, sel, +1, 1, 0 );

    // Raise the two posts at vMax (the +X edge) by a whole cell: one ramp over both cells.
    ASSERT_TRUE( v.ApplyCornerHeights( plane, sel, { 0, 0, CornerDen, CornerDen } ) );
    const CornerHeights back = v.ReadCornerHeights( plane, sel );
    EXPECT_EQ( back, ( CornerHeights{ 0, 0, CornerDen, CornerDen } ) );
    for ( const auto& [key, cell] : v.m_Cells )
        for ( int i = 0; i < 8; ++i )
            EXPECT_EQ( cell.V[i], ( i & 2 ) && ( i & 1 ) ? CornerDen : 0 ) << "corner " << i;

    const auto    mesh = v.Bake();
    const Measure m    = Measured( mesh );
    ExpectClosed( m );
    EXPECT_NEAR( m.Volume, 2.0 * 1.5e6, 2.0 ); // each cell: a 1 x 1 base under a slope rising 0 -> 1
    bool slanted = false;
    for ( const auto& vert : mesh.Vertices )
        slanted |= std::abs( vert.Normal.x + std::sqrt( 0.5f ) ) < 1e-4f &&
                   std::abs( vert.Normal.y - std::sqrt( 0.5f ) ) < 1e-4f;
    EXPECT_TRUE( slanted ) << "the ramp's top faces -X and up at 45 degrees";

    // A hip: one post raised, bilinear over the rectangle (the far corner of cell 0 sits at a quarter).
    Volume     h = Ground();
    WorkPlane  hp;
    const Rect sq{ 0, 1, 0, 1 };
    h.PushPull( hp, sq, +1, 1, 0 );
    ASSERT_TRUE( h.ApplyCornerHeights( hp, sq, { 0, 0, 0, CornerDen } ) );
    EXPECT_EQ( h.m_Cells.at( Pack( { 0, 0, 0 } ) ).V[2 | 1 | 4], CornerDen / 4 );
    ExpectClosed( Measured( h.Bake() ) );
}

TEST( VoxelBlockout, CornerModeOnlyAppliesToAGroundFacingPlane )
{
    Volume    v = Ground();
    WorkPlane side{ 0, 1, 0 };
    v.PushPull( side, Rect{}, +1, 1, 0 );
    EXPECT_FALSE( v.ApplyCornerHeights( side, Rect{}, { 60, 60, 60, 60 } ) );
    EXPECT_EQ( v.ReadCornerHeights( side, Rect{} ), CornerHeights{} );
    for ( const auto& [key, cell] : v.m_Cells )
        EXPECT_TRUE( cell.IsFlat() );
}

TEST( VoxelBlockout, DeformedNeighboursOnlyHideTheSharedFaceWhenTheirCornersAgree )
{
    Volume v = Ground();
    Cell   a;
    Cell   b;
    v.m_Cells[Pack( { 0, 0, 0 } )] = a;
    v.m_Cells[Pack( { 1, 0, 0 } )] = b;
    const int right              = 5; // +X
    EXPECT_TRUE( v.FaceHidden( v.m_Cells, { 0, 0, 0 }, a, right, v.m_Unit, v.m_Frame ) );

    a.V[1 | 2]                   = 30; // lift a's +X top corner only
    v.m_Cells[Pack( { 0, 0, 0 } )] = a;
    EXPECT_FALSE( v.FaceHidden( v.m_Cells, { 0, 0, 0 }, a, right, v.m_Unit, v.m_Frame ) );
    b.V[2]                       = 30; // the neighbour's matching -X top corner
    v.m_Cells[Pack( { 1, 0, 0 } )] = b;
    EXPECT_TRUE( v.FaceHidden( v.m_Cells, { 0, 0, 0 }, a, right, v.m_Unit, v.m_Frame ) );
}

TEST( VoxelBlockout, FrozenLayersCullAcrossUnitsButNotAcrossGridFrames )
{
    Volume    v = Ground();
    WorkPlane plane;
    v.PushPull( plane, Rect{}, +1, 1, 0 );
    ASSERT_TRUE( v.Freeze() );
    EXPECT_TRUE( v.m_Cells.empty() );
    EXPECT_LT( v.m_Unit, 0.0f );
    EXPECT_FALSE( v.Freeze() ) << "nothing left to commit";
    ASSERT_EQ( v.m_Frozen.size(), 1u );

    // A 50 cm volume beside the 100 cm cube: its -X faces touching the cube are hidden.
    v.m_Unit = 50.0f;
    EXPECT_TRUE( v.SolidAt( { 1, 1, 1 }, 50.0f, GridFrame{} ) );
    EXPECT_FALSE( v.SolidAt( { 2, 0, 0 }, 50.0f, GridFrame{} ) );
    EXPECT_FALSE( v.SolidAt( { 1, 1, 1 }, 50.0f, GridFrame{ glm::vec3( 10.0f, 0.0f, 0.0f ) } ) )
         << "another grid frame";

    WorkPlane side{ 0, 1, 2 };
    v.PushPull( side, Rect{ 0, 1, 0, 1 }, +1, 1, 0 ); // 2 x 2 x 1 of 50 cm cells against the cube's +X face
    const auto mesh = v.Bake();
    // Cube: 6 quads. Small slab: 5 visible greedy quads (its -X face is buried against the cube).
    EXPECT_EQ( mesh.Indices.size(), ( 6u + 5u ) * 2u );
}

TEST( VoxelBlockout, RefineSplitsEveryCellAndKeepsTheGeometry )
{
    Volume    v = Ground();
    WorkPlane plane;
    v.PushPull( plane, Rect{ 0, 1, 0, 0 }, +1, 1, 0 );
    ASSERT_TRUE( v.ApplyCornerHeights( plane, Rect{ 0, 1, 0, 0 }, { 0, 0, 0, 0 } ) );
    const Measure before = Measured( v.Bake() );

    v.Refine( 2 );
    EXPECT_FLOAT_EQ( v.m_Unit, 50.0f );
    EXPECT_EQ( v.m_Cells.size(), 16u );
    const Measure after = Measured( v.Bake() );
    EXPECT_NEAR( after.Volume, before.Volume, 1.0 );
    EXPECT_NEAR( after.Area, before.Area, 1e-2 );
}

TEST( VoxelBlockout, RescaleSelectionKeepsTheMarqueeInPlaceAndBlockAligned )
{
    WorkPlane  plane{ 1, 1, 4 };
    glm::ivec2 anchor{ 2, 3 };
    Rect       sel{ 2, 5, -2, 1 };
    RescaleSelection( plane, anchor, sel, 50.0f, 25.0f, 2 );
    EXPECT_EQ( plane.Cell, 8 );
    EXPECT_EQ( anchor, glm::ivec2( 4, 6 ) );
    EXPECT_EQ( sel.UMin, 4 );
    EXPECT_EQ( sel.UMax, 11 );
    EXPECT_EQ( sel.VMin, -4 );
    EXPECT_EQ( sel.VMax, 3 );

    // Coarsening re-snaps outward to whole blocks of K.
    Rect fine{ 3, 4, -1, -1 };
    RescaleSelection( plane, anchor, fine, 25.0f, 100.0f, 1 );
    EXPECT_EQ( plane.Cell, 2 );
    EXPECT_EQ( fine.UMin, 0 );
    EXPECT_EQ( fine.UMax, 1 );
    EXPECT_EQ( fine.VMin, -1 );
    EXPECT_EQ( fine.VMax, -1 );
}

// --- M9: Quick Materials (per-face material IDs, Shift+B), Shift+E/Q slide, Ctrl+drag push/pull ---

namespace
{
    // Every submesh's ranges lie inside the arrays and its (submesh-local) indices inside its vertex range.
    void ExpectSubmeshesConsistent( const Desert::Geometry::RenderMeshData& m )
    {
        ASSERT_EQ( m.Submeshes.size(), m.SubmeshMaterialIds.size() );
        size_t tris = 0;
        for ( const auto& sm : m.Submeshes )
        {
            ASSERT_EQ( sm.IndexOffset % 3, 0u );
            ASSERT_EQ( sm.IndexCount % 3, 0u );
            ASSERT_LE( sm.VertexOffset + sm.VertexCount, m.Vertices.size() );
            for ( uint32_t t = sm.IndexOffset / 3; t < ( sm.IndexOffset + sm.IndexCount ) / 3; ++t )
            {
                EXPECT_LT( m.Indices[t].V1, sm.VertexCount );
                EXPECT_LT( m.Indices[t].V2, sm.VertexCount );
                EXPECT_LT( m.Indices[t].V3, sm.VertexCount );
            }
            tris += sm.IndexCount / 3;
        }
        EXPECT_EQ( tris, m.Indices.size() );
    }

    // Triangles of the submesh carrying material `id` (0 when there is none).
    uint32_t TrianglesOf( const Desert::Geometry::RenderMeshData& m, int id )
    {
        for ( size_t i = 0; i < m.SubmeshMaterialIds.size(); ++i )
            if ( m.SubmeshMaterialIds[i] == id )
                return m.Submeshes[i].IndexCount / 3;
        return 0;
    }

    // Absolute-coordinate version of Measured(): Bake emits submesh-local indices.
    Measure MeasuredAll( const Desert::Geometry::RenderMeshData& m )
    {
        Desert::Geometry::RenderMeshData flat = m;
        for ( const auto& sm : m.Submeshes )
            for ( uint32_t t = sm.IndexOffset / 3; t < ( sm.IndexOffset + sm.IndexCount ) / 3; ++t )
            {
                flat.Indices[t].V1 += sm.VertexOffset;
                flat.Indices[t].V2 += sm.VertexOffset;
                flat.Indices[t].V3 += sm.VertexOffset;
            }
        return Measured( flat );
    }
} // namespace

TEST( VoxelBlockoutMaterials, PullGivesEveryNewFaceTheOpMaterialAndBakeSplitsOneSubmeshPerId )
{
    Volume    v = Ground();
    WorkPlane plane;
    v.PushPull( plane, Rect{ 0, 1, 0, 0 }, +1, 1, 0 ); // 1 (X) x 1 (Y) x 2 (Z), material 0
    v.PushPull( plane, Rect{ 0, 0, 0, 0 }, +1, 1, 2 ); // one cube on top of the z = 0 cell, material 2
    for ( int f = 0; f < 6; ++f )
        EXPECT_EQ( v.m_Cells.at( Pack( { 0, 1, 0 } ) ).Mat[f], 2 ) << "face " << f;

    const auto mesh = v.Bake();
    ExpectSubmeshesConsistent( mesh );
    ASSERT_EQ( mesh.SubmeshMaterialIds, ( std::vector<int>{ 0, 2 } ) ) << "ascending material IDs";
    EXPECT_EQ( TrianglesOf( mesh, 2 ), 5u * 2u ) << "the cube: five faces, its bottom sits on the slab";
    // The slab: greedy quads, one top cell covered. Its two long side walls are fanned from their centres
    // (5 triangles each): the cube's bottom corners land mid-edge on them, a T-junction otherwise.
    EXPECT_EQ( TrianglesOf( mesh, 0 ), 4u * 2u + 2u * 5u ) << "the slab: greedy quads, one top cell covered";
    EXPECT_EQ( NonConformingEdges( mesh ), 0 );
    const Measure m = MeasuredAll( mesh );
    ExpectClosed( m );
    EXPECT_NEAR( m.Volume, 3.0 * 1e6, 1.0 );
}

TEST( VoxelBlockoutMaterials, PushInGivesTheWallsAndFloorOfTheHoleTheOpMaterial )
{
    Volume    v = Ground();
    WorkPlane plane;
    v.PushPull( plane, Rect{ 0, 2, 0, 2 }, +1, 2, 0 ); // 3 x 2 x 3, top at y = 2
    ASSERT_EQ( plane.Cell, 2 );
    v.PushPull( plane, Rect{ 1, 1, 1, 1 }, -1, 1, 3 ); // dig the centre top cell out
    EXPECT_EQ( v.m_Cells.size(), 17u );
    EXPECT_EQ( v.m_Cells.at( Pack( { 1, 0, 1 } ) ).Mat[2], 3 ) << "floor (+Y face of the cell below)";
    EXPECT_EQ( v.m_Cells.at( Pack( { 0, 1, 1 } ) ).Mat[5], 3 ) << "wall (+X face of the -X neighbour)";
    EXPECT_EQ( v.m_Cells.at( Pack( { 0, 1, 1 } ) ).Mat[2], 0 ) << "a face the push did not expose keeps its ID";

    const auto mesh = v.Bake();
    ExpectSubmeshesConsistent( mesh );
    ASSERT_EQ( mesh.SubmeshMaterialIds, ( std::vector<int>{ 0, 3 } ) );
    EXPECT_EQ( TrianglesOf( mesh, 3 ), 5u * 2u ) << "four walls + the floor";
    ExpectClosed( MeasuredAll( mesh ) );
}

TEST( VoxelBlockoutMaterials, PaintFacesRepaintsOnlyTheSelectedFacesInAnyLayerAndKeepsTheGeometry )
{
    Volume    v = Ground();
    WorkPlane plane;
    v.PushPull( plane, Rect{ 0, 1, 0, 1 }, +1, 1, 0 ); // 2 x 1 x 2 slab
    ASSERT_TRUE( v.Freeze() );
    EXPECT_EQ( v.PaintFaces( WorkPlane{ 1, 1, 1 }, Rect{}, 1 ), 0 ) << "no active base chosen yet";
    v.m_Unit             = 100.0f; // the tool re-bases the next volume every frame
    const Measure before = MeasuredAll( v.Bake() );

    EXPECT_EQ( v.PaintFaces( WorkPlane{ 1, 1, 2 }, Rect{ 0, 1, 0, 1 }, 1 ), 0 ) << "a plane above the surface";
    EXPECT_EQ( v.PaintFaces( WorkPlane{ 1, -1, 1 }, Rect{ 0, 1, 0, 1 }, 1 ), 0 ) << "the top, facing down";
    ASSERT_EQ( v.PaintFaces( WorkPlane{ 1, 1, 1 }, Rect{ 0, 0, 0, 0 }, 1 ), 1 )
         << "one top face of the frozen slab";

    const auto mesh = v.Bake();
    ExpectSubmeshesConsistent( mesh );
    ASSERT_EQ( mesh.SubmeshMaterialIds, ( std::vector<int>{ 0, 1 } ) );
    EXPECT_EQ( TrianglesOf( mesh, 1 ), 2u );
    // Greedy never merges across materials: the top splits in two (an L of two quads), and every quad whose
    // edge the painted cell's corners land on is fanned, so the welded surface stays conforming.
    EXPECT_EQ( TrianglesOf( mesh, 0 ), 26u ) << "greedy never merges across materials: the top splits in two";
    EXPECT_EQ( NonConformingEdges( mesh ), 0 );
    const Measure after = MeasuredAll( mesh );
    ExpectClosed( after );
    EXPECT_NEAR( after.Volume, before.Volume, 1.0 );
    EXPECT_NEAR( after.Area, before.Area, 1e-2 );

    // A finer active base paints the coarse layer's face under its centre only.
    v.m_Unit = 50.0f;
    EXPECT_EQ( v.PaintFaces( WorkPlane{ 1, 1, 2 }, Rect{ 2, 3, 0, 1 }, 4 ), 1 ) << "the (x 0, z 1) top face";
    EXPECT_EQ( v.m_Frozen[0].Cells.at( Pack( { 0, 0, 1 } ) ).Mat[2], 4 );
}

TEST( VoxelBlockoutMaterials, RefineKeepsTheFaceMaterials )
{
    Volume    v = Ground();
    WorkPlane plane;
    v.PushPull( plane, Rect{}, +1, 1, 5 );
    v.Refine( 2 );
    ASSERT_EQ( v.m_Cells.size(), 8u );
    for ( const auto& [k, cell] : v.m_Cells )
        for ( int f = 0; f < 6; ++f )
            EXPECT_EQ( cell.Mat[f], 5 );
    EXPECT_EQ( v.Bake().SubmeshMaterialIds, ( std::vector<int>{ 5 } ) );
}

TEST( VoxelBlockoutSlideDrag, SlideSelectionMovesThePlaneAlongItsOutwardNormal )
{
    WorkPlane up{ 1, 1, 0 };
    SlideSelection( up, 2 );
    EXPECT_EQ( up.Cell, 2 );
    WorkPlane west{ 0, -1, 5 };
    SlideSelection( west, 2 );
    EXPECT_EQ( west.Cell, 3 ) << "outward along -X lowers the index";
    SlideSelection( west, -1 );
    EXPECT_EQ( west.Cell, 4 );
}

TEST( VoxelBlockoutSlideDrag, LineParameterClosestToRayMatchesUEDistLine3Ray3 )
{
    const glm::vec3 o( 0.0f ), y( 0.0f, 1.0f, 0.0f );
    EXPECT_NEAR( LineParameterClosestToRay( o, y, { 100.0f, 50.0f, 0.0f }, { -1.0f, 0.0f, 0.0f } ), 50.0f, 1e-3f );
    EXPECT_NEAR( LineParameterClosestToRay( o, y, { 100.0f, 50.0f, 0.0f }, { -1.0f, 1.0f, 0.0f } ), 150.0f, 1e-2f )
         << "the ray is not normalised by the caller";
    EXPECT_NEAR( LineParameterClosestToRay( o, y, { 100.0f, 50.0f, 0.0f }, { 1.0f, 1.0f, 0.0f } ), 50.0f, 1e-3f )
         << "the ray runs away from the line: its origin is the closest point";
    EXPECT_NEAR( LineParameterClosestToRay( o, y, { 10.0f, 20.0f, 0.0f }, y ), 20.0f, 1e-3f ) << "parallel";
}

TEST( VoxelBlockoutSlideDrag, DragExtrudeBlocksRoundsToWholeStepsOfBlocksPerStep )
{
    EXPECT_EQ( DragExtrudeBlocks( 149.0f, 100.0f, 1 ), 1 );
    EXPECT_EQ( DragExtrudeBlocks( 151.0f, 100.0f, 1 ), 2 );
    EXPECT_EQ( DragExtrudeBlocks( -160.0f, 100.0f, 1 ), -2 );
    EXPECT_EQ( DragExtrudeBlocks( 250.0f, 100.0f, 2 ), 2 );
    EXPECT_EQ( DragExtrudeBlocks( 350.0f, 100.0f, 2 ), 4 );
    EXPECT_EQ( DragExtrudeBlocks( 350.0f, 0.0f, 1 ), 0 );
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}

// M10: the grid frame carries a rotation as well as an origin. The edits stay frame-space (cell indices); the bake
// carries every layer into the world by its OWN frame, and layers in turned frames never cull or paint each other.
TEST( VoxelBlockoutFrame, FrameRoundTripsAndMakeGridFrameUsesDegreesAboutXYZ )
{
    const GridFrame f = MakeGridFrame( { 300.0f, 10.0f, -200.0f }, { 20.0f, 30.0f, -45.0f } );
    const glm::vec3 p( 12.0f, -7.0f, 250.0f );
    const glm::vec3 back = f.ToFramePoint( f.ToWorldPoint( p ) );
    EXPECT_NEAR( back.x, p.x, 1e-3f );
    EXPECT_NEAR( back.y, p.y, 1e-3f );
    EXPECT_NEAR( back.z, p.z, 1e-3f );

    const glm::vec3 yawedX =
         MakeGridFrame( glm::vec3( 0.0f ), { 0.0f, 90.0f, 0.0f } ).ToWorldVector( { 1, 0, 0 } );
    EXPECT_NEAR( yawedX.z, -1.0f, 1e-5f ) << "+90 degrees about Y turns +X onto -Z";
    EXPECT_TRUE( f.SameAxes( GridFrame{ glm::vec3( 5.0f ), -f.Rotation } ) ) << "q and -q are one rotation";
    EXPECT_FALSE( f.SameAs( GridFrame{ glm::vec3( 5.0f ), f.Rotation } ) );
}

TEST( VoxelBlockoutFrame, ABlockInARotatedFrameLandsOnTheFramesLatticeInTheWorld )
{
    Volume v  = Ground();
    v.m_Frame = MakeGridFrame( { 300.0f, 0.0f, -200.0f }, { 0.0f, 30.0f, 0.0f } );
    WorkPlane plane;
    v.PushPull( plane, Rect{ 2, 2, 1, 1 }, +1, 1, 0 ); // u is Z, v is X: the cell (x 1, y 0, z 2)
    ASSERT_EQ( v.m_Cells.size(), 1u );
    ASSERT_TRUE( v.m_Cells.contains( Pack( { 1, 0, 2 } ) ) ) << "the edit itself stays in frame cells";

    // Written out by hand, not through GridFrame: +30 degrees about Y is x' = x cos + z sin, z' = z cos - x sin.
    const float cs    = std::cos( glm::radians( 30.0f ) );
    const float sn    = std::sin( glm::radians( 30.0f ) );
    auto        world = [&]( float x, float y, float z )
    { return glm::vec3( 300.0f + x * cs + z * sn, y, -200.0f + z * cs - x * sn ); };
    std::vector<glm::vec3> corners;
    for ( int i = 0; i < 8; ++i )
        corners.push_back( world( ( 1.0f + static_cast<float>( i & 1 ) ) * 100.0f, ( i & 2 ) ? 100.0f : 0.0f,
                                  ( ( i & 4 ) ? 3.0f : 2.0f ) * 100.0f ) );
    const std::vector<glm::vec3> normals = { { 0, 1, 0 },    { 0, -1, 0 },  { cs, 0, -sn },
                                             { -cs, 0, sn }, { sn, 0, cs }, { -sn, 0, -cs } };

    const auto mesh = v.Bake();
    ASSERT_EQ( mesh.Vertices.size(), 24u );
    std::vector<int> used( corners.size(), 0 );
    for ( const auto& vert : mesh.Vertices )
    {
        int at = -1;
        for ( size_t k = 0; k < corners.size(); ++k )
            if ( glm::length( vert.Position - corners[k] ) < 1e-2f )
                at = static_cast<int>( k );
        ASSERT_GE( at, 0 ) << "a vertex off the turned lattice at (" << vert.Position.x << ", " << vert.Position.y
                           << ", " << vert.Position.z << ")";
        ++used[at];
        bool normalOk = false;
        for ( const auto& n : normals )
            normalOk = normalOk || glm::length( vert.Normal - n ) < 1e-4f;
        EXPECT_TRUE( normalOk ) << "the normal turns with the frame";
        EXPECT_NEAR( glm::dot( vert.Tangent, vert.Normal ), 0.0f, 1e-4f ) << "the tangent turns with it too";
    }
    for ( int n : used )
        EXPECT_EQ( n, 3 ) << "every corner of the cube is shared by three faces";
    const Measure m = Measured( mesh );
    ExpectClosed( m );
    EXPECT_NEAR( m.Volume, 1.0e6, 1.0 ) << "one 100 cm cube, rotation keeps the volume";
}

TEST( VoxelBlockoutFrame, LayersInTurnedFramesNeverCullEachOther )
{
    Volume    v = Ground();
    WorkPlane plane;
    v.PushPull( plane, Rect{ 0, 0, 1, 1 }, +1, 1, 0 ); // the cell (x 1) in the world-aligned frame
    ASSERT_TRUE( v.Freeze() );

    v.m_Unit  = 100.0f;
    v.m_Frame = MakeGridFrame( glm::vec3( 0.0f ), { 0.0f, 30.0f, 0.0f } );
    WorkPlane turned;
    v.PushPull( turned, Rect{}, +1, 1,
                0 ); // the cell (x 0) of the turned frame: its +X face is NOT the other's -X
    EXPECT_TRUE( v.SolidAt( { 1, 0, 0 }, 100.0f, GridFrame{} ) );
    EXPECT_FALSE( v.SolidAt( { 1, 0, 0 }, 100.0f, v.m_Frame ) ) << "a turned lattice does not line up";
    EXPECT_EQ( v.Bake().Indices.size(), 12u * 2u ) << "two whole cubes, no face culled between the frames";
}

TEST( VoxelBlockoutFrame, PaintFacesReachesAShiftedLayerButNotATurnedOne )
{
    Volume v  = Ground();
    v.m_Frame = GridFrame{ glm::vec3( 100.0f, 0.0f, 0.0f ) }; // one cell along X, same axes
    WorkPlane a;
    v.PushPull( a, Rect{}, +1, 1, 0 );
    ASSERT_TRUE( v.Freeze() );
    v.m_Unit  = 100.0f;
    v.m_Frame = MakeGridFrame( glm::vec3( 0.0f ), { 0.0f, 30.0f, 0.0f } );
    WorkPlane b;
    v.PushPull( b, Rect{}, +1, 1, 0 );
    ASSERT_TRUE( v.Freeze() );

    v.m_Unit  = 100.0f;
    v.m_Frame = GridFrame{};
    // The top plane over x 0..1: the shifted piece's top is at x 1; the turned piece's top index-coincides
    // with x 0 but lies on another lattice.
    EXPECT_EQ( v.PaintFaces( WorkPlane{ 1, 1, 1 }, Rect{ 0, 0, 0, 1 }, 3 ), 1 );
    EXPECT_EQ( v.m_Frozen[0].Cells.at( Pack( { 0, 0, 0 } ) ).Mat[2], 3 );
    EXPECT_EQ( v.m_Frozen[1].Cells.at( Pack( { 0, 0, 0 } ) ).Mat[2], 0 );
}

TEST( VoxelBlockoutFrame, CtrlMiddleClickPivotIsTheFaceCornerNearestTheRayInTheWorld )
{
    const GridFrame f = MakeGridFrame( { 300.0f, 0.0f, -200.0f }, { 0.0f, 30.0f, 0.0f } );
    // The top face of the cell (1, 0, 2): corners at y 100, x 100..200, z 200..300 in the frame.
    const glm::vec3 farCorner = f.ToWorldPoint( { 200.0f, 100.0f, 300.0f } );
    const glm::vec3 got       = NearestFaceCorner( f, { 1, 0, 2 }, { 0, 1, 0 }, 100.0f,
                                                   farCorner + glm::vec3( 8.0f, 500.0f, -6.0f ), { 0.0f, -1.0f, 0.0f } );
    EXPECT_NEAR( glm::length( got - farCorner ), 0.0f, 1e-2f );

    // The -X face sits on the cell's NEAR side along X: its corners are at x 100, not 200.
    const glm::vec3 low = f.ToWorldPoint( { 100.0f, 0.0f, 200.0f } );
    const glm::vec3 dir = f.ToWorldVector( { 1.0f, 0.0f, 0.0f } );
    const glm::vec3 side =
         NearestFaceCorner( f, { 1, 0, 2 }, { -1, 0, 0 }, 100.0f, low - dir * 400.0f + glm::vec3( 0, 4, 0 ), dir );
    EXPECT_NEAR( glm::length( side - low ), 0.0f, 1e-2f );
}

// M10b: a block pushed out of a floor. The floor's greedy ring runs past the block's corners, and welded by
// position that left T-junctions: edges with a triangle on one side only and bowtie corners, which the editable
// mesh refuses (DynamicMesh3::CheckValidity) - the blockout then got no mesh at all. Conforming means every
// welded edge has exactly one partner running the other way.

TEST( VoxelBlockoutConforming, ABlockPushedOutOfACommittedFloorLeavesNoTJunction )
{
    // The editor's sequence: select 4x4, E; a new marquee on top commits the floor; select 2x2 there, E.
    Volume    v = Ground();
    WorkPlane floor;
    v.PushPull( floor, Rect{ 0, 3, 0, 3 }, +1, 1, 0 );
    ASSERT_TRUE( v.Freeze() );
    v.m_Unit = 100.0f;
    WorkPlane top{ 1, 1, 1 };
    v.PushPull( top, Rect{ 1, 2, 1, 2 }, +1, 1, 0 );

    const auto mesh = v.Bake();
    EXPECT_EQ( NonConformingEdges( mesh ), 0 );
    const Measure m = Measured( mesh );
    ExpectClosed( m );
    EXPECT_NEAR( m.Volume, ( 16.0 + 4.0 ) * 1.0e6, 1.0 );
}

TEST( VoxelBlockoutConforming, TheSameStepInOneLayerConformsToo )
{
    Volume    v = Ground();
    WorkPlane floor;
    v.PushPull( floor, Rect{ 0, 3, 0, 3 }, +1, 1, 0 );
    WorkPlane top{ 1, 1, 1 };
    v.PushPull( top, Rect{ 1, 2, 1, 2 }, +1, 1, 0 );
    const auto mesh = v.Bake();
    EXPECT_EQ( NonConformingEdges( mesh ), 0 ) << "the same step without the commit";
    ExpectClosed( Measured( mesh ) );
}

// M11: the Cube Grid tool reopens on a blockout it built (UE: the selected mesh is the tool's target) and
// Push / Pull carries on over the same pieces. The voxels travel in the entity's space; the pieces come back
// committed, and push-in must still cut them.
namespace
{
    // The editor's first session: a 4x4 floor, a new marquee commits it, a 2x2 block on top.
    Volume FloorWithBlock()
    {
        Volume    v = Ground();
        WorkPlane floor;
        v.PushPull( floor, Rect{ 0, 3, 0, 3 }, +1, 1, 0 );
        EXPECT_TRUE( v.Freeze() );
        v.m_Unit = 100.0f;
        WorkPlane top{ 1, 1, 1 };
        v.PushPull( top, Rect{ 1, 2, 1, 2 }, +1, 1, 0 );
        return v;
    }
} // namespace

TEST( VoxelBlockoutReedit, AReopenedBlockoutOnAMovedEntityIsCutByPushInAndStaysConforming )
{
    const Volume        built  = FloorWithBlock();
    const auto          first  = built.Bake();
    const SavedBlockout saved  = Save( built, 0x1234u ); // Accept: the entity was created at identity
    const GridFrame     entity = MakeGridFrame( { 250.0f, 0.0f, -40.0f }, { 0.0f, 30.0f, 0.0f } );

    // Reopen after the entity was moved and turned: the volume comes into the world through the entity.
    auto loaded = Load( saved );
    ASSERT_TRUE( loaded.IsSuccess() ) << loaded.GetError();
    Volume w = Reframed( loaded.GetValue(), entity );
    ASSERT_EQ( w.m_Frozen.size(), 2u );
    EXPECT_TRUE( w.m_Cells.empty() ) << "every piece comes back committed";
    // Before any edit, the entity's-space bake is the mesh the first session left.
    {
        const auto again = Reframed( w, entity.Inverse() ).Bake();
        EXPECT_EQ( again.Indices.size(), first.Indices.size() );
        EXPECT_NEAR( Measured( again ).Volume, Measured( first ).Volume, 1.0 );
    }
    // The tool puts the grid on the last piece; Q pushes the block and the floor under it down two cells.
    w.m_Frame = w.m_Frozen.back().Frame;
    w.m_Unit  = 100.0f;
    WorkPlane q{ 1, 1, 2 };
    w.PushPull( q, Rect{ 1, 2, 1, 2 }, -1, 2, 0 );
    EXPECT_EQ( q.Cell, 0 );

    const auto after = Reframed( w, entity.Inverse() ).Bake();
    const auto m     = Measured( after );
    ExpectClosed( m );
    EXPECT_NEAR( m.Volume, ( 16.0 + 4.0 - 8.0 ) * 1.0e6, 1.0 ) << "the committed floor was cut, not skipped";
    EXPECT_EQ( NonConformingEdges( after ), 0 );
    EXPECT_TRUE( w.m_Frozen.empty() ) << "both pieces the cut reached were thawed into the active volume";

    // E on the same session builds back on the same mesh.
    WorkPlane e{ 1, 1, 0 };
    w.PushPull( e, Rect{ 1, 1, 1, 1 }, +1, 1, 0 );
    EXPECT_NEAR( Measured( Reframed( w, entity.Inverse() ).Bake() ).Volume, 13.0e6, 1.0 );
}

TEST( VoxelBlockoutReedit, PushInSplitsACoarserFlatPieceAndEmptiesAFinerOne )
{
    Volume v = Ground();
    Layer  coarse;
    coarse.Unit                       = 200.0f;
    coarse.Cells[Pack( { 0, 0, 0 } )] = {};
    v.m_Frozen.push_back( coarse );
    WorkPlane q{ 1, 1, 2 };
    v.PushPull( q, Rect{ 0, 0, 0, 0 }, -1, 1, 0 );
    EXPECT_TRUE( v.m_Frozen.empty() );
    EXPECT_EQ( v.m_Cells.size(), 7u );
    const auto split = v.Bake();
    ExpectClosed( Measured( split ) );
    EXPECT_NEAR( Measured( split ).Volume, 7.0e6, 1.0 );
    EXPECT_EQ( NonConformingEdges( split ), 0 );

    Volume f = Ground();
    Layer  fine;
    fine.Unit = 50.0f;
    for ( int x = 0; x < 2; ++x )
        for ( int y = 0; y < 2; ++y )
            for ( int z = 0; z < 2; ++z )
                fine.Cells[Pack( { x, y, z } )] = {};
    f.m_Frozen.push_back( fine );
    WorkPlane fq{ 1, 1, 1 };
    f.PushPull( fq, Rect{ 0, 0, 0, 0 }, -1, 1, 0 );
    EXPECT_TRUE( f.m_Frozen.empty() ) << "the finer piece lost every cell inside the cut";
    EXPECT_TRUE( f.Bake().Indices.empty() );
}

TEST( VoxelBlockoutReedit, ADeformedCoarserPieceAndATurnedOneStayFrozen )
{
    Volume v = Ground();
    Layer  ramp;
    ramp.Unit = 200.0f;
    Cell c;
    c.V[2]                          = CornerDen;
    ramp.Cells[Pack( { 0, 0, 0 } )] = c;
    v.m_Frozen.push_back( ramp );
    Layer turned;
    turned.Unit                       = 100.0f;
    turned.Frame                      = MakeGridFrame( {}, { 0.0f, 45.0f, 0.0f } );
    turned.Cells[Pack( { 0, 1, 0 } )] = {};
    v.m_Frozen.push_back( turned );
    WorkPlane q{ 1, 1, 2 };
    v.PushPull( q, Rect{ 0, 0, 0, 0 }, -1, 1, 0 );
    ASSERT_EQ( v.m_Frozen.size(), 2u );
    EXPECT_EQ( v.m_Frozen[0].Cells.size(), 1u );
    EXPECT_EQ( v.m_Frozen[1].Cells.size(), 1u );
}

TEST( VoxelBlockoutReedit, SaveLoadKeepsCornersMaterialsFramesAndUnits )
{
    Volume     v = Ground();
    WorkPlane  plane;
    const Rect sel{ 0, 1, 0, 1 };
    v.PushPull( plane, sel, +1, 1, 3 );
    WorkPlane top{ 1, 1, 1 };
    ASSERT_TRUE( v.ApplyCornerHeights( top, sel, { 0, 0, CornerDen, CornerDen } ) );
    ASSERT_TRUE( v.Freeze() );
    v.m_Unit  = 50.0f;
    v.m_Frame = MakeGridFrame( { 10.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 20.0f } );
    WorkPlane p2;
    v.PushPull( p2, Rect{ 0, 0, 0, 0 }, +1, 2, 1 );

    const SavedBlockout saved = Save( v, 0xabcdef0123456789ull );
    EXPECT_EQ( saved.MeshKey, "abcdef0123456789" );
    EXPECT_EQ( ParseMeshKey( saved.MeshKey ), 0xabcdef0123456789ull );
    ASSERT_EQ( saved.Layers.size(), 2u );
    EXPECT_FALSE( saved.Layers[0].Deformed.empty() );

    auto loaded = Load( saved );
    ASSERT_TRUE( loaded.IsSuccess() ) << loaded.GetError();
    const Volume& l = loaded.GetValue();
    ASSERT_EQ( l.m_Frozen.size(), 2u );
    for ( int i = 0; i < 2; ++i )
    {
        const CellMap& want = i == 0 ? v.m_Frozen[0].Cells : v.m_Cells;
        const Layer&   got  = l.m_Frozen[static_cast<size_t>( i )];
        EXPECT_FLOAT_EQ( got.Unit, i == 0 ? 100.0f : 50.0f );
        EXPECT_TRUE( got.Frame.SameAs( i == 0 ? v.m_Frozen[0].Frame : v.m_Frame ) );
        ASSERT_EQ( got.Cells.size(), want.size() );
        for ( const auto& [k, cell] : want )
        {
            const auto it = got.Cells.find( k );
            ASSERT_NE( it, got.Cells.end() );
            EXPECT_TRUE( std::equal( std::begin( cell.V ), std::end( cell.V ), std::begin( it->second.V ) ) );
            EXPECT_TRUE( std::equal( std::begin( cell.Mat ), std::end( cell.Mat ), std::begin( it->second.Mat ) ) );
        }
    }
    EXPECT_NEAR( Measured( l.Bake() ).Volume, Measured( v.Bake() ).Volume, 1.0 );
}

TEST( VoxelBlockoutReedit, LoadRefusesWhatCannotBeAVolumeByLayerAndReason )
{
    const std::vector<int32_t> one{ 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    auto                       refused = [&one]( SavedLayer layer, const char* why )
    {
        SavedBlockout s;
        s.Layers.push_back( SavedLayer{ 100.0f, {}, { 1, 0, 0, 0 }, one, {} } );
        s.Layers.push_back( std::move( layer ) );
        auto r = Load( s );
        ASSERT_FALSE( r.IsSuccess() ) << why;
        EXPECT_NE( r.GetError().find( "layer 1" ), std::string::npos ) << r.GetError();
        EXPECT_NE( r.GetError().find( why ), std::string::npos ) << r.GetError();
    };
    refused( SavedLayer{ 0.0f, {}, { 1, 0, 0, 0 }, one, {} }, "not positive" );
    refused( SavedLayer{ 100.0f, {}, { 1, 0, 0, 0 }, { 0, 0, 0 }, {} }, "whole cells" );
    refused( SavedLayer{ 100.0f, {}, { 2, 0, 0, 0 }, one, {} }, "not a rotation" );
    refused( SavedLayer{ 100.0f, {}, { 1, 0, 0, 0 }, { 0, 0, 0, 300, 0, 0, 0, 0, 0 }, {} }, "material 300" );
    refused( SavedLayer{ 100.0f, {}, { 1, 0, 0, 0 }, { 1 << 21, 0, 0, 0, 0, 0, 0, 0, 0 }, {} }, "outside" );
    std::vector<int32_t> twice = one;
    twice.insert( twice.end(), one.begin(), one.end() );
    refused( SavedLayer{ 100.0f, {}, { 1, 0, 0, 0 }, twice, {} }, "stored twice" );
    EXPECT_FALSE( ParseMeshKey( "12" ).has_value() );
    EXPECT_FALSE( ParseMeshKey( "zzzzzzzzzzzzzzzz" ).has_value() );
}

TEST( VoxelBlockoutReedit, MeshKeyIgnoresOrderAndSeamCopiesButNotTheShape )
{
    const std::vector<glm::vec3> quad{ { 0, 0, 0 }, { 100, 0, 0 }, { 100, 0, 100 }, { 0, 0, 100 } };
    std::vector<glm::vec3>       seams{ quad[2], quad[0], quad[3], quad[1], quad[0], quad[2] };
    EXPECT_EQ( MeshKey( quad, 2 ), MeshKey( seams, 2 ) );
    EXPECT_NE( MeshKey( quad, 2 ), MeshKey( quad, 4 ) );
    auto moved = quad;
    moved[1].y = 5.0f;
    EXPECT_NE( MeshKey( quad, 2 ), MeshKey( moved, 2 ) );
}

TEST( VoxelBlockoutReedit, CompactMaterialsMakesTheIdsInUseTheSlotIndices )
{
    Volume    v = Ground();
    WorkPlane a;
    v.PushPull( a, Rect{ 0, 0, 0, 0 }, +1, 1, 4 );
    ASSERT_TRUE( v.Freeze() );
    v.m_Unit = 100.0f;
    WorkPlane b;
    v.PushPull( b, Rect{ 0, 0, 3, 3 }, +1, 1, 7 );
    const auto before = v.Bake();
    ASSERT_EQ( before.SubmeshMaterialIds, ( std::vector<int>{ 4, 7 } ) );
    v.CompactMaterials( before.SubmeshMaterialIds );
    EXPECT_EQ( v.Bake().SubmeshMaterialIds, ( std::vector<int>{ 0, 1 } ) );
}

TEST( VoxelBlockoutReedit, AFrameThroughAnEntityAndBackIsTheSameFrame )
{
    const GridFrame entity = MakeGridFrame( { 30.0f, -5.0f, 12.0f }, { 10.0f, 70.0f, -20.0f } );
    const GridFrame piece  = MakeGridFrame( { 100.0f, 0.0f, 50.0f }, { 0.0f, 15.0f, 0.0f } );
    EXPECT_TRUE( entity.Inverse().Compose( entity.Compose( piece ) ).SameAs( piece ) );
    const glm::vec3 p( 7.0f, 8.0f, 9.0f );
    const glm::vec3 w = entity.Compose( piece ).ToWorldPoint( p );
    EXPECT_LT( glm::length( w - entity.ToWorldPoint( piece.ToWorldPoint( p ) ) ), 1e-3f );
}
