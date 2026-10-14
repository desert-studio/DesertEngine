// A static mesh's BLAS build input (RT-SCENE): DescribeStaticMeshGeometry is the one rule the Vulkan BLAS object
// maps to VkAccelerationStructureGeometryTrianglesDataKHR + VkAccelerationStructureBuildRangeInfoKHR. Device-free.

#include <Engine/Graphic/Systems/RayTracing/RayTracingGeometry.hpp>

#include <gtest/gtest.h>

#include <vector>

using namespace Desert;
using namespace Desert::Graphic::RayTracing;

namespace
{
    Submesh MakeSubmesh( uint32_t vertexOffset, uint32_t vertexCount, uint32_t indexOffset, uint32_t indexCount )
    {
        Submesh submesh{};
        submesh.VertexOffset = vertexOffset;
        submesh.VertexCount  = vertexCount;
        submesh.IndexOffset  = indexOffset;
        submesh.IndexCount   = indexCount;
        return submesh;
    }
} // namespace

// Red if the stride or the position offset stops following Desert::Vertex (a BLAS built at the wrong stride reads
// normals as positions), if the index type stops being uint32, or if the byte offset is computed in indices.
TEST( RayTracingGeometry, ASubmeshDescribesTheMeshVertexLayoutAndItsIndexRangeInBytes )
{
    const std::vector<Submesh> submeshes{ MakeSubmesh( 100, 24, 36, 60 ) };
    const auto                 segments = DescribeStaticMeshGeometry( submeshes );
    ASSERT_EQ( segments.size(), 1u );
    const RayTracingGeometrySegment& s = segments[0];
    EXPECT_EQ( s.VertexFormat, RayTracingVertexFormat::Float3 );
    EXPECT_EQ( s.VertexStride, sizeof( Vertex ) );
    EXPECT_EQ( s.PositionOffset, offsetof( Vertex, Position ) );
    EXPECT_EQ( s.FirstVertex, 100u );
    EXPECT_EQ( s.MaxVertex, 123u );
    EXPECT_EQ( s.IndexType, RayTracingIndexType::UInt32 );
    EXPECT_EQ( s.IndexByteOffset, 36u * 4u );
    EXPECT_EQ( s.PrimitiveCount, 20u );
}

// Red if the BLAS follows the submesh's own range when it has a LOD chain of a different extent, or reads a
// coarser level: a ray sees LOD 0 whatever the raster drew.
TEST( RayTracingGeometry, TheBuildReadsLodZeroOfAChainedSubmesh )
{
    Submesh submesh     = MakeSubmesh( 0, 8, 0, 36 );
    submesh.LODs        = { { 600, 30 }, { 900, 12 } };
    const auto segments = DescribeStaticMeshGeometry( std::vector<Submesh>{ submesh } );
    ASSERT_EQ( segments.size(), 1u );
    EXPECT_EQ( segments[0].IndexByteOffset, 600u * 4u );
    EXPECT_EQ( segments[0].PrimitiveCount, 10u );
}

// Red if an empty submesh becomes a zero-primitive geometry (legal but a wasted build entry) or a partial triangle
// is counted as one (the build would read past the submesh's indices).
TEST( RayTracingGeometry, EmptySubmeshesAndPartialTrianglesAreNotGeometry )
{
    const std::vector<Submesh> submeshes{ MakeSubmesh( 0, 3, 0, 2 ), MakeSubmesh( 3, 0, 2, 0 ),
                                          MakeSubmesh( 3, 4, 2, 7 ) };
    const auto                 segments = DescribeStaticMeshGeometry( submeshes );
    ASSERT_EQ( segments.size(), 1u );
    EXPECT_EQ( segments[0].FirstVertex, 3u );
    EXPECT_EQ( segments[0].PrimitiveCount, 2u );
}
