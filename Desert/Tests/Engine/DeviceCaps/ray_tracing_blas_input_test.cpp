// The Vulkan BLAS build input of a static mesh (RT-SCENE): VulkanRayTracingGeometry::BuildStatic hands exactly
// TranslateBlasGeometry(DescribeStaticMeshGeometry(submeshes), vertexAddress, indexAddress) to the build-sizes
// query and the build. Device-free: the mapping is checked field by field.

#include <Engine/Graphic/API/Vulkan/VulkanRayTracingBuildInput.hpp>

#include <gtest/gtest.h>

#include <vector>

using namespace Desert;
using namespace Desert::Graphic::API::Vulkan;
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

    constexpr VkDeviceAddress kVertexAddress = 0x10000;
    constexpr VkDeviceAddress kIndexAddress  = 0x80000;
} // namespace

// Red if a segment is merged/dropped, if a submesh's range is not its own (primitiveOffset/firstVertex), or if the
// build-sizes counts differ from the ranges' primitive counts
// (VUID-vkGetAccelerationStructureBuildSizesKHR-03619).
TEST( RayTracingBlasInput, OneGeometryAndRangePerSegmentWithTheSubmeshRange )
{
    const std::vector<Submesh> submeshes = { MakeSubmesh( 0, 4, 0, 6 ), MakeSubmesh( 4, 8, 6, 12 ) };
    const auto                 segments  = DescribeStaticMeshGeometry( submeshes, 0 );
    const VulkanBlasBuildInput input     = TranslateBlasGeometry( segments, kVertexAddress, kIndexAddress );

    ASSERT_EQ( input.Geometries.size(), 2u );
    ASSERT_EQ( input.Ranges.size(), 2u );
    ASSERT_EQ( input.MaxPrimitiveCounts.size(), 2u );

    EXPECT_EQ( input.Ranges[0].primitiveCount, 2u );
    EXPECT_EQ( input.Ranges[0].primitiveOffset, 0u );
    EXPECT_EQ( input.Ranges[0].firstVertex, 0u );
    EXPECT_EQ( input.Ranges[1].primitiveCount, 4u );
    EXPECT_EQ( input.Ranges[1].primitiveOffset, 6u * sizeof( uint32_t ) );
    EXPECT_EQ( input.Ranges[1].firstVertex, 4u );
    for ( std::size_t i = 0; i < input.Ranges.size(); ++i )
        EXPECT_EQ( input.MaxPrimitiveCounts[i], input.Ranges[i].primitiveCount );
}

// Red if the triangles read anything other than the mesh's position stream (format, stride, offset) and its uint32
// indices, or if a geometry is not opaque while no any-hit path exists.
TEST( RayTracingBlasInput, TrianglesReadTheMeshPositionsAndIndicesAsOpaque )
{
    const std::vector<Submesh> submeshes = { MakeSubmesh( 10, 5, 0, 3 ) };
    const VulkanBlasBuildInput input =
         TranslateBlasGeometry( DescribeStaticMeshGeometry( submeshes, 0 ), kVertexAddress, kIndexAddress );
    ASSERT_EQ( input.Geometries.size(), 1u );

    const VkAccelerationStructureGeometryKHR& geometry = input.Geometries[0];
    EXPECT_EQ( geometry.sType, VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR );
    EXPECT_EQ( geometry.geometryType, VK_GEOMETRY_TYPE_TRIANGLES_KHR );
    EXPECT_EQ( geometry.flags, static_cast<VkGeometryFlagsKHR>( VK_GEOMETRY_OPAQUE_BIT_KHR ) );

    const VkAccelerationStructureGeometryTrianglesDataKHR& triangles = geometry.geometry.triangles;
    EXPECT_EQ( triangles.sType, VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR );
    EXPECT_EQ( triangles.vertexFormat, VK_FORMAT_R32G32B32_SFLOAT );
    EXPECT_EQ( triangles.vertexStride, sizeof( Vertex ) );
    EXPECT_EQ( triangles.vertexData.deviceAddress, kVertexAddress + offsetof( Vertex, Position ) );
    EXPECT_EQ( triangles.maxVertex, 14u ); // VertexOffset + VertexCount - 1
    EXPECT_EQ( triangles.indexType, VK_INDEX_TYPE_UINT32 );
    EXPECT_EQ( triangles.indexData.deviceAddress, kIndexAddress );
}

// Red if the build is not a bottom-level BUILD of exactly the translated geometries, or if the scratch alignment
// rule can hand out an address that is not 256-aligned (or skips past the 256-byte padding the scratch buffer
// carries).
TEST( RayTracingBlasInput, BuildInfoIsABottomLevelBuildOfTheGeometriesAndScratchAligns )
{
    const std::vector<Submesh> submeshes = { MakeSubmesh( 0, 3, 0, 3 ), MakeSubmesh( 3, 3, 3, 3 ) };
    const VulkanBlasBuildInput input =
         TranslateBlasGeometry( DescribeStaticMeshGeometry( submeshes, 0 ), kVertexAddress, kIndexAddress );
    const VkAccelerationStructureBuildGeometryInfoKHR info = MakeBlasBuildInfo( input );

    EXPECT_EQ( info.type, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR );
    EXPECT_EQ( info.mode, VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR );
    EXPECT_EQ( info.geometryCount, 2u );
    EXPECT_EQ( info.pGeometries, input.Geometries.data() );
    EXPECT_EQ( info.ppGeometries, nullptr );

    for ( VkDeviceAddress address : { VkDeviceAddress{ 0 }, VkDeviceAddress{ 1 }, VkDeviceAddress{ 255 },
                                      VkDeviceAddress{ 256 }, VkDeviceAddress{ 0x1234567 } } )
    {
        const VkDeviceAddress aligned = AlignScratchAddress( address );
        EXPECT_EQ( aligned % kMaxAccelerationStructureScratchAlignment, 0u );
        EXPECT_GE( aligned, address );
        EXPECT_LT( aligned - address, kMaxAccelerationStructureScratchAlignment );
    }
}
