#include <gtest/gtest.h>

#include <Engine/Geometry/MeshSimplifier.hpp>

#include <vector>

using Desert::Geometry::SimplifyMesh;

namespace
{
    // A flat NxN grid of vertices triangulated into a plane — plenty of coplanar triangles for the
    // simplifier to collapse.
    void MakeGrid( int n, std::vector<float>& positions, std::vector<uint32_t>& indices )
    {
        for ( int y = 0; y <= n; ++y )
            for ( int x = 0; x <= n; ++x )
            {
                positions.push_back( static_cast<float>( x ) );
                positions.push_back( static_cast<float>( y ) );
                positions.push_back( 0.0f );
            }
        const int stride = n + 1;
        for ( int y = 0; y < n; ++y )
            for ( int x = 0; x < n; ++x )
            {
                const uint32_t a = y * stride + x;
                const uint32_t b = a + 1;
                const uint32_t c = a + stride;
                const uint32_t d = c + 1;
                indices.insert( indices.end(), { a, c, b, b, c, d } );
            }
    }
} // namespace

TEST( MeshSimplifier, ReducesTriangleCount )
{
    std::vector<float>    pos;
    std::vector<uint32_t> idx;
    MakeGrid( 32, pos, idx );
    const std::size_t vertexCount = pos.size() / 3;

    const auto lod = SimplifyMesh( pos.data(), vertexCount, idx, 0.25f );

    EXPECT_LT( lod.Indices.size(), idx.size() );      // simplified
    EXPECT_EQ( lod.Indices.size() % 3, 0u );          // still whole triangles
    EXPECT_GE( lod.Indices.size(), 3u );              // not empty
    for ( uint32_t i : lod.Indices )
        EXPECT_LT( i, vertexCount );                  // indices stay in range (LOD reuses the vertex buffer)
}

TEST( MeshSimplifier, LowerRatioYieldsFewerTriangles )
{
    std::vector<float>    pos;
    std::vector<uint32_t> idx;
    MakeGrid( 40, pos, idx );
    const std::size_t vc = pos.size() / 3;

    const auto half    = SimplifyMesh( pos.data(), vc, idx, 0.5f );
    const auto quarter = SimplifyMesh( pos.data(), vc, idx, 0.1f );

    EXPECT_LE( quarter.Indices.size(), half.Indices.size() );
}

TEST( MeshSimplifier, DegenerateInputReturnedUnchanged )
{
    std::vector<uint32_t> idx = { 0, 1, 2 };
    const float           pos[9] = { 0, 0, 0, 1, 0, 0, 0, 1, 0 };
    // Fewer than 3 indices / null positions -> passthrough.
    EXPECT_TRUE( SimplifyMesh( nullptr, 3, idx, 0.5f ).Indices == idx );
    EXPECT_TRUE( SimplifyMesh( pos, 3, {}, 0.5f ).Indices.empty() );
}
