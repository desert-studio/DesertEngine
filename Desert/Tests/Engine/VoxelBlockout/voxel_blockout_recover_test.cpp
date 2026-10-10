// CG-REEDIT1: the CubeGrid tool reopens on a mesh that carries no voxels - the blocks are recovered from its
// axis-aligned faces (VoxelBlockout::FromBoxMesh). The relations pinned:
//  - what the bake of a volume recovers to IS that volume: the same cells in the world, the same material on
//    every exposed face, whatever the bake merged or split;
//  - the lattice is found from the mesh (a 3x1x1 box of 12 triangles anywhere recovers to three blocks of
//    its step, at its corner);
//  - what is not a closed volume of whole axis-aligned blocks is refused, by reason.
#include <Engine/Geometry/VoxelBlockout.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <map>
#include <string>
#include <tuple>
#include <vector>

using namespace Desert::Geometry::VoxelBlockout;

namespace
{
    // A bake as FromBoxMesh reads a mesh: positions, triangles, the material of each triangle.
    struct Soup
    {
        std::vector<glm::dvec3>         Positions;
        std::vector<std::array<int, 3>> Triangles;
        std::vector<int>                Materials;
    };

    Soup SoupOf( const Desert::Geometry::RenderMeshData& m )
    {
        Soup s;
        for ( const auto& vtx : m.Vertices )
            s.Positions.emplace_back( vtx.Position );
        for ( size_t sm = 0; sm < m.Submeshes.size(); ++sm )
        {
            const auto& sub = m.Submeshes[sm];
            for ( uint32_t t = sub.IndexOffset / 3; t < ( sub.IndexOffset + sub.IndexCount ) / 3; ++t )
            {
                const auto& i = m.Indices[t];
                s.Triangles.push_back( { static_cast<int>( sub.VertexOffset + i.V1 ),
                                         static_cast<int>( sub.VertexOffset + i.V2 ),
                                         static_cast<int>( sub.VertexOffset + i.V3 ) } );
                s.Materials.push_back( m.SubmeshMaterialIds.empty() ? 0 : m.SubmeshMaterialIds[sm] );
            }
        }
        return s;
    }

    // Every exposed face of every layer, keyed by its world centre (to 1/100 cm) and direction, with its
    // material: equal maps are the same solid with the same paint, however it is split into layers and cells.
    using FaceKey = std::tuple<long long, long long, long long, int>;
    std::map<FaceKey, int> ExposedFaces( const Volume& v )
    {
        std::map<FaceKey, int> out;
        for ( const Layer& l : v.m_Frozen )
            for ( const auto& [key, cell] : l.Cells )
            {
                const glm::ivec3 c = Unpack( key );
                for ( int f = 0; f < 6; ++f )
                {
                    if ( v.SolidAt( c + kNeighbor[f], l.Unit, l.Frame ) )
                        continue;
                    const glm::vec3 centre = l.Frame.ToWorldPoint(
                         ( glm::vec3( c ) + 0.5f + 0.5f * glm::vec3( kNeighbor[f] ) ) * l.Unit );
                    out[{ std::llround( centre.x * 100.0 ), std::llround( centre.y * 100.0 ),
                          std::llround( centre.z * 100.0 ), f }] = cell.Mat[f];
                }
            }
        return out;
    }

    // An L of two materials at Block Size 50: a 3x1x2 slab, then a 1x2x1 post on its corner in material 2.
    Volume PaintedL()
    {
        Volume v;
        v.m_Unit = 50.0f;
        WorkPlane floor;
        v.PushPull( floor, Rect{ 0, 1, 0, 2 }, +1, 1, 1 );
        WorkPlane top{ 1, 1, 1 };
        v.PushPull( top, Rect{ 0, 0, 0, 0 }, +1, 2, 2 );
        return v;
    }
} // namespace

TEST( VoxelBlockoutRecover, TheBakeOfAVolumeRecoversToThatVolume )
{
    Volume built = PaintedL();
    ASSERT_TRUE( built.Freeze() );
    const Soup soup = SoupOf( built.Bake() );
    ASSERT_FALSE( soup.Triangles.empty() );

    auto recovered = FromBoxMesh( soup.Positions, soup.Triangles, soup.Materials, 1.0f );
    ASSERT_TRUE( recovered.IsSuccess() ) << recovered.GetError();
    ASSERT_EQ( recovered.GetValue().m_Frozen.size(), 1u );
    EXPECT_FLOAT_EQ( recovered.GetValue().m_Frozen[0].Unit, 50.0f );
    EXPECT_EQ( recovered.GetValue().m_Frozen[0].Cells.size(), 6u + 2u );

    const auto want = ExposedFaces( built );
    const auto got  = ExposedFaces( recovered.GetValue() );
    ASSERT_EQ( got.size(), want.size() );
    EXPECT_EQ( got, want ) << "a recovered face moved, vanished or changed material";
}

TEST( VoxelBlockoutRecover, TheLatticeIsFoundFromTheMesh )
{
    // One 300 x 100 x 100 box at (130, -20, 7): 8 corners, 12 triangles, every face one merged quad.
    const glm::dvec3        o( 130.0, -20.0, 7.0 );
    std::vector<glm::dvec3> p;
    p.reserve( 8 );
    for ( int i = 0; i < 8; ++i )
        p.push_back( o + glm::dvec3( ( ( i & 1 ) != 0 ) ? 300.0 : 0.0, ( ( i & 2 ) != 0 ) ? 100.0 : 0.0,
                                     ( ( i & 4 ) != 0 ) ? 100.0 : 0.0 ) );
    // Outward CCW: -X, +X, -Y, +Y, -Z, +Z.
    const std::vector<std::array<int, 3>> tris = { { 0, 4, 6 }, { 0, 6, 2 }, { 1, 3, 7 }, { 1, 7, 5 },
                                                   { 0, 1, 5 }, { 0, 5, 4 }, { 2, 6, 7 }, { 2, 7, 3 },
                                                   { 0, 2, 3 }, { 0, 3, 1 }, { 4, 5, 7 }, { 4, 7, 6 } };
    auto                                  r    = FromBoxMesh( p, tris, std::vector<int>( tris.size(), 0 ), 1.0f );
    ASSERT_TRUE( r.IsSuccess() ) << r.GetError();
    const Layer& l = r.GetValue().m_Frozen.at( 0 );
    EXPECT_FLOAT_EQ( l.Unit, 100.0f );
    EXPECT_EQ( l.Cells.size(), 3u );
    EXPECT_LT( glm::length( l.Frame.Origin - glm::vec3( o ) ), 1e-4f );
    for ( int x = 0; x < 3; ++x )
        EXPECT_TRUE( l.Cells.contains( Pack( { x, 0, 0 } ) ) ) << "block " << x;

    // Turned inside out (every triangle wound backwards) it encloses nothing: refused, not inverted.
    std::vector<std::array<int, 3>> inverted = tris;
    for ( auto& t : inverted )
        std::swap( t[1], t[2] );
    auto inside = FromBoxMesh( p, inverted, std::vector<int>( tris.size(), 0 ), 1.0f );
    ASSERT_FALSE( inside.IsSuccess() );
    EXPECT_NE( inside.GetError().find( "not a closed volume" ), std::string::npos ) << inside.GetError();
}

TEST( VoxelBlockoutRecover, WhatIsNotWholeBlocksIsRefusedByReason )
{
    auto why = []( const Soup& s, float minUnit )
    {
        auto r = FromBoxMesh( s.Positions, s.Triangles, s.Materials, minUnit );
        return r.IsSuccess() ? std::string( "<recovered>" ) : r.GetError();
    };
    Volume one;
    one.m_Unit = 100.0f;
    WorkPlane floor;
    one.PushPull( floor, Rect{ 0, 0, 0, 0 }, +1, 1, 0 );
    const Soup cube = SoupOf( one.Bake() );
    ASSERT_EQ( why( cube, 1.0f ), "<recovered>" );

    EXPECT_NE( why( Soup{}, 1.0f ).find( "no triangles" ), std::string::npos );
    // Finer than the smallest Block Size.
    EXPECT_NE( why( cube, 150.0f ).find( "finer than the smallest Block Size" ), std::string::npos );
    // One triangle of a quad missing: half a square.
    Soup holed = cube;
    holed.Triangles.pop_back();
    holed.Materials.pop_back();
    EXPECT_NE( why( holed, 1.0f ).find( "not whole blocks" ), std::string::npos ) << why( holed, 1.0f );
    // A whole face missing: the X sweep closes but a face is not there, or a column never closes.
    Soup open = cube;
    open.Triangles.resize( open.Triangles.size() - 2 );
    open.Materials.resize( open.Materials.size() - 2 );
    EXPECT_NE( why( open, 1.0f ).find( "not a closed volume" ), std::string::npos ) << why( open, 1.0f );
    // A face doubled: two faces on one square.
    Soup doubled = cube;
    doubled.Triangles.push_back( cube.Triangles[0] );
    doubled.Triangles.push_back( cube.Triangles[1] );
    doubled.Materials.push_back( 0 );
    doubled.Materials.push_back( 0 );
    EXPECT_NE( why( doubled, 1.0f ).find( "overlap" ), std::string::npos ) << why( doubled, 1.0f );
    // A Corner Mode slope.
    Volume ramp = one;
    ASSERT_TRUE( ramp.ApplyCornerHeights( WorkPlane{ 1, 1, 1 }, Rect{ 0, 0, 0, 0 },
                                          { 0, 0, CornerDen / 2, CornerDen / 2 }, false )
                      .IsSuccess() );
    EXPECT_NE( why( SoupOf( ramp.Bake() ), 1.0f ).find( "not axis-aligned" ), std::string::npos );
    // A material that does not fit a cell face.
    Soup painted         = cube;
    painted.Materials[0] = 300;
    EXPECT_NE( why( painted, 1.0f ).find( "material 300" ), std::string::npos );
}
