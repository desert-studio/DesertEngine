#include <Engine/Geometry/GreedyMesher.hpp>
#include <Engine/Geometry/LODSelection.hpp>
#include <Engine/Geometry/MeshLOD.hpp>

#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <tuple>

using Desert::Index;
using Desert::Submesh;
using Desert::Vertex;
using Desert::Geometry::BuildLODIndexBuffer;
using Desert::Geometry::SelectLOD;
using Desert::Geometry::SimplifyLODLevels;

namespace
{
    // An N*N-quad grid on the XY plane -> 2*N*N triangles, submesh-local 0-based indices. Big enough (>8 tris)
    // that meshopt actually simplifies.
    struct Grid
    {
        std::vector<Vertex> Vertices;
        std::vector<Index>  Indices;
        Submesh             Sub;
    };

    Grid MakeGrid( uint32_t n )
    {
        Grid g;
        for ( uint32_t y = 0; y <= n; ++y )
            for ( uint32_t x = 0; x <= n; ++x )
            {
                Vertex v{};
                v.Position = { static_cast<float>( x ), static_cast<float>( y ), 0.0f };
                g.Vertices.push_back( v );
            }
        const uint32_t stride = n + 1;
        for ( uint32_t y = 0; y < n; ++y )
            for ( uint32_t x = 0; x < n; ++x )
            {
                const uint32_t a = y * stride + x, b = a + 1, c = a + stride, d = c + 1;
                g.Indices.push_back( { a, c, b } );
                g.Indices.push_back( { b, c, d } );
            }
        g.Sub.Name         = "grid";
        g.Sub.VertexOffset = 0;
        g.Sub.VertexCount  = static_cast<uint32_t>( g.Vertices.size() );
        g.Sub.IndexOffset  = 0;
        g.Sub.IndexCount   = static_cast<uint32_t>( g.Indices.size() * 3 );
        return g;
    }

    bool SameTris( const std::vector<Index>& a, const std::vector<Index>& b )
    {
        if ( a.size() != b.size() )
            return false;
        for ( size_t i = 0; i < a.size(); ++i )
            if ( a[i].V1 != b[i].V1 || a[i].V2 != b[i].V2 || a[i].V3 != b[i].V3 )
                return false;
        return true;
    }
} // namespace

// The whole point of baking: cook (SimplifyLODLevels) + load (BuildLODIndexBuffer assemble) must produce the
// EXACT same GPU index buffer + per-submesh LOD ranges as generating at load. Behaviour-preserving.
TEST( MeshLOD, BakedEqualsGenerated )
{
    const Grid g = MakeGrid( 6 ); // 72 triangles

    // Generate-at-load (BakedLODs empty).
    std::vector<Submesh> genSubs = { g.Sub };
    const auto           gpuGen  = BuildLODIndexBuffer( g.Vertices, g.Indices, genSubs );

    // Cook: bake the levels, then assemble from them (BakedLODs populated).
    std::vector<float> pos;
    for ( const auto& v : g.Vertices )
    {
        pos.push_back( v.Position.x );
        pos.push_back( v.Position.y );
        pos.push_back( v.Position.z );
    }
    const auto           baked     = SimplifyLODLevels( pos.data(), g.Sub.VertexCount, g.Indices );
    std::vector<Submesh> bakedSubs = { g.Sub };
    bakedSubs[0].BakedLODs         = baked;
    const auto gpuBaked            = BuildLODIndexBuffer( g.Vertices, g.Indices, bakedSubs );

    EXPECT_TRUE( SameTris( gpuGen, gpuBaked ) ) << "baked GPU index buffer differs from generated";

    ASSERT_EQ( genSubs[0].LODs.size(), bakedSubs[0].LODs.size() );
    ASSERT_EQ( genSubs[0].LODs.size(), 4u ); // LOD0 + 3 ratios
    for ( size_t i = 0; i < genSubs[0].LODs.size(); ++i )
    {
        EXPECT_EQ( genSubs[0].LODs[i].IndexOffset, bakedSubs[0].LODs[i].IndexOffset ) << "LOD " << i;
        EXPECT_EQ( genSubs[0].LODs[i].IndexCount, bakedSubs[0].LODs[i].IndexCount ) << "LOD " << i;
    }
    // LOD0 is byte-identical to the base range.
    EXPECT_EQ( genSubs[0].LODs[0].IndexOffset, g.Sub.IndexOffset );
    EXPECT_EQ( genSubs[0].LODs[0].IndexCount, g.Sub.IndexCount );
}

// Assembly arithmetic in isolation (no meshopt): hand-crafted baked levels -> correct offsets/counts, and an
// empty level reuses the previous range (monotonic).
TEST( MeshLOD, AssembleOffsetsAndReuse )
{
    // 10-triangle base submesh.
    std::vector<Index>  base( 10, Index{ 0, 1, 2 } );
    std::vector<Vertex> verts( 8 ); // >0, positions unused on the baked path
    Submesh             s;
    s.VertexOffset = 0;
    s.VertexCount  = 8;
    s.IndexOffset  = 0;
    s.IndexCount   = 30;
    s.BakedLODs    = {
         std::vector<Index>( 4, Index{ 0, 1, 2 } ), // L1: 4 tris
         std::vector<Index>( 2, Index{ 0, 1, 2 } ), // L2: 2 tris
         std::vector<Index>{},                      // L3: empty -> reuse L2
    };

    std::vector<Submesh> subs = { s };
    const auto           gpu  = BuildLODIndexBuffer( verts, base, subs );

    ASSERT_EQ( subs[0].LODs.size(), 4u );
    EXPECT_EQ( subs[0].LODs[0].IndexOffset, 0u );
    EXPECT_EQ( subs[0].LODs[0].IndexCount, 30u );
    EXPECT_EQ( subs[0].LODs[1].IndexOffset, 30u ); // after 10 base tris (30 indices)
    EXPECT_EQ( subs[0].LODs[1].IndexCount, 12u );  // 4 tris
    EXPECT_EQ( subs[0].LODs[2].IndexOffset, 42u ); // after 14 tris
    EXPECT_EQ( subs[0].LODs[2].IndexCount, 6u );   // 2 tris
    EXPECT_EQ( subs[0].LODs[3].IndexOffset, 42u ); // empty -> reused L2
    EXPECT_EQ( subs[0].LODs[3].IndexCount, 6u );
    EXPECT_EQ( gpu.size(), 16u ); // 10 base + 4 + 2
}

// ---------------------------------------------------------------------------------------------------
// LOD SELECTION (Geometry::SelectLOD) — the policy the renderer draws with AND the Details panel
// reports. Both read the same function, so these cases pin what "drawing LOD n" means.
// ---------------------------------------------------------------------------------------------------

namespace
{
    // One submesh with a symmetric AABB of the given half-size (unit-ish mesh centred on the origin).
    std::vector<Submesh> BoundedSubmesh( float halfSize )
    {
        Submesh s{};
        s.BoundingBox.Min = glm::vec3( -halfSize );
        s.BoundingBox.Max = glm::vec3( halfSize );
        return { s };
    }

    glm::mat4 AtOrigin( float scale = 1.0f )
    {
        return glm::scale( glm::mat4( 1.0f ), glm::vec3( scale ) );
    }
} // namespace

TEST( LODSelection, ForcedLevelWinsOverEverything )
{
    const auto subs = BoundedSubmesh( 50.0f );
    // Far away (would be the coarsest level) but pinned to LOD 1.
    EXPECT_EQ( SelectLOD( AtOrigin(), subs, glm::vec3( 0.0f, 0.0f, 100000.0f ), /*forced*/ 1, /*bias*/ 0 ), 1u );
    // The bias is ignored while a level is forced.
    EXPECT_EQ( SelectLOD( AtOrigin(), subs, glm::vec3( 0.0f, 0.0f, 100000.0f ), /*forced*/ 0, /*bias*/ 3 ), 0u );
}

TEST( LODSelection, CoarsensWithDistance )
{
    const auto subs = BoundedSubmesh( 50.0f ); // radius ~86.6 cm

    // (not named near/far: those are legacy macros in the Windows SDK headers)
    const uint32_t closeUp = SelectLOD( AtOrigin(), subs, glm::vec3( 0.0f, 0.0f, 100.0f ), -1, 0 );
    const uint32_t middle  = SelectLOD( AtOrigin(), subs, glm::vec3( 0.0f, 0.0f, 800.0f ), -1, 0 );
    const uint32_t distant = SelectLOD( AtOrigin(), subs, glm::vec3( 0.0f, 0.0f, 100000.0f ), -1, 0 );

    EXPECT_EQ( closeUp, 0u );
    EXPECT_GT( middle, closeUp );
    EXPECT_EQ( distant, 3u ); // clamped to the coarsest level the policy may pick
}

TEST( LODSelection, SizeAwareAndBiasShifts )
{
    const auto small = BoundedSubmesh( 50.0f );
    const auto big   = BoundedSubmesh( 5000.0f );
    const auto eye   = glm::vec3( 0.0f, 0.0f, 5000.0f );

    // A bigger object keeps finer detail at the same distance...
    EXPECT_LT( SelectLOD( AtOrigin(), big, eye, -1, 0 ), SelectLOD( AtOrigin(), small, eye, -1, 0 ) );
    // ...and so does the same object scaled up by its transform.
    EXPECT_LT( SelectLOD( AtOrigin( 100.0f ), small, eye, -1, 0 ), SelectLOD( AtOrigin(), small, eye, -1, 0 ) );

    // The bias shifts the automatic pick and stays inside [0, kMaxAutoLOD].
    const uint32_t base = SelectLOD( AtOrigin(), small, eye, -1, 0 );
    EXPECT_EQ( SelectLOD( AtOrigin(), small, eye, -1, 1 ), std::min( base + 1u, 3u ) );
    EXPECT_EQ( SelectLOD( AtOrigin(), small, eye, -1, -9 ), 0u );
    EXPECT_EQ( SelectLOD( AtOrigin(), small, eye, -1, 9 ), 3u );
}

TEST( LODSelection, EmptyMeshIsLODZero )
{
    EXPECT_EQ( SelectLOD( AtOrigin(), {}, glm::vec3( 0.0f, 0.0f, 100000.0f ), -1, 0 ), 0u );
}

// ---------------------------------------------------------------------------------------------------
// GREEDY MESHING (Geometry::GreedyMeshFaces) — merges coplanar voxel faces into as few quads as
// possible. The CubeGrid bake draws what this returns, so these cases pin BOTH the count and the
// coverage: a merge that loses a cell is far worse than one that merges nothing.
// ---------------------------------------------------------------------------------------------------

namespace
{
    using Desert::Geometry::FaceAxes;
    using Desert::Geometry::GreedyMeshFaces;
    using Desert::Geometry::VoxelFaceQuad;

    // A solid box of cells [0,w) x [0,h) x [0,d).
    std::vector<glm::ivec3> SolidBox( int w, int h, int d )
    {
        std::vector<glm::ivec3> cells;
        for ( int x = 0; x < w; ++x )
            for ( int y = 0; y < h; ++y )
                for ( int z = 0; z < d; ++z )
                    cells.push_back( { x, y, z } );
        return cells;
    }

    // "Exposed" = the neighbour in that direction is not part of the volume (plain face culling).
    auto CullAgainst( const std::vector<glm::ivec3>& cells )
    {
        return [set = std::set<std::tuple<int, int, int>>(
                     [&]
                     {
                         std::set<std::tuple<int, int, int>> s;
                         for ( const auto& c : cells )
                             s.insert( { c.x, c.y, c.z } );
                         return s;
                     }() )]( const glm::ivec3& c, int face )
        {
            const glm::ivec3 n = c + Desert::Geometry::kVoxelFaceNormal[face];
            return set.find( { n.x, n.y, n.z } ) == set.end();
        };
    }

    // Total cells covered by the quads of one face — must equal the exposed-face count.
    int CoveredCells( const std::vector<VoxelFaceQuad>& quads, int face )
    {
        int total = 0;
        for ( const auto& q : quads )
            if ( q.Face == face )
                total += q.SizeU * q.SizeV;
        return total;
    }
} // namespace

TEST( GreedyMesh, FlatSlabCollapsesToOneQuadPerFace )
{
    const auto cells = SolidBox( 8, 1, 5 ); // a floor slab
    const auto quads = GreedyMeshFaces( cells, CullAgainst( cells ) );

    // Six faces, each a single merged rectangle — 40 cells would otherwise be 240 quads.
    ASSERT_EQ( quads.size(), 6u );
    for ( int f = 0; f < 6; ++f )
    {
        const auto it =
             std::find_if( quads.begin(), quads.end(), [f]( const VoxelFaceQuad& q ) { return q.Face == f; } );
        ASSERT_NE( it, quads.end() ) << "face " << f << " missing";
        EXPECT_EQ( it->SizeU * it->SizeV, f == 2 || f == 3 ? 8 * 5 : ( f <= 1 ? 8 * 1 : 5 * 1 ) );
    }
}

TEST( GreedyMesh, CoversExactlyTheExposedFaces )
{
    const auto cells   = SolidBox( 4, 3, 2 );
    const auto exposed = CullAgainst( cells );
    const auto quads   = GreedyMeshFaces( cells, exposed );

    for ( int f = 0; f < 6; ++f )
    {
        int expected = 0;
        for ( const auto& c : cells )
            if ( exposed( c, f ) )
                ++expected;
        EXPECT_EQ( CoveredCells( quads, f ), expected ) << "face " << f;
    }
}

TEST( GreedyMesh, HoleSplitsTheRunInsteadOfSwallowingIt )
{
    // A 3x1x3 floor with the middle cell missing: the top face cannot be one rectangle.
    std::vector<glm::ivec3> cells;
    for ( int x = 0; x < 3; ++x )
        for ( int z = 0; z < 3; ++z )
            if ( !( x == 1 && z == 1 ) )
                cells.push_back( { x, 0, z } );

    const auto quads = GreedyMeshFaces( cells, CullAgainst( cells ) );

    // Top (+Y) covers exactly the 8 remaining cells, and needs more than one quad to do it.
    EXPECT_EQ( CoveredCells( quads, 2 ), 8 );
    const int topQuads = static_cast<int>(
         std::count_if( quads.begin(), quads.end(), []( const VoxelFaceQuad& q ) { return q.Face == 2; } ) );
    EXPECT_GT( topQuads, 1 );
}

TEST( GreedyMesh, MergeKeyKeepsDifferentSurfacesApart )
{
    const auto cells = SolidBox( 4, 1, 1 );
    // Two materials along the row: the top face must break where the key changes.
    const auto quads = GreedyMeshFaces( cells, CullAgainst( cells ),
                                        []( const glm::ivec3& c, int ) -> uint64_t { return c.x < 2 ? 1 : 2; } );

    const int topQuads = static_cast<int>(
         std::count_if( quads.begin(), quads.end(), []( const VoxelFaceQuad& q ) { return q.Face == 2; } ) );
    EXPECT_EQ( topQuads, 2 );
    EXPECT_EQ( CoveredCells( quads, 2 ), 4 );
}

TEST( GreedyMesh, EmptyVolumeAndFullyEnclosedCellProduceNothing )
{
    EXPECT_TRUE( GreedyMeshFaces( {}, []( const glm::ivec3&, int ) { return true; } ).empty() );

    // A single cell surrounded on all six sides: nothing of it is visible.
    const std::vector<glm::ivec3> one = { { 0, 0, 0 } };
    EXPECT_TRUE( GreedyMeshFaces( one, []( const glm::ivec3&, int ) { return false; } ).empty() );
}

TEST( GreedyMesh, IsDeterministic )
{
    auto       cells = SolidBox( 3, 2, 3 );
    const auto a     = GreedyMeshFaces( cells, CullAgainst( cells ) );
    std::reverse( cells.begin(), cells.end() ); // insertion order must not matter
    const auto b = GreedyMeshFaces( cells, CullAgainst( cells ) );

    ASSERT_EQ( a.size(), b.size() );
    for ( size_t i = 0; i < a.size(); ++i )
    {
        EXPECT_EQ( a[i].Face, b[i].Face );
        EXPECT_EQ( a[i].Cell, b[i].Cell );
        EXPECT_EQ( a[i].SizeU, b[i].SizeU );
        EXPECT_EQ( a[i].SizeV, b[i].SizeV );
    }
}

// ═══ PER-INSTANCE LOD: a batch is ONE draw and therefore ONE level ═══════════════════════════════
//
// Until В4 the batched draw paths passed no level at all. The per-object path beside them computed one
// and handed it to the draw; the instanced path recorded its draw with the default, level 0. So the
// SAME object rendered at a different detail depending only on whether it happened to find a twin to
// batch with — both ends of the chain looked right and the link between them dropped a property.
//
// Two rules make the fix safe, and both are asserted here rather than described in a comment:
//   * a batch splits into one draw per DISTINCT level, ascending;
//   * a mesh with no LOD chain reports level 0 for everything, so its batch does NOT split — otherwise
//     the "optimization" turns one draw into four that rasterize identical geometry.

namespace
{
    Desert::Submesh SubmeshWithLevels( std::size_t levelCount )
    {
        Desert::Submesh sm;
        sm.BoundingBox = Common::Math::AABB{ glm::vec3( -100.0f ), glm::vec3( 100.0f ) };
        sm.LODs.resize( levelCount );
        return sm;
    }
} // namespace

TEST( MeshLOD, AMeshWithNoLODChainReportsLevelZeroAsItsCoarsest )
{
    const std::vector<Desert::Submesh> submeshes( 2 );
    EXPECT_EQ( Desert::Geometry::MaxAvailableLOD( submeshes ), 0u )
         << "a primitive or procedural mesh draws its base index range whatever level it is asked for; "
            "reporting anything else splits its instanced batch into identical draws";
}

TEST( MeshLOD, TheCoarsestAvailableLevelIsTheLongestChainsLastLevel )
{
    std::vector<Desert::Submesh> submeshes;
    submeshes.push_back( SubmeshWithLevels( 4 ) );
    submeshes.push_back( SubmeshWithLevels( 2 ) );
    EXPECT_EQ( Desert::Geometry::MaxAvailableLOD( submeshes ), 3u );
}

TEST( MeshLOD, ABatchOfMixedLevelsBecomesOneDrawPerLevelAscending )
{
    const std::vector<uint32_t> levels = { 2, 0, 2, 3, 0, 0 };
    const auto                  groups = Desert::Geometry::DistinctLODs( levels );
    ASSERT_EQ( groups.size(), 3u ) << "three distinct levels among six instances";
    EXPECT_EQ( groups[0], 0u );
    EXPECT_EQ( groups[1], 2u );
    EXPECT_EQ( groups[2], 3u );
}

TEST( MeshLOD, ABatchThatAgreesOnOneLevelStaysASingleDraw )
{
    const std::vector<uint32_t> levels( 4096, 0u );
    EXPECT_EQ( Desert::Geometry::DistinctLODs( levels ).size(), 1u )
         << "a chain-less mesh answers 0 for every instance, and its batch must not split";
}

TEST( MeshLOD, TheBoundsTakingAndSubmeshTakingSelectorsAreOnePolicy )
{
    // One policy, two spellings: the ISM path asks per instance against bounds it computed once, the
    // rest asks with the submeshes in hand. A second implementation of the coverage curve is exactly
    // the drift this pair exists to make impossible.
    const std::vector<Desert::Submesh> submeshes{ SubmeshWithLevels( 4 ) };

    for ( float distance = 100.0f; distance < 200000.0f; distance *= 2.0f )
    {
        const glm::mat4 transform = glm::translate( glm::mat4( 1.0f ), glm::vec3( 0.0f, 0.0f, -distance ) );
        EXPECT_EQ( Desert::Geometry::SelectLOD( transform, submeshes, glm::vec3( 0.0f ), -1, 0 ),
                   Desert::Geometry::SelectLODFromBounds( transform, Desert::Geometry::LocalBounds( submeshes ),
                                                          glm::vec3( 0.0f ), -1, 0 ) )
             << "at " << distance << " cm";
    }
}

// THE CENSUS: every INSTANCED draw names its level. A batch recorded without one is invisible on a
// frame — it renders, at the wrong detail, and looks exactly like a batch rendered at the right one.
TEST( MeshLOD, EveryInstancedDrawCallNamesItsLODLevel )
{
    namespace fs  = std::filesystem;
    fs::path root = fs::current_path();
    for ( int i = 0; i < 8 && !( fs::exists( root / "Desert" / "Common" ) && fs::exists( root / "Editor" ) ); ++i )
    {
        root = root.parent_path();
    }
    ASSERT_TRUE( fs::exists( root / "Desert" / "Common" ) );

    const std::ifstream in( root / "Desert" / "Desert" / "Source" / "Engine" / "Graphic" / "Systems" / "Scene" /
                            "Mesh" / "MeshRenderer.cpp" );
    std::stringstream   buffer;
    buffer << in.rdbuf();
    const std::string source = buffer.str();
    ASSERT_FALSE( source.empty() );

    struct Row
    {
        const char* Pipeline; ///< the instanced pipeline the call binds
        const char* Level;    ///< the expression that must appear as its lodLevel argument
        const char* Why;
    };

    const Row rows[] = {
         { "renderer.RenderMesh( instancedPipeline", "d.LodLevel",
           "the opaque instanced batch — auto-batched statics and ISM instances" },
         { "renderer.RenderMesh( m_ShadowInstancedPipeline.get()", "b.LodLevel",
           "the cascade's instanced casters; a caster at a coarser level than the object the camera "
           "sees casts a silhouette that does not match it" },
    };

    for ( const auto& row : rows )
    {
        const std::size_t at = source.find( row.Pipeline );
        ASSERT_NE( at, std::string::npos ) << row.Pipeline << " — the call was renamed or removed";
        const std::size_t end = source.find( ");", at );
        ASSERT_NE( end, std::string::npos );
        EXPECT_NE( source.substr( at, end - at ).find( row.Level ), std::string::npos )
             << row.Pipeline << " records its draw without a LOD level (" << row.Why << ")";
    }
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
