#pragma once

#include <Engine/Graphic/Systems/RayTracing/RayTracingGeometry.hpp>

#include <vulkan/vulkan.h>

#include <span>
#include <vector>

// THE VULKAN MAPPING OF A STATIC MESH'S BLAS BUILD INPUT: DescribeStaticMeshGeometry's segments one to one onto
// VkAccelerationStructureGeometryKHR + VkAccelerationStructureBuildRangeInfoKHR. Pure (no device), so the
// DeviceCaps suite checks it; VulkanRayTracingGeometry builds from exactly this.
namespace Desert::Graphic::API::Vulkan
{
    // The required-limits table caps minAccelerationStructureScratchOffsetAlignment at 256 (a power of two), so a
    // scratch buffer padded by 256 bytes always holds an aligned address without querying the property.
    inline constexpr VkDeviceSize kMaxAccelerationStructureScratchAlignment = 256;

    [[nodiscard]] constexpr VkDeviceAddress AlignScratchAddress( VkDeviceAddress address )
    {
        return ( address + kMaxAccelerationStructureScratchAlignment - 1 ) &
               ~( kMaxAccelerationStructureScratchAlignment - 1 );
    }

    // One Vulkan geometry and one build range per segment; MaxPrimitiveCounts feeds the build-sizes query.
    struct VulkanBlasBuildInput
    {
        std::vector<VkAccelerationStructureGeometryKHR>       Geometries;
        std::vector<VkAccelerationStructureBuildRangeInfoKHR> Ranges;
        std::vector<uint32_t>                                 MaxPrimitiveCounts;
    };

    [[nodiscard]] inline VkFormat ToVkVertexFormat( RayTracing::RayTracingVertexFormat format )
    {
        switch ( format )
        {
            case RayTracing::RayTracingVertexFormat::Float3:
                return VK_FORMAT_R32G32B32_SFLOAT;
        }
        return VK_FORMAT_UNDEFINED;
    }

    [[nodiscard]] inline VkIndexType ToVkIndexType( RayTracing::RayTracingIndexType type )
    {
        switch ( type )
        {
            case RayTracing::RayTracingIndexType::UInt32:
                return VK_INDEX_TYPE_UINT32;
        }
        return VK_INDEX_TYPE_NONE_KHR;
    }

    // Every geometry reads the mesh's one vertex buffer (from the position's offset) and one index buffer; the
    // submesh is selected by the range (primitiveOffset = byte offset into the indices, firstVertex = base
    // vertex). OPAQUE: no any-hit / alpha-test path exists, so every triangle terminates a ray; a masked material
    // gets NON_OPAQUE with the alpha-test piece.
    [[nodiscard]] inline VulkanBlasBuildInput
    TranslateBlasGeometry( std::span<const RayTracing::RayTracingGeometrySegment> segments,
                           VkDeviceAddress vertexBufferAddress, VkDeviceAddress indexBufferAddress )
    {
        VulkanBlasBuildInput input;
        input.Geometries.reserve( segments.size() );
        input.Ranges.reserve( segments.size() );
        input.MaxPrimitiveCounts.reserve( segments.size() );
        for ( const RayTracing::RayTracingGeometrySegment& segment : segments )
        {
            VkAccelerationStructureGeometryKHR geometry{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
            geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
            geometry.flags =
                 segment.Opaque ? VK_GEOMETRY_OPAQUE_BIT_KHR : VK_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION_BIT_KHR;
            VkAccelerationStructureGeometryTrianglesDataKHR& triangles = geometry.geometry.triangles;
            triangles.sType        = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
            triangles.vertexFormat = ToVkVertexFormat( segment.VertexFormat );
            triangles.vertexData.deviceAddress = vertexBufferAddress + segment.PositionOffset;
            triangles.vertexStride             = segment.VertexStride;
            triangles.maxVertex                = segment.MaxVertex;
            triangles.indexType                = ToVkIndexType( segment.IndexType );
            triangles.indexData.deviceAddress  = indexBufferAddress;
            input.Geometries.push_back( geometry );

            VkAccelerationStructureBuildRangeInfoKHR range{};
            range.primitiveCount  = segment.PrimitiveCount;
            range.primitiveOffset = segment.IndexByteOffset;
            range.firstVertex     = segment.FirstVertex;
            range.transformOffset = 0;
            input.Ranges.push_back( range );
            input.MaxPrimitiveCounts.push_back( segment.PrimitiveCount );
        }
        return input;
    }

    // The build (and the build-sizes query) of a static BLAS: built once at upload, traced for its lifetime, so
    // PREFER_FAST_TRACE (compaction comes with the memory piece). Points into @p input.
    [[nodiscard]] inline VkAccelerationStructureBuildGeometryInfoKHR
    MakeBlasBuildInfo( const VulkanBlasBuildInput& input )
    {
        VkAccelerationStructureBuildGeometryInfoKHR info{
             VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
        info.type          = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        info.flags         = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        info.mode          = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        info.geometryCount = static_cast<uint32_t>( input.Geometries.size() );
        info.pGeometries   = input.Geometries.data();
        return info;
    }
} // namespace Desert::Graphic::API::Vulkan
