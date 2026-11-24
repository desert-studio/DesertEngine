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
        // VK_EXT_opacity_micromap: one entry per segment that carries a micromap, chained into its geometry's
        // triangles.pNext. Reserved to the segment count before the first push, so the pointers the geometries
        // hold never move; a MOVE of the input keeps them (std::vector moves its buffer), a copy would not, so
        // the input is built where it is used (BuildStatic) and never copied.
        std::vector<VkAccelerationStructureTrianglesOpacityMicromapEXT> Micromaps;
    };

    // THE MICROMAP A NON-OPAQUE SEGMENT IS BUILT WITH (UE FRayTracingGeometrySegment + its OMM; NVIDIA OMM SDK
    // GPU baker output): the built VkMicromapEXT and the per-triangle index buffer the bake wrote (uint32, one per
    // segment triangle: the triangle's own micromap triangle, or a special index -1..-4 for a uniform triangle).
    // Micromap null = this segment has none (opaque, or not baked): today's any-hit path for all its candidates.
    struct VulkanSegmentMicromap
    {
        VkMicromapEXT   Micromap     = VK_NULL_HANDLE;
        VkDeviceAddress IndexAddress = 0;
    };

    // OPACITY MICROMAP LAYOUT (VK_OPACITY_MICROMAP_FORMAT_4_STATE_EXT): a triangle at subdivision level L has 4^L
    // micro-triangles of 2 bits each, so its data is 4^L / 4 bytes, at least one byte. One level for a whole
    // micromap (the bake writes triangle t's data at t * bytes), so the usage table has one row.
    // Level 4 (256 micro-triangles per triangle, UE / OMM SDK default): the required minimum of
    // maxOpacity4StateSubdivisionLevel, so every device with the micromap feature builds it and no property is read.
    inline constexpr uint32_t kOpacityMicromapLevel = 4u;

    [[nodiscard]] constexpr uint32_t OpacityMicromapBytesPerTriangle( const uint32_t level )
    {
        const uint32_t microTriangles = 1u << ( 2u * level );
        return microTriangles / 4u > 0u ? microTriangles / 4u : 1u;
    }
    static_assert( OpacityMicromapBytesPerTriangle( kOpacityMicromapLevel ) == 64u );

    // The build input of one micromap of @p triangleCount triangles at @p level: one usage row, 4-state.
    [[nodiscard]] inline VkMicromapUsageEXT OpacityMicromapUsage( const uint32_t triangleCount,
                                                                 const uint32_t level )
    {
        return VkMicromapUsageEXT{ .count            = triangleCount,
                                   .subdivisionLevel = level,
                                   .format           = VK_OPACITY_MICROMAP_FORMAT_4_STATE_EXT };
    }

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
                           VkDeviceAddress vertexBufferAddress, VkDeviceAddress indexBufferAddress,
                           std::span<const VulkanSegmentMicromap> micromaps = {} )
    {
        VulkanBlasBuildInput input;
        input.Geometries.reserve( segments.size() );
        input.Ranges.reserve( segments.size() );
        input.MaxPrimitiveCounts.reserve( segments.size() );
        input.Micromaps.reserve( segments.size() ); // never reallocated: geometries point into it
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
            // A non-opaque segment with a baked micromap (empty span = none at all): its fully opaque / fully
            // transparent micro-triangles are resolved by the traversal hardware, only unknown ones reach any-hit.
            const std::size_t segmentIndex = input.Geometries.size();
            if ( !segment.Opaque && segmentIndex < micromaps.size() &&
                 micromaps[segmentIndex].Micromap != VK_NULL_HANDLE )
            {
                VkAccelerationStructureTrianglesOpacityMicromapEXT& link = input.Micromaps.emplace_back();
                link.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_TRIANGLES_OPACITY_MICROMAP_EXT;
                link.indexType                 = VK_INDEX_TYPE_UINT32;
                link.indexBuffer.deviceAddress = micromaps[segmentIndex].IndexAddress;
                link.indexStride               = sizeof( uint32_t );
                link.baseTriangle              = 0;
                link.micromap                  = micromaps[segmentIndex].Micromap;
                triangles.pNext                = &link;
            }
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
